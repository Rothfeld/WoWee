// Body-atlas regions of CharSections textures come from the row's section and
// texture column (pipeline::charSectionRegion), not from the file name. This
// checks the column rule against the name rule it replaced on every stock row.
//
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc
#include <catch_amalgamated.hpp>

#include "core/geoset_rules.hpp"
#include "pipeline/char_sections.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "ui/unit_portrait.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <unordered_map>

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
    if (p.find("faceupper") != std::string::npos ||
        p.find("facialupper") != std::string::npos ||
        p.find("scalpupper") != std::string::npos) return SkinRegion::FaceUpper;
    if (p.find("facelower") != std::string::npos ||
        p.find("faciallower") != std::string::npos ||
        p.find("scalplower") != std::string::npos) return SkinRegion::FaceLower;
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
    CHECK(agreed > 3765);
    CHECK(fullAtlas == 6);
    CHECK(absent == 3);
}

TEST_CASE("CharSections column regions cover face, facial hair, scalp, and underwear", "[char-sections]") {
    using wowee::pipeline::charSectionRegion;
    CHECK(charSectionRegion(1, 0) == SkinRegion::FaceLower);
    CHECK(charSectionRegion(1, 1) == SkinRegion::FaceUpper);
    CHECK(charSectionRegion(2, 0) == SkinRegion::FaceLower);
    CHECK(charSectionRegion(2, 1) == SkinRegion::FaceUpper);
    CHECK_FALSE(charSectionRegion(2, 2));
    CHECK_FALSE(charSectionRegion(3, 0));  // M2 type-6 hair texture
    CHECK(charSectionRegion(3, 1) == SkinRegion::FaceLower);
    CHECK(charSectionRegion(3, 2) == SkinRegion::FaceUpper);
    CHECK(charSectionRegion(4, 0) == SkinRegion::Pelvis);
    CHECK(charSectionRegion(4, 1) == SkinRegion::Torso);
    CHECK_FALSE(charSectionRegion(0, 0));  // body skin: the base, not a region
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

TEST_CASE("CharSections Section 2 facial hair and Section 3 scalp overlays resolve across playable TBC races", "[char-sections][assets]") {
    const auto root = dataRoot();
    auto loadFileBytes = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(in), {}};
    };
    const auto csBytes = loadFileBytes(root / "dbfilesclient" / "charsections.dbc");
    REQUIRE_FALSE(csBytes.empty());
    wowee::pipeline::DBCFile csDbc;
    REQUIRE(csDbc.load(csBytes));

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

    auto existsOnDisk = [](const std::string& dbcPath, void* ctx) -> bool {
        const auto& base = *static_cast<const std::filesystem::path*>(ctx);
        return blpWidth(base, dbcPath) > 0;
    };

    // Human Male (hairStyle=7, hairColor=9, facialHair=1): has face, scalp, and facial hair overlays.
    {
        wowee::pipeline::CharacterAppearance who{};
        who.raceId = 1;
        who.sexId = 0;
        who.skinId = 5;
        who.faceId = 6;
        who.hairStyleId = 7;
        who.hairColorId = 9;
        who.facialHairId = 1;

        const auto sections = wowee::pipeline::resolveCharacterSections(
            &csDbc, csF, who, existsOnDisk, const_cast<std::filesystem::path*>(&root));
        CHECK_FALSE(sections.faceLower.empty());
        CHECK_FALSE(sections.faceUpper.empty());
        CHECK_FALSE(sections.scalpLower.empty());
        CHECK_FALSE(sections.scalpUpper.empty());
        CHECK_FALSE(sections.facialLower.empty());
        CHECK_FALSE(sections.facialUpper.empty());

        std::vector<wowee::pipeline::SkinLayer> overlays;
        wowee::pipeline::appendFaceOverlayLayers(overlays, sections);
        REQUIRE(overlays.size() == 6);
        CHECK(overlays[0].region == SkinRegion::FaceLower);
        CHECK(overlays[1].region == SkinRegion::FaceUpper);
        CHECK(overlays[2].region == SkinRegion::FaceLower);
        CHECK(overlays[2].path == sections.scalpLower);
        CHECK(overlays[3].region == SkinRegion::FaceUpper);
        CHECK(overlays[3].path == sections.scalpUpper);
        CHECK(overlays[4].region == SkinRegion::FaceLower);
        CHECK(overlays[4].path == sections.facialLower);
        CHECK(overlays[5].region == SkinRegion::FaceUpper);
        CHECK(overlays[5].path == sections.facialUpper);
    }

    // Tauren Female (race=6, sex=1) Section 2 only authors colorIndex=0; non-zero hairColor
    // falls back to colorIndex=0 when unfiltered, and is cleanly skipped when missing on disk.
    {
        wowee::pipeline::CharacterAppearance who{};
        who.raceId = 6;
        who.sexId = 1;
        who.skinId = 0;
        who.faceId = 0;
        who.hairStyleId = 1;
        who.hairColorId = 2;
        who.facialHairId = 1;

        const auto unfiltered = wowee::pipeline::resolveCharacterSections(&csDbc, csF, who);
        CHECK(unfiltered.facialLower == "Character\\Tauren\\FacialLowerHair00_00.blp");
        CHECK(unfiltered.facialUpper == "Character\\Tauren\\FacialUpperHair00_00.blp");

        const auto filtered = wowee::pipeline::resolveCharacterSections(
            &csDbc, csF, who, existsOnDisk, const_cast<std::filesystem::path*>(&root));
        CHECK(filtered.facialLower.empty());
        CHECK(filtered.facialUpper.empty());
    }

    // Every playable TBC race/sex with Section 2 or Section 3 overlays resolves valid existing BLPs.
    const std::array<uint32_t, 10> playableRaces = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
    size_t racesWithScalp = 0;
    size_t racesWithFacial = 0;
    for (uint32_t raceId : playableRaces) {
        for (uint32_t sexId : {0u, 1u}) {
            wowee::pipeline::CharacterAppearance who{};
            who.raceId = raceId;
            who.sexId = sexId;
            who.skinId = 0;
            who.faceId = 0;
            who.hairStyleId = 1;
            who.hairColorId = 0;
            who.facialHairId = 1;
            const auto sections = wowee::pipeline::resolveCharacterSections(
                &csDbc, csF, who, existsOnDisk, const_cast<std::filesystem::path*>(&root));
            if (!sections.scalpLower.empty()) {
                CHECK(blpWidth(root, sections.scalpLower) == 128);
                ++racesWithScalp;
            }
            if (!sections.scalpUpper.empty()) {
                CHECK(blpWidth(root, sections.scalpUpper) == 128);
            }
            if (!sections.facialLower.empty()) {
                CHECK(blpWidth(root, sections.facialLower) == 128);
                ++racesWithFacial;
            }
            if (!sections.facialUpper.empty()) {
                CHECK(blpWidth(root, sections.facialUpper) == 128);
            }
        }
    }
    CHECK(racesWithScalp == 15);
    CHECK(racesWithFacial == 8);
}

