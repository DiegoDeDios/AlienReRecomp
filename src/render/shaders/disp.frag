#version 450
#extension GL_GOOGLE_include_directive : require
// Display area of VRAM to the window: 15-bit (filtered) or 24-bit (MDEC
// movies, reassembled from raw VRAM bytes).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D vram;
layout(set = 3, binding = 0) uniform DUni { vec4 rect; float scale; uint rgb24; float pad0; float pad1; } u;
#include "common.glsl"

void main() {
    if (u.rgb24 != 0u) {
        uint px = uint(v_uv.x * u.rect.z), py = uint(v_uv.y * u.rect.w);
        uint bo = uint(u.rect.x) * 2u + px * 3u;
        uint y = uint(u.rect.y) + py;
        uint w0 = vram16(vram, uvec2(bo / 2u, y), u.scale);
        uint w1 = vram16(vram, uvec2(bo / 2u + 1u, y), u.scale);
        uint v = w0 | (w1 << 16);
        uint sh = (bo & 1u) * 8u;
        o_col = vec4(float((v >> sh) & 255u), float((v >> (sh + 8u)) & 255u),
                     float((v >> (sh + 16u)) & 255u), 255.0) / 255.0;
        return;
    }
    vec2 p = (u.rect.xy + v_uv * u.rect.zw) / vec2(1024.0, 512.0);
    vec4 c = texture(vram, p);
    o_col = vec4(c.rgb, 1.0);
}
