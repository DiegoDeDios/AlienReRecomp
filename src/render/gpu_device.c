/*
 * GPU device and shader creation shared by the game renderer and the
 * launcher: picks the graphics API for the platform and builds shaders
 * from the generated multi-format blobs (shaders_gen.h).
 *
 *   macOS     Metal       MSL
 *   Windows   Vulkan      SPIR-V   (preferred: same shader binary as Linux)
 *             Direct3D 12 HLSL compiled to DXBC at run time (fallback)
 *   others    Vulkan      SPIR-V
 */
#include <string.h>

#include "../runtime/psx.h"
#include "render.h"
#include "shader_blob.h"

const char *g_render_driver = "auto";

/* shaders that must compile for Direct3D 12 to be chosen (game renderer) */
static const ShaderBlob *const *g_required;
static const SDL_GPUShaderStage *g_required_stage;
static int g_nrequired;

void render_require_shaders(const ShaderBlob *const *blobs, const SDL_GPUShaderStage *stages, int n) {
    g_required = blobs;
    g_required_stage = stages;
    g_nrequired = n;
}

#ifndef __APPLE__
/* Direct3D 12 compiles HLSL at run time: only offer it if every shader the
 * caller needs compiles with this system's d3dcompiler */
static bool hlsl_shaders_compile(void) {
    if (!render_hlsl_available())
        return false;
    for (int i = 0; i < g_nrequired; i++) {
        size_t size;
        void *code = render_compile_hlsl(g_required[i]->hlsl,
                                         g_required_stage[i] == SDL_GPU_SHADERSTAGE_VERTEX ? "vs_5_1"
                                                                                          : "ps_5_1",
                                         &size);
        if (!code) {
            LOGW("render", "Direct3D 12 disabled: %s", SDL_GetError());
            return false;
        }
        SDL_free(code);
    }
    return true;
}
#endif

#ifndef __APPLE__
static SDL_GPUDevice *try_device(SDL_GPUShaderFormat fmt, const char *name) {
    if (!SDL_GPUSupportsShaderFormats(fmt, name)) {
        LOGW("render", "%s: not supported on this system", name);
        return NULL;
    }
    SDL_GPUDevice *d = SDL_CreateGPUDevice(fmt, false, name);
    if (!d)
        LOGW("render", "%s unavailable: %s", name, SDL_GetError());
    return d;
}
#endif

SDL_GPUDevice *render_create_device(void) {
#ifdef __APPLE__
    return SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB, false, NULL);
#else
    const char *pref = g_render_driver ? g_render_driver : "auto";
    bool d3d_ok = hlsl_shaders_compile();
    SDL_GPUDevice *d = NULL;
    if (!strcmp(pref, "direct3d12") && d3d_ok)
        d = try_device(SDL_GPU_SHADERFORMAT_DXBC, "direct3d12");
    if (!d)
        d = try_device(SDL_GPU_SHADERFORMAT_SPIRV, "vulkan");
    if (!d && d3d_ok)
        d = try_device(SDL_GPU_SHADERFORMAT_DXBC, "direct3d12");
    if (!d)
        SDL_SetError("no Vulkan or Direct3D 12 device (update the graphics driver?)");
    return d;
#endif
}

SDL_GPUShader *render_create_shader(SDL_GPUDevice *dev, const ShaderBlob *b,
                                    SDL_GPUShaderStage stage, int samplers, int uniforms) {
    SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(dev);
    SDL_GPUShaderCreateInfo ci = {
        .stage = stage,
        .num_samplers = (Uint32)samplers,
        .num_uniform_buffers = (Uint32)uniforms,
    };
    void *compiled = NULL;
    if (fmts & SDL_GPU_SHADERFORMAT_MSL) {
        ci.code = (const Uint8 *)b->msl;
        ci.code_size = strlen(b->msl);
        ci.entrypoint = "main0";
        ci.format = SDL_GPU_SHADERFORMAT_MSL;
    } else if (fmts & SDL_GPU_SHADERFORMAT_SPIRV) {
        ci.code = b->spirv;
        ci.code_size = b->spirv_size;
        ci.entrypoint = "main";
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    } else if (fmts & SDL_GPU_SHADERFORMAT_DXBC) {
        size_t size = 0;
        compiled = render_compile_hlsl(b->hlsl, stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vs_5_1" : "ps_5_1",
                                       &size);
        if (!compiled)
            return NULL;
        ci.code = compiled;
        ci.code_size = size;
        ci.entrypoint = "main";
        ci.format = SDL_GPU_SHADERFORMAT_DXBC;
    } else {
        SDL_SetError("no supported shader format");
        return NULL;
    }
    SDL_GPUShader *s = SDL_CreateGPUShader(dev, &ci);
    SDL_free(compiled);
    return s;
}

#ifndef _WIN32
bool render_hlsl_available(void) { return false; }
void *render_compile_hlsl(const char *src, const char *profile, size_t *size) {
    (void)src, (void)profile, (void)size;
    SDL_SetError("HLSL compilation is only available on Windows");
    return NULL;
}
#endif
