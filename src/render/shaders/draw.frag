#version 450
#extension GL_GOOGLE_include_directive : require
// PS1 polygon shading: 4/8/15-bit textures with CLUTs decoded from raw VRAM
// words, texture windows, modulation, semi-transparency passes, mask bit.
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uvec4 v_info;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D vram;
layout(set = 3, binding = 0) uniform FUni { float scale; uint pass; uint pad0; uint pad1; } u;
#include "common.glsl"

void main() {
    uint flags = v_info.x >> 16;
    bool textured = (flags & 1u) != 0u, raw = (flags & 2u) != 0u,
         semi = (flags & 4u) != 0u, setmask = (flags & 8u) != 0u;
    vec3 rgb = v_col.rgb;
    float mask = setmask ? 1.0 : 0.0;
    if (textured) {
        uint tpage = v_info.x & 0x1FFu;
        uvec2 uv = uvec2(ivec2(floor(v_uv))) & 0xFFu;
        uint tw = v_info.z;
        uint mx = tw & 31u, my = (tw >> 5) & 31u, ox = (tw >> 10) & 31u, oy = (tw >> 15) & 31u;
        uv.x = (uv.x & ~(mx * 8u)) | ((ox & mx) * 8u);
        uv.y = (uv.y & ~(my * 8u)) | ((oy & my) * 8u);
        uint tx = (tpage & 15u) * 64u, ty = ((tpage >> 4) & 1u) * 256u;
        uint depth = (tpage >> 7) & 3u;
        uint clut = v_info.y;
        uint cx = (clut & 63u) * 16u, cy = (clut >> 6) & 511u;
        uint texel;
        if (depth == 0u) {
            uint w = vram16(vram, uvec2(tx + uv.x / 4u, ty + uv.y), u.scale);
            texel = vram16(vram, uvec2(cx + ((w >> ((uv.x & 3u) * 4u)) & 15u), cy), u.scale);
        } else if (depth == 1u) {
            uint w = vram16(vram, uvec2(tx + uv.x / 2u, ty + uv.y), u.scale);
            texel = vram16(vram, uvec2(cx + ((w >> ((uv.x & 1u) * 8u)) & 255u), cy), u.scale);
        } else {
            texel = vram16(vram, uvec2(tx + uv.x, ty + uv.y), u.scale);
        }
        if (texel == 0u)
            discard;
        bool stp = (texel & 0x8000u) != 0u;
        if (semi && u.pass == 1u && stp)
            discard;
        if (semi && u.pass == 2u && !stp)
            discard;
        vec3 t = vec3(float(texel & 31u), float((texel >> 5) & 31u), float((texel >> 10) & 31u)) / 31.0;
        rgb = raw ? t : min(t * v_col.rgb * (255.0 / 128.0), vec3(1.0));
        mask = (stp || setmask) ? 1.0 : 0.0;
    }
    o_col = vec4(rgb, mask);
}
