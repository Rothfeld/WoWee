#version 450

// Sandstorm grain: Shaders\Vertex\sand.bls (2.4.3 game data), with positions
// relative to the camera. Each vertex carries its start position and birth
// time, velocity and death time; the age is clamped to the lifetime, the
// colour is the material colour (the fog colour) with alpha faded in over the
// first 0.2 s and out over the last, and the point size is
// clamp(distance * c.x + c.y, 0, 1) * c.z.

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
    vec4 timeAndSize;  // x = now, yzw = point size parameters
} push;

layout(location = 0) in vec4 aStartBirth;
layout(location = 1) in vec4 aVelocityDeath;

layout(location = 0) out vec4 vColor;

void main() {
    float life = aVelocityDeath.w - aStartBirth.w;
    float age = clamp(push.timeAndSize.x - aStartBirth.w, 0.0, life);
    vec3 rel = aStartBirth.xyz + aVelocityDeath.xyz * age;
    gl_Position = projection * view * vec4(viewPos.xyz + rel, 1.0);

    float fadeOut = clamp((life - age) * 5.0, 0.0, 1.0);
    float fadeIn = clamp(age * 5.0, 0.0, 1.0);
    vColor = vec4(fogColor.rgb, fadeIn * fadeOut);

    float size = clamp(length(rel) * push.timeAndSize.y + push.timeAndSize.z, 0.0, 1.0) * push.timeAndSize.w;
    gl_PointSize = max(size, 1.0);
}
