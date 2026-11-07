#version 450
// A high resolution image in place of game textures: premultiplied alpha, tinted
// by the vertex colour. Alpha is the image's coverage; the VRAM target keeps its
// own alpha, which holds the PS1 mask bit.
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uvec4 v_info;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D image;

void main() {
    vec4 t = texture(image, v_uv);
    o_col = vec4(t.rgb * v_col.rgb, t.a) * v_col.a;
}
