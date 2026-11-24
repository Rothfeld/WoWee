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
    vec4 playerPos;
    vec4 playerWake;
    vec4 localLightPosRadius[64];
    vec4 localLightColorIntensity[64];
    ivec4 localLightMeta;
    vec4 volumetricParams;
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    vec4 specularColor;
};

layout(push_constant) uniform Push {
    mat4 model;
} push;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aLayerUV;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out vec2 LayerUV;
layout(location = 4) out vec3 VertexSpecular;

void main() {
    vec4 worldPos = push.model * vec4(aPosition, 1.0);
    FragPos = worldPos.xyz;
    Normal = aNormal;
    TexCoord = aTexCoord;
    LayerUV = aLayerUV;
    vec3 viewDir = normalize(viewPos.xyz - FragPos);
    vec3 halfDir = normalize(normalize(-lightDir.xyz) + viewDir);
    VertexSpecular = specularColor.rgb * pow(max(dot(normalize(aNormal), halfDir), 0.0), 16.0);
    gl_Position = projection * (view * worldPos);
}
