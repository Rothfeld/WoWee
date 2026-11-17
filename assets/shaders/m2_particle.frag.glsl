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

layout(location = 0) out vec4 outColor;

void main() {
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

    // Texture supplies the sprite silhouette. The alpha and additive
    // pipelines use SRC_ALPHA, so RGB is not premultiplied here.
    float alpha = texColor.a * vColor.a * vFogVisibility;
    vec3 rgb = texColor.rgb * vColor.rgb;
    if (push.blendMode == 5) {
        // Mod (dst * src): fade toward white, which leaves dst unchanged.
        outColor = vec4(mix(vec3(1.0), rgb, alpha), 1.0);
    } else if (push.blendMode == 6) {
        // Mod2x (dst * src * 2): neutral is mid grey.
        outColor = vec4(mix(vec3(0.5), rgb, alpha), 1.0);
    } else {
        outColor = vec4(rgb, alpha);
    }
}
