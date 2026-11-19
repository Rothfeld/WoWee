#version 450

layout(set = 1, binding = 0) uniform sampler2D uFlake;

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 t = texture(uFlake, gl_PointCoord);
    outColor = vec4(vColor.rgb * t.rgb, vColor.a * t.a);
}
