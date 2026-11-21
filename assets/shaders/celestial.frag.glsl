#version 450

layout(push_constant) uniform Push {
    mat4 model;
    vec4 celestialColor; // xyz = color, w = 1.0 sun / 0.0 moon
    float intensity;
    float moonPhase;
    float animTime;
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
    float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
    float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
    float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    vec2 uv = TexCoord - 0.5;
    float dist = length(uv);

    bool isMoon = push.celestialColor.w < 0.5;
    // Circular cutoff: 0.50 for both sun radial glare and moon MoonGlare halo
    float maxRadius = 0.50;
    if (dist > maxRadius) discard;

    vec3 color = push.celestialColor.rgb;
    float alpha;

    if (!isMoon) {
        // ---------------- Sun ----------------
        // Soft core + smooth radial glare matching TBC SunCenter + SunGlare
        float core = smoothstep(0.24, 0.13, dist);
        float innerGlow = exp(-dist * dist * 28.0) * 0.65;
        float outerGlow = exp(-dist * dist * 8.0) * 0.28 * (1.0 - smoothstep(0.32, 0.50, dist));

        alpha = clamp(max(core, innerGlow) + outerGlow, 0.0, 1.0) * push.intensity;

        // Subtle warm-white core brightening
        color = mix(color, vec3(1.0, 0.99, 0.94), core * 0.75);
    } else {
        // ---------------- Moon ----------------
        const float r = 0.35;              // moon disc radius in UV space (70% of quad)
        float disc = smoothstep(r + 0.015, r - 0.025, dist);
        float d = min(dist / r, 1.0);

        // Sphere normal for the disc point
        float nz = sqrt(max(1.0 - d * d, 0.0));
        vec3 n = vec3(uv / r, nz);

        // Spherical phase terminator (moonPhase == 0.5 -> full luminous disc
        // matching Native TBC 2.4.3 Textures\Moon.blp + MoonGlare.blp).
        float ph = (push.moonPhase - 0.5) * 6.2831853;
        vec3 l = vec3(sin(ph), 0.0, cos(ph));
        float lit = smoothstep(-0.25, 0.15, dot(n, l));
        lit = max(lit, 0.12);

        // Soft lunar maria mottling matching Textures\Moon.blp
        float seed = push.celestialColor.r * 43.0;
        vec2 suv = uv / r;
        float maria  = valueNoise(suv * 2.6 + seed);
        float detail = valueNoise(suv * 7.0 + seed * 1.7) * 0.6
                     + valueNoise(suv * 14.0 - seed) * 0.4;
        float surfaceShade = 1.0
            - smoothstep(0.42, 0.82, maria) * 0.16
            - smoothstep(0.58, 0.85, detail) * 0.08;

        // Gentle limb softening
        float limb = mix(1.0, 0.78, d * d);

        color *= surfaceShade * limb * lit;

        // Luminous MoonGlare radial halo extending from disc edge to 0.50
        float glow = exp(-dist * dist * 16.0) * 0.35 * (1.0 - smoothstep(0.36, 0.50, dist)) * lit;

        alpha = clamp(max(disc, glow), 0.0, 1.0) * push.intensity;
    }

    if (alpha < 0.01) discard;
    // Pre-multiply for additive blending: RGB is the light contribution
    outColor = vec4(color * alpha, alpha);
}
