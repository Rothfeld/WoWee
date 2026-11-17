#pragma once

#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include <cassert>
#include <stdexcept>

namespace wowee::game {

// CMaNGOS GO_FLAG_INTERACT_COND is lifted by the per-player ACTIVATE bit.
// Quest chests and questgiver objects use this instead of a client quest list.
inline bool gameObjectInteractionEnabled(uint32_t flags, uint32_t dynamicFlags) {
    constexpr uint32_t inUse = 0x01, conditional = 0x04, noInteract = 0x10;
    if (flags & (inUse | noInteract)) return false;
    return !(flags & conditional) || (dynamicFlags & 0x01u);
}

inline bool gameObjectInteractionEnabled(const GameObject& object) {
    const auto flags = fieldIndex(UF::GAMEOBJECT_FLAGS);
    const auto dynamic = fieldIndex(UF::GAMEOBJECT_DYNAMIC);
    if (flags == 0xFFFF || dynamic == 0xFFFF)
        throw std::runtime_error("update_fields.json lacks game-object interaction fields; deploy current expansion metadata");
    return gameObjectInteractionEnabled(object.getField(flags), object.getField(dynamic));
}

/// The name tooltip a hovered object shows.
enum class ObjectNameTooltip { None, Floating, Default };

/// GAMEOBJECT_TYPE_GENERIC (5) data0 floatingTooltip, data1 highlight
/// (CMaNGOS GameObject.h _generic). floatingTooltip labels the object at the
/// cursor; highlight alone (Guild Master & Tabards, 183865) shows its name in
/// the default bottom-right tooltip; neither is unlabelled scenery. Chairs
/// (7) keep only their cursor. Every other type shows its name.
inline ObjectNameTooltip objectNameTooltip(uint32_t type, uint32_t data0, uint32_t data1) {
    if (type == 7) return ObjectNameTooltip::None;
    if (type != 5) return ObjectNameTooltip::Default;
    if (data0 != 0) return ObjectNameTooltip::Floating;
    return data1 != 0 ? ObjectNameTooltip::Default : ObjectNameTooltip::None;
}

} // namespace wowee::game
