// Body-atlas regions of CharSections textures come from the row's section and
// texture column (pipeline::charSectionRegion), not from the file name. This
// checks the column rule against the name rule it replaced on every stock row.
//
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc
#include <catch_amalgamated.hpp>

#include "pipeline/char_sections.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>

using wowee::pipeline::SkinRegion;

namespace {

std::filesystem::path dataRoot() {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    return root;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// The file-name rule character_renderer used before (regionFor by name).
std::optional<SkinRegion> regionByName(const std::string& path) {
    const std::string p = lower(path);
    if (p.find("faceupper") != std::string::npos) return SkinRegion::FaceUpper;
    if (p.find("facelower") != std::string::npos) return SkinRegion::FaceLower;
    if (p.find("pelvis") != std::string::npos) return SkinRegion::Pelvis;
    if (p.find("torso") != std::string::npos) return SkinRegion::Torso;
    return std::nullopt;
}

// Width of a BLP2 texture from its header, 0 when unreadable.
uint32_t blpWidth(const std::filesystem::path& root, const std::string& dbcPath) {
    std::string rel = lower(dbcPath);
    std::replace(rel.begin(), rel.end(), '\\', '/');
    if (rel.find('.') == std::string::npos) rel += ".blp";
    std::ifstream in(root / rel, std::ios::binary);
    char header[20] = {};
    if (!in.read(header, sizeof(header)) || std::memcmp(header, "BLP2", 4) != 0) return 0;
    uint32_t width = 0;
    std::memcpy(&width, header + 12, 4);
    return width;
}

} // namespace

TEST_CASE("CharSections column regions agree with the old name rule", "[char-sections][assets]") {
    const auto root = dataRoot();
    std::ifstream in(root / "dbfilesclient" / "charsections.dbc", std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    REQUIRE_FALSE(bytes.empty());
    wowee::pipeline::DBCFile dbc;
    REQUIRE(dbc.load(bytes));

    // TBC layout: 3 BaseSection, 6..8 Texture1..3.
    size_t compared = 0, agreed = 0, fullAtlas = 0, absent = 0;
    for (uint32_t row = 0; row < dbc.getRecordCount(); ++row) {
        const uint32_t section = dbc.getUInt32(row, 3);
        for (uint32_t column = 0; column < 3; ++column) {
            const std::string texture = dbc.getString(row, 6 + column);
            if (texture.empty()) continue;
            const auto byColumn = wowee::pipeline::charSectionRegion(section, column);
            if (!byColumn) continue;  // not composited onto the body
            ++compared;
            const auto byName = regionByName(texture);
            if (byName == byColumn) { ++agreed; continue; }
            // The only disagreements allowed: a name the old rule could not
            // place, which is either a whole skin drawn over the full atlas
            // either way, or a file the data does not ship (skipped by
            // resolveCharacterSections' existence check before compositing).
            INFO(texture);
            CHECK_FALSE(byName.has_value());
            const uint32_t width = blpWidth(root, texture);
            if (width == 0) { ++absent; continue; }
            CHECK(width >= 256);
            ++fullAtlas;
        }
    }
    CHECK(compared == agreed + fullAtlas + absent);
    CHECK(agreed == 3765);
    CHECK(fullAtlas == 6);
    CHECK(absent == 3);
}

TEST_CASE("Only face and underwear columns have body regions", "[char-sections]") {
    using wowee::pipeline::charSectionRegion;
    CHECK(charSectionRegion(1, 0) == SkinRegion::FaceLower);
    CHECK(charSectionRegion(1, 1) == SkinRegion::FaceUpper);
    CHECK(charSectionRegion(4, 0) == SkinRegion::Pelvis);
    CHECK(charSectionRegion(4, 1) == SkinRegion::Torso);
    CHECK_FALSE(charSectionRegion(0, 0));  // body skin: the base, not a region
    CHECK_FALSE(charSectionRegion(3, 0));  // hair
    CHECK_FALSE(charSectionRegion(4, 2));
}
