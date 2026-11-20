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
    vec4 zenithColor;      // DBC skyTopColor (90 deg)
    vec4 midColor;         // DBC skyMiddleColor (30 deg)
    vec4 horizonColor;     // DBC skyBand1Color (15 deg)
    vec4 fogColorPush;     // DBC skyBand2Color (5 deg)
    vec4 smogColor;        // xyz = DBC skySmogColor (2.5 deg), w = smog weight (FloatBand 3)
    vec4 horizonFogColor;  // xyz = DBC fogColor (0 deg & below)
    vec4 sunDirAndTime;    // xyz = sun direction, w = timeOfDay
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

void main() {
    // Reconstruct world-space ray direction from screen position.
    float ndcX =  TexCoord.x * 2.0 - 1.0;
    float ndcY = -(TexCoord.y * 2.0 - 1.0);

    vec3 viewDir = vec3(ndcX / projection[0][0],
                        ndcY / abs(projection[1][1]),
                        -1.0);

    mat3 invViewRot = transpose(mat3(view));
    vec3 worldDir = normalize(invViewRot * viewDir);

    vec3 sunDir = push.sunDirAndTime.xyz;
    float timeOfDay = push.sunDirAndTime.w;

    // Elevation: +1 = zenith, 0 = horizon, -1 = nadir
    float elev = worldDir.z;
    float elevClamped = clamp(elev, 0.0, 1.0);
    vec3 sky;

    if (lightDir.w != 0.0) {
        // TBC 2.4.3 CSkyScene 6-ring sky dome (angles verified against Native 8606
        // scanlines at matched 44.10 deg vertical FOV):
        //   90.00 deg : IntBand[2] (skyTopColor)
        //   17.00 deg : IntBand[3] (skyMiddleColor)
        //   10.00 deg : IntBand[4] (skyBand1Color)
        //    3.75 deg : IntBand[5] (skyBand2Color)
        //    1.20 deg : IntBand[6] (skySmogColor weighted by FloatBand[3])
        //  <=-1.0 deg : IntBand[7] (fogColor, matching distance fog seamlessly)
        float elevDeg = degrees(asin(clamp(elev, -1.0, 1.0)));
        vec3 baseFringe = mix(push.horizonFogColor.rgb, push.fogColorPush.rgb, 0.5);
        vec3 smogRing = mix(baseFringe, push.smogColor.rgb, clamp(push.smogColor.w, 0.0, 1.0));
        if (elevDeg >= 17.0) {
            sky = mix(push.midColor.rgb, push.zenithColor.rgb, (elevDeg - 17.0) / 73.0);
        } else if (elevDeg >= 10.0) {
            sky = mix(push.horizonColor.rgb, push.midColor.rgb, (elevDeg - 10.0) / 7.0);
        } else if (elevDeg >= 3.75) {
            sky = mix(push.fogColorPush.rgb, push.horizonColor.rgb, (elevDeg - 3.75) / 6.25);
        } else if (elevDeg >= 1.2) {
            sky = mix(smogRing, push.fogColorPush.rgb, (elevDeg - 1.2) / 2.55);
        } else if (elevDeg >= -1.0) {
            sky = mix(push.horizonFogColor.rgb, smogRing, (elevDeg + 1.0) / 2.2);
        } else {
            sky = push.horizonFogColor.rgb;
        }
    } else {
        if (elevClamped > 0.4) {
            float t = (elevClamped - 0.4) / 0.6;
            sky = mix(push.midColor.rgb, push.zenithColor.rgb, t);
        } else if (elevClamped > 0.05) {
            float t = (elevClamped - 0.05) / 0.35;
            sky = mix(push.horizonColor.rgb, push.midColor.rgb, t);
        } else {
            float t = elevClamped / 0.05;
            sky = mix(push.fogColorPush.rgb, push.horizonColor.rgb, t);
        }

        if (elev < 0.0) {
            float nadirFade = clamp(-elev * 3.0, 0.0, 1.0);
            vec3 nadirColor = push.fogColorPush.rgb * 0.3;
            sky = mix(push.fogColorPush.rgb, nadirColor, nadirFade);
        }

        float sunDot = max(dot(worldDir, sunDir), 0.0);
        float sunAboveHorizon = clamp(sunDir.z, 0.0, 1.0);

        float rayleighStrength = pow(1.0 - elevClamped, 3.0) * 0.15;
        vec3 scatterColor = mix(vec3(0.8, 0.45, 0.15), vec3(0.3, 0.5, 1.0), elevClamped);
        sky += scatterColor * rayleighStrength * sunDot * sunAboveHorizon;

        float mieSharp = pow(sunDot, 64.0) * 0.4;
        float mieSoft  = pow(sunDot, 8.0) * 0.1;
        vec3 sunGlowColor = mix(vec3(1.0, 0.85, 0.55), vec3(1.0, 1.0, 0.95), elevClamped);
        sky += sunGlowColor * (mieSharp + mieSoft) * sunAboveHorizon;

        float hazeDensity = exp(-elevClamped * 12.0) * 0.06;
        sky += push.horizonColor.rgb * hazeDensity * sunAboveHorizon;

        if (sunDir.z < 0.0) {
            float moonlight = clamp(-sunDir.z * 0.5, 0.0, 0.15);
            sky += vec3(0.02, 0.03, 0.08) * moonlight;
        }
    }

    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeSky(TexCoord);
        sky = sky * air.a + air.rgb;
    }

    outColor = vec4(sky, 1.0);
}
