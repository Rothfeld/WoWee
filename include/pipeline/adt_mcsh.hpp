#pragma once

#include <array>
#include <cstdint>

namespace wowee::pipeline {

/// Unpack a 512-byte ADT MCNK MCSH 64x64 1-bit baked shadow map into a
/// 4096-byte R8_UNORM row-major texture (row = Y in [0,63], col = X in [0,63]).
/// Bit 1 = shadowed (255), bit 0 = lit (0).
inline std::array<uint8_t, 4096> unpackMcshShadowMap(const std::array<uint8_t, 512>& shadowMap) {
    std::array<uint8_t, 4096> out{};
    for (size_t row = 0; row < 64; ++row) {
        const size_t srcRow = row * 8;
        const size_t dstRow = row * 64;
        for (size_t col = 0; col < 64; ++col) {
            const bool shadowed = ((shadowMap[srcRow + (col >> 3)] >> (col & 7)) & 1u) != 0;
            out[dstRow + col] = shadowed ? 255u : 0u;
        }
    }
    return out;
}

}  // namespace wowee::pipeline
