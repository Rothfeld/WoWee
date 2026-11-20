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
    mat4 model;
    vec4 celestialColor; // xyz = color, w = unused
    float intensity;
    float moonPhase;
    float animTime;
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = aTexCoord;
    // Extract celestial body center and billboard size from push.model
    // (translate(center) * scale(w, h, 1)). Build an orthonormal basis tangent
    // to the celestial sphere at `center` so the quad always faces the observer
    // head-on (circular at all elevations, never foreshortened into an oval).
    vec3 center = push.model[3].xyz;
    vec2 size = vec2(length(push.model[0].xyz), length(push.model[1].xyz));

    vec3 toBody = normalize(center);
    vec3 refUp = abs(toBody.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(toBody, refUp));
    vec3 up = cross(right, toBody);

    vec3 worldPos = center + right * (aPos.x * size.x) + up * (aPos.y * size.y);

    // Sky object: remove camera translation so celestial bodies are at infinite distance
    mat4 rotView = mat4(mat3(view));
    vec4 clip = projection * rotView * vec4(worldPos, 1.0);
    // On the far plane, as the clouds are, so the depth test the sky is now
    // drawn under rejects it wherever the world has already been drawn.
    gl_Position = vec4(clip.xy, clip.w, clip.w);
}
