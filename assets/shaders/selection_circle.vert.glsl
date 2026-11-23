#version 450
layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};
layout(push_constant) uniform Push {
    vec4 color;
    vec4 square;          // centre x, y; half side; u sign
    vec4 axisAndHeights;  // right axis x, y; lowest and highest z drawn
} push;
layout(location = 0) in vec3 aPos;
layout(location = 0) out vec3 worldPos;
void main() {
    worldPos = aPos;
    gl_Position = projection * (view * vec4(aPos, 1.0));
}
