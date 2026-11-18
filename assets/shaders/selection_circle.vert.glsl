#version 450
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 square;          // centre x, y; half side
    vec4 axisAndHeights;  // right axis x, y; lowest and highest z drawn
} push;
layout(location = 0) in vec3 aPos;
layout(location = 0) out vec3 worldPos;
void main() {
    worldPos = aPos;
    gl_Position = push.mvp * vec4(aPos, 1.0);
}
