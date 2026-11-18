#pragma once

/// Which LightParams row and which channel a light band belongs to.
///
/// LightIntBand and LightFloatBand hold one row per channel per LightParams
/// row: eighteen colour channels and six float channels. A row's own id is the
/// block index, and both the ids and the LightParams ids they address start at
/// one.
///
/// The mapping was `id / channels` and `id % channels`, which is off by one in
/// both terms: band 1 - the first colour channel of LightParams 1 - came out
/// as LightParams 0, channel 1. Every channel landed on the wrong row and in
/// the wrong slot, and LightParams 0 does not exist, so the first seventeen
/// channels of every params row were dropped entirely.
///
/// The counts check out against the files: LightIntBand has 15300 rows and
/// ids running to 16506, which is 850 rows of 18 and 917 x 18; LightFloatBand
/// has 5100 rows and ids to 5502, which is 850 x 6 and 917 x 6.

#include <algorithm>
#include <array>
#include <cstdint>

namespace wowee::rendering {

/// Colour channels per LightParams row in LightIntBand.
inline constexpr uint32_t LIGHT_INT_CHANNELS = 18;

/// Float channels per LightParams row in LightFloatBand.
inline constexpr uint32_t LIGHT_FLOAT_CHANNELS = 6;

struct LightBandSlot {
    uint32_t lightParamsId = 0;
    uint32_t channel = 0;
    bool valid = false;
};

/// Split a band's block index into the row it belongs to and the channel it is.
///
/// `blockIndex` is the band row's own id, as stored. A zero is not a band:
/// the ids are one-based.
inline LightBandSlot lightBandSlot(uint32_t blockIndex, uint32_t channelsPerRow) {
    LightBandSlot out;
    if (blockIndex == 0 || channelsPerRow == 0) return out;
    const uint32_t zeroBased = blockIndex - 1;
    out.lightParamsId = zeroBased / channelsPerRow + 1;
    out.channel = zeroBased % channelsPerRow;
    out.valid = true;
    return out;
}

/// Put a band's keys in ascending time order, values alongside.
///
/// Sampling assumes ascending times. 239 LightIntBand and 41 LightFloatBand
/// rows in 2.4.3 list a key out of order: LightParams 8, the storm set of
/// Ruins of Ahn'Qiraj's default light, has its 00:00 key last (720, 1440, 0),
/// and read in file order that band held the 06:00 colour all afternoon.
/// 2.4.3 draws that storm with the ground colours of the clear set 394, whose
/// keys are the same values in ascending order.
template <typename Band, typename Value>
void sortBandKeys(Band& band, Value (&values)[16]) {
    std::array<uint8_t, 16> order{};
    for (uint8_t k = 0; k < band.numKeyframes; ++k) order[k] = k;
    std::stable_sort(order.begin(), order.begin() + band.numKeyframes,
                     [&](uint8_t a, uint8_t b) { return band.times[a] < band.times[b]; });
    uint16_t times[16];
    Value sorted[16];
    for (uint8_t k = 0; k < band.numKeyframes; ++k) {
        times[k] = band.times[order[k]];
        sorted[k] = values[order[k]];
    }
    for (uint8_t k = 0; k < band.numKeyframes; ++k) {
        band.times[k] = times[k];
        values[k] = sorted[k];
    }
}

}  // namespace wowee::rendering
