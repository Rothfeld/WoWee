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
    // x: gain on the TBC unit light, 1 + 2 x the area's AmbientMultiplier
    // (GPUPerFrameData::unitLight).
    vec4 unitLight;
};

#include "rt_lighting.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform CharMaterial {
    float opacity;
    int alphaTest;
    int colorKeyBlack;
    int unlit;
    float emissiveBoost;
    // Keep these as scalar floats to match the C++ UBO packing. A std140 vec3
    // would insert padding here and shift the following material flags.
    float emissiveTintR;
    float emissiveTintG;
    float emissiveTintB;
    float specularIntensity;
    int enableNormalMap;
    int enablePOM;
    float pomScale;
    int pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
    int hairMaterial;
    float hoverHighlight;
    // SpellVisualKit CharProc 1 colour of the unit's latest aura kit; white
    // when none. Multiplies the shaded model (Vertex Color Black renders a
    // black silhouette, Immolate a red-tinted one).
    float kitColorR;
    float kitColorG;
    float kitColorB;
    int textureCount;
    int texCoordSet;
    int texCoordSet2;
    int blendMode;
    float uvOffsetX;
    float uvOffsetY;
    float uvOffset2X;
    float uvOffset2Y;
    int combiner0;
    int combiner1;
    // Light of a unit standing in a locally lit WMO interior group: the
    // floor's MOCV under its feet (WMORenderer::interiorObjectLight). Takes
    // the place of the outdoor light while interiorLit is set.
    float interiorLightR;
    float interiorLightG;
    float interiorLightB;
    int interiorLit;
    // SpellVisualKit CharProc 13 colour (white when none), multiplying all
    // light on the unit: ambient, sun, interior and local lights. Demonic
    // Alignment / Shadow Image's black leaves only fog: native 2.4.3 shows a
    // pitch-black unit on GM Island at night under an ambient of 0x637d5b.
    float kitLightR;
    float kitLightG;
    float kitLightB;
    // The batch's M2 colour, as m2.frag's tint.
    float tintR;
    float tintG;
    float tintB;
    float unitDiffuseR;
    float unitDiffuseG;
    float unitDiffuseB;
    float unitLightDirX;
    float unitLightDirY;
    float unitLightDirZ;
    float unitModelCenterX;
    float unitModelCenterY;
    float unitModelCenterZ;
    float unitModelRadius;
    int unitHasCustomLight;
};

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;
layout(set = 1, binding = 3) uniform sampler2D uTexture2;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoordIn;
layout(location = 3) in vec3 Tangent;
layout(location = 4) in vec3 Bitangent;
layout(location = 5) in vec2 TexCoord2In;
layout(location = 6) in vec2 EnvCoord;
layout(location = 7) in float NormalWeight;

layout(location = 0) out vec4 outColor;

const int PREVIEW_SIMPLE_TEXTURE_MODE = -31336;

// The M2 texture unit's UV source: 0/1 mesh UV set, -1 environment map.
vec2 unitCoord(int set, vec2 offset) {
    return (set == -1 ? EnvCoord : (set == 1 ? TexCoord2In : TexCoordIn)) + offset;
}

vec2 TexCoord;

// Texture stage combiners (M2 texture combiner combos: 0 Opaque, 1 Mod,
// 2 Decal, 3 Add, 4 Mod2x), as the pixel shaders listed for the M2 format
// (wowdev.wiki M2/.skin): a non-Opaque first stage keeps its alpha; the
// second stage Opaque multiplies colour and keeps alpha, Mod2x doubles both,
// Add adds colour weighted by its alpha, the rest modulate.
vec4 combineStages(vec4 t0, vec4 t1) {
    vec3 rgb = t0.rgb;
    float a = combiner0 == 0 ? 1.0 : t0.a;
    if (textureCount == 2) {
        if (combiner1 == 0) {
            rgb *= t1.rgb;
        } else if (combiner1 == 3) {
            rgb += t1.rgb * t1.a;
        } else if (combiner1 == 4) {
            rgb *= t1.rgb * 2.0;
            a *= t1.a * 2.0;
        } else {
            rgb *= t1.rgb;
            a *= t1.a;
        }
    }
    return vec4(rgb, a);
}

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

