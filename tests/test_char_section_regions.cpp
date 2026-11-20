// Body-atlas regions of CharSections textures come from the row's section and
// texture column (pipeline::charSectionRegion), not from the file name. This
// checks the column rule against the name rule it replaced on every stock row.
//
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc
#include <catch_amalgamated.hpp>

#include "pipeline/char_sections.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>

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

TEST_CASE("resolveSkinExtraTexture prefers skinExtra, then underwear, then bodySkin", "[char-sections]") {
    using wowee::pipeline::CharacterSectionTextures;
    using wowee::pipeline::resolveSkinExtraTexture;

    CharacterSectionTextures tex;
    CHECK(resolveSkinExtraTexture(tex).empty());

    tex.bodySkin = "Character\\Tauren\\Male\\TaurenMaleSkin00_00.blp";
    CHECK(resolveSkinExtraTexture(tex) == tex.bodySkin);

    tex.underwear.push_back({"Character\\Tauren\\Male\\TaurenMaleNakedPelvisSkin00_00.blp", SkinRegion::Pelvis});
    CHECK(resolveSkinExtraTexture(tex) == tex.underwear.front().path);

    tex.skinExtra = "Character\\Tauren\\Male\\TaurenMaleSkin00_00_Extra.blp";
    CHECK(resolveSkinExtraTexture(tex) == tex.skinExtra);
}

