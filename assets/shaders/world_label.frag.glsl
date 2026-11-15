#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(set=1,binding=0) uniform sampler2D sceneDepth;
layout(location=0) in Vertex { vec4 color; vec2 uv; vec2 screenUV; } vertex;
layout(location=0) out vec4 outputColor;
void main() {
    float depth = texture(sceneDepth, vertex.screenUV).r;
    if (gl_FragCoord.z > depth) discard;
    outputColor = vertex.color * texture(image, vertex.uv);
}
