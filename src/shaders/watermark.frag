// Lyra watermark — luminance-keyed display of the lyre/constellation
// JPG.  The source image is a bright lyre + stars on a dark navy
// starfield; keying alpha to luminance drops the dark background to
// transparent so only the lyre + stars show (the old-Lyra additive
// trick, done as alpha so a plain blend works).  Faint by qt_Opacity.
#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
};

layout(binding = 1) uniform sampler2D source;

void main() {
    vec3 c = texture(source, qt_TexCoord0).rgb;
    float lum = max(c.r, max(c.g, c.b));          // bright lyre/stars high
    // Gentle gamma so the lyre body shows, not just the brightest stars.
    float a = pow(clamp(lum * 1.4, 0.0, 1.0), 1.2);
    fragColor = vec4(c * a, a) * qt_Opacity;      // premultiplied
}
