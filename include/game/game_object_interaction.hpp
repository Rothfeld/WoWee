#pragma once

#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include <cassert>

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
    assert(flags != 0xFFFF && dynamic != 0xFFFF);
    return gameObjectInteractionEnabled(object.getField(flags), object.getField(dynamic));
}

} // namespace wowee::game
