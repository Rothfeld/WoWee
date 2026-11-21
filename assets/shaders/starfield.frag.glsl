#version 450

layout(location = 0) in float vBrightness;
layout(location = 1) in vec3 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Radius across the sprite: 0 at the centre, 1 at the edge.
    float r = length(gl_PointCoord - vec2(0.5)) * 2.0;
    if (r > 1.0) discard;

    // Crisp anti-aliased point-star core + subtle halo matching Native TBC 2.4.3.
    float core = exp(-r * r * 4.5);
    float halo = 0.18 * exp(-r * r * 1.8);
    float profile = clamp(core + halo, 0.0, 1.0);

    // Blending is SRC_ALPHA, ONE, so the destination receives rgb * a and
    // brightness must therefore appear in exactly one of the two. It used to be
    // in both, which squared it: a star at the faint end of the range landed at
    // a tenth of its intended value and disappeared, leaving only the bright
    // ones - each of which was also drawn at the largest point size. Few, fat
    // and soft is the look that reads as bad focus.
    outColor = vec4(vColor * vBrightness * profile, 1.0);
}
