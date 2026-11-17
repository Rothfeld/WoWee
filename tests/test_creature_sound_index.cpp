// Creature sounds resolved from the game's tables, not from model names:
// display -> CreatureSoundData, FootstepTerrainLookup by TerrainType.
//
// Reads an extracted TBC tree:
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc
#include <catch_amalgamated.hpp>

#include "audio/creature_sound_index.hpp"
#include "pipeline/dbc_loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

using namespace wowee;

namespace {

pipeline::DBCFile loadDbc(const char* name) {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    std::ifstream in(std::filesystem::path(root) / "dbfilesclient" / name, std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    REQUIRE_FALSE(bytes.empty());
    pipeline::DBCFile dbc;
    REQUIRE(dbc.load(bytes));
    return dbc;
}

std::string soundName(const pipeline::DBCFile& entries, uint32_t id) {
    const int32_t row = entries.findRecordById(id);
    REQUIRE(row >= 0);
    return entries.getString(static_cast<uint32_t>(row), 2);
}

} // namespace

TEST_CASE("Footsteps come from FootstepTerrainLookup by sound set and terrain", "[audio][assets]") {
    const auto display = loadDbc("creaturedisplayinfo.dbc");
    const auto model = loadDbc("creaturemodeldata.dbc");
    const auto sound = loadDbc("creaturesounddata.dbc");
    const auto lookup = loadDbc("footstepterrainlookup.dbc");
    const auto terrain = loadDbc("terraintype.dbc");
    const auto entries = loadDbc("soundentries.dbc");
    audio::CreatureSoundIndex index;
    index.load(display, model, sound, lookup, terrain);

    // Display 2404, a riding horse: CreatureModelData 216 names sound set 65.
    constexpr uint32_t horse = 2404, snow = 3, dirt = 0;
    CHECK(soundName(entries, index.footstepSound(horse, snow, false)) == "FootstepHorseSnow");
    CHECK(soundName(entries, index.footstepSound(horse, dirt, false)) == "FootstepHorseDirt");
    // Display 8840, the Ice Claw Bear (model 83).
    CHECK(soundName(entries, index.footstepSound(8840, snow, false)) == "CharacterSmallSnow");
    // The horse's sound set names its own vocals.
    const uint32_t set = index.soundDataForDisplay(horse);
    CHECK(soundName(entries, index.sound(set, audio::CreatureSoundColumn::Aggro)) == "HorseAggro");
    CHECK(soundName(entries, index.sound(set, audio::CreatureSoundColumn::Injury)) == "HorseWound");
    CHECK(index.sound(set, audio::CreatureSoundColumn::JumpStart) == 0);
}

TEST_CASE("A SoundEntries row lists its files under its directory", "[audio][assets]") {
    const auto entries = loadDbc("soundentries.dbc");
    const auto entry = audio::readSoundEntry(entries, 1137);  // FootstepHorseDirt
    REQUIRE(entry);
    REQUIRE(entry->files.size() == 5);
    CHECK(entry->files[0] == "Sound\\Creature\\Horse\\mFootStepsHorseDirt01.wav");
    CHECK(entry->volume == Catch::Approx(0.2f));
    std::mt19937 rng(1);
    for (int i = 0; i < 20; ++i) {
        const auto& file = audio::pickSoundFile(*entry, rng);
        CHECK(std::find(entry->files.begin(), entry->files.end(), file) != entry->files.end());
    }
}
