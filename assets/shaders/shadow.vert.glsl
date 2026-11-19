#version 450

// The caster side of the shadow map, for terrain, buildings and doodads.
layout(push_constant) uniform Push {
    mat4 lightSpaceModel;   // light-space * model, multiplied on the CPU
    ivec4 flags;            // x useTexture, y alphaTest
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aBoneWeights;
layout(location = 3) in vec4 aBoneIndicesF;

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = aTexCoord;
    gl_Position = push.lightSpaceModel * vec4(aPos, 1.0);
}
