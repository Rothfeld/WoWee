#version 450

// Snow flakes: Shaders\Vertex\snowpoint.bls (2.4.3 game data). The CPU moves
// each flake (start + velocity * clamped age) and computes its alpha (fade in
// over the first second, out over a quarter second after it lands) and point
// size (clamp(distance * a + b, 0, 1) * c, at least one pixel).

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
    vec4 color;     // Weather.dbc EffectColor
    vec4 size;      // xyz: snowpoint.bls c_pointSize
} push;

layout(location = 0) in vec4 aPositionAlpha;  // xyz camera-relative, w alpha

layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = projection * (view * vec4(viewPos.xyz + aPositionAlpha.xyz, 1.0));
    float dist = length(aPositionAlpha.xyz);
    float fog = clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0);
    vColor = vec4(mix(fogColor.rgb, push.color.rgb, fog), aPositionAlpha.w);
    gl_PointSize = max(clamp(dist * push.size.x + push.size.y, 0.0, 1.0) * push.size.z, 1.0);
}
