#version 450

// M2 geometry-model particle fragment shader.
// Matches m2_particle.frag.glsl M2Blend semantics (0 opaque, 1 alpha key,
// 2 alpha, 3 no-alpha add, 4 add, 5 mod, 6 mod2x) for 3D particle meshes.

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    int blendMode;  // M2Blend: 0 opaque, 1 alpha key, 2 alpha, 3 no-alpha add, 4 add, 5 mod, 6 mod2x
} push;

layout(location = 0) in vec3 vColor;
layout(location = 1) in float vAlpha;
layout(location = 2) in vec2 vUV;
layout(location = 3) in float vFogFactor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uTexture, vUV);

    // AlphaKey cuts on the texture's own alpha at the M2 reference (224/255),
    // matching m2_particle.frag.glsl and M2 mesh AlphaKey batches.
    if (push.blendMode == 1 && tex.a < 224.0 / 255.0) discard;
    if (push.blendMode == 0 || push.blendMode == 1 || push.blendMode == 3) tex.a = 1.0;

    float alpha = tex.a * vAlpha * vFogFactor;
    vec3 rgb = tex.rgb * vColor;
    if (push.blendMode == 5) {
        // Mod (dst * src): fade toward white, which leaves dst unchanged.
        outColor = vec4(mix(vec3(1.0), rgb, alpha), 1.0);
    } else if (push.blendMode == 6) {
        // Mod2x (dst * src * 2): neutral is mid grey.
        outColor = vec4(mix(vec3(0.5), rgb, alpha), 1.0);
    } else {
        if (alpha < 0.001) discard;
        outColor = vec4(rgb, alpha);
    }
}
