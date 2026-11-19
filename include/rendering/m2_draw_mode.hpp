#pragma once

#include <algorithm>

namespace wowee::rendering {

/// What the M2 vertex shader is told about a model as a whole.
struct M2DrawMode {
    /// -1 sky, 0 placed model, 2 ground clutter (lit as the ground under it).
    int mode = 0;
    /// The model's own height, for fragments that want to know how far up
    /// the model they sit (a flame card fading out over its own tip).
    float modelHeight = 0.0f;
};

/// The draw mode of a model, from whether it is the sky or ground clutter and
/// from its own bounds. Height is measured from the model's base rather than
/// from its origin: a few detail doodads sit with geometry below z=0.
///
/// Nothing here moves a vertex. 2.4.3 moves a placed model, a banner and a
/// tuft of ground clutter only by its own keyed bones.
inline M2DrawMode m2DrawModeFor(bool sky, bool groundDetail, float boundMinZ, float boundMaxZ) {
    M2DrawMode out;
    out.modelHeight = std::max(boundMaxZ - std::min(boundMinZ, 0.0f), 0.05f);
    out.mode = sky ? -1 : (groundDetail ? 2 : 0);
    return out;
}

}  // namespace wowee::rendering
