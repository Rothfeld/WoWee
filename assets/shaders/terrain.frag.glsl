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
    vec4 localLightPosRadius[64];
    vec4 localLightColorIntensity[64];
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    vec4 specularColor;
    vec4 outdoorFogColor;   // the outdoor fog, also while the camera is in a WMO interior
    vec4 outdoorFogParams;  // x = start, y = end
};

#include "rt_lighting.glsli"

layout(set = 1, binding = 0) uniform sampler2D uBaseTexture;
layout(set = 1, binding = 1) uniform sampler2D uLayer1Texture;
layout(set = 1, binding = 2) uniform sampler2D uLayer2Texture;
layout(set = 1, binding = 3) uniform sampler2D uLayer3Texture;
layout(set = 1, binding = 4) uniform sampler2D uLayer1Alpha;
layout(set = 1, binding = 5) uniform sampler2D uLayer2Alpha;
layout(set = 1, binding = 6) uniform sampler2D uLayer3Alpha;

layout(set = 1, binding = 7) uniform TerrainParams {
    int layerCount;
    int hasLayer1;
    int hasLayer2;
    int hasLayer3;
    int bigAlpha;
    int _pad0;
    int _pad1;
    int _pad2;
    vec4 layerUvVel01;
    vec4 layerUvVel23;
    vec4 layerOverbright;
};
layout(set = 1, binding = 8) uniform sampler2D uMcshShadowMap;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec2 LayerUV;

layout(location = 0) out vec4 outColor;

#include "shadow_pcf.glsli"

vec3 localLightContribution(vec3 pos, vec3 normal, vec3 albedo) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < min(localLightMeta.x, 64); ++i) {
        vec3 toLight = localLightPosRadius[i].xyz - pos;
        float radius = localLightPosRadius[i].w;
        // Rejected on the squared distance, before the square root and the
        // divide: most of the sixty-four are out of range of any one pixel,
        // and this is what each of them costs.
        float distSq = dot(toLight, toLight);
        if (radius <= 0.0 || distSq >= radius * radius) continue;
        float dist = sqrt(distSq);
        float attenuation = 1.0 - dist / radius;
        attenuation *= attenuation;
        float wrappedDiffuse = 0.22 + 0.78 * max(dot(normal, toLight / max(dist, 0.001)), 0.0);
        sum += albedo * localLightColorIntensity[i].rgb *
               (localLightColorIntensity[i].w * attenuation * wrappedDiffuse);
    }
    return sum;
}

/// How much of the seam blur below is worth paying for at this distance.
/// Set once in main() from the fragment's distance; see sampleAlpha.
float gBlurDistFade = 1.0;

float sampleAlpha(sampler2D tex, vec2 uv) {
    // Smooth 9-tap box near chunk edges to hide alpha-map seams;
    // blends gradually to avoid a visible ring at the transition.
    // Wider feather (8 texels) makes per-chunk alpha differences
    // bleed across the boundary so the chunk grid stops reading
    // as a hard step.
    vec2 edge = min(uv, 1.0 - uv);
    float border = min(edge.x, edge.y);
    // Full 4-tap half-texel tent weight at chunk borders to hide seams, plus a
    // close-range interior tent weight (0.65) to soften 1-texel stair-steps on
    // shallow diagonal 4-bit (0..15) 64x64 MCAL roads without diluting uniform
    // 0.0 / 1.0 coverage. Fades to 0 past 260 yd so far chunks take one tap.
    float blurWeight = max(1.0 - smoothstep(1.0 / 64.0, 8.0 / 64.0, border), 0.65);
    blurWeight *= gBlurDistFade;
    float center = texture(tex, uv).r;
    if (blurWeight < 0.001) return center;
    // Four taps at half-texel offsets, not nine at whole ones. The sampler
    // is linear, so each tap already averages a 2x2 block, and the four
    // together cover the same 3x3 footprint as a tent rather than a box.
    // The band this runs in is 44% of every chunk, on the pass that
    // covers most of the screen, so the tap count is what this costs.
    vec2 h = vec2(0.5 / 64.0);
    float avg = texture(tex, uv + vec2(-h.x, -h.y)).r
              + texture(tex, uv + vec2( h.x, -h.y)).r
              + texture(tex, uv + vec2(-h.x,  h.y)).r
              + texture(tex, uv + vec2( h.x,  h.y)).r;
    avg *= 0.25;
    return mix(center, avg, blurWeight);
}

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

// The fog this surface takes: always the outdoor Light.dbc fog, also while
// the camera is in a WMO interior (docs/wmo-fog.md). Set by main().
vec4 gFogColor;
vec4 gFogParams;

