// Lyra panadapter glow (additive bloom) — Qt 6 RHI fragment shader.
//
// Used as a QML `layer.effect` over the Panadapter item.  Keeps the
// original (sharp) pixels, then ADDS a blurred copy of only the BRIGHT
// pixels (above a threshold) — so the bright trace blooms a luminous
// halo while the dim glass background + grid (below threshold) stay
// crisp.  This is the "floating glow" look.  Authored once in GLSL;
// qsb cross-compiles to SPIR-V/MSL/HLSL/GLSL for Vulkan/Metal/D3D/GL.
#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float glowStrength;   // bloom intensity (0 = none)
    float glowThreshold;  // only pixels brighter than this bloom
    float glowSpread;     // tap spacing in texels (halo radius)
    vec2  texSize;        // source texture size in px (textureSize() is
                          // unavailable in the GLSL 120/ES targets qsb
                          // also compiles, so pass it as a uniform)
};

layout(binding = 1) uniform sampler2D source;

void main() {
    vec4 base = texture(source, qt_TexCoord0);

    vec2 texel = glowSpread / max(texSize, vec2(1.0));
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    const int R = 4;                 // 9x9 gaussian-weighted taps
    for (int dy = -R; dy <= R; ++dy) {
        for (int dx = -R; dx <= R; ++dx) {
            float w = exp(-float(dx * dx + dy * dy) / 10.0);
            vec3 c = texture(source,
                             qt_TexCoord0 + vec2(float(dx), float(dy)) * texel).rgb;
            acc  += max(c - glowThreshold, vec3(0.0)) * w;  // bright part only
            wsum += w;
        }
    }
    acc /= max(wsum, 0.0001);

    // Additive: sharp original + bloom of the bright parts.
    fragColor = vec4(base.rgb + acc * glowStrength, base.a) * qt_Opacity;
}
