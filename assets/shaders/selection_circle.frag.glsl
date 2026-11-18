#version 450
// The selection texture projected straight down over the square centre +-
// half side: a point takes the texture coordinates of the square point above
// or below it, on a floor, a slope or a wall alike.
layout(set = 0, binding = 0) uniform sampler2D selectionTexture;
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 square;          // centre x, y; half side
    vec4 axisAndHeights;  // right axis x, y; lowest and highest z drawn
} push;
layout(location = 0) in vec3 worldPos;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 right = push.axisAndHeights.xy;
    vec2 forward = vec2(-right.y, right.x);
    vec2 d = worldPos.xy - push.square.xy;
    vec2 uv = vec2(0.5 + dot(d, right) / (2.0 * push.square.z),
                   0.5 - dot(d, forward) / (2.0 * push.square.z));
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) ||
        worldPos.z < push.axisAndHeights.z || worldPos.z > push.axisAndHeights.w) discard;
    outColor = texture(selectionTexture, uv) * push.color;
    if (outColor.a < 0.01) discard;
}
