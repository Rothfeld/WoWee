#version 450
layout(push_constant) uniform Push { mat4 mvp; } push;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 uv;
void main() {
    uv = aUV;
    gl_Position = push.mvp * vec4(aPos, 1.0);
}
