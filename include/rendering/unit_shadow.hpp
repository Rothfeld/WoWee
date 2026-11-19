#pragma once

#include <glm/glm.hpp>

namespace wowee::rendering {

/// The ground blob shadow 2.4.3 draws under every unit:
/// `Textures\ShadowBlob.blp` (32x32, grey 160 in the middle fading to white
/// at the rim) multiplied onto the ground under the unit's position.
///
/// The quad is the unit's CreatureModelData GeoBox seen from above, at the
/// unit's scale, centred on the unit's position (not on the box's centre)
/// and turned with its facing. Fitted on 74 sweep cases where the ground
/// around the shadow was visible, the native shadow measured 0.98 of the box
/// extents (median); Riding Nether Ray 21152 (box x -4.53..1.99, y
/// -2.47..2.34) sat centred on the ray, not 1.27 yd behind it, and the
/// ground under the middle was 0.63 of its plate brightness, 160/255.
/// `_experiments/20261119T082500Z.orient_shadow/` (blob_fit.py,
/// blob_batch.txt) in the workspace.
///
/// It is drawn on the ground no further than `reach` below the unit's
/// position (and as far above): the GeoBox height times scale divided by
/// sqrt(2). Native 2.4.3, creature 2852 tempspawned hovering h yards over the
/// flat Thousand Needles floor (hoverblob.py there), the shadow at full
/// strength until it is cut away pixel by pixel as the ground leaves the
/// range:
///   Riding Nether Ray 21152 (box height 4.92): h 3 full, 3.5 a trace, 4 none
///   the same at object scale 0.5 (2.46): h 1.5 full, 1.75 half, 2 none
///   Hammerhead shark 12195 (2.37 x 0.85): h 1.25 full, 1.5 a trace, 1.75 none
///   Ressan's fel bat 9750 (5.93 x 0.3): h 1.25 partly, 1.5 none
/// The x-y extents play no part (the shark's footprint is the widest, its
/// cut the lowest); only the box height does. The brackets above put the cut
/// between 0.70 and 0.73 of the box height (0.25 yd height steps, ground
/// flat to about 0.1 yd). 0.707 = 1/sqrt(2) is a value chosen inside that
/// bracket, a fitted constant: neither the data nor the game files give it.
/// Narrowing it needs finer native height steps.
inline constexpr float kUnitShadowReachPerBoxHeight = 0.70710678f;

struct UnitShadowBlob {
    glm::vec3 position{0.0f};     // the unit's position (render space)
    glm::vec2 axisX{1.0f, 0.0f};  // the unit's facing, unit length
    glm::vec2 halfExtent{0.0f};   // half the GeoBox width (x) and depth (y), scaled
    float reach = 0.0f;           // how far below and above the unit the ground takes it
};

} // namespace wowee::rendering