vec3 tbcM2PointLightSum(vec3 worldPos, vec3 norm, vec3 boundsCenter, float boundsRadius) {
    int totalLights = min(localLightMeta.y, 64);
    if (totalLights <= 0) return vec3(0.0);
    vec3 sum = vec3(0.0);
    int applied = 0;
    for (int i = 0; i < totalLights && applied < 4; ++i) {
        vec3 lightPos = localLightPosRadius[i].xyz;
        float radius = localLightPosRadius[i].w;
        float maxDist = radius + boundsRadius;
        vec3 centerDelta = lightPos - boundsCenter;
        if (radius <= 0.0 || dot(centerDelta, centerDelta) > maxDist * maxDist) continue;
        applied++;
        vec3 toLight = lightPos - worldPos;
        float d2 = dot(toLight, toLight);
        float invD = inversesqrt(max(d2, 1e-8));
        float d = d2 * invD;
        float ndotl = max(dot(norm, toLight * invD), 0.0);
        sum += localLightColorIntensity[i].rgb * (ndotl / max(0.7 * d + 0.03 * d2, 1e-5));
    }
    return sum;
}

// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    return smoothstep(0.0001, 0.005, texelDensity);
}

vec3 safeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    if (len2 > 1e-8) {
        return v * inversesqrt(len2);
    }
    return fallback;
}

vec3 fallbackTangent(vec3 n) {
    vec3 axis = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    return safeNormalize(cross(axis, n), vec3(1.0, 0.0, 0.0));
}

bool finiteVec3(vec3 v) {
    return all(equal(v, v)) && all(lessThan(abs(v), vec3(1e10)));
}

bool isMagentaKeyColor(vec4 color) {
    return color.r >= 0.58 && color.b >= 0.58 && color.g <= 0.48 &&
           color.r >= color.g + 0.22 && color.b >= color.g + 0.22 &&
           abs(color.r - color.b) <= 0.38;
}

ivec2 wrapPreviewTexel(ivec2 texel, ivec2 texSize) {
    return ivec2((texel.x % texSize.x + texSize.x) % texSize.x,
                 (texel.y % texSize.y + texSize.y) % texSize.y);
}

vec4 samplePreviewTexture(sampler2D tex, vec2 uv) {
    ivec2 texSize = textureSize(tex, 0);
    if (texSize.x <= 0 || texSize.y <= 0) {
        return textureLod(tex, uv, 0.0);
    }

    vec2 wrappedUv = uv - floor(uv);
    ivec2 baseTexel = ivec2(floor(wrappedUv * vec2(texSize)));
    baseTexel = wrapPreviewTexel(baseTexel, texSize);

    vec4 color = texelFetch(tex, baseTexel, 0);
    if (!isMagentaKeyColor(color)) {
        return color;
    }

    for (int radius = 1; radius <= 4; ++radius) {
        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                if (abs(x) != radius && abs(y) != radius) {
                    continue;
                }
                vec4 candidate = texelFetch(tex, wrapPreviewTexel(baseTexel + ivec2(x, y), texSize), 0);
                if (!isMagentaKeyColor(candidate)) {
                    return vec4(candidate.rgb, 0.0);
                }
            }
        }
    }

    return vec4(0.0);
}