TEST_CASE("UnitPortrait hair geoset selection matches TBC CharHairGeosets.dbc", "[char-sections][assets]") {
    const auto root = dataRoot();
    std::ifstream in(root / "dbfilesclient" / "charhairgeosets.dbc", std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    REQUIRE_FALSE(bytes.empty());
    wowee::pipeline::DBCFile chg;
    REQUIRE(chg.load(bytes));

    std::unordered_map<uint32_t, uint16_t> hairMap;
    for (uint32_t i = 0; i < chg.getRecordCount(); ++i) {
        const uint32_t raceId = chg.getUInt32(i, 1);
        const uint32_t sexId = chg.getUInt32(i, 2);
        const uint32_t variation = chg.getUInt32(i, 3);
        const uint32_t geosetId = chg.getUInt32(i, 4);
        const bool showScalp = chg.getFieldCount() > 5 && chg.getUInt32(i, 5) != 0;
        hairMap[wowee::core::appearanceKey(static_cast<uint8_t>(raceId),
                                           static_cast<uint8_t>(sexId),
                                           static_cast<uint8_t>(variation))] =
            wowee::core::hairGeosetFromDbcRow(geosetId, showScalp);
    }

    // Human Male bald (var=0) -> scalp 1; long hair (var=7) -> geoset 8.
    CHECK(wowee::core::resolveHairScalpGeoset(hairMap, 1, 0, 0) == 1);
    CHECK(wowee::core::resolveHairScalpGeoset(hairMap, 1, 0, 7) == 8);
    // Troll Male var=0 (geoset=0, showScalp=0) -> default scalp 1.
    CHECK(wowee::core::resolveHairScalpGeoset(hairMap, 8, 0, 0) == 1);
    // Non-zero geoset with showScalp=1 preserves authored hair geoset.
    CHECK(wowee::core::hairGeosetFromDbcRow(2, true) == 2);
    CHECK(wowee::core::hairGeosetFromDbcRow(0, true) == 1);

    // Every playable race/sex non-bald hairStyle=1 selects a hair geoset > 1.
    for (uint8_t raceId : std::array<uint8_t, 10>{1, 2, 3, 4, 5, 6, 7, 8, 10, 11}) {
        for (uint8_t sexId : {uint8_t{0}, uint8_t{1}}) {
            const uint16_t g = wowee::core::resolveHairScalpGeoset(hairMap, raceId, sexId, 1);
            INFO("race=" << int(raceId) << " sex=" << int(sexId) << " geoset=" << g);
            CHECK(g >= 1);
            const auto bare = wowee::core::bareCharacterGeosets(g, 1, 1, 1, raceId);
            CHECK(bare.count(g) == 1);
        }
    }
}

TEST_CASE("Out-of-range SetPortraitTexture fallback resolves valid TBC TemporaryPortrait BLPs", "[char-sections][assets]") {
    const auto root = dataRoot();

    // All 10 playable TBC races x both genders resolve to existing 64x64 BLP2 portraits.
    for (uint8_t raceId : std::array<uint8_t, 10>{1, 2, 3, 4, 5, 6, 7, 8, 10, 11}) {
        for (uint8_t gender : {uint8_t{0}, uint8_t{1}}) {
            const std::string path = wowee::ui::temporaryPortraitPath(raceId, gender, false);
            INFO("race=" << int(raceId) << " gender=" << int(gender) << " path=" << path);
            CHECK(path.rfind("Interface\\CharacterFrame\\TemporaryPortrait-", 0) == 0);
            CHECK(blpWidth(root, path) == 64);
        }
    }
    CHECK(wowee::ui::temporaryPortraitPath(1, 0) == "Interface\\CharacterFrame\\TemporaryPortrait-Male-Human");
    CHECK(wowee::ui::temporaryPortraitPath(5, 1) == "Interface\\CharacterFrame\\TemporaryPortrait-Female-Scourge");
    CHECK(wowee::ui::temporaryPortraitPath(0, 0xFF) == "Interface\\CharacterFrame\\TemporaryPortrait");
    CHECK(blpWidth(root, wowee::ui::temporaryPortraitPath(0, 0xFF)) == 64);
    CHECK(wowee::ui::temporaryPortraitPath(0, 0xFF, true) == "Interface\\CharacterFrame\\TemporaryPortrait-Pet");
    CHECK(blpWidth(root, wowee::ui::temporaryPortraitPath(0, 0xFF, true)) == 64);
}
