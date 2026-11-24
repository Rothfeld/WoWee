#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

namespace wowee::rendering {

/// Parsed SpellVisualKit CharProc 15 (timed opacity fade):
/// e.g. kit 6795 Teleport Test (p0=0, p1=1, p2=1000 ms),
/// kit 6978 Coalesce (p0=1, p1=0, p2=1 s),
/// kit 7032 Materialize (p0=0.5, p1=0, p2=1 s),
/// kit 3269 Jeff Dummy 1 (p0=0.6, p1=2.0 -> 1.0, p2=500 ms).
struct KitOpacityFadeSpec {
    float startAlpha = 1.0f;
    float endAlpha = 1.0f;
    float durationSec = 0.0f;
};

inline std::optional<KitOpacityFadeSpec> parseKitOpacityFadeCharProc(float p0, float p1, float p2) {
    if (!std::isfinite(p0) || !std::isfinite(p1) || !std::isfinite(p2)) return std::nullopt;
    if (p2 <= 0.0f) return std::nullopt;
    const float durationSec = p2 > 10.0f ? (p2 / 1000.0f) : p2;
    return KitOpacityFadeSpec{
        .startAlpha = std::clamp(p0, 0.0f, 1.0f),
        .endAlpha = std::clamp(p1, 0.0f, 1.0f),
        .durationSec = durationSec,
    };
}

/// Parsed SpellVisualKit CharProc 17 (temporary visual equipment override):
/// CharParamZero = equipment slot (0 = Head), CharParamOne = Item.dbc entry ID
/// (e.g. kit 8381 Jack-o'-Lanterned! 44185 and kit 8081 Conflagration 42380:
/// slot 0, item 33292 Hallowed Helm -> ItemDisplayInfo 46514 Helm_Mask_Pumpkin).
struct KitEquipOverrideSpec {
    uint32_t slot = 0;
    uint32_t itemId = 0;
};

inline std::optional<KitEquipOverrideSpec> parseKitEquipOverrideCharProc(float p0, float p1) {
    if (!std::isfinite(p0) || !std::isfinite(p1)) return std::nullopt;
    if (p0 < 0.0f || p1 <= 0.0f || p1 >= 16777216.0f) return std::nullopt;
    return KitEquipOverrideSpec{
        .slot = static_cast<uint32_t>(p0),
        .itemId = static_cast<uint32_t>(p1),
    };
}

struct CharacterRaceGender {
    uint8_t raceId = 0;
    uint8_t genderId = 0;
};

/// Infer TBC playable race/gender IDs from a Character M2 model name
/// (e.g. "HumanMale" -> {1, 0}, "BloodElfFemale" -> {10, 1}, "ScourgeMale" -> {5, 0})
/// so SpellVisualKit CharProc 17 head overrides select the matching per-race helm cut.
inline std::optional<CharacterRaceGender> raceGenderFromCharacterModelName(std::string_view modelName) {
    std::string lower;
    lower.reserve(modelName.size());
    for (const char ch : modelName) {
        lower.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? (ch - 'A' + 'a') : ch));
    }
    uint8_t gender = 0;
    if (lower.find("female") != std::string::npos) {
        gender = 1;
    } else if (lower.find("male") != std::string::npos) {
        gender = 0;
    } else {
        return std::nullopt;
    }
    if (lower.find("bloodelf") != std::string::npos) return CharacterRaceGender{10, gender};
    if (lower.find("nightelf") != std::string::npos) return CharacterRaceGender{4, gender};
    if (lower.find("draenei") != std::string::npos) return CharacterRaceGender{11, gender};
    if (lower.find("scourge") != std::string::npos || lower.find("undead") != std::string::npos)
        return CharacterRaceGender{5, gender};
    if (lower.find("human") != std::string::npos) return CharacterRaceGender{1, gender};
    if (lower.find("orc") != std::string::npos) return CharacterRaceGender{2, gender};
    if (lower.find("dwarf") != std::string::npos) return CharacterRaceGender{3, gender};
    if (lower.find("tauren") != std::string::npos) return CharacterRaceGender{6, gender};
    if (lower.find("gnome") != std::string::npos) return CharacterRaceGender{7, gender};
    if (lower.find("troll") != std::string::npos) return CharacterRaceGender{8, gender};
    return std::nullopt;
}

/// True when a SpellVisualKit CharProc 1 (kCharProcColor) RGB multiplier should
/// also tint attached M2 spell-effect instances spawned by the kit.
/// Non-black colors (e.g. 35409 Brain Damage kit 7095 = 0x800000 on
/// arcanepower_state_chest.m2, kits 7099/7122/7653/8404) recolor neutral white
/// kit M2s, whereas pure black 0x000000 character-silhouette kits (e.g. 33810
/// Rock Shell kit 6926 on ArcaneForceShield_Dark.m2, kit 7641 Vertex Color Black)
/// darken only the unit's body while leaving the outer shield M2 bright.
inline bool kitCharProcTintsAttachedEffects(const glm::vec3& kitColor) {
    return kitColor.r > 0.0f || kitColor.g > 0.0f || kitColor.b > 0.0f;
}

