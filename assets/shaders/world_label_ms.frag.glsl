#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(set=1,binding=0) uniform sampler2DMS sceneDepth;
layout(location=0) in Vertex { vec4 color; vec2 uv; vec2 screenUV; } vertex;
layout(location=0) out vec4 outputColor;
void main() {
    ivec2 pixel = clamp(ivec2(vertex.screenUV * textureSize(sceneDepth)), ivec2(0), textureSize(sceneDepth) - 1);
    float depth = 1.0;
    for (int sampleIndex = 0; sampleIndex < textureSamples(sceneDepth); ++sampleIndex)
        depth = min(depth, texelFetch(sceneDepth, pixel, sampleIndex).r);
    if (gl_FragCoord.z > depth) discard;
    outputColor = vertex.color * texture(image, vertex.uv);
}
