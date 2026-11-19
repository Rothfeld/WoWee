#version 450

// Rain drops (Shaders\Vertex\rain.bls) and their splashes (patter.bls), both
// 2.4.3 game data. The CPU places every corner (weather.cpp), camera-relative;
// this adds the camera and passes the distance on for the fog, as the
// programs' fogcoord does.

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

layout(location = 0) in vec4 aPosition;  // xyz camera-relative
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out float vFog;

void main() {
    gl_Position = projection * view * vec4(viewPos.xyz + aPosition.xyz, 1.0);
    vTexCoord = aTexCoord;
    float dist = length(aPosition.xyz);
    vFog = clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0);
}
