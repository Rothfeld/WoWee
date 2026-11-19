#include "rendering/loot_sparkles.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"
#include <unordered_set>

namespace wowee {
namespace rendering {

namespace {
constexpr const char* kLootFxPath = "Particles\\LootFX.m2";
}  // namespace

bool LootSparkles::ensureModel(M2Renderer* m2, pipeline::AssetManager* assets) {
    if (m2->hasModel(MODEL_ID)) return true;
    // Not there: never loaded, or a map change emptied the renderer, which
    // took every sparkle instance with it.
    instances_.clear();
    if (failed_ || !assets) return false;

    const auto m2Data = assets->readFile(kLootFxPath);
    pipeline::M2Model model = m2Data.empty() ? pipeline::M2Model{} : pipeline::M2Loader::load(m2Data);
    // Particles only, so no geometry is expected.
    if (model.particleEmitters.empty() && model.vertices.empty()) {
        failed_ = true;
        LOG_WARNING("LootSparkles: ", kLootFxPath, " is missing or empty - corpses will not sparkle");
        return false;
    }
    if (model.name.empty()) model.name = kLootFxPath;
    if (model.version >= 264) {
        const auto skin = assets->readFile(pipeline::skinPathForM2(kLootFxPath));
        if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, model);
    }
    if (!m2->loadModel(model, MODEL_ID)) {
        failed_ = true;
        LOG_WARNING("LootSparkles: could not load ", kLootFxPath, " - corpses will not sparkle");
        return false;
    }
    // An effect, so its particles are drawn and its animation runs.
    m2->markModelAsSpellEffect(MODEL_ID);
    return true;
}

void LootSparkles::update(M2Renderer* m2, pipeline::AssetManager* assets,
                          const std::vector<Spot>& spots) {
    if (m2 != m2_) {
        // A different renderer holds none of the instances kept here.
        instances_.clear();
        m2_ = m2;
    }
    if (!m2) return;
    if (spots.empty() && instances_.empty()) return;
    if (!ensureModel(m2, assets)) return;

    std::unordered_set<uint64_t> live;
    live.reserve(spots.size());
    for (const auto& spot : spots) {
        live.insert(spot.guid);
        auto it = instances_.find(spot.guid);
        if (it != instances_.end() && m2->hasInstance(it->second.id) && it->second.scale == spot.scale) {
            // Kept in place: a corpse can still be settling into place.
            m2->setInstancePosition(it->second.id, spot.position);
            if (spot.roomSource != 0) m2->copyInstanceRoom(it->second.id, spot.roomSource);
            continue;
        }
        // New, or rescaled (an object whose model changed): placed afresh.
        if (it != instances_.end() && m2->hasInstance(it->second.id)) m2->removeInstance(it->second.id);
        const uint32_t id = m2->createInstance(MODEL_ID, spot.position, glm::vec3(0.0f), spot.scale);
        if (id != 0 && spot.roomSource != 0) m2->copyInstanceRoom(id, spot.roomSource);
        if (id != 0) instances_[spot.guid] = {id, spot.scale};
        else instances_.erase(spot.guid);
    }

    for (auto it = instances_.begin(); it != instances_.end();) {
        if (live.count(it->first) == 0) {
            m2->removeInstance(it->second.id);
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace rendering
} // namespace wowee
