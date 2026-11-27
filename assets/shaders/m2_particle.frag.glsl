#version 450

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    vec2 tileCount;
    int blendMode;  // M2Blend: 0 opaque, 1 alpha key, 2 alpha, 3 no-alpha add, 4 add, 5 mod, 6 mod2x
    float viewportHeight;
} push;

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vTile;
layout(location = 2) in float vFogVisibility;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec3 vFogInscatter;

layout(location = 0) out vec4 outColor;

void main() {
    if (vFogVisibility < 0.0) discard;
    // Quad geometry grows upward in view space; BLP rows start at the top.
    vec2 p = vec2(vUV.x, 1.0 - vUV.y);
    float tile = floor(vTile);
    float tx = mod(tile, push.tileCount.x);
    float ty = floor(tile / push.tileCount.x);
    vec2 uv = (vec2(tx, ty) + p) / push.tileCount;
    vec4 texColor = texture(uTexture, uv);

    // Alpha key cuts on the texture's own alpha at the M2 reference (224),
    // as mesh AlphaKey batches do. PARTROCK/bone debris (41 TBC emitters)
    // carry 8-bit alpha. Opaque, alpha-key and no-alpha-add ignore texture
    // alpha after that; the authored lifecycle alpha still fades them.
    if (push.blendMode == 1 && texColor.a < 224.0 / 255.0) discard;
    if (push.blendMode == 0 || push.blendMode == 1 || push.blendMode == 3) texColor.a = 1.0;

    float baseAlpha = texColor.a * vColor.a;
    float foggedAlpha = baseAlpha * vFogVisibility;
    vec3 rgb = texColor.rgb * vColor.rgb;
    if (push.blendMode == 3 || push.blendMode == 4) {
        // Additive modes (NoAlphaAdd / Add): TBC 2.4.3 binds black fog color
        // (D3DRS_FOGCOLOR = 0) on additive batches, attenuating RGB toward zero
        // by fog visibility without adding scene fogColor. Premultiply RGB and
        // emit peak luminance in alpha for off-screen preview FBO coverage.
        vec3 premul = rgb * foggedAlpha;
        outColor = vec4(premul, max(max(premul.r, premul.g), premul.b));
    } else if (push.blendMode == 5) {
        // Mod (dst * src): fade toward white, which leaves dst unchanged.
        outColor = vec4(mix(vec3(1.0), rgb, foggedAlpha), 1.0);
    } else if (push.blendMode == 6) {
        // Mod2x (dst * src * 2): neutral is mid grey.
        outColor = vec4(mix(vec3(0.5), rgb, foggedAlpha), 1.0);
    } else {
        // Opaque / AlphaKey / Alpha (M2Blend 0, 1, 2): TBC 2.4.3 fogs RGB toward
        // the active fog color while preserving authored coverage alpha so stacked
        // splashes/smoke cap at mix(fogColor, rgb, fogFactor) and AlphaKey debris
        // fogs to fogColor rather than turning transparent.
        outColor = vec4(rgb * vFogVisibility + vFogInscatter, baseAlpha);
    }
}
