#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wowee::pipeline {

/// Guild tabard emblem parameters carried by SMSG_GUILD_QUERY_RESPONSE and
/// edited live in TabardFrame (TabardModel).
struct GuildTabardEmblem {
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    static constexpr uint32_t kGuildTabardDisplayId = 20621u;
    static constexpr uint32_t kNumEmblemStyles = 170u;       // 0..169
    static constexpr uint32_t kNumEmblemColors = 17u;        // 0..16
    static constexpr uint32_t kNumDesignerBorderStyles = 6u; // 0..5 in TabardFrame
    static constexpr uint32_t kMaxBorderStyle = 9u;          // 0..9 in BLPs
    static constexpr uint32_t kNumBorderColors = 17u;        // 0..16 for styles 0..5
    static constexpr uint32_t kNumBackgroundColors = 51u;    // 0..50

    uint32_t emblemStyle = kNone;
    uint32_t emblemColor = kNone;
    uint32_t borderStyle = kNone;
    uint32_t borderColor = kNone;
    uint32_t backgroundColor = kNone;

    [[nodiscard]] bool isValid() const {
        if (backgroundColor >= kNumBackgroundColors ||
            emblemStyle >= kNumEmblemStyles ||
            emblemColor >= kNumEmblemColors ||
            borderStyle > kMaxBorderStyle) {
            return false;
        }
        const uint32_t maxBorderColor = (borderStyle <= 5u) ? 16u : 3u;
        return borderColor <= maxBorderColor;
    }

    [[nodiscard]] bool operator==(const GuildTabardEmblem& o) const = default;
};

/// Case-insensitive check for the default un-emblazoned guild tabard component
/// textures referenced by ItemDisplayInfo.dbc (e.g. displayId 20621, item 5976).
inline bool isGuildTabardTextureName(std::string_view texName) {
    auto iequals = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i];
            char cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
            if (ca != cb) return false;
        }
        return true;
    };
    return iequals(texName, "Tabard_A_05Default_Chest_TU") ||
           iequals(texName, "Tabard_A_05Default_Chest_TL");
}

inline std::string guildTabardBackgroundPath(uint32_t backgroundColor, bool upper,
                                             bool withExtension = true) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Textures\\GuildEmblems\\Background_%02u_%s_U%s",
                  backgroundColor, upper ? "TU" : "TL", withExtension ? ".blp" : "");
    return buf;
}

inline std::string guildTabardBorderPath(uint32_t borderStyle, uint32_t borderColor, bool upper,
                                         bool withExtension = true) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Textures\\GuildEmblems\\Border_%02u_%02u_%s_U%s",
                  borderStyle, borderColor, upper ? "TU" : "TL", withExtension ? ".blp" : "");
    return buf;
}

inline std::string guildTabardEmblemPath(uint32_t emblemStyle, uint32_t emblemColor, bool upper,
                                         bool withExtension = true) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Textures\\GuildEmblems\\Emblem_%02u_%02u_%s_U%s",
                  emblemStyle, emblemColor, upper ? "TU" : "TL", withExtension ? ".blp" : "");
    return buf;
}

/// Append the 6 guild tabard layers (Background -> Border -> Emblem, TorsoUpper
/// region 3 + TorsoLower region 4) to a character compositing layer list.
inline bool appendGuildTabardRegionLayers(std::vector<std::pair<int, std::string>>& regionLayers,
                                          const GuildTabardEmblem& emblem) {
    if (!emblem.isValid()) return false;
    regionLayers.emplace_back(3, guildTabardBackgroundPath(emblem.backgroundColor, true));
    regionLayers.emplace_back(4, guildTabardBackgroundPath(emblem.backgroundColor, false));
    regionLayers.emplace_back(3, guildTabardBorderPath(emblem.borderStyle, emblem.borderColor, true));
    regionLayers.emplace_back(4, guildTabardBorderPath(emblem.borderStyle, emblem.borderColor, false));
    regionLayers.emplace_back(3, guildTabardEmblemPath(emblem.emblemStyle, emblem.emblemColor, true));
    regionLayers.emplace_back(4, guildTabardEmblemPath(emblem.emblemStyle, emblem.emblemColor, false));
    return true;
}

/// Wrap-around cycling for TabardModel:CycleVariation(id, delta).
/// id: 1=emblemStyle, 2=emblemColor, 3=borderStyle, 4=borderColor, 5=backgroundColor.
inline bool cycleGuildTabardVariation(GuildTabardEmblem& emblem, int variationId, int delta) {
    auto wrap = [delta](uint32_t& val, uint32_t count) {
        if (count == 0) return;
        if (val >= count) val = 0;
        const int64_t next = (static_cast<int64_t>(val) + delta) % static_cast<int64_t>(count);
        val = static_cast<uint32_t>(next < 0 ? next + count : next);
    };
    switch (variationId) {
        case 1: wrap(emblem.emblemStyle, GuildTabardEmblem::kNumEmblemStyles); return true;
        case 2: wrap(emblem.emblemColor, GuildTabardEmblem::kNumEmblemColors); return true;
        case 3: wrap(emblem.borderStyle, GuildTabardEmblem::kNumDesignerBorderStyles); return true;
        case 4: wrap(emblem.borderColor, GuildTabardEmblem::kNumBorderColors); return true;
        case 5: wrap(emblem.backgroundColor, GuildTabardEmblem::kNumBackgroundColors); return true;
        default: return false;
    }
}

}  // namespace wowee::pipeline
