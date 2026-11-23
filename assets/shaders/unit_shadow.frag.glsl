#version 450
// Textures\ShadowBlob.blp projected straight down over the unit's GeoBox
// rectangle and multiplied onto what is already drawn there. Fogged toward
// white like the ground it lies on: a far shadow fades into the fog
// (Lykul Stinger 19367 at 24 yd: 0.68 of its near depth in the sweep).
layout(set = 1, binding = 0) uniform sampler2D blobTexture;
layout(push_constant) uniform Push {
    vec4 centreAxis;
    vec4 halfAndZ;
    vec4 cameraFog;
    vec4 fog;
} push;
layout(location = 0) in vec3 worldPos;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 forward = push.centreAxis.zw;
    vec2 left = vec2(-forward.y, forward.x);
    vec2 d = worldPos.xy - push.centreAxis.xy;
    vec2 uv = vec2(0.5 + dot(d, forward) / (2.0 * push.halfAndZ.x),
                   0.5 + dot(d, left) / (2.0 * push.halfAndZ.y));
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) ||
        worldPos.z < push.halfAndZ.z || worldPos.z > push.halfAndZ.w) discard;
    float dist = length(push.cameraFog.xyz - worldPos);
    float visibility = clamp((push.fog.y - dist) / max(push.fog.y - push.fog.x, 0.001), 0.0, 1.0);
    vec3 shade = mix(vec3(1.0), texture(blobTexture, uv).rgb, visibility);
    outColor = vec4(shade, 1.0);
}
