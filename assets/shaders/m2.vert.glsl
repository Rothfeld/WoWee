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
    vec4 playerPos;   // xyz = player world position, w = horizontal speed
    vec4 playerWake;  // xyz = trailing player position (springback reference)
};

// Per-draw push constants (batch-level data only)
layout(push_constant) uniform Push {
    int texCoordSet;         // UV set index (0 or 1)
    int drawMode;            // -1 sky, 0 placed model, 2 ground clutter
    int instanceDataOffset;  // Base index into InstanceSSBO for this draw group
    float modelHeight;       // The model's own height (model space)
    int texCoordSet2;
} push;

layout(set = 2, binding = 0) readonly buffer BoneSSBO {
    mat4 bones[];
};

// Per-instance data read via gl_InstanceIndex (GPU instancing)
struct InstanceData {
    mat4 model;
    vec2 uvOffset;
    float fadeAlpha;
    int useBones;
    int boneBase;
    int boneCount;
    // 0 for an ordinary instance, 1 while the player is pressing on it. Sits
    // in what was padding, so the entry is the same 96 bytes it always was.
    float highlight;
    // Bit 1: placement lighting from MODD in bits 8..31. Also in what was padding.
    int flags;
    vec4 uvOffset2;
};
layout(set = 3, binding = 0) readonly buffer InstanceSSBO {
    InstanceData instanceData[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 aBoneWeights;
layout(location = 4) in vec4 aBoneIndicesF;
layout(location = 5) in vec2 aTexCoord2;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) flat out vec3 InstanceOrigin;
layout(location = 4) out float ModelHeight;
layout(location = 5) out float vFadeAlpha;
layout(location = 6) flat out int vSkyMode;
layout(location = 7) flat out float vHighlight;
layout(location = 8) flat out vec4 vIndoorLighting;
layout(location = 9) out vec3 VertexLighting;
layout(location = 10) out vec2 TexCoord2;

vec3 normalizedNormal(vec3 value) {
    // Authored zero-scale bones have zero normals during hidden phases.
    return value * inversesqrt(max(dot(value, value), 1e-20));
}

void main() {
    // Fetch per-instance data from SSBO
    int instIdx = push.instanceDataOffset + gl_InstanceIndex;
    mat4 model = instanceData[instIdx].model;
    vec2 uvOff = instanceData[instIdx].uvOffset;
    float fade = instanceData[instIdx].fadeAlpha;
    int uBones = instanceData[instIdx].useBones;
    int bBase  = instanceData[instIdx].boneBase;

    vec4 pos = vec4(aPos, 1.0);
    vec4 norm = vec4(aNormal, 0.0);

    if (uBones != 0) {
        // Clamp to the range this instance actually owns. A model whose bone
        // count exceeds the ceiling would otherwise index into the next
        // instance's matrices and fling its vertices across the world.
        int bCount = max(instanceData[instIdx].boneCount, 1);
        ivec4 bi = clamp(ivec4(aBoneIndicesF), ivec4(0), ivec4(bCount - 1));
        mat4 skinMat = bones[bBase + bi.x] * aBoneWeights.x
                     + bones[bBase + bi.y] * aBoneWeights.y
                     + bones[bBase + bi.z] * aBoneWeights.z
                     + bones[bBase + bi.w] * aBoneWeights.w;
        pos = skinMat * pos;
        norm = skinMat * norm;
    }

    vec4 worldPos = model * pos;

    FragPos = worldPos.xyz;
    Normal = mat3(model) * norm.xyz;
    // Ground clutter is lit as the ground it stands on. Its quads carry
    // horizontal normals, and lighting those turned every tuft facing away
    // from the sun into ambient-only blue-grey; 2.4.3 draws the same tufts in
    // the terrain's own light whichever way they face (Dustwallow Marsh, Beezil's Wreck).
    vec3 lightingNormal = push.drawMode == 2 ? vec3(0.0, 0.0, 1.0) : normalizedNormal(Normal);
    float diffuse = max(dot(lightingNormal, normalize(-lightDir.xyz)), 0.0);
    // TBC: a highlighted object (pointer over it) takes its light twice
    // before saturating, as a highlighted unit takes one more unit of its gain.
    float lightGain = 1.0 + (lightDir.w != 0.0 ? instanceData[instIdx].highlight : 0.0);
    VertexLighting = clamp((ambientColor.rgb + diffuse * lightColor.rgb) * lightGain, 0.0, 1.0);

    TexCoord = (push.texCoordSet == 1 ? aTexCoord2 : aTexCoord) + uvOff;
    vec3 eyeNormal = normalizedNormal(mat3(view) * Normal);
    vec2 environmentUV = eyeNormal.xy * 0.5 + 0.5;
    if (push.texCoordSet == -1) TexCoord = environmentUV + uvOff;
    TexCoord2 = (push.texCoordSet2 == -1 ? environmentUV :
        (push.texCoordSet2 == 1 ? aTexCoord2 : aTexCoord)) +
        instanceData[push.instanceDataOffset + gl_InstanceIndex].uvOffset2.xy;

    InstanceOrigin = model[3].xyz;
    // How far up its own model this vertex is, as a fraction of the model's
    // height (a flame card fades out over its own tip with it).
    ModelHeight = push.modelHeight > 0.0 ? clamp(pos.z / push.modelHeight, 0.0, 1.0) : 1.0;
    vFadeAlpha = fade;
    vSkyMode = push.drawMode < 0 ? 1 : 0;
    vHighlight = instanceData[instIdx].highlight;
    uint lighting = uint(instanceData[instIdx].flags);
    vIndoorLighting = vec4(vec3((lighting >> 8) & 255u, (lighting >> 16) & 255u,
                              (lighting >> 24) & 255u) / 255.0,
                          (lighting & 2u) != 0u ? 1.0 : 0.0);

    gl_Position = projection * view * worldPos;
    // A sky model sits on the far plane whatever its radius, so the depth
    // test rejects it wherever terrain has already been drawn. The sky is
    // drawn after the ground now, and this is what makes that free.
    if (push.drawMode < 0) gl_Position.z = gl_Position.w;
}
