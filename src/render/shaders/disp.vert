#version 450
// Fullscreen triangle for presenting the display area.
layout(location = 0) out vec2 v_uv;

void main() {
    uint vid = uint(gl_VertexIndex);
    vec2 p = vec2(float((vid << 1) & 2u), float(vid & 2u));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = vec2(p.x, 1.0 - p.y);
}