TEST_CASE("TBC character models bind valid BLPs for skin, hair, and Skin Extra (type 8) slots", "[char-sections][assets]") {
    const auto root = dataRoot();

    auto loadFileBytes = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(in), {}};
    };

    const auto csBytes = loadFileBytes(root / "dbfilesclient" / "charsections.dbc");
    REQUIRE_FALSE(csBytes.empty());
    wowee::pipeline::DBCFile csDbc;
    REQUIRE(csDbc.load(csBytes));

    // TBC 2.4.3 CharSections.dbc layout (variation-first).
    wowee::pipeline::CharSectionsFields csF{};
    csF.raceId = 1;
    csF.sexId = 2;
    csF.baseSection = 3;
    csF.variationIndex = 4;
    csF.colorIndex = 5;
    csF.texture1 = 6;
    csF.texture2 = 7;
    csF.texture3 = 8;
    csF.flags = 9;

    struct ModelSpec {
        uint32_t raceId;
        uint32_t sexId;
        const char* raceFolder;
        const char* m2RelPath;
        bool expectType8;
    };

    const std::array<ModelSpec, 22> models = {{
        {1,  0, "Human",    "character/human/male/humanmale.m2",         false},
        {1,  1, "Human",    "character/human/female/humanfemale.m2",     false},
        {2,  0, "Orc",      "character/orc/male/orcmale.m2",             false},
        {2,  1, "Orc",      "character/orc/female/orcfemale.m2",         false},
        {3,  0, "Dwarf",    "character/dwarf/male/dwarfmale.m2",         false},
        {3,  1, "Dwarf",    "character/dwarf/female/dwarffemale.m2",     false},
        {4,  0, "NightElf", "character/nightelf/male/nightelfmale.m2",   false},
        {4,  1, "NightElf", "character/nightelf/female/nightelffemale.m2", false},
        {5,  0, "Scourge",  "character/scourge/male/scourgemale.m2",     false},
        {5,  1, "Scourge",  "character/scourge/female/scourgefemale.m2", false},
        {6,  0, "Tauren",   "character/tauren/male/taurenmale.m2",       true},
        {6,  1, "Tauren",   "character/tauren/female/taurenfemale.m2",   true},
        {7,  0, "Gnome",    "character/gnome/male/gnomemale.m2",         false},
        {7,  1, "Gnome",    "character/gnome/female/gnomefemale.m2",     false},
        {8,  0, "Troll",    "character/troll/male/trollmale.m2",         false},
        {8,  1, "Troll",    "character/troll/female/trollfemale.m2",     false},
        {10, 0, "BloodElf", "character/bloodelf/male/bloodelfmale.m2",   false},
        {10, 1, "BloodElf", "character/bloodelf/female/bloodelffemale.m2", false},
        {11, 0, "Draenei",  "character/draenei/male/draeneimale.m2",     false},
        {11, 1, "Draenei",  "character/draenei/female/draeneifemale.m2", false},
        {13, 0, "Naga_",    "character/naga_/male/naga_male.m2",         true},
        {13, 1, "Naga_",    "character/naga_/female/naga_female.m2",     true},
    }};

    for (const auto& spec : models) {
        INFO(spec.m2RelPath);
        const auto m2Bytes = loadFileBytes(root / spec.m2RelPath);
        REQUIRE_FALSE(m2Bytes.empty());
        auto m2 = wowee::pipeline::M2Loader::load(m2Bytes);
        REQUIRE(m2.isValid());

        bool hasType8 = false;
        for (const auto& tex : m2.textures) {
            if (tex.type == 8) hasType8 = true;
        }
        CHECK(hasType8 == spec.expectType8);

        // Verify skin=0 (and for TaurenMale also skin=7 = Horserichy) binds
        // every replaceable slot (type 1, 6, 8) to a valid BLP2 file on disk.
        std::vector<uint8_t> skinsToCheck = {0};
        if (spec.raceId == 6 && spec.sexId == 0) {
            skinsToCheck = {0, 3, 6, 7, 9, 12, 15, 18};
        } else if (spec.raceId == 6 && spec.sexId == 1) {
            skinsToCheck = {0, 2, 4, 6, 8, 10};
        }

        for (uint8_t skinId : skinsToCheck) {
            wowee::pipeline::CharacterAppearance who{};
            who.raceId = spec.raceId;
            who.sexId = spec.sexId;
            who.skinId = skinId;
            who.faceId = 0;
            who.hairStyleId = 1;
            who.hairColorId = 0;

            const auto sections = wowee::pipeline::resolveCharacterSections(&csDbc, csF, who);
            auto modelCopy = m2;
            wowee::pipeline::applyCharacterTextures(modelCopy, sections, spec.raceFolder);

            for (const auto& tex : modelCopy.textures) {
                if (tex.type == 1 || tex.type == 6 || tex.type == 8) {
                    INFO("skin=" << int(skinId) << " texType=" << tex.type << " path=" << tex.filename);
                    CHECK_FALSE(tex.filename.empty());
                    CHECK(blpWidth(root, tex.filename) > 0);
                }
            }
            if (spec.expectType8) {
                INFO("skin=" << int(skinId) << " skinExtra=" << sections.skinExtra);
                CHECK_FALSE(sections.skinExtra.empty());
                CHECK(wowee::pipeline::resolveSkinExtraTexture(sections) == sections.skinExtra);
                CHECK(blpWidth(root, sections.skinExtra) > 0);
            }
        }
    }

    // TaurenMale group 15 contains both cape (type 2) and tail (type 8) batches;
    // overriding group 15 as a whole would paint the cape onto the tail.
    const auto tmBytes = loadFileBytes(root / "character/tauren/male/taurenmale.m2");
    auto tm = wowee::pipeline::M2Loader::load(tmBytes);
    REQUIRE(tm.isValid());
    bool g15HasType2 = false;
    bool g15HasType8 = false;
    for (const auto& batch : tm.batches) {
        if (batch.submeshId / 100 != 15) continue;
        REQUIRE(batch.textureIndex < tm.textureLookup.size());
        const uint16_t slot = tm.textureLookup[batch.textureIndex];
        REQUIRE(slot < tm.textures.size());
        if (tm.textures[slot].type == 2) g15HasType2 = true;
        if (tm.textures[slot].type == 8) g15HasType8 = true;
    }
    CHECK(g15HasType2);
    CHECK(g15HasType8);

    // CreatureDisplayInfoExtra.dbc Tauren humanoid NPC rows all resolve valid skinExtra BLPs.
    const auto cdieBytes = loadFileBytes(root / "dbfilesclient" / "creaturedisplayinfoextra.dbc");
    REQUIRE_FALSE(cdieBytes.empty());
    wowee::pipeline::DBCFile cdieDbc;
    REQUIRE(cdieDbc.load(cdieBytes));
    size_t taurenNpcRows = 0;
    for (uint32_t r = 0; r < cdieDbc.getRecordCount(); ++r) {
        const uint32_t raceId = cdieDbc.getUInt32(r, 1);
        const uint32_t sexId = cdieDbc.getUInt32(r, 2);
        const uint32_t skinId = cdieDbc.getUInt32(r, 3);
        if (raceId != 6) continue;
        ++taurenNpcRows;
        wowee::pipeline::CharacterAppearance who{};
        who.raceId = raceId;
        who.sexId = sexId;
        who.skinId = static_cast<uint8_t>(skinId);
        const auto sections = wowee::pipeline::resolveCharacterSections(&csDbc, csF, who);
        CHECK_FALSE(sections.skinExtra.empty());
        CHECK(blpWidth(root, sections.skinExtra) > 0);
    }
    CHECK(taurenNpcRows > 0);
}
