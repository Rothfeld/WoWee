#version 450

// Sandstorm mist puff: a camera-facing WeatherMistGrainy01 quad, positions
// relative to the camera, tinted with the fog colour.

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

layout(location = 0) in vec4 aPositionAlpha;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColor;

void main() {
    gl_Position = projection * view * vec4(viewPos.xyz + aPositionAlpha.xyz, 1.0);
    vTexCoord = aTexCoord;
    vColor = vec4(fogColor.rgb, aPositionAlpha.w);
}
