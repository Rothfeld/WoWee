#version 450
layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) in Vertex { vec4 color; vec2 uv; } vertex;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 sampled = texture(image, vertex.uv);
    float gray = dot(sampled.rgb, vec3(0.299, 0.587, 0.114));
    outColor = vertex.color * vec4(vec3(gray), sampled.a);
}
