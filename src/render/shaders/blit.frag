#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D src;
layout(set = 3, binding = 0) uniform BUni { float setmask; float pad0; float pad1; float pad2; } u;

void main() {
    vec4 c = texelFetch(src, ivec2(floor(v_uv)), 0);
    if (u.setmask > 0.5)
        c.a = 1.0;
    o_col = c;
}
