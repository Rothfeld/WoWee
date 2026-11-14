#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(location=0) in Vertex { vec4 color; vec2 uv; } vertex;
layout(location=0) out vec4 outputColor;
void main() { outputColor = vertex.color * texture(image, vertex.uv); }