/// True when starting or ending a channel of `channelVisualId` on a unit
/// shares the same SpellVisual ID as an active aura StateKit (`auraVisualId`)
/// on that same unit. In TBC 2.4.3 (verified on Tempest-Smith mutual 37112
/// Power Up, SV 8636: ChannelKit 7371 + StateKit 7373), starting or ending a
/// channel of visualId V retires/suppresses an already-active aura StateKit of
/// the same visualId V on that unit until the aura is removed and re-applied,
/// while leaving auras with different visualIds untouched.
inline constexpr bool channelVisualSuppressesAuraVisual(uint32_t channelVisualId,
                                                        uint32_t auraVisualId) {
    return channelVisualId != 0 && channelVisualId == auraVisualId;
}

/// Choose the active aura StateKit animation from `candidateAnimIds` ordered
/// newest-application-first. Dedicated action/pose StateKit animations (e.g.
/// 42696 Drunken Master kit 8158 Whirlwind 126, 36402 Drowned 132) take
/// priority over generic Stun (14) from co-applied stun helper auras (e.g.
/// 42695 kit 8163 Stun 14), while Stun (14) still beats Stand (0) when no
/// dedicated action StateKit is present.
inline uint32_t selectAuraStateAnimation(std::span<const uint32_t> candidateAnimIds,
                                         uint32_t noAnim,
                                         uint32_t standAnim,
                                         uint32_t stunAnim) {
    uint32_t fallbackAnim = noAnim;
    for (const uint32_t animId : candidateAnimIds) {
        if (animId == noAnim) continue;
        if (animId != standAnim && animId != stunAnim) return animId;
        if (fallbackAnim == noAnim) fallbackAnim = animId;
        else if (fallbackAnim == standAnim && animId == stunAnim) fallbackAnim = animId;
    }
    return fallbackAnim;
}

/// True when an active aura StateKit animation holds a dedicated full-body loop
/// (neither Stand nor generic Stun) that must not be interrupted by periodic
/// self-proc ImpactKit one-shots (e.g. 42696 Drunken Master Whirlwind 126 vs
/// 42695 ImpactKit 6766 Knockdown 121 every 500 ms).
inline bool auraStateSuppressesOneShotKitAnim(uint32_t auraStateAnim,
                                              uint32_t noAnim,
                                              uint32_t standAnim,
                                              uint32_t stunAnim) {
    return auraStateAnim != noAnim &&
           auraStateAnim != standAnim &&
           auraStateAnim != stunAnim;
}

/// True when a TBC 2.4.3 Spell.dbc EffectImplicitTarget value designates a
/// destination ground/world coordinate (TARGET_TYPE_LOCATION_DEST / UNIT_DEST)
/// rather than a caster/source-centered or unit-only target (such as 22 =
/// TARGET_CASTER_COORDINATES + 15 = TARGET_ALL_ENEMY_IN_AREA on 42052 Volcanic Geyser).
inline constexpr bool spellTargetIsDestinationCoordinate(uint32_t implicitTarget) {
    switch (implicitTarget) {
        case 16: // TARGET_ALL_ENEMY_IN_AREA_INSTANT
        case 17: // TARGET_TABLE_X_Y_Z_COORDINATES
        case 18: // TARGET_EFFECT_SELECT (dest at caster, e.g. Flames of Shahram / Consecration)
        case 28: // TARGET_ALL_ENEMY_IN_AREA_CHANNELED
        case 30: // TARGET_ALL_FRIENDLY_UNITS_AROUND_CASTER (dest)
        case 31: // TARGET_ALL_FRIENDLY_UNITS_IN_AREA
        case 32: // TARGET_MINION_COORDINATES (e.g. 4929 Summon Burning Exile)
        case 39: // TARGET_CENTER_OF_CASTER_FISHING
        case 41: // TARGET_TOTEM_EARTH
        case 42: // TARGET_TOTEM_WATER
        case 43: // TARGET_TOTEM_AIR
        case 44: // TARGET_TOTEM_FIRE
        case 46: // TARGET_DYNAMIC_OBJECT_COORDINATES
        case 47: // TARGET_DYNAMIC_OBJECT_FRONT
        case 48: // TARGET_DYNAMIC_OBJECT_BEHIND
        case 49: // TARGET_DYNAMIC_OBJECT_LEFT_SIDE
        case 50: // TARGET_DYNAMIC_OBJECT_RIGHT_SIDE
        case 51: // TARGET_AREA_EFFECT_CUSTOM
        case 52: // TARGET_CURRENT_ENEMY_COORDINATES_AREA
        case 53: // TARGET_CURRENT_ENEMY_COORDINATES (e.g. 30511 Burning Abyssal, 41276 Meteor)
        case 55: // TARGET_FRONT_LEAP
        case 62: // TARGET_TEST_TARGETS
        case 63: // TARGET_DUELVSPLAYER_COORDINATES
        case 64: // TARGET_INFRONT_OF_VICTIM
        case 65: // TARGET_BEHIND_VICTIM
        case 66: // TARGET_RIGHT_FROM_VICTIM
        case 67: // TARGET_LEFT_FROM_VICTIM
        case 72: // TARGET_RANDOM_NEARBY_LOC (e.g. 40118 Volcanic Geyser missile)
        case 73: // TARGET_RANDOM_CIRCUMFERENCE_POINT (e.g. 36723 Phoenix)
        case 74: // TARGET_RANDOM_POINT_NEAR_TARGET
        case 75: // TARGET_RANDOM_POINT_NEAR_TARGET_PATH
        case 76: // TARGET_DEST_TARGET_ANY
        case 78: // TARGET_DEST_CASTER_NORTH
        case 79: // TARGET_DEST_CASTER_SOUTH
        case 80: // TARGET_DEST_CASTER_RANDOM
        case 81: // TARGET_DEST_CASTER_BACK
        case 82: // TARGET_DEST_CASTER_LEFT
        case 83: // TARGET_DEST_CASTER_RIGHT
        case 84: // TARGET_DEST_CASTER_FRONT_LEFT
        case 85: // TARGET_DEST_CASTER_BACK_LEFT
        case 86: // TARGET_DEST_CASTER_BACK_RIGHT
        case 87: // TARGET_DEST_CASTER_FRONT_RIGHT
        case 89: // TARGET_DEST_TRAJ
            return true;
        default:
            return false;
    }
}

