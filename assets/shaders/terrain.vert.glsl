#version 450
invariant gl_Position;

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
    vec4 horizonParams; // x = 1.0 for WDL horizon mesh, y = farclip, z = horizonFarClip
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
layout(location = 5) out vec3 VertexLocalLight;

void main() {
    vec4 worldPos = push.model * vec4(aPosition, 1.0);
    FragPos = worldPos.xyz;
    Normal = aNormal;
    TexCoord = aTexCoord;
    LayerUV = aLayerUV;
    vec3 normN = normalize(aNormal);
    vec3 viewDir = normalize(viewPos.xyz - FragPos);
    vec3 halfDir = normalize(normalize(-lightDir.xyz) + viewDir);
    VertexSpecular = specularColor.rgb * pow(max(dot(normN, halfDir), 0.0), 16.0);
    // TBC authored M2 point lights evaluated per-vertex on the terrain grid
    // rather than per-pixel so close held lights (e.g. NPC lanterns) do not
    // burn sharp spotlights directly under a character's feet.
    vec3 localSum = vec3(0.0);
    for (int i = 0; i < min(localLightMeta.y, 64); ++i) {
        vec3 toLight = localLightPosRadius[i].xyz - FragPos;
        float radius = localLightPosRadius[i].w;
        float distSq = dot(toLight, toLight);
        if (radius <= 0.0 || distSq >= radius * radius) continue;
        float dist = sqrt(distSq);
        float edgeFade = 1.0 - dist / radius;
        float atten = (edgeFade * edgeFade) / max(1.0 + 0.7 * dist + 0.03 * distSq, 1e-4);
        float ndotl = max(dot(normN, toLight / max(dist, 0.001)), 0.0);
        localSum += localLightColorIntensity[i].rgb * (atten * ndotl);
    }
    VertexLocalLight = localSum;
    gl_Position = projection * (view * worldPos);
    if (push.horizonParams.x > 0.5) {
        VertexSpecular = vec3(0.0);
        VertexLocalLight = vec3(0.0);
        // Pure affine clip-space depth transform z_clip = kHorizonNdcMax * w_clip + bWdl:
        // maps w >= horizonNear monotonically into [ndcZ(farclip + 0.5), 0.99999) so
        // coarse 33.3 yd WDL triangles never occlude any MCNK, WMO, M2, character, or
        // water within eyeDepth <= farclip, occlude sky layers at depth 1.0, and preserve
        // exact hardware near-plane clipping at w_near = -bWdl / kHorizonNdcMax (~0.18 yd).
        float w1 = max(push.horizonParams.y - 16.0, 32.0);
        float d0 = clamp(-projection[2][2] + projection[3][2] / (push.horizonParams.y + 0.5),
                         0.0, 0.99992);
        const float kHorizonNdcMax = 0.99999;
        float bWdl = (d0 - kHorizonNdcMax) * w1;
        gl_Position.z = kHorizonNdcMax * gl_Position.w + bWdl;
    }
}
