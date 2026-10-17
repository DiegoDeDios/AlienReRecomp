#version 450
// Primitive vertices in native VRAM coordinates. w > 1 carries depth from
// precision geometry (PGXP): attributes are then interpolated with
// perspective correction; w = 1 reproduces the PS1's affine mapping.
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec4 in_col;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_info;
layout(location = 4) in float in_w;
layout(location = 0) out vec4 v_col;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uvec4 v_info;
layout(set = 1, binding = 0) uniform VUni { vec2 vram_size; vec2 pad; } u;

void main() {
    vec2 n = in_pos / u.vram_size * 2.0 - 1.0;
    gl_Position = vec4(n.x * in_w, -n.y * in_w, 0.0, in_w);
    v_col = in_col;
    v_uv = in_uv;
    v_info = in_info;
}