/// True when a TBC 2.4.3 Spell.dbc record genuinely targets a destination point
/// (`Targets & TARGET_FLAG_DEST_LOCATION`, or any effect's ImplicitTargetA/B
/// uses a destination coordinate target). Distinguishes destination spells
/// sharing SpellVisual 33 (e.g. 4929 Summon Burning Exile tA=32) from source-only
/// PB-AoEs (e.g. 42052 Volcanic Geyser tA=22, tB=15) when SMSG_SPELL_GO echoes
/// TARGET_FLAG_DEST_LOCATION on the wire.
inline constexpr bool spellHasInstantAreaDestination(
    uint32_t spellTargetsMask,
    const std::array<uint32_t, 3>& implicitTargetA,
    const std::array<uint32_t, 3>& implicitTargetB) {
    constexpr uint32_t kTargetFlagDestLocation = 0x00000040u;
    if ((spellTargetsMask & kTargetFlagDestLocation) != 0u) return true;
    for (size_t i = 0; i < 3; ++i) {
        if (spellTargetIsDestinationCoordinate(implicitTargetA[i]) ||
            spellTargetIsDestinationCoordinate(implicitTargetB[i])) {
            return true;
        }
    }
    return false;
}

/// True when a PersistentAreaKit BaseEffect names the exact same M2 asset
/// (case-insensitive, slash-normalized) that the kit's CharProc 9 area-impact
/// procedure already drops randomly across the area. Native 2.4.3 does not
/// spawn a duplicate looping center copy for Volley (kit 9356 ArcaneShot_Area),
/// Blizzard (kit 9350 Blizzard_Impact_Base), or Rain of Fire (kit 9352
/// RainOfFire_Impact_Base), while keeping distinct center/cloud models such as
/// Hurricane (kit 9354 LightningStorm_Cloud_State) and Cave In (kit 9373
/// Smash_Impact_Chest).
inline bool persistentAreaSkipsDuplicateBaseEffect(std::string_view effectPath,
                                                   std::string_view areaImpactPath) {
    if (effectPath.empty() || areaImpactPath.empty() || effectPath.size() != areaImpactPath.size())
        return false;
    auto norm = [](char c) -> char {
        if (c == '/') return '\\';
        if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
        return c;
    };
    for (size_t i = 0; i < effectPath.size(); ++i) {
        if (norm(effectPath[i]) != norm(areaImpactPath[i])) return false;
    }
    return true;
}

/// Effective CharProc 9 area-impact spawn rate (impacts/s) from SpellVisualKit
/// CharParamOne. Measured on Native 2.4.3 (build 8606, default spellEffectLevel 6,
/// particleDensity 1.0): Volley (kit 9356, CharParamOne 5), Blizzard (kit 9350, 5),
/// and Rain of Fire (kit 9352, 5) drop ~2.2-2.5 impacts/s (6-8 live 3.3s instances),
/// half the raw CharParamOne count.
inline constexpr float areaImpactEffectiveRate(float dbcPerSecond) {
    return (dbcPerSecond > 0.0f) ? (dbcPerSecond * 0.5f) : 0.0f;
}

/// Radial offset for a CharProc 9 area impact within `radius` yards from a
/// uniform [0, 1] draw. Uniform radial sampling concentrates drops around the
/// AoE center (90% of horizontal projections within +-0.625 * radius = +-5 yd
/// on an 8 yd Volley), matching Native 2.4.3 instead of pushing 75% to the rim.
inline constexpr float areaImpactSampleDistance(float radius, float unitRandom) {
    const float r = (radius > 0.0f) ? radius : 0.0f;
    const float u = (unitRandom < 0.0f) ? 0.0f : ((unitRandom > 1.0f) ? 1.0f : unitRandom);
    return r * u;
}

} // namespace wowee::rendering