// Parallax Occlusion Mapping with angle-adaptive sampling
vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float lodFactor) {
    float VdotN = abs(viewDirTS.z);

    if (VdotN < 0.15) return uv;

    float angleFactor = clamp(VdotN, 0.15, 1.0);
    int maxS = pomMaxSamples;
    int minS = max(maxS / 4, 4);
    int numSamples = int(mix(float(minS), float(maxS), angleFactor));
    numSamples = int(mix(float(minS), float(numSamples), 1.0 - lodFactor));

    float layerDepth = 1.0 / float(numSamples);
    float currentLayerDepth = 0.0;

    vec2 P = viewDirTS.xy / max(VdotN, 0.15) * pomScale;
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

    for (int i = 0; i < 64; i++) {
        if (i >= numSamples || currentLayerDepth >= currentDepthMapValue) break;
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;
        currentLayerDepth += layerDepth;
    }

    vec2 prevUV = currentUV + deltaUV;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = (1.0 - textureLod(uNormalHeightMap, prevUV, lod).a) - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth + 0.0001);
    vec2 result = mix(currentUV, prevUV, weight);

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
    selectFog(interiorLit != 0);
    TexCoord = unitCoord(texCoordSet, vec2(uvOffsetX, uvOffsetY));
    vec4 secondColor = textureCount == 2
        ? texture(uTexture2, unitCoord(texCoordSet2, vec2(uvOffset2X, uvOffset2Y)))
        : vec4(1.0);
    // Clamped: Mod2x can double alpha past one.
    if (enablePOM == PREVIEW_SIMPLE_TEXTURE_MODE) {
        vec4 texColor = combineStages(samplePreviewTexture(uTexture, TexCoord), secondColor);
        texColor.rgb *= vec3(tintR, tintG, tintB);
        texColor.a = min(texColor.a, 1.0);
        if (isMagentaKeyColor(texColor)) {
            discard;
        }
        if (alphaTest != 0 && texColor.a < 0.5) {
            discard;
        }
        if (alphaTest != 0 && hairMaterial != 0) {
            texColor.a = 1.0;
        }
        if (colorKeyBlack != 0) {
            float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
            float ck = smoothstep(0.12, 0.30, lum);
            texColor.a *= ck;
            if (texColor.a < 0.01) discard;
        }
        outColor = vec4(texColor.rgb, texColor.a * opacity);
        return;
    }

    float lodFactor = computeLodFactor();
    // Gradients of the authored UV, taken here where every pixel of the quad
    // still agrees on them. Parallax moves the UV by a different amount per
    // pixel, and a mip level chosen from the moved UV flickers.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    vec3 vertexNormal = safeNormalize(Normal, vec3(0.0, 0.0, 1.0));
    // TBC (legacy light, lightDir.w != 0) lights both sides of a two-sided
    // material alike: a Chillwind Chimaera's wings (Chimera.m2 materials 2-3,
    // flags 0x04) seen from below are as pale as from above in build 8606.
    // Turning the normal round lit their undersides by the ambient alone.
    if (!gl_FrontFacing && lightDir.w == 0.0) vertexNormal = -vertexNormal;

    vec2 finalUV = TexCoord;

    bool usePOM = enablePOM != 0 &&
                  alphaTest == 0 &&
                  colorKeyBlack == 0 &&
                  heightMapVariance > 0.001 &&
                  lodFactor < 0.99;
    // TBC model2.bls shades characters and creatures strictly from smooth
    // vertex normals; Sobel albedo-gradient normal maps create harsh micro-ridges
    // on bare skin/sleeves under 2.5x unit diffuse.
    bool useNormalMap = enableNormalMap != 0 &&
                        lightDir.w == 0.0 &&
                        unlit == 0 &&
                        lodFactor < 0.99 &&
                        normalMapStrength > 0.001;
    mat3 TBN;
    if (usePOM || useNormalMap) {
        vec3 T = safeNormalize(Tangent, fallbackTangent(vertexNormal));
        T = safeNormalize(T - dot(T, vertexNormal) * vertexNormal, fallbackTangent(vertexNormal));
        vec3 B = safeNormalize(Bitangent, safeNormalize(cross(vertexNormal, T), vec3(0.0, 1.0, 0.0)));
        TBN = mat3(T, B, vertexNormal);
    }

    if (usePOM) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(TexCoord, viewDirTS, lodFactor);
    }

    vec4 texColor = combineStages(textureGrad(uTexture, finalUV, uvDx, uvDy), secondColor);
    vec3 batchTint = vec3(tintR, tintG, tintB);
    vec3 rawTexRgb = texColor.rgb;
    texColor.rgb *= batchTint;
    texColor.a = min(texColor.a, 1.0);
    // Repair dark DXT fringes on alpha-cut character textures such as hair.
    // Transparent edge texels can carry black/garbage RGB even when alpha is
    // valid; pull color from a coarser mip and trust the source more as alpha
    // approaches opaque. This matches the generic M2 path.
    if (alphaTest != 0 && texColor.a > 0.01 && texColor.a < 1.0) {
        vec3 mipColor = textureLod(uTexture, finalUV, 4.0).rgb;
        float trust = smoothstep(0.0, 0.9, texColor.a);
        rawTexRgb = mix(mipColor, rawTexRgb, trust);
        texColor.rgb = mix(mipColor, texColor.rgb, trust);
    }

    // Some classic/TBC character textures use bright magenta as a color key.
    // Apply this before any material-specific alpha path because a few preview
    // batches report as opaque/blended even when their texture still carries
    // mask-color texels.
    if (texColor.r > 0.78 && texColor.g < 0.28 && texColor.b > 0.78) {
        discard;
    }

    if (alphaTest != 0 && hairMaterial != 0) {
        if (texColor.a < 0.5) {
            discard;
        }
        texColor.a = 1.0;
    } else if (alphaTest != 0) {
        // Screen-space sharpened alpha for alpha-to-coverage anti-aliasing.
        // Rescales alpha so the 0.5 cutoff maps to exactly the texel boundary,
        // giving smooth edges when MSAA + alpha-to-coverage is active.
        float aGrad = fwidth(texColor.a);
        texColor.a = clamp((texColor.a - 0.5) / max(aGrad, 0.001) * 0.5 + 0.5, 0.0, 1.0);
        if (texColor.a < 1.0 / 255.0) discard;
    }
    if (colorKeyBlack != 0) {
        float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        float ck = smoothstep(0.12, 0.30, lum);
        texColor.a *= ck;
        if (texColor.a < 0.01) discard;
    }

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (useNormalMap) {
        vec3 mapNormal = textureGrad(uNormalHeightMap, finalUV, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal.xy *= normalMapStrength;
        mapNormal = safeNormalize(mapNormal, vec3(0.0, 0.0, 1.0));
        vec3 worldNormal = safeNormalize(TBN * mapNormal, vertexNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        float blendFactor = max(lodFactor, 1.0 - normalMapStrength);
        norm = safeNormalize(mix(worldNormal, vertexNormal, blendFactor), vertexNormal);
    }

    vec3 result;

    if (unlit != 0) {
        vec3 emissiveTint = vec3(emissiveTintR, emissiveTintG, emissiveTintB);
        vec3 warm = emissiveTint * emissiveBoost;
        // TBC legacy light (lightDir.w != 0): unlit is the texture as it is,
        // as m2.frag.
        result = lightDir.w != 0.0 ? texColor.rgb : texColor.rgb * (1.0 + warm);
    } else {
        // Zero-normal M2 vertices (e.g. TitanFemale.m2 skirt, 106/660 TBC creatures)
        // evaluate N.L = 0 in D3D9/TBC Gouraud lighting instead of snapping to a
        // synthetic (0, 0, 1) normal. Scaling by interpolated NormalWeight preserves
        // smooth falloff across triangles mixing unit-normal and zero-normal vertices.
        vec3 lightNorm = (dot(Normal, Normal) > 1e-8) ? (norm * clamp(NormalWeight, 0.0, 1.0)) : vec3(0.0);

        vec3 ldir = normalize(-lightDir.xyz);
        vec3 kitLight = vec3(kitLightR, kitLightG, kitLightB);
        vec3 ambient = ambientColor.rgb * kitLight;
        vec3 sunLight = lightColor.rgb * kitLight;
        // A hard clamp at zero takes a surface turned away from the sun down to
        // the ambient term alone, and the ambient term does not vary across a
        // surface, so those pixels come out one flat tone that reads as a
        // separate, untextured piece - the bare shins against the lit thighs
        // were the case. 0.22 is the floor localLightContribution uses.
        float diff = 0.22 + 0.78 * max(dot(lightNorm, ldir), 0.0);

        vec3 viewDir = normalize(viewPos.xyz - FragPos);
        vec3 halfDir = normalize(ldir + viewDir);
        float spec = pow(max(dot(lightNorm, halfDir), 0.0), 32.0) * specularIntensity;

        float shadow = 1.0;
        if (shadowParams.x > 0.5) {
            float normalOffset = shadowTexel() * 2.0 * (1.0 - abs(dot(lightNorm, ldir)));
            vec3 biasedPos = FragPos + lightNorm * normalOffset;
            vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
            vec3 proj = lsPos.xyz / lsPos.w;
            proj.xy = proj.xy * 0.5 + 0.5;
            if (proj.x >= 0.0 && proj.x <= 1.0 &&
                proj.y >= 0.0 && proj.y <= 1.0 &&
                proj.z >= 0.0 && proj.z <= 1.0) {
                float bias = max(0.0005 * (1.0 - abs(dot(lightNorm, ldir))), 0.00005);
                shadow = sampleShadowPCF(uShadowMap, vec3(proj.xy, proj.z - bias));
            }
            shadow = mix(1.0, shadow, shadowParams.y);
        }
        RtLight rt = rtLightAt(FragPos);
        shadow = rtShadow(rt, shadow);

        result = rtAmbient(rt, ambient) * texColor.rgb
               + shadow * (diff * sunLight * texColor.rgb + spec * sunLight);
        if (lightDir.w != 0.0) {
            // TBC model2.bls lighting:
            // - Outdoor: ambient = (1 + 2*AmbientMultiplier)*Ambient_DBC,
            //   diffuse = (mcshShadow ? 0.5 : 2.5)*Diffuse_DBC along surface sun dir.
            // - Indoor: ambient = capMax(MOCV, 96/255), diffuse = floorMax(MOCV, 168/255)
            //   along (-0.3082, -0.3082, -0.9), merged with group MOLR point lights
            //   and blended with outdoor light on transBatch vertices (MOCV.a).
            // - Authored M2Light point lights add diffColor*diffIntensity*N.L/(0.7*d + 0.03*d^2)
            //   before the [0, 1] clamp.
            // - Hover and target add 1.0 * Ambient_DBC (c29) after batch tint,
            //   CharProc 13 kitLight, and CharProc 1 kitColor, clamped to [0, 1], so
            //   tinted/shadowform (CharProc 1) and dark-lit (CharProc 13, e.g. Stolen
            //   Soul Visual 32395 with kitLight = 0) units brighten on mouseover/target.
            vec3 effAmbient = (unitHasCustomLight != 0
                ? vec3(interiorLightR, interiorLightG, interiorLightB)
                : ambientColor.rgb * unitLight.x) * kitLight;
            vec3 effDiffuse = (unitHasCustomLight != 0
                ? vec3(unitDiffuseR, unitDiffuseG, unitDiffuseB)
                : lightColor.rgb * 2.5) * kitLight;
            vec3 effDir = unitHasCustomLight != 0
                ? safeNormalize(-vec3(unitLightDirX, unitLightDirY, unitLightDirZ), ldir)
                : ldir;
            vec3 m2Point = tbcM2PointLightSum(
                FragPos, lightNorm,
                vec3(unitModelCenterX, unitModelCenterY, unitModelCenterZ),
                unitModelRadius) * kitLight;
            vec3 baseLight = clamp(effAmbient + max(dot(lightNorm, effDir), 0.0) * effDiffuse + m2Point, 0.0, 1.0);
            vec3 shaded = clamp(effAmbient + m2Point, 0.0, 1.0);
            vec3 litFactor = (interiorLit != 0) ? baseLight : mix(shaded, baseLight, shadow);
            vec3 hoverAdd = hoverHighlight * ambientColor.rgb;
            result = rawTexRgb * clamp(batchTint * litFactor * vec3(kitColorR, kitColorG, kitColorB) + hoverAdd, 0.0, 1.0);
        } else if (interiorLit != 0) {
            vec3 interiorLight = vec3(interiorLightR, interiorLightG, interiorLightB) * kitLight;
            result = texColor.rgb * interiorLight;
        }
    }

    // TBC model2.bls (lightDir.w != 0.0) already accumulates M2 point lights
    // and applies kitColor before hoverAdd inside the [0, 1] ADD_SAT clamp above.
    if (unlit == 0 && lightDir.w == 0.0)
        result += localLightContribution(FragPos, (dot(Normal, Normal) > 1e-8) ? (norm * clamp(NormalWeight, 0.0, 1.0)) : vec3(0.0), texColor.rgb) * vec3(kitLightR, kitLightG, kitLightB);
    if (unlit != 0 || lightDir.w == 0.0)
        result *= vec3(kitColorR, kitColorG, kitColorB);

    // Without the TBC light, unit highlight doubles the shaded colour. Before
    // fog, as the distance haze still applies to a highlighted unit. Under
    // the TBC light it is part of the unit light's gain above.
    if (lightDir.w == 0.0) result *= 1.0 + hoverHighlight;

    float dist = length(viewPos.xyz - FragPos);
    float fogDepth = max(-(view * vec4(FragPos, 1.0)).z, 0.0);
    if (blendMode == 5 || blendMode == 6) {
        float fogFactor = clamp((gFogParams.y - fogDepth) / (gFogParams.y - gFogParams.x), 0.0, 1.0);
        vec3 neutral = vec3(blendMode == 5 ? 1.0 : 0.5);
        // Use shaded `result` rather than raw `texColor.rgb`: in TBC model2.bls,
        // lit Mod/Mod2x batches (rf_flags 0x10 / unlit == 0, e.g. FelBeast.m2
        // Batch[4] Mod2x carapace pass) multiply texture by vertex diffuse light
        // before framebuffer modulation, while unlit != 0 batches keep texColor.rgb.
        outColor = vec4(mix(neutral, result, fogFactor * opacity), texColor.a * opacity);
        return;
    }
    if (blendMode == 3 || blendMode == 4 || blendMode == 7) {
        float fogFactor = clamp((gFogParams.y - fogDepth) / (gFogParams.y - gFogParams.x), 0.0, 1.0);
        result *= fogFactor;
        if (volumetricParams.x > 0.5) result *= fogVolumeAt(FragPos).a;
    } else {
        result = applyFog(result, FragPos, dist);
    }
    if (!finiteVec3(result)) {
        result = texColor.rgb;
    }

    outColor = vec4(result, texColor.a * opacity);
}
