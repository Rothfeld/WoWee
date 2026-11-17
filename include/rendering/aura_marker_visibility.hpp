#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace wowee::rendering {

inline bool isConcealmentDetectionMarker(std::string_view path) {
    const auto separator = path.find_last_of("/\\");
    std::string name(path.substr(separator == std::string_view::npos ? 0 : separator + 1));
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name == "detectstealth_state_head.m2" || name == "detectstealth_state_head.mdx";
}

inline bool auraConcealsObserver(std::span<const uint32_t> effects) {
    return std::any_of(effects.begin(), effects.end(),
        [](uint32_t effect) { return effect == 16u || effect == 18u; });
}

} // namespace wowee::rendering
