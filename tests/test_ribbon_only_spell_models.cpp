// Regression tests for ribbon-only spell M2 models (0 vertices, 0 particles, >0 ribbons),
// including Gothik the Harvester's soul-drain missile (spells/zig_missile.m2, visual 114)
// and the rest of the TBC 2.4.3 ribbon-only spell corpus.

#include <catch_amalgamated.hpp>

#include "pipeline/m2_loader.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto sz = in.tellg();
    if (sz <= 0) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

} // namespace

TEST_CASE("M2Model::hasRenderableContent accepts ribbon-only spell models and rejects empty models", "[m2][spell_visual][ribbons]") {
    wowee::pipeline::M2Model empty;
    CHECK_FALSE(empty.isValid());
    CHECK_FALSE(empty.hasRenderableContent());

    wowee::pipeline::M2Model ribbonOnly;
    ribbonOnly.ribbonEmitters.emplace_back();
    CHECK_FALSE(ribbonOnly.isValid());
    CHECK(ribbonOnly.vertices.empty());
    CHECK(ribbonOnly.particleEmitters.empty());
    CHECK(ribbonOnly.hasRenderableContent());
}

TEST_CASE("TBC 2.4.3 ribbon-only spell M2 corpus parses with renderable ribbons", "[m2][spell_visual][ribbons][tbc]") {
    const std::filesystem::path spellsDir = "/workspace/azerothcore/client/wowee-data/expansions/tbc/spells";
    if (!std::filesystem::exists(spellsDir)) {
        SKIP("Extracted TBC spell assets not present at " + spellsDir.string());
    }

    struct ExpectedRibbonModel {
        const char* filename;
        size_t minRibbons;
    };
    const ExpectedRibbonModel cases[] = {
        {"zig_missile.m2", 4},
        {"soulshatter_missile.m2", 2},
        {"shadow_impactdd_med_base.m2", 3},
        {"slicedice_chest.m2", 12},
    };

    for (const auto& c : cases) {
        CAPTURE(c.filename);
        const auto bytes = readFileBytes(spellsDir / c.filename);
        REQUIRE_FALSE(bytes.empty());

        const auto model = wowee::pipeline::M2Loader::load(bytes);
        CHECK(model.vertices.empty());
        CHECK(model.particleEmitters.empty());
        CHECK(model.ribbonEmitters.size() >= c.minRibbons);
        CHECK(model.hasRenderableContent());
    }
}
