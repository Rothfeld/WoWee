#version 450
// Ground blob shadow of one unit (rendering/unit_shadow.hpp), drawn on the
// world triangles under it. Uses the same PerFrame view/projection order as
// terrain.vert.glsl and wmo.vert.glsl so depth matches at large world coords.
layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};
layout(push_constant) uniform Push {
    vec4 centreAxis;    // unit position x, y; facing x, y
    vec4 halfAndZ;      // half extent along facing, across; lowest and highest z drawn
    vec4 cameraFog;     // camera x, y, z; unused
    vec4 fog;           // fog start, fog end; unused
} push;
layout(location = 0) in vec3 aPos;
layout(location = 0) out vec3 worldPos;
void main() {
    worldPos = aPos;
    gl_Position = projection * (view * vec4(aPos, 1.0));
}
