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
};

layout(push_constant) uniform Push {
    float time;
    float intensity;
    float viewportHeight;
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aBrightness;
layout(location = 2) in float aTwinklePhase;
layout(location = 3) in float aColorTemp;

layout(location = 0) out float vBrightness;
layout(location = 1) out vec3 vColor;

// Cool red-orange, through white, to hot blue-white: the spread a real field
// shows. Every star used to be the same (0.9, 0.95, 1.0).
vec3 starColor(float t) {
    const vec3 cool = vec3(1.00, 0.80, 0.62);
    const vec3 mid  = vec3(1.00, 0.97, 0.94);
    const vec3 hot  = vec3(0.78, 0.86, 1.00);
    return t < 0.5 ? mix(cool, mid, t * 2.0)
                   : mix(mid, hot, (t - 0.5) * 2.0);
}

void main() {
    mat4 rotView = mat4(mat3(view));

    // Scintillation is not one slow sine across the whole sky. Two rates that
    // do not divide into each other never repeat, which is what keeps it from
    // reading as a pulse.
    float twinkle = 0.84
        + 0.10 * sin(push.time * 2.3 + aTwinklePhase)
        + 0.06 * sin(push.time * 5.7 + aTwinklePhase * 1.7);
    // Fade stars gently near the horizon (0-7 deg), keeping full crispness
    // above distant mountains like Native TBC Environments\Stars\Stars.m2.
    float horizonFade = smoothstep(0.01, 0.12, normalize(aPos).z);
    vBrightness = aBrightness * twinkle * push.intensity * horizonFade;
    vColor = starColor(aColorTemp);

    const float kAngularRadius = 0.0024;
    float sizeScale = mix(0.85, 1.40, aBrightness);
    gl_PointSize = clamp(
        kAngularRadius * push.viewportHeight * projection[1][1] * sizeScale,
        2.0, 4.8);

    vec4 clip = projection * rotView * vec4(aPos, 1.0);
    // On the far plane, so the world drawn before the sky occludes it.
    gl_Position = vec4(clip.xy, clip.w, clip.w);
}
