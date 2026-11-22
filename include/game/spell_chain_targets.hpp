#pragma once

#include "network/packet.hpp"
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace wowee::game {

/// Decoded SMSG_SPELL_UPDATE_CHAIN_TARGETS (0x330) payload.
/// Wire format (CMaNGOS-TBC Spell.cpp:4744):
///   uint64 casterGuid
///   uint32 spellId
///   uint32 targetCount (capped at 32 by server; validated against remaining bytes)
///   uint64 targetGuids[targetCount] (primary channel target is first)
struct SpellUpdateChainTargetsData {
    uint64_t casterGuid = 0;
    uint32_t spellId = 0;
    std::vector<uint64_t> targets;
};

inline std::optional<SpellUpdateChainTargetsData> parseSpellUpdateChainTargetsBytes(
    const uint8_t* data, size_t size) {
    if (!data || size < 16) return std::nullopt;
    SpellUpdateChainTargetsData out;
    std::memcpy(&out.casterGuid, data + 0, 8);
    std::memcpy(&out.spellId, data + 8, 4);
    uint32_t count = 0;
    std::memcpy(&count, data + 12, 4);
    constexpr uint32_t kMaxChainTargets = 64;
    if (count > kMaxChainTargets || size - 16 < static_cast<size_t>(count) * 8u) {
        return std::nullopt;
    }
    out.targets.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t guid = 0;
        std::memcpy(&guid, data + 16 + static_cast<size_t>(i) * 8u, 8);
        out.targets.push_back(guid);
    }
    return out;
}

inline std::optional<SpellUpdateChainTargetsData> parseSpellUpdateChainTargets(network::Packet& packet) {
    const size_t pos = packet.getReadPos();
    const size_t rem = packet.getRemainingSize();
    auto parsed = parseSpellUpdateChainTargetsBytes(
        rem > 0 ? packet.getData().data() + pos : nullptr, rem);
    if (parsed) {
        packet.setReadPos(pos + 16 + parsed->targets.size() * 8u);
    }
    return parsed;
}

} // namespace wowee::game
