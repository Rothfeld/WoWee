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
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
};

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(push_constant) uniform Push {
    vec2 tileCount;
    int blendMode;
    float viewportHeight;
} push;


layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aSize;
layout(location = 3) in float aTile;
layout(location = 4) in vec3 aTail;
layout(location = 5) in float aRotation;
layout(location = 6) in vec3 aQuadY;

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vTile;
layout(location = 2) out float vFogVisibility;
layout(location = 3) out vec2 vUV;

// The air between the camera and this point, out of the fog volume: rgb is
// the light it scatters toward the camera, a how much of the point shows
// through it. See VolumetricFog.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    // Each slice holds the air up to its far edge, so a point is read half a
    // slice back from where it stands.
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

void main() {
    vec4 viewPos4 = view * vec4(aPos, 1.0);
    const vec2 corners[6] = vec2[](vec2(0,0), vec2(1,0), vec2(1,1),
                                   vec2(0,0), vec2(1,1), vec2(0,1));
    vUV = corners[gl_VertexIndex];
    if (aSize < 0.0) {
        // XYQuad: flat in the emitter bone's XY plane; aTail is its X axis,
        // aQuadY its Y axis (world space).
        vec3 axisX = aTail;
        vec3 axisY = aQuadY;
        vec2 c = (vUV * 2.0 - 1.0) * -aSize;
        viewPos4 = view * vec4(aPos + axisX * c.x + axisY * c.y, 1.0);
    } else if (dot(aTail, aTail) > 0.00000001) {
        vec3 tailView = mat3(view) * aTail;
        float projectedLength = length(tailView.xy);
        vec2 side = projectedLength > 0.00001
            ? vec2(-tailView.y, tailView.x) / projectedLength : vec2(1,0);
        viewPos4.xyz += tailView * vUV.y;
        viewPos4.xy += side * ((vUV.x * 2.0 - 1.0) * aSize);
        // The texture's U axis runs along the tail, U = 0 at the particle:
        // tail emitters use wide textures (333 of 3118 in TBC, against 3 of
        // 12548 head emitters), e.g. SlimeBlob.blp (64x16), a drop whose
        // head is at its left edge.
        vUV = vUV.yx;
    } else {
        // Spin: the sprite turns by spin * age (aRotation, radians).
        vec2 corner = (vUV * 2.0 - 1.0) * aSize;
        float c = cos(aRotation), s = sin(aRotation);
        viewPos4.xy += vec2(c * corner.x - s * corner.y, s * corner.x + c * corner.y);
    }
    vColor = aColor;
    vTile = aTile;
    float worldDist = length(viewPos.xyz - aPos);
    float fogRange = max(fogParams.y - fogParams.x, 0.001);
    vFogVisibility = clamp((fogParams.y - worldDist) / fogRange, 0.0, 1.0);
    // And thinned by the air in front of it, the way distance thins it:
    // a spark deep in a bank of mist is mostly the mist.
    if (volumetricParams.x > 0.5) vFogVisibility *= fogVolumeAt(aPos).a;
    gl_Position = projection * viewPos4;
}
