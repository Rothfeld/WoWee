#version 450

layout(set = 0, binding = 0) uniform sampler2D uTileTexture;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(uTileTexture, vec2(TexCoord.y, TexCoord.x));
    if (c.a < 0.5) discard;
    outColor = vec4(c.rgb / c.a, 1.0);
}