// The zone's distance fog, then the air in front of it. The distance fog is
// the far haze the sky is painted to meet, so it goes on first; the volume is
// everything between the camera and that, sunlit shafts and torch glow
// included.
vec3 applyFog(vec3 color, vec3 worldPos, float dist) {
    float fogDepth = max(-(view * vec4(worldPos, 1.0)).z, 0.0);
    float fogFactor = clamp((gFogParams.y - fogDepth) / (gFogParams.y - gFogParams.x), 0.0, 1.0);
    color = mix(gFogColor.rgb, color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}

void main() {
    gFogColor = outdoorFogColor;
    gFogParams = outdoorFogParams;
    float fragDist = length(viewPos.xyz - FragPos);
    gBlurDistFade = 1.0 - smoothstep(140.0, 260.0, fragDist);

    // Derivatives must be computed before per-fragment alpha branches. Implicit
    // LOD inside those branches can select incorrect mips along layer edges.
    vec2 textureDx = dFdx(TexCoord);
    vec2 textureDy = dFdy(TexCoord);
    // ADT MCLY 0x40 animated layers: linear UV scroll wrapped per-tile with
    // fract() so precision holds across long sessions (fogParams.z = time).
    float animTime = fogParams.z;
    vec2 uv0 = TexCoord + fract(layerUvVel01.xy * animTime);
    vec2 uv1 = TexCoord + fract(layerUvVel01.zw * animTime);
    vec2 uv2 = TexCoord + fract(layerUvVel23.xy * animTime);
    vec2 uv3 = TexCoord + fract(layerUvVel23.zw * animTime);
    vec4 baseColor = textureGrad(uBaseTexture, uv0, textureDx, textureDy);

    // Legacy alpha maps paint each layer over the preceding one. We use the
    // WDT big-alpha flag to select independent coverage weights, as in
    // Northrend: the base gets what the three overlay weights leave.
    float a1 = 0.0;
    float a2 = 0.0;
    float a3 = 0.0;
    vec4 color1 = vec4(0.0);
    vec4 color2 = vec4(0.0);
    vec4 color3 = vec4(0.0);
    if (hasLayer1 != 0) {
        a1 = sampleAlpha(uLayer1Alpha, LayerUV);
        if (a1 > 0.0) color1 = textureGrad(uLayer1Texture, uv1, textureDx, textureDy);
    }
    if (hasLayer2 != 0) {
        a2 = sampleAlpha(uLayer2Alpha, LayerUV);
        if (a2 > 0.0) color2 = textureGrad(uLayer2Texture, uv2, textureDx, textureDy);
    }
    if (hasLayer3 != 0) {
        a3 = sampleAlpha(uLayer3Alpha, LayerUV);
        if (a3 > 0.0) color3 = textureGrad(uLayer3Texture, uv3, textureDx, textureDy);
    }
    vec4 finalColor;
    float overbright;
    if (bigAlpha != 0) {
        float w0 = max(1.0 - a1 - a2 - a3, 0.0);
        finalColor = baseColor * w0 + color1 * a1 + color2 * a2 + color3 * a3;
        overbright = layerOverbright.x * w0
                   + layerOverbright.y * a1
                   + layerOverbright.z * a2
                   + layerOverbright.w * a3;
    } else {
        finalColor = mix(mix(mix(baseColor, color1, a1), color2, a2), color3, a3);
        overbright = mix(mix(mix(layerOverbright.x, layerOverbright.y, a1), layerOverbright.z, a2), layerOverbright.w, a3);
    }

    vec3 norm = normalize(Normal);

    // Terrain diffuse textures contain painted color, not height. Deriving
    // normals from their screen-space brightness amplifies mip sampling noise.

    vec3 lightDir2 = normalize(-lightDir.xyz);
    // Lambert, as every other surface has it. This took abs() of the angle
    // and floored it at 0.2, so a slope turned from the sun was lit as one
    // turned toward it, and hills had no shape at low sun. The ambient term
    // is what keeps the shaded side from black, as it does for the models
    // standing on it.
    float diff = max(dot(norm, lightDir2), 0.0);

    float shadow = 1.0;
    if (shadowParams.x > 0.5) {
        vec3 ldir = normalize(-lightDir.xyz);
        float normalOffset = shadowTexel() * 2.0 * (1.0 - abs(dot(norm, ldir)));
        vec3 biasedPos = FragPos + norm * normalOffset;
        vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
        vec3 proj = lsPos.xyz / lsPos.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        if (proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0 && proj.z >= 0.0 && proj.z <= 1.0) {
            float bias = max(0.0005 * (1.0 - abs(dot(norm, ldir))), 0.00005);
            shadow = sampleShadowPCF(uShadowMap, vec3(proj.xy, proj.z - bias));
            shadow = mix(1.0, shadow, shadowParams.y);
        }
    }

    RtLight rt = rtLightAt(FragPos);
    shadow = rtShadow(rt, shadow);
    vec3 lightFactor = rtAmbient(rt, ambientColor.rgb) + shadow * diff * lightColor.rgb;
    // TBC 2.4.3 terrain.bls clamps vertex primary color (ambient + NdotL * diffuse)
    // to [0, 1] before multiplying blended layer albedo.
    if (lightDir.w != 0.0) {
        lightFactor = clamp(lightFactor, 0.0, 1.0);
    }
    vec3 result = lightFactor * finalColor.rgb;
    // ADT MCSH 64x64 baked terrain shadow map (1.0 = shadowed, 0.0 = lit).
    // Native TBC 2.4.3 darkens terrain under baked MCSH shadows by ~30% (0.7x)
    // across both night and day lighting even when dynamic shadows are off.
    // When dynamic shadows are active, avoid double-darkening fragments
    // already in dynamic shadow.
    float mcsh = texture(uMcshShadowMap, LayerUV).r;
    float mcshFactor = mix(1.0, 0.7, mcsh);
    if (shadowParams.x > 0.5) {
        float dynDarken = mix(0.7, 1.0, shadow);
        result *= min(1.0, mcshFactor / max(dynDarken, 0.7));
    } else {
        result *= mcshFactor;
    }
    result += localLightContribution(FragPos, norm, finalColor.rgb);
    // ADT MCLY 0x80 overbright layers (glowing lava/magma/energy terrain)
    // bypass diffuse/ambient/MCSH darkening and glow emissively before fog.
    result = mix(result, finalColor.rgb, clamp(overbright, 0.0, 1.0));

    result = applyFog(result, FragPos, fragDist);

    outColor = vec4(result, 1.0);
}
