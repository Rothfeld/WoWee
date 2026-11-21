#pragma once

#include <cstdint>

namespace wowee::core {

/// Valid option counts for a target (race, sex) from CharSections.dbc,
/// CharHairGeosets.dbc, and CharacterFacialHairStyles.dbc.
struct PlayerIllusionRaceLimits {
    uint8_t skinCount = 0;
    uint8_t faceCount = 0;
    uint8_t hairStyleCount = 0;
    uint8_t hairColorCount = 0;
    uint8_t facialHairCount = 0;
};

/// Minimal subset of a CreatureDisplayInfoExtra.dbc row needed to decide how a
/// player morphed to that display should be rendered.
struct HumanoidDisplayExtraInfo {
    uint8_t raceId = 0;
    uint8_t sexId = 0;
    uint8_t skinId = 0;
    uint8_t faceId = 0;
    uint8_t hairStyleId = 0;
    uint8_t hairColorId = 0;
    uint8_t facialHairId = 0;
    bool canEquip = false;
};

/// Result of resolving a player's humanoid morph display against
/// CreatureDisplayInfoExtra.dbc (col 19, CanEquip).
struct PlayerHumanoidMorph {
    /// True when CreatureDisplayInfoExtra.CanEquip is set: the morphed player
    /// wears their live equipped armor, weapons, and enchants on the target
    /// race/sex model rather than the NPC's baked appearance.
    bool isEquipablePlayerIllusion = false;
    /// True when the illusion row has all-zero preset appearance fields so the
    /// player's own skin/face/hair/facial-hair bytes are mapped onto the target
    /// race/sex.
    bool usePlayerAppearance = false;
    uint8_t targetRace = 0;
    uint8_t targetSex = 0;
    /// Packed PLAYER_BYTES: skin | (face << 8) | (hairStyle << 16) | (hairColor << 24).
    uint32_t appearanceBytes = 0;
    uint8_t facialHair = 0;
};

inline uint8_t wrapAppearanceIndex(uint8_t value, uint8_t count) {
    if (count == 0) return 0;
    return static_cast<uint8_t>(value % count);
}

inline uint32_t packAppearanceBytes(uint8_t skin, uint8_t face,
                                    uint8_t hairStyle, uint8_t hairColor) {
    return static_cast<uint32_t>(skin)
         | (static_cast<uint32_t>(face) << 8)
         | (static_cast<uint32_t>(hairStyle) << 16)
         | (static_cast<uint32_t>(hairColor) << 24);
}

/// Data-driven TBC 2.4.3 rule for humanoid player morphs:
/// - CreatureDisplayInfoExtra.CanEquip (col 19, bit 0x1) marks displays that
///   composite a player's live equipment (e.g. Old Hillsbrad Human Illusion
///   19723/19724, Orb of Deception 20316..20323, racial illusions 20578..20585,
///   Broken/Fel Orc illusions 21105/21266/21267).
/// - When CanEquip is set and all five appearance columns (skin/face/hair/
///   hairColor/facialHair) are zero, the player's own PLAYER_BYTES and
///   PLAYER_BYTES_2 are mapped into the target race/sex's valid option ranges.
/// - When CanEquip is set with non-zero preset appearance (e.g. Blood Elf
///   Illusion 20368..20371), the preset appearance is kept while still
///   equipping the player's live gear.
/// - When CanEquip is false, the display is an ordinary humanoid NPC morph and
///   uses the NPC's own appearance and baked equipment.
inline PlayerHumanoidMorph resolvePlayerHumanoidMorph(
    const HumanoidDisplayExtraInfo& extra,
    uint32_t playerAppearanceBytes,
    uint8_t playerFacialHair,
    const PlayerIllusionRaceLimits& limits) {
    PlayerHumanoidMorph out;
    out.targetRace = extra.raceId;
    out.targetSex = extra.sexId;
    out.isEquipablePlayerIllusion = extra.canEquip;

    const bool hasPresetAppearance =
        (extra.skinId | extra.faceId | extra.hairStyleId |
         extra.hairColorId | extra.facialHairId) != 0;

    if (extra.canEquip && !hasPresetAppearance) {
        out.usePlayerAppearance = true;
        const uint8_t pSkin      = static_cast<uint8_t>( playerAppearanceBytes        & 0xFFu);
        const uint8_t pFace      = static_cast<uint8_t>((playerAppearanceBytes >> 8)  & 0xFFu);
        const uint8_t pHairStyle = static_cast<uint8_t>((playerAppearanceBytes >> 16) & 0xFFu);
        const uint8_t pHairColor = static_cast<uint8_t>((playerAppearanceBytes >> 24) & 0xFFu);

        const uint8_t skin      = wrapAppearanceIndex(pSkin, limits.skinCount);
        const uint8_t face      = wrapAppearanceIndex(pFace, limits.faceCount);
        const uint8_t hairStyle = wrapAppearanceIndex(pHairStyle, limits.hairStyleCount);
        const uint8_t hairColor = wrapAppearanceIndex(pHairColor, limits.hairColorCount);
        out.appearanceBytes = packAppearanceBytes(skin, face, hairStyle, hairColor);
        out.facialHair = wrapAppearanceIndex(playerFacialHair, limits.facialHairCount);
    } else {
        out.usePlayerAppearance = false;
        out.appearanceBytes = packAppearanceBytes(
            extra.skinId, extra.faceId, extra.hairStyleId, extra.hairColorId);
        out.facialHair = extra.facialHairId;
    }
    return out;
}

} // namespace wowee::core
