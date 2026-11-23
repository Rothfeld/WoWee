#version 450

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
};

layout(push_constant) uniform Push {
    mat4 model;
    vec4 tint;
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out float fogVisibility;

void main() {
    texCoord = aTexCoord;
    vec4 worldPos = push.model * vec4(aPos, 1.0);
    float dist = length(worldPos.xyz - viewPos.xyz);
    float fogRange = max(fogParams.y - fogParams.x, 1.0);
    fogVisibility = 1.0 - clamp((dist - fogParams.x) / fogRange, 0.0, 1.0);
    gl_Position = projection * (view * worldPos);
}
