#version 450

layout(push_constant) uniform Push {
    vec2 origin;
    vec2 axisX;
    vec2 axisY;
} push;

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = aUV;
    vec2 pos = push.origin + aPos.x * push.axisX + aPos.y * push.axisY;
    pos = pos * 2.0 - 1.0;
    gl_Position = vec4(pos, 0.0, 1.0);
}
