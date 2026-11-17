#pragma once

/// WMO group liquid (MLIQ) rules taken from the file format and LiquidType.dbc.
///
/// MLIQ tile byte: low nibble = legacy liquid type, 0x0F = this tile holds no
/// liquid. Bits 0x40/0x80 are fishable/shared flags and do not affect
/// coverage. Every TBC 2.4.3 WMO group with MLIQ (333 groups) also has MOGP
/// flag 0x1000, leaves MOGP groupLiquid at 0 and lacks MOHD flag 0x4; its
/// wet tiles all carry one nibble, whose low two bits name the basic type:
///   0 water  (kl_blackfathom_instance, md_junglecave, nibble 4 or 0)
///   1 ocean
///   2 magma  (az_blackrock, kl_orgrimmarlavadungeon, nibble 6 or 2)
///   3 slime  (ld_stratholme, stratholme_raid, nibble 7 or 3)
/// Group flag 0x80000 marks basic water as ocean.
/// TBC LiquidType.dbc IDs: 1 Water, 2 Ocean, 3 Magma, 4 Slime.

#include <cassert>
#include <cstdint>
#include <optional>
#include <span>

namespace wowee::pipeline {

constexpr uint8_t kWmoLiquidTileEmpty = 0x0F;
constexpr uint32_t kWmoRootLiquidTypeIsDbcId = 0x4;   // MOHD flag
constexpr uint32_t kWmoGroupHasLiquid = 0x1000;       // MOGP flag
constexpr uint32_t kWmoGroupWaterIsOcean = 0x80000;   // MOGP flag

constexpr bool wmoLiquidTileWet(uint8_t tile) {
    return (tile & 0x0F) != kWmoLiquidTileEmpty;
}

/// Legacy nibble of the group's wet tiles; nullopt when no tile is wet.
inline std::optional<uint8_t> wmoLiquidTileNibble(std::span<const uint8_t> tiles) {
    std::optional<uint8_t> nibble;
    for (uint8_t tile : tiles) {
        if (!wmoLiquidTileWet(tile)) continue;
        if (!nibble) nibble = tile & 0x0F;
        assert(*nibble == (tile & 0x0F) && "MLIQ group mixes liquid types across tiles");
    }
    return nibble;
}

/// LiquidType.dbc ID of a WMO group's liquid; 0 when the group has no wet tile.
/// Files with MOHD flag 0x4 name the LiquidType ID in MOGP groupLiquid.
/// Otherwise a nonzero groupLiquid follows the legacy rule (15 = take it from
/// the tiles, else groupLiquid + 1) and a zero groupLiquid takes it from the
/// tiles, which is how every TBC group is authored. Legacy entries below 21
/// collapse onto the four basic types.
inline uint32_t resolveWmoLiquidType(uint32_t rootFlags, uint32_t groupFlags,
                                     uint32_t groupLiquid, std::span<const uint8_t> tiles) {
    uint32_t entry = 0;
    if (rootFlags & kWmoRootLiquidTypeIsDbcId) {
        entry = groupLiquid;
    } else if (groupLiquid != 0 && groupLiquid != 15) {
        entry = groupLiquid + 1;
    } else if (const auto nibble = wmoLiquidTileNibble(tiles)) {
        entry = *nibble + 1u;
    }
    if (entry == 0 || entry >= 21) return entry;
    switch ((entry - 1) & 3) {
        case 0: return (groupFlags & kWmoGroupWaterIsOcean) ? 2 : 1;
        case 1: return 2;
        case 2: return 3;
        default: return 4;
    }
}

/// Water and ocean vertices carry a depth byte; magma and slime vertices
/// carry texture coordinates in the same bytes.
constexpr bool wmoLiquidHasDepth(uint32_t liquidType) {
    return liquidType == 1 || liquidType == 2;
}

}  // namespace wowee::pipeline
