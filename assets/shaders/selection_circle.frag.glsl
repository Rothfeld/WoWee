#version 450
layout(set = 0, binding = 0) uniform sampler2D selectionTexture;
layout(push_constant) uniform Push { mat4 mvp; vec4 color; } push;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = texture(selectionTexture, uv) * push.color;
    if (outColor.a < 0.01) discard;
}
