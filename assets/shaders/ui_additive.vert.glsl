#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
layout(push_constant) uniform Transform { vec2 scale; vec2 translate; } transform;
layout(location=0) out Vertex { vec4 color; vec2 uv; } vertex;
void main() {
    vertex.color = color;
    vertex.uv = uv;
    gl_Position = vec4(position * transform.scale + transform.translate, 0.0, 1.0);
}
