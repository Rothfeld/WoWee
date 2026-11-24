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
    vec4 outdoorFogColor;
    vec4 outdoorFogParams;  // x = start, y = end
};

#include "rt_lighting.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform WMOMaterial {
    int hasTexture;
    int alphaTest;
    int unlit;
    int isInterior;
    float specularIntensity;
    int hasEnvMap;
    int enableNormalMap;
    int enablePOM;
    float pomScale;
    int pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
    int isMetal;
    float wmoAmbientR;
    float wmoAmbientG;
    float wmoAmbientB;
    int emissive;
    int hasVertexColors;
    int specularAlphaMask;
    int sidnColor;  // MOMT sidnColor, BGRA bytes
};

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;
layout(set = 1, binding = 3) uniform sampler2D uEnvMap;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec4 VertColor;
layout(location = 4) in vec3 Tangent;
layout(location = 5) in vec3 Bitangent;
layout(location = 6) in vec3 VertexLighting;
layout(location = 7) in vec3 VertexSpecular;

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
        vec3 lightVector = toLight / max(dist, 0.001);
        float attenuation = 1.0 - dist / radius;
        attenuation *= attenuation;
        float wrappedDiffuse = 0.22 + 0.78 * max(dot(normal, lightVector), 0.0);
        sum += albedo * localLightColorIntensity[i].rgb *
               (localLightColorIntensity[i].w * attenuation * wrappedDiffuse);
    }
    for (int i = 0; i < min(localLightMeta.y, 64); ++i) {
        vec3 toLight = localLightPosRadius[i].xyz - pos;
        float radius = localLightPosRadius[i].w;
        float distSq = dot(toLight, toLight);
        if (radius <= 0.0 || distSq >= radius * radius) continue;
        float dist = sqrt(distSq);
        float atten = 1.0 / max(1.0 + 0.7 * dist + 0.03 * distSq, 1e-4);
        float edgeFade = 1.0 - dist / radius;
        atten *= edgeFade * edgeFade;
        float ndotl = max(dot(normal, toLight / max(dist, 0.001)), 0.0);
        sum += albedo * localLightColorIntensity[i].rgb * (atten * ndotl);
    }
    return sum;
}

// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    // Low density = close/head-on = full detail (0)
    // High density = far/steep = vertex normals only (1)
    return smoothstep(0.0001, 0.005, texelDensity);
}

// Parallax Occlusion Mapping with angle-adaptive sampling
vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float lodFactor) {
    float VdotN = abs(viewDirTS.z);  // 1=head-on, 0=grazing

    // Fade out POM at grazing angles to avoid distortion
    if (VdotN < 0.15) return uv;

    float angleFactor = clamp(VdotN, 0.15, 1.0);
    int maxS = pomMaxSamples;
    int minS = max(maxS / 4, 4);
    int numSamples = int(mix(float(minS), float(maxS), angleFactor));
    numSamples = int(mix(float(minS), float(numSamples), 1.0 - lodFactor));

    float layerDepth = 1.0 / float(numSamples);
    float currentLayerDepth = 0.0;

    // Direction to shift UV per layer - clamp denominator to prevent explosion at grazing angles
    vec2 P = viewDirTS.xy / max(VdotN, 0.15) * pomScale;
    // Hard-clamp total UV offset to prevent texture swimming
    float maxOffset = pomScale * 3.0;
    P = clamp(P, vec2(-maxOffset), vec2(maxOffset));
    vec2 deltaUV = P / float(numSamples);

    // The mip level is chosen once, from the undisplaced UV, and used for
    // every sample in the march. Inside the loop the UV differs from one
    // pixel to the next by how many steps each has taken, so the implicit
    // derivatives were noise and the level chosen from them was too - which
    // showed as sparkle on relief at distance and cost a gradient fetch
    // per sample on top.
    float lod = textureQueryLod(uNormalHeightMap, uv).x;
    vec2 currentUV = uv;
    float currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;

    // Ray march through layers
    for (int i = 0; i < 64; i++) {
        if (i >= numSamples || currentLayerDepth >= currentDepthMapValue) break;
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;
        currentLayerDepth += layerDepth;
    }

    // Interpolate between last two layers for smooth result
    vec2 prevUV = currentUV + deltaUV;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = (1.0 - textureLod(uNormalHeightMap, prevUV, lod).a) - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth + 0.0001);
    vec2 result = mix(currentUV, prevUV, weight);

    // Fade toward original UV at grazing angles for smooth transition
    float fadeFactor = smoothstep(0.15, 0.35, VdotN);
    return mix(uv, result, fadeFactor);
}

