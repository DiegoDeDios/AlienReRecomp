// Shared helpers. VRAM is an RGBA8 texture where every texel holds one PS1
// 16-bit pixel (5-5-5 colour, mask bit in alpha), upscaled by `scale`.

uint pack16(vec4 v) {
    uint r = uint(roundEven(v.r * 31.0)), g = uint(roundEven(v.g * 31.0)),
         b = uint(roundEven(v.b * 31.0));
    return r | (g << 5) | (b << 10) | (v.a > 0.5 ? 0x8000u : 0u);
}

// raw 16-bit VRAM word at native coordinates p
uint vram16(sampler2D t, uvec2 p, float s) {
    p.x &= 1023u;
    p.y &= 511u;
    return pack16(texelFetch(t, ivec2(uvec2(vec2(p) * s)), 0));
}
