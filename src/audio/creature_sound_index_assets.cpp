// The asset-manager half of CreatureSoundIndex, apart so the table logic can
// be tested with DBC files alone.
#include "audio/creature_sound_index.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

namespace wowee {
namespace audio {

bool CreatureSoundIndex::load(pipeline::AssetManager& assets) {
    const auto display = assets.loadDBC("CreatureDisplayInfo.dbc");
    const auto model = assets.loadDBC("CreatureModelData.dbc");
    const auto sound = assets.loadDBC("CreatureSoundData.dbc");
    const auto lookup = assets.loadDBC("FootstepTerrainLookup.dbc");
    const auto terrain = assets.loadDBC("TerrainType.dbc");
    if (!display || !model || !sound || !lookup || !terrain) {
        LOG_ERROR("Creature sounds: a required DBC is missing (CreatureDisplayInfo, CreatureModelData, "
                  "CreatureSoundData, FootstepTerrainLookup, TerrainType)");
        return false;
    }
    load(*display, *model, *sound, *lookup, *terrain);
    return true;
}

} // namespace audio
} // namespace wowee
