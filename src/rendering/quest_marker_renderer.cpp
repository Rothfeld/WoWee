#include "rendering/quest_marker_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"
#include <array>
#include <stdexcept>

namespace wowee::rendering {

void QuestMarkerRenderer::setMarker(uint64_t guid, const glm::vec3& position,
                                    int markerType, float boundingHeight, float grayscale) {
    markers_[guid] = {position, markerType, boundingHeight, grayscale};
}

void QuestMarkerRenderer::clear() { markers_.clear(); }

void QuestMarkerRenderer::syncModels(M2Renderer& renderer, pipeline::AssetManager& assets) {
    constexpr uint32_t firstModelId = 999910;
    constexpr std::array<const char*, 4> paths{
        "Interface\\Buttons\\TalkToMe.m2",
        "Interface\\Buttons\\TalkToMeQuestionMark.m2",
        "Interface\\Buttons\\TalkToMeQuestion_Grey.m2",
        "Interface\\Buttons\\TalkToMeGrey.m2"
    };
    for (auto it = placements_.begin(); it != placements_.end();) {
        if (!markers_.contains(it->first)) {
            renderer.removeInstance(it->second.instanceId);
            it = placements_.erase(it);
        } else ++it;
    }
    for (const auto& [guid, marker] : markers_) {
        if (marker.type < 0 || marker.type > 2) throw std::logic_error("Invalid quest marker type");
        const uint32_t variant = marker.type == 0 && marker.grayscale > 0.5f ? 3 : marker.type;
        const uint32_t modelId = firstModelId + variant;
        const bool modelWasMissing = !renderer.hasModel(modelId);
        if (modelWasMissing) {
            auto model = pipeline::M2Loader::load(assets.readFile(paths[variant]));
            model.name = paths[variant];
            if (model.version >= 264) {
                const auto skin = assets.readFile(pipeline::skinPathForM2(paths[variant]));
                pipeline::M2Loader::loadSkin(skin, model);
            }
            if (model.vertices.empty() || !renderer.loadModel(model, modelId)) {
                LOG_ERROR("Cannot load quest marker model: ", paths[variant]);
                throw std::runtime_error("Quest marker model failed to load");
            }
            renderer.markModelAsSpellEffect(modelId); // animated, no world collision
            LOG_INFO("Quest marker model loaded: ", paths[variant]);
        }
        glm::vec3 position = marker.position;
        position.z += marker.boundingHeight + 0.3f;
        auto it = placements_.find(guid);
        if (it != placements_.end() &&
            (modelWasMissing || it->second.modelId != modelId ||
             !renderer.hasInstance(it->second.instanceId))) {
            renderer.removeInstance(it->second.instanceId);
            placements_.erase(it);
            it = placements_.end();
        }
        if (it == placements_.end()) {
            const uint32_t instance = renderer.createInstance(modelId, position, glm::vec3(0), 1.0f, false);
            if (!instance) throw std::runtime_error("Quest marker instance failed to create");
            placements_[guid] = {instance, modelId};
        } else {
            renderer.setInstancePosition(it->second.instanceId, position);
        }
    }
}
} // namespace wowee::rendering
