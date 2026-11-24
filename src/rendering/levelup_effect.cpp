#include "rendering/levelup_effect.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace rendering {

LevelUpEffect::LevelUpEffect() = default;
LevelUpEffect::~LevelUpEffect() = default;

bool LevelUpEffect::loadModel(Kind kind,
                               M2Renderer* m2Renderer,
                               const std::vector<uint8_t>& m2FileData,
                               const std::vector<uint8_t>& skinFileData) {
    if (!m2Renderer || m2FileData.empty()) return false;

    m2Renderer_ = m2Renderer;
    const char* defaultName = (kind == Kind::ReputationLevelUp)
        ? "Spells\\ReputationLevelUp.m2"
        : "Spells\\LevelUp.m2";

    pipeline::M2Model model = pipeline::M2Loader::load(m2FileData);
    if (model.name.empty()) model.name = defaultName;
    // Spell effect M2s may have no geometry (particle-only), so don't require isValid()
    if (model.vertices.empty() && model.particleEmitters.empty()) {
        LOG_WARNING("LevelUpEffect: M2 ", defaultName, " has no vertices and no particle emitters");
        return false;
    }

    if (!skinFileData.empty() && model.version >= 264) {
        pipeline::M2Loader::loadSkin(skinFileData, model);
    }

    if (!m2Renderer_->loadModel(model, modelIdFor(kind))) {
        LOG_WARNING("LevelUpEffect: failed to load ", defaultName, " to GPU");
        return false;
    }
    m2Renderer_->markModelAsSpellEffect(modelIdFor(kind));

    modelLoaded_[static_cast<size_t>(kind)] = true;
    LOG_INFO("LevelUpEffect: loaded ", defaultName, " - vertices=", model.vertices.size(),
             " indices=", model.indices.size(),
             " emitters=", model.particleEmitters.size(),
             " lights=", model.lights.size(),
             " batches=", model.batches.size());
    return true;
}

void LevelUpEffect::trigger(Kind kind, const glm::vec3& position) {
    if (!isModelLoaded(kind) || !m2Renderer_) return;

    uint32_t instanceId = m2Renderer_->createInstance(modelIdFor(kind), position,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("LevelUpEffect: failed to create instance for kind=", static_cast<int>(kind));
        return;
    }

    // Play from the beginning once; retain the final pose while particles drain.
    m2Renderer_->setInstanceAnimationHeld(instanceId, 0, false);
    activeEffects_.push_back({.instanceId = instanceId, .elapsed = 0.0f});
}

void LevelUpEffect::update(float deltaTime) {
    if (activeEffects_.empty() || !m2Renderer_) return;

    for (auto it = activeEffects_.begin(); it != activeEffects_.end(); ) {
        it->elapsed += deltaTime;
        if (it->elapsed >= EFFECT_DURATION) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeEffects_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace rendering
} // namespace wowee
