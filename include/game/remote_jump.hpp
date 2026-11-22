#pragma once

#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

namespace wowee::game {

inline float remoteJumpHorizontalSpeed(const MovementInfo& info, bool retainedMomentum,
                                       bool isKnockBack = false) {
    // Native stationary water jumps carry the swim speed in jumpXYSpeed even
    // though their X/Y never change. Direction flags establish travel at
    // takeoff; later packets may release those flags while retaining momentum.
    // Knockbacks (MSG_MOVE_KNOCK_BACK) always impart horizontal velocity even
    // when the victim was standing still with no directional flags set.
    const bool directional = info.hasFlag(MovementFlags::FORWARD) ||
        info.hasFlag(MovementFlags::BACKWARD) ||
        info.hasFlag(MovementFlags::STRAFE_LEFT) ||
        info.hasFlag(MovementFlags::STRAFE_RIGHT);
    return directional || retainedMomentum || isKnockBack ? info.jumpXYSpeed : 0.0f;
}

/// Trailing knockback trajectory vector appended after MovementInfo in
/// MSG_MOVE_KNOCK_BACK (0x0F1) (CMaNGOS-TBC MovementHandler.cpp:580-586):
///   float cosAngle, float sinAngle, float xySpeed, float zSpeed
struct KnockBackTrailingVector {
    float cosAngle = 0.0f;
    float sinAngle = 0.0f;
    float xySpeed = 0.0f;
    float zSpeed = 0.0f;
};

inline std::optional<KnockBackTrailingVector> parseKnockBackTrailingBytes(const uint8_t* data, size_t size) {
    if (!data || size < 16) return std::nullopt;
    KnockBackTrailingVector v;
    std::memcpy(&v.cosAngle, data + 0, 4);
    std::memcpy(&v.sinAngle, data + 4, 4);
    std::memcpy(&v.xySpeed,  data + 8, 4);
    std::memcpy(&v.zSpeed,   data + 12, 4);
    if (!std::isfinite(v.cosAngle) || !std::isfinite(v.sinAngle) ||
        !std::isfinite(v.xySpeed) || !std::isfinite(v.zSpeed)) {
        return std::nullopt;
    }
    return v;
}

inline std::optional<KnockBackTrailingVector> parseKnockBackTrailing(network::Packet& packet) {
    if (!packet.hasRemaining(16)) return std::nullopt;
    const size_t pos = packet.getReadPos();
    const auto parsed = parseKnockBackTrailingBytes(packet.getData().data() + pos, packet.getRemainingSize());
    if (parsed) packet.setReadPos(pos + 16);
    return parsed;
}

} // namespace wowee::game
