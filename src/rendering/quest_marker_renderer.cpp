#include "rendering/quest_marker_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"
#include <array>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace wowee::rendering {

void QuestMarkerRenderer::setMarker(uint64_t guid, const glm::vec3& position,
                                    int markerType, float boundingHeight, float grayscale) {
    markers_[guid] = {position, markerType, boundingHeight, grayscale};
}

void QuestMarkerRenderer::clear() { markers_.clear(); }

void QuestMarkerRenderer::syncModels(M2Renderer& renderer, pipeline::AssetManager& assets,
                                     const glm::vec3& cameraPosition) {
    constexpr uint32_t firstModelId = 999910;
    constexpr std::array<const char*, 5> paths{
        "Interface\\Buttons\\TalkToMe.m2",
        "Interface\\Buttons\\TalkToMeQuestionMark.m2",
        "Interface\\Buttons\\TalkToMeQuestion_Grey.m2",
        "Interface\\Buttons\\TalkToMeGrey.m2",
        "Interface\\Buttons\\TalkToMeGreen.m2"
    };
    for (auto it = placements_.begin(); it != placements_.end();) {
        if (!markers_.contains(it->first)) {
            renderer.removeInstance(it->second.instanceId);
            it = placements_.erase(it);
        } else ++it;
    }
    for (const auto& [guid, marker] : markers_) {
        if (marker.type < 0 || marker.type > 3) throw std::logic_error("Invalid quest marker type");
        const uint32_t variant = marker.type == 3 ? 4 :
            (marker.type == 0 && marker.grayscale > 0.5f ? 3 : marker.type);
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
            // Keep authored base and highlight materials. Spell-effect overrides
            // turn the grey marker additive and wash it out to white.
            LOG_INFO("Quest marker model loaded: ", paths[variant]);
        }
        glm::vec3 position = marker.position;
        // Flight discovery is half a yard above the authored name attachment.
        const bool flightDiscovery = marker.type == 3;
        position.z += flightDiscovery ? 0.5f : marker.boundingHeight + 0.3f;
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
            renderer.setSkipCollision(instance, true);
            placements_[guid] = {instance, modelId};
        } else {
            renderer.setInstancePosition(it->second.instanceId, position);
        }
        if (flightDiscovery) {
            // TalkToMeGreen is a solid +X-facing mesh, with no billboard bone.
            // Service markers face the viewer while keeping world Z upright.
            const glm::vec3 toCamera = cameraPosition - position;
            const float yaw = std::atan2(toCamera.y, toCamera.x);
            const glm::mat4 transform = glm::translate(glm::mat4(1), position) *
                glm::rotate(glm::mat4(1), yaw, glm::vec3(0, 0, 1));
            renderer.setInstanceTransform(placements_.at(guid).instanceId, transform);
        }
    }
}
} // namespace wowee::rendering
