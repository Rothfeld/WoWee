#version 450

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    vec2 tileCount;
    int alphaKey;
    float viewportHeight;
} push;

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vTile;
layout(location = 2) in float vFogVisibility;
layout(location = 3) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 p = vUV;
    float tile = floor(vTile);
    float tx = mod(tile, push.tileCount.x);
    float ty = floor(tile / push.tileCount.x);
    vec2 uv = (vec2(tx, ty) + p) / push.tileCount;
    vec4 texColor = texture(uTexture, uv);

    if (push.alphaKey != 0) {
        float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        if (lum < 0.05) discard;
    }

    // Texture supplies the sprite silhouette. Both pipelines use SRC_ALPHA,
    // so multiplying RGB by alpha here would apply opacity twice.
    float alpha = texColor.a * vColor.a * vFogVisibility;
    vec3 rgb = texColor.rgb * vColor.rgb;
    outColor = vec4(rgb, alpha);
}
