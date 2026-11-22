#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

} // namespace wowee::rendering
