#version 450

// Mod2x (destination * source * 2): a texel of 0.5 leaves the scene as it is,
// brighter texels lighten it in its own colour. RainDrop01 and RainDropSplash01
// are grey (0.49-0.63); their alpha is not used. Measured on 2.4.3: a drop that
// covers whole pixels scales the scene behind it by 1.07-1.15 in every channel
// alike, and distant streaks a pixel or less wide still lighten it by 3-6%
// (mip-averaged texels). Fog takes the texel toward 0.5, which leaves the
// scene unchanged.

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    vec4 color;  // Weather.dbc EffectColor
} push;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in float vFog;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 t = texture(uTexture, vTexCoord);
    outColor = vec4(mix(vec3(0.5), t.rgb * push.color.rgb, vFog), 1.0);
}
