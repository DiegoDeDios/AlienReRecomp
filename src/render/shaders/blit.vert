#version 450
// Copy of a native-resolution upload texture into the scaled VRAM.
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec2 v_uv;
layout(set = 1, binding = 0) uniform VUni { vec2 vram_size; vec2 pad; } u;

void main() {
    vec2 n = in_pos / u.vram_size * 2.0 - 1.0;
    gl_Position = vec4(n.x, -n.y, 0.0, 1.0);
    v_uv = in_uv;
}
