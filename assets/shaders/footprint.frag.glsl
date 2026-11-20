#version 450

layout(set = 1, binding = 0) uniform sampler2D footprintTexture;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 tint;
} push;

layout(location = 0) in vec2 texCoord;
layout(location = 1) in float fogVisibility;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(footprintTexture, texCoord);
    float alpha = tex.a * push.tint.a * fogVisibility;
    if (alpha < 0.005) discard;
    outColor = vec4(tex.rgb * push.tint.rgb, alpha);
}
