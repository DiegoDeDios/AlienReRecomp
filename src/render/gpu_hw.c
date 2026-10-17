/*
 * Hardware GPU backend on the SDL3 GPU API (Metal on macOS, Vulkan or
 * Direct3D 12 on Windows).
 *
 * VRAM lives in an upscaled RGBA8 render target (1024*S x 512*S). Every
 * texel encodes a 16-bit PS1 pixel (5-5-5 colour + mask bit in alpha), so
 * the fragment shader can reconstruct raw VRAM words to decode 4/8/15-bit
 * textures and CLUTs exactly like the hardware does, while primitives are
 * rasterized at S times the native resolution.
 *
 * Textures are sampled from a second texture (vram_read) that is refreshed
 * from the render target only when a draw samples an area that was
 * rendered to since the last refresh (render-to-texture).
 *
 * Semi-transparency maps to fixed-function blending:
 *   mode 0  B/2 + F/2   (constant 0.5, 0.5)
 *   mode 1  B + F
 *   mode 2  B - F       (reverse subtract)
 *   mode 3  B + F/4     (constant 0.25, 1)
 * Textured semi-transparent primitives are drawn in two passes (opaque
 * texels, then texels with the STP bit), as on the PS1 only STP texels
 * blend.
 */
#include <stdlib.h>

#include "../hw/gpu.h"
#include "../runtime/psx.h"
#include "../runtime/widescreen.h"
#include "render.h"

/* shaders: GLSL sources in shaders/, compiled for every backend by
 * tools/compile_shaders.py */
#include "shaders_gen.h"

/* ------------------------------------------------------------------ */

typedef struct {
    float x, y;
    uint8_t r, g, b, a;
    float u, v;
    uint32_t info[4];
    float w;        /* 1 = affine; otherwise depth for perspective (PGXP) */
} HwVertex;

typedef struct {
    uint32_t first, count;
    int blend;      /* -1 none, 0..3 semi mode */
    uint32_t pass;  /* 0 all, 1 opaque texels, 2 stp texels */
    int sx, sy, sw, sh; /* scissor in native pixels */
} DrawCmd;

#define MAX_VERTS 65536
#define MAX_DRAWS 8192
/* upload staging: room for several flushes / uploads per frame */
#define VTB_SIZE (4u * MAX_VERTS * (uint32_t)sizeof(HwVertex))
#define TEX_TB_SIZE (8u * 1024u * 1024u)

static struct {
    SDL_GPUDevice *dev;
    SDL_Window *win;
    RenderConfig cfg;
    int S;
    SDL_GPUTexture *vram, *vram_read, *upload, *tmp, *offscreen;
    int off_w, off_h;
    SDL_GPUBuffer *vbuf;
    SDL_GPUTransferBuffer *vtb, *tex_tb, *dl_tb;
    SDL_GPUGraphicsPipeline *draw_pipe[5]; /* none, mode0..3 */
    SDL_GPUGraphicsPipeline *blit_pipe;
    SDL_GPUGraphicsPipeline *disp_pipe, *disp_off_pipe;
    SDL_GPUSampler *nearest, *linear;
    SDL_GPUCommandBuffer *cmd;
    HwVertex verts[MAX_VERTS];
    uint32_t nverts;
    DrawCmd draws[MAX_DRAWS];
    uint32_t ndraws;
    /* rendered-since-last-refresh area of vram (native coords) */
    int dirty_x1, dirty_y1, dirty_x2, dirty_y2;
    bool dirty;
    uint8_t *capture;
    int cap_w, cap_h;
    bool capture_wanted;
    RenderOverlayFn overlay;
    uint32_t vtb_off, tex_off; /* staging offsets in this command buffer */
    /* fences of the last frames submitted (see submit_frame) */
    SDL_GPUFence *inflight[2];
    int inflight_pos;
    bool hold_aspect;
    float held_aspect;
} H;

static SDL_GPUShader *make_shader(const ShaderBlob *b, SDL_GPUShaderStage stage,
                                  int samplers, int uniforms) {
    SDL_GPUShader *s = render_create_shader(H.dev, b, stage, samplers, uniforms);
    if (!s)
        fatal("shader: %s", SDL_GetError());
    return s;
}

static SDL_GPUColorTargetBlendState blend_state(int mode) {
    SDL_GPUColorTargetBlendState b = {0};
    if (mode < 0)
        return b;
    b.enable_blend = true;
    b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    switch (mode) {
    case 0:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
        b.color_blend_op = SDL_GPU_BLENDOP_ADD;
        break;
    case 1:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.color_blend_op = SDL_GPU_BLENDOP_ADD;
        break;
    case 2:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.color_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
        break;
    default:
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.color_blend_op = SDL_GPU_BLENDOP_ADD;
        break;
    }
    return b;
}

static SDL_GPUGraphicsPipeline *make_draw_pipeline(SDL_GPUShader *vs,
                                                   SDL_GPUShader *fs, int mode) {
    SDL_GPUVertexBufferDescription vbd = {
        .slot = 0, .pitch = sizeof(HwVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
    SDL_GPUVertexAttribute attrs[5] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(HwVertex, x)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(HwVertex, r)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(HwVertex, u)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, offsetof(HwVertex, info)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(HwVertex, w)},
    };
    SDL_GPUColorTargetDescription ctd = {
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .blend_state = blend_state(mode)};
    SDL_GPUGraphicsPipelineCreateInfo ci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .vertex_input_state = {&vbd, 1, attrs, 5},
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL,
                             .cull_mode = SDL_GPU_CULLMODE_NONE},
        .target_info = {.color_target_descriptions = &ctd,
                        .num_color_targets = 1},
    };
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(H.dev, &ci);
    if (!p)
        fatal("draw pipeline: %s", SDL_GetError());
    return p;
}

typedef struct { float x, y, u, v; } BlitVertex;

static SDL_GPUGraphicsPipeline *make_blit_pipeline(void) {
    SDL_GPUShader *vs = make_shader(&k_blit_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader *fs = make_shader(&k_blit_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUVertexBufferDescription vbd = {
        .slot = 0, .pitch = sizeof(BlitVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
    SDL_GPUVertexAttribute attrs[2] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8},
    };
    SDL_GPUColorTargetDescription ctd = {
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM};
    SDL_GPUGraphicsPipelineCreateInfo ci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .vertex_input_state = {&vbd, 1, attrs, 2},
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.cull_mode = SDL_GPU_CULLMODE_NONE},
        .target_info = {.color_target_descriptions = &ctd,
                        .num_color_targets = 1},
    };
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(H.dev, &ci);
    SDL_ReleaseGPUShader(H.dev, vs);
    SDL_ReleaseGPUShader(H.dev, fs);
    if (!p)
        fatal("blit pipeline: %s", SDL_GetError());
    return p;
}

