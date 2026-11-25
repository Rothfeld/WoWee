#version 450
invariant gl_Position;
// Ground blob shadow of one unit (rendering/unit_shadow.hpp), drawn on the
// world triangles under it. Applies push.model * vec4(aPos, 1.0) and
// projection * (view * worldPos) with invariant gl_Position, matching
// terrain.vert.glsl and wmo.vert.glsl bit-for-bit at large world coords.
layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};
layout(push_constant) uniform Push {
    mat4 model;
    vec4 centreAxis;    // unit position x, y; facing x, y
    vec4 halfAndZ;      // half extent along facing, across; lowest and highest z drawn
    vec4 cameraFog;     // camera x, y, z; unused
    vec4 fog;           // fog start, end; unused
} push;
layout(location = 0) in vec3 aPos;
layout(location = 0) out vec3 worldPos;
void main() {
    vec4 worldPos4 = push.model * vec4(aPos, 1.0);
    worldPos = worldPos4.xyz;
    gl_Position = projection * (view * worldPos4);
}
