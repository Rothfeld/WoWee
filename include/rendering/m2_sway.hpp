#pragma once

#include <algorithm>

namespace wowee::rendering {

/// What the M2 vertex shader adds to a model's authored pose: the player
/// brush through ground clutter, and the cloth sway of banners.
struct M2Sway {
    /// What the vertex shader switches on: -1 sky, 0 none, 2 ground clutter,
    /// 3 hanging cloth, 4 cloth on a planted pole.
    int mode = 0;
    /// The height the bend is normalised against, in model space.
    float refHeight = 20.0f;
    /// Cloth throw.
    float amp = 1.0f;
    /// The model's own height, for the brush a player pushes through it with.
    float plantHeight = 0.0f;
};

/// The sway a model gets from what it is and how big it is.
///
/// `boundMinZ`/`boundMaxZ` are the model's own bounds. Height is measured from
/// the model's base rather than from its origin: a few detail doodads sit with
/// geometry below z=0.
/// `standingCloth` says the cloth is carried on a pole planted in the ground
/// rather than hung from a bar. The two are the same wind with the weight
/// turned end for end, and nothing in the bounds tells them apart - both reach
/// z=0 - so the caller decides from the geometry and passes the answer here.
inline M2Sway m2SwayFor(bool sky, bool hangingCloth, bool cutoutFoliage, bool groundDetail,
                        float boundMinZ, float boundMaxZ, bool standingCloth = false) {
    M2Sway out;
    // The model's own height, whatever it is made of. It used to be filled in
    // only on the paths that sway, so anything else reached the shader with
    // zero and ModelHeight came out as a flat 1.0 - which is no use to a
    // fragment that wants to know how far up the model it is. A fire is not
    // foliage and still has a top.
    out.plantHeight = std::max(boundMaxZ - std::min(boundMinZ, 0.0f), 0.05f);
    if (sky) {
        out.mode = -1;
        return out;
    }
    if (hangingCloth) {
        // Held at one end and free at the other, so the shader is given the
        // top and the span rather than a height to normalise against - which
        // end is held is the mode. The throw is a twentieth of the cloth's own
        // drop: a banner indoors breathes, it does not flap.
        const float span = std::max(boundMaxZ - boundMinZ, 0.05f);
        out.mode = standingCloth ? 4 : 3;
        out.refHeight = boundMaxZ;
        out.plantHeight = span;
        out.amp = span * 0.05f;
        return out;
    }
    // Only ground clutter: a placed doodad keeps its authored pose, as in 2.4.3
    // (see disableAnimation in m2_model_classifier.cpp).
    out.mode = cutoutFoliage && groundDetail ? 2 : 0;
    return out;
}

}  // namespace wowee::rendering