static SDL_GPUGraphicsPipeline *make_display_pipeline(SDL_GPUTextureFormat fmt) {
    SDL_GPUShader *vs = make_shader(&k_disp_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *fs = make_shader(&k_disp_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUColorTargetDescription ctd = {.format = fmt};
    SDL_GPUGraphicsPipelineCreateInfo ci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.cull_mode = SDL_GPU_CULLMODE_NONE},
        .target_info = {.color_target_descriptions = &ctd,
                        .num_color_targets = 1},
    };
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(H.dev, &ci);
    SDL_ReleaseGPUShader(H.dev, vs);
    SDL_ReleaseGPUShader(H.dev, fs);
    if (!p)
        fatal("display pipeline: %s", SDL_GetError());
    return p;
}

static SDL_GPUTexture *make_texture(int w, int h, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo ci = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = usage,
        .width = (Uint32)w,
        .height = (Uint32)h,
        .layer_count_or_depth = 1,
        .num_levels = 1,
    };
    SDL_GPUTexture *t = SDL_CreateGPUTexture(H.dev, &ci);
    if (!t)
        fatal("texture %dx%d: %s", w, h, SDL_GetError());
    return t;
}

static void ensure_cmd(void) {
    if (!H.cmd) {
        H.cmd = SDL_AcquireGPUCommandBuffer(H.dev);
        if (!H.cmd)
            fatal("acquire command buffer: %s", SDL_GetError());
        H.vtb_off = H.tex_off = 0;
    }
}

/* Staging for uploads: within one command buffer, successive uploads go to
 * disjoint ranges of the same transfer buffer, and the buffer is cycled
 * (SDL hands out an instance no pending command buffer uses) only for the
 * first upload of a command buffer or when it is full. Cycling on every
 * upload made SDL allocate a new instance per flush while the earlier ones
 * were still referenced by the same command buffer: hundreds of megabytes
 * per frame in flight. Returns the mapped range; *at is its offset and
 * *cycled says whether a destination buffer written from it must be cycled
 * too (its earlier contents may still be in use). */
static uint8_t *stage_map(SDL_GPUTransferBuffer *tb, uint32_t *off, uint32_t size,
                          uint32_t bytes, uint32_t align, uint32_t *at, bool *cycled) {
    bool cycle = *off == 0 || *off + bytes > size;
    if (cycle)
        *off = 0;
    uint8_t *p = SDL_MapGPUTransferBuffer(H.dev, tb, cycle);
    if (!p)
        fatal("map transfer buffer: %s", SDL_GetError());
    *at = *off;
    *off = (*off + bytes + align - 1) & ~(align - 1);
    if (cycled)
        *cycled = cycle;
    return p + *at;
}

static inline void rgba_from16(uint16_t p, uint8_t *o) {
    int r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
    o[0] = (uint8_t)((r * 255 + 15) / 31);
    o[1] = (uint8_t)((g * 255 + 15) / 31);
    o[2] = (uint8_t)((b * 255 + 15) / 31);
    o[3] = (p & 0x8000) ? 255 : 0;
}

static inline uint16_t from_rgba(const uint8_t *c) {
    int r = (c[0] * 31 + 127) / 255, g = (c[1] * 31 + 127) / 255,
        b = (c[2] * 31 + 127) / 255;
    return (uint16_t)(r | (g << 5) | (b << 10) | (c[3] > 127 ? 0x8000 : 0));
}

/* ------------------------------------------------------------------ */
/* dirty tracking                                                      */

static void mark_dirty(int x1, int y1, int x2, int y2) {
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > 1023) x2 = 1023;
    if (y2 > 511) y2 = 511;
    if (x1 > x2 || y1 > y2)
        return;
    if (!H.dirty) {
        H.dirty_x1 = x1, H.dirty_y1 = y1, H.dirty_x2 = x2, H.dirty_y2 = y2;
        H.dirty = true;
        return;
    }
    if (x1 < H.dirty_x1) H.dirty_x1 = x1;
    if (y1 < H.dirty_y1) H.dirty_y1 = y1;
    if (x2 > H.dirty_x2) H.dirty_x2 = x2;
    if (y2 > H.dirty_y2) H.dirty_y2 = y2;
}

static bool dirty_overlaps(int x1, int y1, int x2, int y2) {
    if (!H.dirty)
        return false;
    return !(x2 < H.dirty_x1 || x1 > H.dirty_x2 || y2 < H.dirty_y1 ||
             y1 > H.dirty_y2);
}

/* ------------------------------------------------------------------ */
/* batch submission                                                    */

