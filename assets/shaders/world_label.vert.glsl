#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
layout(push_constant) uniform Transform { vec2 scale; vec2 translate; } transform;
layout(location=0) out Vertex { vec4 color; vec2 uv; vec2 screenUV; } vertex;
void main() {
    vec2 ndc = position * transform.scale + transform.translate;
    vertex.color = color;
    vertex.uv = uv;
    vertex.screenUV = ndc * 0.5 + 0.5;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