// The air between the camera and this point, out of the fog volume: rgb is
// the light it scatters toward the camera, a how much of the point shows
// through it. See VolumetricFog.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * (view * vec4(worldPos, 1.0));
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    // Each slice holds the air up to its far edge, so a point is read half a
    // slice back from where it stands.
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

// The fog a surface takes (selectFog): fogColor/fogParams are the fog around
// the camera - a WMO interior's MFOG while it is in one - which belongs to
// surfaces lit as an interior; everything else takes the outdoor Light.dbc
// fog, outdoorFogColor/outdoorFogParams (docs/wmo-fog.md).
vec4 gFogColor;
vec4 gFogParams;
void selectFog(bool interior) {
    gFogColor = interior ? fogColor : outdoorFogColor;
    gFogParams = interior ? fogParams : outdoorFogParams;
}

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
    selectFog(isInterior != 0);
    float lodFactor = computeLodFactor();
    // Gradients of the authored UV, taken here where every pixel of the quad
    // still agrees on them. Parallax moves the UV by a different amount per
    // pixel, and a mip level chosen from the moved UV flickers.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    vec3 vertexNormal = normalize(Normal);
    if (!gl_FrontFacing) vertexNormal = -vertexNormal;

    // Compute final UV (with POM if enabled)
    vec2 finalUV = TexCoord;

    // Build TBN matrix
    vec3 T = normalize(Tangent);
    vec3 B = normalize(Bitangent);
    vec3 N = vertexNormal;
    mat3 TBN = mat3(T, B, N);

    if (enablePOM != 0 && heightMapVariance > 0.001 && lodFactor < 0.99) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(TexCoord, viewDirTS, lodFactor);
    }

    vec4 texColor = hasTexture != 0 ? textureGrad(uTexture, finalUV, uvDx, uvDy) : vec4(1.0);
    if (alphaTest != 0 && texColor.a < 0.5) discard;

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (enableNormalMap != 0 && lodFactor < 0.99 && normalMapStrength > 0.001) {
        vec3 mapNormal = textureGrad(uNormalHeightMap, finalUV, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal = normalize(mapNormal);
        vec3 worldNormal = normalize(TBN * mapNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        // Linear blend: strength controls how much normal map detail shows,
        // LOD fades out at distance. Both multiply for smooth falloff.
        float blend = clamp(normalMapStrength, 0.0, 1.0) * (1.0 - lodFactor);
        norm = normalize(mix(vertexNormal, worldNormal, blend));
    }

    vec3 result;

    // Sample shadow map for all groups.  Interior groups receive attenuated
    // shadow (30%) so they get subtle light/shadow variation without the full
    // outdoor darkening that makes them look wrong.
    float shadow = 1.0;
    if (shadowParams.x > 0.5) {
        vec3 ldir = normalize(-lightDir.xyz);
        float normalOffset = shadowTexel() * 2.0 * (1.0 - abs(dot(norm, ldir)));
        vec3 biasedPos = FragPos + norm * normalOffset;
        vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
        vec3 proj = lsPos.xyz / lsPos.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        if (proj.x >= 0.0 && proj.x <= 1.0 &&
            proj.y >= 0.0 && proj.y <= 1.0 &&
            proj.z >= 0.0 && proj.z <= 1.0) {
            float bias = max(0.0005 * (1.0 - abs(dot(norm, ldir))), 0.00005);
            shadow = sampleShadowPCF(uShadowMap, vec3(proj.xy, proj.z - bias));
        }
        shadow = mix(1.0, shadow, shadowParams.y);
    }
    RtLight rt = rtLightAt(FragPos);
    shadow = rtShadow(rt, shadow);

    if (isInterior != 0) {
        // WMO interior: vertex colors (MOCV) are pre-baked lighting from the artist,
        // and indoor groups are shielded from outdoor directional sun shadows.
        // In TBC 2.4.3 (pixel/mapobjoverbright.bls), intBatch MOCV alpha scales
        // interior vertex color by (1 + 4 * VertColor.a), while groups without
        // MOCV use MOHD ambient directly.
        vec3 wmoAmbient = vec3(wmoAmbientR, wmoAmbientG, wmoAmbientB);
        if (lightDir.w != 0.0) {
            vec3 mocv = hasVertexColors != 0
                ? VertColor.rgb * (1.0 + 4.0 * VertColor.a)
                : wmoAmbient;
            result = texColor.rgb * mocv;
        } else {
            wmoAmbient = max(wmoAmbient, vec3(0.15));
            vec3 mocv = max(VertColor.rgb, wmoAmbient);
            result = texColor.rgb * mocv;
        }
    } else if (unlit != 0) {
        // Outdoor unlit surface - still receives directional shadows
        result = texColor.rgb * shadow;
    } else {
        vec3 ldir = normalize(-lightDir.xyz);
        float diff = max(dot(norm, ldir), 0.0);

        vec3 viewDir = normalize(viewPos.xyz - FragPos);
        vec3 halfDir = normalize(ldir + viewDir);
        float spec = pow(max(dot(norm, halfDir), 0.0), 32.0) * specularIntensity;
        if (specularAlphaMask != 0) spec *= texColor.a;

        vec3 highlight = lightDir.w != 0.0 && specularAlphaMask != 0
            ? VertexSpecular * texColor.a : spec * lightColor.rgb;
        if (isMetal != 0) {
            highlight *= mix(vec3(1.0), texColor.rgb, 0.5) * 1.5;
        }
        result = rtAmbient(rt, ambientColor.rgb) * texColor.rgb
               + shadow * (diff * lightColor.rgb * texColor.rgb + highlight);
        if (lightDir.w != 0.0) {
            // Legacy primary vertex color saturates before interpolation.
            result = texColor.rgb * (ambientColor.rgb + shadow * (VertexLighting - ambientColor.rgb))
                   + shadow * highlight;
        }

        // An exterior group's vertex colour is light baked into it - a lamp's
        // pool on a wall - and it adds to the sun, as it does in the client.
        // It was multiplied in as if it were baked shadow, floored at a
        // quarter: Stormwind has two exterior groups with vertex colours at
        // all, both near black, and they drew at a quarter of the light the
        // rest of the city had, shadows on or off - and so did 278 of the 362
        // exterior groups in the game that carry any. A group without them has
        // white filled in by the loader, which must add nothing.
        //
        // At half: a handful of Icecrown exteriors ship them pure white, and
        // added at full those would double in brightness.
        if (hasVertexColors != 0) result += texColor.rgb * VertColor.rgb * 0.5;
    }

    result += localLightContribution(FragPos, norm, texColor.rgb);

    if (hasEnvMap != 0) {
        // Camera-space reflection vector mapped onto a 2D spherical environment
        // map (MOMT shader 3 Env and shader 5 EnvMetal, texture2).
        vec3 viewPosEye = (view * vec4(FragPos, 1.0)).xyz;
        vec3 viewDirEye = normalize(viewPosEye);
        vec3 viewNorm = normalize(mat3(view) * norm);
        vec3 refl = reflect(viewDirEye, viewNorm);
        float reflM = 2.0 * sqrt(refl.x * refl.x + refl.y * refl.y + (refl.z + 1.0) * (refl.z + 1.0));
        vec2 envUV = (reflM > 1e-4) ? (refl.xy / reflM + 0.5) : (viewNorm.xy * 0.5 + 0.5);
        vec3 envColor = texture(uEnvMap, envUV).rgb;
        if (isMetal != 0) {
            // MOMT shader 5 (EnvMetal): metallic reflection modulated by lit base color and masked by alpha
            // (so dark interior surfaces like Steamvault Coilfang pipes do not glow neon unlit).
            result += envColor * result * texColor.a;
        } else {
            // MOMT shader 3 (Env): glossy reflection added on top, masked by alpha
            result += envColor * texColor.a;
        }
    }

    if (emissive == 1) {
        // MOMT F_SIDN: the material's authored sidnColor is emitted on top of
        // its lighting. Data: STORMWINDLAMPGLASS and MM_CLOCKFACE_01 in
        // duskwood_townhall.wmo carry white; ten F_SIDN materials (ctforc_a.wmo
        // among them) carry black, i.e. no emission, and so draw lit only.
        // Non-TBC profiles only. How native scales it with the time of day is
        // not established, so the full colour is emitted at all hours.
        vec3 sidn = vec3(float((sidnColor >> 16) & 255),
                         float((sidnColor >> 8) & 255),
                         float(sidnColor & 255)) / 255.0;
        result += texColor.rgb * sidn;
    }

    float dist = length(viewPos.xyz - FragPos);
    result = applyFog(result, FragPos, dist);

    float alpha = texColor.a;

    outColor = vec4(result, alpha);
}