static void flush_draws(void) {
    if (!H.ndraws) {
        H.nverts = 0;
        return;
    }
    ensure_cmd();
    /* upload vertices */
    uint32_t bytes = H.nverts * (uint32_t)sizeof(HwVertex), at;
    bool cycle;
    uint8_t *p = stage_map(H.vtb, &H.vtb_off, VTB_SIZE, bytes, 256, &at, &cycle);
    memcpy(p, H.verts, bytes);
    SDL_UnmapGPUTransferBuffer(H.dev, H.vtb);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(H.cmd);
    SDL_GPUTransferBufferLocation src = {H.vtb, at};
    SDL_GPUBufferRegion dst = {H.vbuf, at, bytes};
    SDL_UploadToGPUBuffer(cp, &src, &dst, cycle);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUColorTargetInfo ct = {.texture = H.vram,
                                 .load_op = SDL_GPU_LOADOP_LOAD,
                                 .store_op = SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(H.cmd, &ct, 1, NULL);
    SDL_GPUBufferBinding vb = {H.vbuf, at};
    SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
    SDL_GPUTextureSamplerBinding ts = {H.vram_read, H.nearest};
    SDL_BindGPUFragmentSamplers(rp, 0, &ts, 1);
    float vuni[4] = {1024.0f, 512.0f, 0, 0};
    SDL_PushGPUVertexUniformData(H.cmd, 0, vuni, sizeof vuni);
    SDL_GPUViewport vp = {0, 0, 1024.0f * H.S, 512.0f * H.S, 0, 1};
    SDL_SetGPUViewport(rp, &vp);
    int cur_blend = -2;
    uint32_t cur_pass = 99;
    for (uint32_t i = 0; i < H.ndraws; i++) {
        DrawCmd *d = &H.draws[i];
        if (d->blend != cur_blend) {
            SDL_BindGPUGraphicsPipeline(rp, H.draw_pipe[d->blend + 1]);
            if (d->blend == 0) {
                SDL_FColor k = {0.5f, 0.5f, 0.5f, 0.5f};
                SDL_SetGPUBlendConstants(rp, k);
            } else if (d->blend == 3) {
                SDL_FColor k = {0.25f, 0.25f, 0.25f, 0.25f};
                SDL_SetGPUBlendConstants(rp, k);
            }
            cur_blend = d->blend;
        }
        if (d->pass != cur_pass) {
            uint32_t funi[4];
            float sc = (float)H.S;
            memcpy(&funi[0], &sc, 4);
            funi[1] = d->pass;
            funi[2] = funi[3] = 0;
            SDL_PushGPUFragmentUniformData(H.cmd, 0, funi, sizeof funi);
            cur_pass = d->pass;
        }
        SDL_Rect sc = {d->sx * H.S, d->sy * H.S, d->sw * H.S, d->sh * H.S};
        SDL_SetGPUScissor(rp, &sc);
        SDL_DrawGPUPrimitives(rp, d->count, 1, d->first, 0);
    }
    SDL_EndGPURenderPass(rp);
    H.ndraws = 0;
    H.nverts = 0;
}

/* copy the dirty region of the render target into the sampling copy */
static void refresh_read(void) {
    if (!H.dirty)
        return;
    flush_draws();
    ensure_cmd();
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(H.cmd);
    int x = H.dirty_x1 * H.S, y = H.dirty_y1 * H.S;
    int w = (H.dirty_x2 - H.dirty_x1 + 1) * H.S, h = (H.dirty_y2 - H.dirty_y1 + 1) * H.S;
    SDL_GPUTextureLocation s = {.texture = H.vram, .x = (Uint32)x, .y = (Uint32)y};
    SDL_GPUTextureLocation d = {.texture = H.vram_read, .x = (Uint32)x, .y = (Uint32)y};
    SDL_CopyGPUTextureToTexture(cp, &s, &d, (Uint32)w, (Uint32)h, 1, false);
    SDL_EndGPUCopyPass(cp);
    H.dirty = false;
}

/* ------------------------------------------------------------------ */
/* primitive submission                                                */

/* Before a textured draw: if the texels/CLUT it samples were rendered to
 * since the last refresh, flush and refresh the sampling copy. The check
 * uses the primitive's UV bounding box (whole window if a texture window
 * is active). */
static void before_textured(const GpuState *s, uint16_t tpage, uint16_t clut,
                            int umin, int vmin, int umax, int vmax) {
    if (!H.dirty)
        return;
    if (s->tw_mask_x) umin = 0, umax = 255;
    if (s->tw_mask_y) vmin = 0, vmax = 255;
    if (umin < 0) umin = 0;
    if (vmin < 0) vmin = 0;
    if (umax > 255) umax = 255;
    if (vmax > 255) vmax = 255;
    int tx = (tpage & 0xF) * 64, ty = ((tpage >> 4) & 1) * 256;
    int depth = (tpage >> 7) & 3;
    int sh = depth == 0 ? 2 : (depth == 1 ? 1 : 0);
    bool hit = dirty_overlaps(tx + (umin >> sh), ty + vmin, tx + (umax >> sh),
                              ty + vmax);
    if (!hit && depth < 2) {
        int cx = (clut & 0x3F) * 16, cy = (clut >> 6) & 0x1FF;
        hit = dirty_overlaps(cx, cy, cx + (depth == 0 ? 15 : 255), cy);
    }
    if (hit)
        refresh_read();
}

static void push_draw(int blend, uint32_t pass, uint32_t first, uint32_t count,
                      const GpuState *s) {
    int sx = s->area_x1, sy = s->area_y1;
    int sw = s->area_x2 - s->area_x1 + 1, sh = s->area_y2 - s->area_y1 + 1;
    if (sw <= 0 || sh <= 0)
        return;
    if (H.ndraws) {
        DrawCmd *l = &H.draws[H.ndraws - 1];
        if (l->blend == blend && l->pass == pass && l->sx == sx && l->sy == sy &&
            l->sw == sw && l->sh == sh && l->first + l->count == first) {
            l->count += count;
            return;
        }
    }
    if (H.ndraws >= MAX_DRAWS)
        flush_draws();
    H.draws[H.ndraws++] = (DrawCmd){first, count, blend, pass, sx, sy, sw, sh};
}

static uint32_t make_info0(uint16_t tpage, uint32_t flags, const GpuState *s) {
    uint32_t f = 0;
    if (flags & PRIM_TEXTURED) f |= 1;
    if (flags & PRIM_RAW) f |= 2;
    if (flags & PRIM_SEMI) f |= 4;
    if (s->set_mask) f |= 8;
    return (tpage & 0x1FFu) | (f << 16);
}

static uint32_t texwin(const GpuState *s) {
    return (uint32_t)s->tw_mask_x | ((uint32_t)s->tw_mask_y << 5) |
           ((uint32_t)s->tw_off_x << 10) | ((uint32_t)s->tw_off_y << 15);
}

/* Queue triangles (n vertices, multiple of 3) with PS1 semantics for the
 * semi-transparency passes. */
static void emit(const HwVertex *v, int n, uint32_t flags, uint16_t tpage,
                 const GpuState *s) {
    if (H.nverts + (uint32_t)n * 2 > MAX_VERTS)
        flush_draws();
    int x1 = 4096, y1 = 4096, x2 = -4096, y2 = -4096;
    for (int i = 0; i < n; i++) {
        if (v[i].x < x1) x1 = (int)v[i].x;
        if (v[i].y < y1) y1 = (int)v[i].y;
        if (v[i].x > x2) x2 = (int)v[i].x + 1;
        if (v[i].y > y2) y2 = (int)v[i].y + 1;
    }
    if (x1 < s->area_x1) x1 = s->area_x1;
    if (y1 < s->area_y1) y1 = s->area_y1;
    if (x2 > s->area_x2) x2 = s->area_x2;
    if (y2 > s->area_y2) y2 = s->area_y2;
    uint32_t first = H.nverts;
    memcpy(&H.verts[H.nverts], v, sizeof(HwVertex) * (size_t)n);
    H.nverts += (uint32_t)n;
    int mode = (tpage >> 5) & 3;
    if (!(flags & PRIM_SEMI)) {
        push_draw(-1, 0, first, (uint32_t)n, s);
    } else if (flags & PRIM_TEXTURED) {
        push_draw(-1, 1, first, (uint32_t)n, s);
        push_draw(mode, 2, first, (uint32_t)n, s);
    } else {
        push_draw(mode, 0, first, (uint32_t)n, s);
    }
    mark_dirty(x1, y1, x2, y2);
}

/* widescreen: squash non-GTE geometry back to its original proportions */
static inline float ws_x(const GpuState *s, uint32_t flags, float x) {
    if (flags & (PRIM_WS_2D | PRIM_WS_SPRITE))
        return s->ws_anchor + (x - s->ws_anchor) * g_ws_k;
    return x;
}

static void set_vertex(HwVertex *o, const GVtx *v, uint32_t info0,
                       uint16_t clut, uint32_t tw, bool shaded,
                       const GVtx *flat) {
    if (v->precise) {
        o->x = v->fx + 0.5f;
        o->y = v->fy + 0.5f;
    } else {
        o->x = (float)v->x + 0.5f;
        o->y = (float)v->y + 0.5f;
    }
    o->w = 1.0f;
    const GVtx *c = shaded ? v : flat;
    o->r = c->r;
    o->g = c->g;
    o->b = c->b;
    o->a = 255;
    o->u = (float)v->u;
    o->v = (float)v->v;
    o->info[0] = info0;
    o->info[1] = clut;
    o->info[2] = tw;
    o->info[3] = 0;
}

/* Horizontal span limit for the hardware's oversized-primitive rejection.
 * With widescreen the GTE narrows 3D projections by k, so the limit must be
 * applied to the original (unscaled) span: otherwise near-camera polygons
 * that the PS1 discards would be drawn as long stretched "explosions". */
static inline int span_limit_x(uint32_t flags) {
    if (g_ws_enabled && !(flags & (PRIM_WS_2D | PRIM_WS_SPRITE)))
        return (int)(1024.0f * g_ws_k);
    return 1024;
}

static void hw_triangle(const GpuState *s, const GVtx v[3], uint32_t flags,
                        uint16_t clut, uint16_t tpage) {
    int lim = span_limit_x(flags);
    /* The PS1 rejects oversized triangles; that also discards garbage from
     * vertices behind the camera. Triangles whose vertices are all precise
     * (PGXP) are genuine projections in front of the camera and are drawn
     * whole, with the GPU clipping them to the screen. */
    bool all_precise = v[0].precise && v[1].precise && v[2].precise;
    for (int i = 0; i < 3 && !all_precise; i++) {
        int j = (i + 1) % 3;
        if (abs(v[i].x - v[j].x) >= lim || abs(v[i].y - v[j].y) >= 512) {
            static unsigned long n;
            if (abs(v[i].x - v[j].x) < 1024 && abs(v[i].y - v[j].y) < 512 && (++n & 63) == 1)
                LOGD("render", "rejected widened near-camera triangle (span %d, total %lu)",
                     abs(v[i].x - v[j].x), n);
            return;
        }
    }
    if (flags & PRIM_TEXTURED) {
        int umin = 255, vmin = 255, umax = 0, vmax = 0;
        for (int i = 0; i < 3; i++) {
            if (v[i].u < umin) umin = v[i].u;
            if (v[i].u > umax) umax = v[i].u;
            if (v[i].v < vmin) vmin = v[i].v;
            if (v[i].v > vmax) vmax = v[i].v;
        }
        before_textured(s, tpage, clut, umin, vmin, umax, vmax);
    }
    uint32_t info0 = make_info0(tpage, flags, s);
    uint32_t tw = texwin(s);
    HwVertex hv[3];
    bool persp = v[0].precise && v[1].precise && v[2].precise;
    for (int i = 0; i < 3; i++) {
        set_vertex(&hv[i], &v[i], info0, clut, tw, flags & PRIM_SHADED, &v[0]);
        hv[i].x = ws_x(s, flags, hv[i].x);
        /* any positive scale works; keep w around 1..256 */
        if (persp)
            hv[i].w = v[i].fz * (1.0f / 64.0f) + 0.001f;
    }
    emit(hv, 3, flags, tpage, s);
}

static void hw_rect(const GpuState *s, const GVtx *v, int w, int h,
                    uint32_t flags, uint16_t clut) {
    uint16_t tpage = s->tpage;
    if (flags & PRIM_TEXTURED) {
        int u0 = v->u, v0 = v->v;
        int u1 = s->rect_flip_x ? u0 - w + 1 : u0 + w - 1;
        int v1 = s->rect_flip_y ? v0 - h + 1 : v0 + h - 1;
        bool wrap = u1 > 255 || u1 < 0 || v1 > 255 || v1 < 0;
        before_textured(s, tpage, clut, wrap ? 0 : (u0 < u1 ? u0 : u1),
                        wrap ? 0 : (v0 < v1 ? v0 : v1),
                        wrap ? 255 : (u0 > u1 ? u0 : u1),
                        wrap ? 255 : (v0 > v1 ? v0 : v1));
    }
    uint32_t info0 = make_info0(tpage, flags, s);
    uint32_t tw = texwin(s);
    float x0 = (float)v->x + 0.5f, y0 = (float)v->y + 0.5f;
    float x1 = x0 + (float)w, y1 = y0 + (float)h;
    x0 = ws_x(s, flags, x0);
    x1 = ws_x(s, flags, x1);
    float u0 = (float)v->u, v0 = (float)v->v;
    float u1 = s->rect_flip_x ? u0 - (float)w + 1.0f : u0 + (float)w;
    float v1 = s->rect_flip_y ? v0 - (float)h + 1.0f : v0 + (float)h;
    if (s->rect_flip_x) u0 += 1.0f;
    if (s->rect_flip_y) v0 += 1.0f;
    HwVertex q[6];
    const float px[6] = {x0, x1, x0, x1, x1, x0};
    const float py[6] = {y0, y0, y1, y0, y1, y1};
    const float pu[6] = {u0, u1, u0, u1, u1, u0};
    const float pv[6] = {v0, v0, v1, v0, v1, v1};
    for (int i = 0; i < 6; i++) {
        q[i].x = px[i];
        q[i].y = py[i];
        q[i].r = v->r, q[i].g = v->g, q[i].b = v->b, q[i].a = 255;
        q[i].u = pu[i];
        q[i].v = pv[i];
        q[i].info[0] = info0;
        q[i].info[1] = clut;
        q[i].info[2] = tw;
        q[i].info[3] = 0;
        q[i].w = 1.0f;
    }
    emit(q, 6, flags, tpage, s);
}

static void hw_line(const GpuState *s, const GVtx *a, const GVtx *b,
                    uint32_t flags) {
    int dx = b->x - a->x, dy = b->y - a->y;
    if (abs(dx) >= span_limit_x(flags) || abs(dy) >= 512)
        return;
    /* expand into a one-pixel wide quad */
    float ax = ws_x(s, flags, (float)a->x + 0.5f), ay = (float)a->y + 0.5f;
    float bx = ws_x(s, flags, (float)b->x + 0.5f), by = (float)b->y + 0.5f;
    float ox, oy;
    if (abs(dx) >= abs(dy)) {
        ox = 0.0f, oy = 0.5f;
        if (dx >= 0) bx += 1.0f; else ax += 1.0f;
    } else {
        ox = 0.5f, oy = 0.0f;
        if (dy >= 0) by += 1.0f; else ay += 1.0f;
    }
    uint32_t info0 = make_info0(s->tpage, flags & ~(uint32_t)PRIM_TEXTURED, s);
    bool shaded = flags & PRIM_SHADED;
    HwVertex q[6];
    const GVtx *cv[6] = {a, b, a, b, b, a};
    const float px[6] = {ax - ox, bx - ox, ax + ox, bx - ox, bx + ox, ax + ox};
    const float py[6] = {ay - oy, by - oy, ay + oy, by - oy, by + oy, ay + oy};
    for (int i = 0; i < 6; i++) {
        const GVtx *c = shaded ? cv[i] : a;
        q[i].x = px[i];
        q[i].y = py[i];
        q[i].r = c->r, q[i].g = c->g, q[i].b = c->b, q[i].a = 255;
        q[i].u = q[i].v = 0;
        q[i].info[0] = info0;
        q[i].info[1] = 0;
        q[i].info[2] = 0;
        q[i].info[3] = 0;
        q[i].w = 1.0f;
    }
    emit(q, 6, flags & ~(uint32_t)PRIM_TEXTURED, s->tpage, s);
}

/* ------------------------------------------------------------------ */
/* VRAM operations                                                     */

extern const GpuBackend g_gpu_sw;

/* draw a native-resolution rectangle of the upload texture into vram and
 * mirror it into vram_read */
static void blit_upload(int x, int y, int w, int h, bool setmask) {
    ensure_cmd();
    BlitVertex q[6];
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    const float px[6] = {x0, x1, x0, x1, x1, x0};
    const float py[6] = {y0, y0, y1, y0, y1, y1};
    for (int i = 0; i < 6; i++) {
        q[i].x = px[i];
        q[i].y = py[i];
        q[i].u = px[i];
        q[i].v = py[i];
    }
    uint32_t at;
    bool cycle;
    uint8_t *p = stage_map(H.vtb, &H.vtb_off, VTB_SIZE, sizeof q, 256, &at, &cycle);
    memcpy(p, q, sizeof q);
    SDL_UnmapGPUTransferBuffer(H.dev, H.vtb);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(H.cmd);
    SDL_GPUTransferBufferLocation src = {H.vtb, at};
    SDL_GPUBufferRegion dst = {H.vbuf, at, sizeof q};
    SDL_UploadToGPUBuffer(cp, &src, &dst, cycle);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUColorTargetInfo ct = {.texture = H.vram,
                                 .load_op = SDL_GPU_LOADOP_LOAD,
                                 .store_op = SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(H.cmd, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(rp, H.blit_pipe);
    SDL_GPUBufferBinding vb = {H.vbuf, at};
    SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
    SDL_GPUTextureSamplerBinding ts = {H.upload, H.nearest};
    SDL_BindGPUFragmentSamplers(rp, 0, &ts, 1);
    float vuni[4] = {1024.0f, 512.0f, 0, 0};
    SDL_PushGPUVertexUniformData(H.cmd, 0, vuni, sizeof vuni);
    float buni[4] = {setmask ? 1.0f : 0.0f, 0, 0, 0};
    SDL_PushGPUFragmentUniformData(H.cmd, 0, buni, sizeof buni);
    SDL_GPUViewport vp = {0, 0, 1024.0f * H.S, 512.0f * H.S, 0, 1};
    SDL_SetGPUViewport(rp, &vp);
    SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
    SDL_EndGPURenderPass(rp);

    cp = SDL_BeginGPUCopyPass(H.cmd);
    SDL_GPUTextureLocation s = {.texture = H.vram, .x = (Uint32)(x * H.S), .y = (Uint32)(y * H.S)};
    SDL_GPUTextureLocation d = {.texture = H.vram_read, .x = (Uint32)(x * H.S), .y = (Uint32)(y * H.S)};
    SDL_CopyGPUTextureToTexture(cp, &s, &d, (Uint32)(w * H.S), (Uint32)(h * H.S), 1, false);
    SDL_EndGPUCopyPass(cp);
}

/* upload a native rect (no wrap) from g_vram into the GPU textures */
static void upload_rect(int x, int y, int w, int h, bool setmask) {
    if (w <= 0 || h <= 0)
        return;
    ensure_cmd();
    uint32_t at;
    /* 512: D3D12's texture upload offset alignment */
    uint8_t *p = stage_map(H.tex_tb, &H.tex_off, TEX_TB_SIZE, (uint32_t)(w * h * 4), 512, &at, NULL);
    for (int j = 0; j < h; j++) {
        const uint16_t *row = &g_vram[(y + j) * VRAM_W + x];
        uint8_t *o = p + (size_t)j * (size_t)w * 4;
        for (int i = 0; i < w; i++)
            rgba_from16(row[i], o + i * 4);
    }
    SDL_UnmapGPUTransferBuffer(H.dev, H.tex_tb);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(H.cmd);
    SDL_GPUTextureTransferInfo ti = {H.tex_tb, at, (Uint32)w, (Uint32)h};
    SDL_GPUTextureRegion r = {.texture = H.upload, .x = (Uint32)x, .y = (Uint32)y,
                              .w = (Uint32)w, .h = (Uint32)h, .d = 1};
    SDL_UploadToGPUTexture(cp, &ti, &r, false);
    SDL_EndGPUCopyPass(cp);
    blit_upload(x, y, w, h, setmask);
}

static void upload_wrapped(int x, int y, int w, int h, bool setmask) {
    int w1 = x + w > VRAM_W ? VRAM_W - x : w;
    int h1 = y + h > VRAM_H ? VRAM_H - y : h;
    upload_rect(x, y, w1, h1, setmask);
    if (w1 < w)
        upload_rect(0, y, w - w1, h1, setmask);
    if (h1 < h) {
        upload_rect(x, 0, w1, h - h1, setmask);
        if (w1 < w)
            upload_rect(0, 0, w - w1, h - h1, setmask);
    }
}

static void hw_vram_write(const GpuState *s, int x, int y, int w, int h,
                          const uint16_t *data) {
    flush_draws();
    /* keep the CPU shadow current, then upload from it */
    g_gpu_sw.vram_write(s, x, y, w, h, data);
    upload_wrapped(x, y, w, h, false);
}

static void hw_fill(int x, int y, int w, int h, uint16_t color) {
    g_gpu_sw.fill(x, y, w, h, color);
    /* a fill ignores draw area and mask: queue an untextured quad */
    GpuState full = {0};
    full.area_x2 = 1023;
    full.area_y2 = 511;
    uint8_t c[4];
    rgba_from16(color, c);
    int pieces[4][4];
    int n = 0;
    int w1 = x + w > VRAM_W ? VRAM_W - x : w;
    int h1 = y + h > VRAM_H ? VRAM_H - y : h;
    pieces[n][0] = x, pieces[n][1] = y, pieces[n][2] = w1, pieces[n][3] = h1, n++;
    if (w1 < w) { pieces[n][0] = 0, pieces[n][1] = y, pieces[n][2] = w - w1, pieces[n][3] = h1, n++; }
    if (h1 < h) {
        pieces[n][0] = x, pieces[n][1] = 0, pieces[n][2] = w1, pieces[n][3] = h - h1, n++;
        if (w1 < w) { pieces[n][0] = 0, pieces[n][1] = 0, pieces[n][2] = w - w1, pieces[n][3] = h - h1, n++; }
    }
    for (int k = 0; k < n; k++) {
        float x0 = (float)pieces[k][0], y0 = (float)pieces[k][1];
        float x1 = x0 + (float)pieces[k][2], y1 = y0 + (float)pieces[k][3];
        HwVertex q[6];
        const float px[6] = {x0, x1, x0, x1, x1, x0};
        const float py[6] = {y0, y0, y1, y0, y1, y1};
        for (int i = 0; i < 6; i++) {
            memset(&q[i], 0, sizeof q[i]);
            q[i].x = px[i];
            q[i].y = py[i];
            q[i].r = c[0], q[i].g = c[1], q[i].b = c[2], q[i].a = 255;
            q[i].w = 1.0f;
        }
        emit(q, 6, 0, 0, &full);
    }
}

static void hw_vram_copy(const GpuState *s, int sx, int sy, int dx, int dy,
                         int w, int h) {
    g_gpu_sw.vram_copy(s, sx, sy, dx, dy, w, h);
    flush_draws();
    if (sx + w > VRAM_W || sy + h > VRAM_H || dx + w > VRAM_W ||
        dy + h > VRAM_H) {
        /* wrapping copies: fall back to the CPU shadow (rendered content in
         * the source is lost) */
        upload_wrapped(dx, dy, w, h, false);
        return;
    }
    ensure_cmd();
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(H.cmd);
    SDL_GPUTextureLocation a = {.texture = H.vram, .x = (Uint32)(sx * H.S), .y = (Uint32)(sy * H.S)};
    SDL_GPUTextureLocation t = {.texture = H.tmp, .x = 0, .y = 0};
    SDL_CopyGPUTextureToTexture(cp, &a, &t, (Uint32)(w * H.S), (Uint32)(h * H.S), 1, false);
    SDL_GPUTextureLocation d = {.texture = H.vram, .x = (Uint32)(dx * H.S), .y = (Uint32)(dy * H.S)};
    SDL_CopyGPUTextureToTexture(cp, &t, &d, (Uint32)(w * H.S), (Uint32)(h * H.S), 1, false);
    SDL_GPUTextureLocation r = {.texture = H.vram_read, .x = (Uint32)(dx * H.S), .y = (Uint32)(dy * H.S)};
    SDL_CopyGPUTextureToTexture(cp, &t, &r, (Uint32)(w * H.S), (Uint32)(h * H.S), 1, false);
    SDL_EndGPUCopyPass(cp);
}

/* read back a VRAM region into the CPU shadow (for GPUREAD transfers) */
static void hw_vram_sync(int x, int y, int w, int h) {
    flush_draws();
    if (x + w > VRAM_W) w = VRAM_W - x;
    if (y + h > VRAM_H) h = VRAM_H - y;
    ensure_cmd();
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(H.cmd);
    SDL_GPUTextureRegion r = {.texture = H.vram, .x = (Uint32)(x * H.S), .y = (Uint32)(y * H.S),
                              .w = (Uint32)(w * H.S), .h = (Uint32)(h * H.S), .d = 1};
    SDL_GPUTextureTransferInfo ti = {H.dl_tb, 0, (Uint32)(w * H.S), (Uint32)(h * H.S)};
    SDL_DownloadFromGPUTexture(cp, &r, &ti);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(H.cmd);
    H.cmd = NULL;
    SDL_WaitForGPUFences(H.dev, true, &f, 1);
    SDL_ReleaseGPUFence(H.dev, f);
    const uint8_t *p = SDL_MapGPUTransferBuffer(H.dev, H.dl_tb, false);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            g_vram[(y + j) * VRAM_W + x + i] =
                from_rgba(p + ((size_t)(j * H.S) * (size_t)(w * H.S) + (size_t)(i * H.S)) * 4);
    SDL_UnmapGPUTransferBuffer(H.dev, H.dl_tb);
}

const GpuBackend g_gpu_hw = {
    .name = "hardware",
    .triangle = hw_triangle,
    .rect = hw_rect,
    .line = hw_line,
    .fill = hw_fill,
    .vram_write = hw_vram_write,
    .vram_copy = hw_vram_copy,
    .vram_sync = hw_vram_sync,
    .end_frame = NULL,
};

/* ------------------------------------------------------------------ */
/* presentation                                                        */

/* FMV (24-bit) stays 4:3; everything else uses the configured display
 * aspect (widescreen) */
static float display_aspect(const DisplayInfo *d) {
    if (H.hold_aspect)
        return H.held_aspect;
    return (g_ws_wide_display && !d->rgb24) ? g_ws_aspect : 4.0f / 3.0f;
}

static void display_pass(SDL_GPUTexture *target, SDL_GPUGraphicsPipeline *pipe,
                         int tw, int th) {
    DisplayInfo d;
    gpu_display_info(&d);
    SDL_GPUColorTargetInfo ct = {.texture = target,
                                 .clear_color = {0, 0, 0, 1},
                                 .load_op = SDL_GPU_LOADOP_CLEAR,
                                 .store_op = SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(H.cmd, &ct, 1, NULL);
    if (d.enabled) {
        float aspect = display_aspect(&d);
        float dw = (float)tw, dh = dw / aspect;
        if (dh > (float)th) {
            dh = (float)th;
            dw = dh * aspect;
        }
        SDL_GPUViewport vp = {((float)tw - dw) / 2, ((float)th - dh) / 2, dw, dh, 0, 1};
        SDL_SetGPUViewport(rp, &vp);
        SDL_BindGPUGraphicsPipeline(rp, pipe);
        SDL_GPUTextureSamplerBinding ts = {H.vram,
                                           H.cfg.smooth ? H.linear : H.nearest};
        SDL_BindGPUFragmentSamplers(rp, 0, &ts, 1);
        struct { float rect[4]; float scale; uint32_t rgb24; float pad[2]; } u = {
            {(float)d.x, (float)d.y, (float)d.w, (float)d.h},
            (float)H.S, d.rgb24 ? 1u : 0u, {0, 0}};
        SDL_PushGPUFragmentUniformData(H.cmd, 0, &u, sizeof u);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
}

static void upload_full_vram(void) {
    /* software renderer: mirror the whole of g_vram into the texture */
    upload_rect(0, 0, VRAM_W, VRAM_H, false);
}

/* bring the VRAM texture up to date before reading the display area */
static void present_prepare(void) {
    if (H.cfg.software)
        upload_full_vram();
    else
        flush_draws();
    ensure_cmd();
}

/* Submit the frame's command buffer, then wait until the frame submitted
 * two frames ago has finished on the GPU. Presenting to a window already
 * blocks on the swapchain, but without one (headless runs, minimised
 * window, fast forward) nothing else stops the emulator from queueing
 * frames faster than the GPU draws them; each queued frame keeps its
 * cycled transfer buffers alive, and memory use grows without bound. */
static void submit_frame(void) {
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(H.cmd);
    H.cmd = NULL;
    SDL_GPUFence **slot = &H.inflight[H.inflight_pos];
    if (*slot) {
        SDL_WaitForGPUFences(H.dev, true, slot, 1);
        SDL_ReleaseGPUFence(H.dev, *slot);
    }
    *slot = f;
    H.inflight_pos = (H.inflight_pos + 1) % 2;
}

void render_present(void) {
    if (!H.dev)
        return;
    present_prepare();
    if (H.win && !H.cfg.headless) {
        SDL_GPUTexture *sw = NULL;
        Uint32 w = 0, h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(H.cmd, H.win, &sw, &w, &h)) {
            LOGW("render", "swapchain: %s", SDL_GetError());
        }
        if (sw) {
            display_pass(sw, H.disp_pipe, (int)w, (int)h);
            if (H.overlay)
                H.overlay(H.cmd, sw, (int)w, (int)h);
        }
    } else if (H.capture_wanted) {
        DisplayInfo d;
        gpu_display_info(&d);
        int h = (d.h > 0 && d.h <= 512 ? d.h : 240) * H.S;
        int w = (int)((float)h * (g_ws_enabled ? g_ws_aspect : 4.0f / 3.0f) + 0.5f);
        if (!H.offscreen || H.off_w != w || H.off_h != h) {
            if (H.offscreen)
                SDL_ReleaseGPUTexture(H.dev, H.offscreen);
            H.offscreen = make_texture(w, h, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                                 SDL_GPU_TEXTUREUSAGE_SAMPLER);
            H.off_w = w, H.off_h = h;
        }
        display_pass(H.offscreen, H.disp_off_pipe, w, h);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(H.cmd);
        SDL_GPUTextureRegion r = {.texture = H.offscreen, .w = (Uint32)w, .h = (Uint32)h, .d = 1};
        SDL_GPUTextureTransferInfo ti = {H.dl_tb, 0, (Uint32)w, (Uint32)h};
        SDL_DownloadFromGPUTexture(cp, &r, &ti);
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(H.cmd);
        H.cmd = NULL;
        SDL_WaitForGPUFences(H.dev, true, &f, 1);
        SDL_ReleaseGPUFence(H.dev, f);
        const uint8_t *p = SDL_MapGPUTransferBuffer(H.dev, H.dl_tb, false);
        free(H.capture);
        H.capture = malloc((size_t)w * (size_t)h * 3);
        for (int i = 0; i < w * h; i++) {
            H.capture[i * 3 + 0] = p[i * 4 + 0];
            H.capture[i * 3 + 1] = p[i * 4 + 1];
            H.capture[i * 3 + 2] = p[i * 4 + 2];
        }
        SDL_UnmapGPUTransferBuffer(H.dev, H.dl_tb);
        H.cap_w = w, H.cap_h = h;
        H.capture_wanted = false;
        return;
    }
    submit_frame();
}

bool render_capture(uint8_t **rgb, int *w, int *h) {
    if (!H.dev)
        return false;
    H.capture_wanted = true;
    bool was_headless = H.cfg.headless;
    H.cfg.headless = true;
    render_present();
    H.cfg.headless = was_headless;
    if (!H.capture)
        return false;
    *rgb = H.capture;
    *w = H.cap_w;
    *h = H.cap_h;
    H.capture = NULL;
    return true;
}

const char *render_device_name(void) {
    return H.dev ? SDL_GetGPUDeviceDriver(H.dev) : "none";
}

void render_set_overlay(RenderOverlayFn fn) { H.overlay = fn; }

SDL_GPUDevice *render_gpu_device(void) { return H.dev; }

SDL_GPUTextureFormat render_window_format(void) {
    if (!H.dev || !H.win || H.cfg.headless)
        return SDL_GPU_TEXTUREFORMAT_INVALID;
    return SDL_GetGPUSwapchainTextureFormat(H.dev, H.win);
}

bool render_capture_window(uint8_t **rgb, int *w, int *h) {
    if (!H.dev || !H.win || H.cfg.headless || !H.disp_pipe)
        return false;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(H.win, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        return false;
    SDL_GPUTextureFormat fmt = render_window_format();
    SDL_GPUTextureCreateInfo ti = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = fmt,
        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = (Uint32)pw,
        .height = (Uint32)ph,
        .layer_count_or_depth = 1,
        .num_levels = 1,
    };
    SDL_GPUTexture *t = SDL_CreateGPUTexture(H.dev, &ti);
    SDL_GPUTransferBufferCreateInfo bi = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                          .size = (Uint32)(pw * ph * 4)};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(H.dev, &bi);
    if (!t || !tb) {
        LOGW("render", "window capture: %s", SDL_GetError());
        if (t) SDL_ReleaseGPUTexture(H.dev, t);
        if (tb) SDL_ReleaseGPUTransferBuffer(H.dev, tb);
        return false;
    }
    present_prepare();
    display_pass(t, H.disp_pipe, pw, ph);
    if (H.overlay)
        H.overlay(H.cmd, t, pw, ph);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(H.cmd);
    SDL_GPUTextureRegion r = {.texture = t, .w = (Uint32)pw, .h = (Uint32)ph, .d = 1};
    SDL_GPUTextureTransferInfo dst = {tb, 0, (Uint32)pw, (Uint32)ph};
    SDL_DownloadFromGPUTexture(cp, &r, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(H.cmd);
    H.cmd = NULL;
    SDL_WaitForGPUFences(H.dev, true, &f, 1);
    SDL_ReleaseGPUFence(H.dev, f);
    const uint8_t *p = SDL_MapGPUTransferBuffer(H.dev, tb, false);
    bool bgra = fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    uint8_t *out = malloc((size_t)pw * (size_t)ph * 3);
    for (int i = 0; i < pw * ph; i++) {
        out[i * 3 + 0] = p[i * 4 + (bgra ? 2 : 0)];
        out[i * 3 + 1] = p[i * 4 + 1];
        out[i * 3 + 2] = p[i * 4 + (bgra ? 0 : 2)];
    }
    SDL_UnmapGPUTransferBuffer(H.dev, tb);
    SDL_ReleaseGPUTransferBuffer(H.dev, tb);
    SDL_ReleaseGPUTexture(H.dev, t);
    *rgb = out;
    *w = pw;
    *h = ph;
    return true;
}

void render_set_smooth(bool smooth) { H.cfg.smooth = smooth; }

void render_hold_display_aspect(bool hold) {
    if (hold && !H.hold_aspect) {
        DisplayInfo d;
        gpu_display_info(&d);
        H.held_aspect = display_aspect(&d);
    }
    H.hold_aspect = hold;
}

bool render_init(SDL_Window *win, const RenderConfig *cfg) {
    memset(&H, 0, sizeof H);
    H.cfg = *cfg;
    H.S = cfg->software ? 1 : (cfg->scale < 1 ? 1 : cfg->scale);
    H.win = win;
    static const ShaderBlob *const shaders[] = {&k_draw_vert, &k_draw_frag, &k_blit_vert,
                                                &k_blit_frag, &k_disp_vert, &k_disp_frag};
    static const SDL_GPUShaderStage stages[] = {
        SDL_GPU_SHADERSTAGE_VERTEX, SDL_GPU_SHADERSTAGE_FRAGMENT, SDL_GPU_SHADERSTAGE_VERTEX,
        SDL_GPU_SHADERSTAGE_FRAGMENT, SDL_GPU_SHADERSTAGE_VERTEX, SDL_GPU_SHADERSTAGE_FRAGMENT};
    render_require_shaders(shaders, stages, 6);
    H.dev = render_create_device();
    render_require_shaders(NULL, NULL, 0);
    if (!H.dev) {
        LOGE("render", "no usable GPU device: %s", SDL_GetError());
        return false;
    }
    if (win && !cfg->headless) {
        if (!SDL_ClaimWindowForGPUDevice(H.dev, win)) {
            LOGE("render", "claim window: %s", SDL_GetError());
            return false;
        }
        /* VSYNC avoids tearing; without it prefer mailbox, then immediate */
        SDL_GPUPresentMode pm = SDL_GPU_PRESENTMODE_VSYNC;
        if (!cfg->vsync) {
            if (SDL_WindowSupportsGPUPresentMode(H.dev, win, SDL_GPU_PRESENTMODE_MAILBOX))
                pm = SDL_GPU_PRESENTMODE_MAILBOX;
            else if (SDL_WindowSupportsGPUPresentMode(H.dev, win, SDL_GPU_PRESENTMODE_IMMEDIATE))
                pm = SDL_GPU_PRESENTMODE_IMMEDIATE;
        }
        SDL_SetGPUSwapchainParameters(H.dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm);
        /* Two frames in flight: the swapchain wait then blocks once per
         * display refresh (pacing emulation under vsync) while leaving a
         * full refresh interval for the next frame. With one frame in
         * flight the wait returns only after presentation and frames slip
         * to every other refresh. */
        SDL_SetGPUAllowedFramesInFlight(H.dev, 2);
        LOGI("render", "present mode: %s%s",
             pm == SDL_GPU_PRESENTMODE_VSYNC ? "vsync" :
             pm == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox" : "immediate",
             cfg->vsync_paced ? " (paced by display refresh)" : "");
    }
    int W = 1024 * H.S, Ht = 512 * H.S;
    H.vram = make_texture(W, Ht, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    H.vram_read = make_texture(W, Ht, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    H.tmp = make_texture(W, Ht, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    H.upload = make_texture(1024, 512, SDL_GPU_TEXTUREUSAGE_SAMPLER);

    SDL_GPUBufferCreateInfo bci = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = VTB_SIZE};
    H.vbuf = SDL_CreateGPUBuffer(H.dev, &bci);
    SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                           .size = VTB_SIZE};
    H.vtb = SDL_CreateGPUTransferBuffer(H.dev, &tci);
    tci.size = TEX_TB_SIZE;
    H.tex_tb = SDL_CreateGPUTransferBuffer(H.dev, &tci);
    SDL_GPUTransferBufferCreateInfo dci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                           .size = (Uint32)(W * Ht * 4)};
    H.dl_tb = SDL_CreateGPUTransferBuffer(H.dev, &dci);
    if (!H.vbuf || !H.vtb || !H.tex_tb || !H.dl_tb)
        fatal("gpu buffers: %s", SDL_GetError());

    SDL_GPUSamplerCreateInfo sn = {
        .min_filter = SDL_GPU_FILTER_NEAREST, .mag_filter = SDL_GPU_FILTER_NEAREST,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
    H.nearest = SDL_CreateGPUSampler(H.dev, &sn);
    sn.min_filter = sn.mag_filter = SDL_GPU_FILTER_LINEAR;
    H.linear = SDL_CreateGPUSampler(H.dev, &sn);

    SDL_GPUShader *vs = make_shader(&k_draw_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader *fs = make_shader(&k_draw_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    for (int m = -1; m < 4; m++)
        H.draw_pipe[m + 1] = make_draw_pipeline(vs, fs, m);
    SDL_ReleaseGPUShader(H.dev, vs);
    SDL_ReleaseGPUShader(H.dev, fs);
    H.blit_pipe = make_blit_pipeline();
    H.disp_off_pipe = make_display_pipeline(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    if (win && !cfg->headless)
        H.disp_pipe = make_display_pipeline(SDL_GetGPUSwapchainTextureFormat(H.dev, win));

    /* clear VRAM textures */
    ensure_cmd();
    SDL_GPUTexture *ts[3] = {H.vram, H.vram_read, H.tmp};
    for (int i = 0; i < 3; i++) {
        SDL_GPUColorTargetInfo ct = {.texture = ts[i], .clear_color = {0, 0, 0, 0},
                                     .load_op = SDL_GPU_LOADOP_CLEAR,
                                     .store_op = SDL_GPU_STOREOP_STORE};
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(H.cmd, &ct, 1, NULL));
    }
    SDL_SubmitGPUCommandBuffer(H.cmd);
    H.cmd = NULL;

    if (!cfg->software)
        g_gpu_backend = &g_gpu_hw;
    LOGI("render", "GPU device: %s, %s renderer, internal resolution %dx%d",
         SDL_GetGPUDeviceDriver(H.dev), cfg->software ? "software" : "hardware",
         W, Ht);
    return true;
}

void render_shutdown(void) {
    if (!H.dev)
        return;
    SDL_WaitForGPUIdle(H.dev);
    for (int i = 0; i < 2; i++)
        if (H.inflight[i]) {
            SDL_ReleaseGPUFence(H.dev, H.inflight[i]);
            H.inflight[i] = NULL;
        }
    SDL_DestroyGPUDevice(H.dev);
    H.dev = NULL;
}
