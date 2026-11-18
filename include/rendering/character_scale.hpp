#pragma once

namespace wowee::rendering {

// A player character is drawn at its display's CreatureDisplayInfo scale
// times its CreatureModelData scale times OBJECT_FIELD_SCALE_X, as a creature
// is. Build 8606, pair 22 in Nagrand: a tauren male (display 59, 1.35) and a
// gnome male (display 1563, 1.15) stand and ride larger than WoWee's scale-1
// models did, a night elf (55, 1.0) the same; the horse under each grows by the
// same factor; ChrRaces' float column 7 (night elf 1.2, gnome 0.8) changes
// nothing visible on the horse. Evidence:
// _experiments/20261118T160000Z.player_scale in the workspace.
inline constexpr float characterRenderScale(float objectScale, float displayScale, float modelScale) {
    return objectScale * displayScale * modelScale;
}

} // namespace wowee::rendering
