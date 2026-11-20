#version 450

layout(set = 1, binding = 0) uniform sampler2D uMist;
layout(set = 2, binding = 1) uniform sampler2D uSceneDepth;

layout(push_constant) uniform MistPush {
    vec4 depthParams; // x = near, y = far, z = maxFadeRange (0 = disabled), w = depthFadeSlope
    vec4 screenSize;  // xy = render target extent in pixels, z = minFadeRange, w = unused
} push;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

float linearizeDepth(float d, float near, float far) {
    return near * far / (far - d * (far - near));
}

void main() {
    vec4 t = texture(uMist, vTexCoord);
    float alpha = vColor.a * t.a;
    if (push.depthParams.z > 0.0) {
        vec2 screenUV = clamp(gl_FragCoord.xy / max(push.screenSize.xy, vec2(1.0)),
                              vec2(0.0), vec2(1.0));
        float sceneDepth = texture(uSceneDepth, screenUV).r;
        float sceneLin = linearizeDepth(sceneDepth, push.depthParams.x, push.depthParams.y);
        float fragLin = linearizeDepth(gl_FragCoord.z, push.depthParams.x, push.depthParams.y);
        float depthDelta = sceneLin - fragLin;
        float minRange = min(push.screenSize.z, push.depthParams.z);
        float effRange = clamp(fragLin * push.depthParams.w, minRange, push.depthParams.z);
        float fade = smoothstep(0.0, 1.0, clamp(depthDelta / max(effRange, 1e-3), 0.0, 1.0));
        alpha *= fade;
    }
    outColor = vec4(vColor.rgb * t.rgb, alpha);
}
