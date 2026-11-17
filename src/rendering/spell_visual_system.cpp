#include "rendering/spell_visual_system.hpp"
#include "rendering/aura_marker_visibility.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/placement_transform.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <cassert>
#include <limits>
#include <glm/gtc/constants.hpp>

namespace wowee {
namespace rendering {

void SpellVisualSystem::initialize(M2Renderer* m2Renderer, Renderer* renderer) {
    m2Renderer_ = m2Renderer;
    renderer_ = renderer;
}

void SpellVisualSystem::shutdown() {
    reset();
    m2Renderer_ = nullptr;
    renderer_ = nullptr;
    cachedAssetManager_ = nullptr;
}

// Load SpellVisual DBC chain: SpellVisualEffectName → SpellVisualKit → SpellVisual
// to build cast/impact M2 path lookup maps.
void SpellVisualSystem::loadSpellVisualDbc() {
    if (spellVisualDbcLoaded_) return;

    if (!cachedAssetManager_) {
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    }
    // Not an attempt. "Set early to prevent re-entry on failure" was the
    // intent, but there is no failure yet - only assets that have not
    // arrived. Latching here left spell visuals off for the session
    // whenever this was reached first.
    if (!cachedAssetManager_) return;
    spellVisualDbcLoaded_ = true;  // a real attempt follows; do not repeat it

    auto* layout = pipeline::getActiveDBCLayout();
    const pipeline::DBCFieldMap* svLayout  = layout ? layout->getLayout("SpellVisual")           : nullptr;
    const pipeline::DBCFieldMap* kitLayout = layout ? layout->getLayout("SpellVisualKit")        : nullptr;
    const pipeline::DBCFieldMap* fxLayout  = layout ? layout->getLayout("SpellVisualEffectName") : nullptr;

    uint32_t svCastKitField   = svLayout  ? (*svLayout)["CastKit"]       : 2;
    uint32_t svPrecastKitField = svLayout  ? (*svLayout)["PrecastKit"]    : 1;
    uint32_t svImpactKitField = svLayout  ? (*svLayout)["ImpactKit"]     : 3;
    uint32_t svMissileField   = svLayout  ? (*svLayout)["MissileModel"]  : 8;
    uint32_t fxFilePathField  = fxLayout  ? (*fxLayout)["FilePath"]       : 2;

    // Kit effect fields to probe, in priority order.
    // SpecialEffect0 > BaseEffect > LeftHand > RightHand > Chest > Head > Breath
    struct KitField { const char* name; uint32_t fallback; };
    static constexpr KitField kitFieldDefs[] = {
        {.name = "SpecialEffect0",  .fallback = 11}, {.name = "BaseEffect",       .fallback = 5},
        {.name = "LeftHandEffect",   .fallback = 6}, {.name = "RightHandEffect",  .fallback = 7},
        {.name = "ChestEffect",      .fallback = 4}, {.name = "HeadEffect",       .fallback = 3},
        {.name = "BreathEffect",     .fallback = 8}, {.name = "SpecialEffect1",  .fallback = 12},
        {.name = "SpecialEffect2",  .fallback = 13},
    };
    constexpr size_t numKitFields = sizeof(kitFieldDefs) / sizeof(kitFieldDefs[0]);
    uint32_t kitFields[numKitFields];
    for (size_t k = 0; k < numKitFields; ++k)
        kitFields[k] = kitLayout ? kitLayout->field(kitFieldDefs[k].name) : kitFieldDefs[k].fallback;

    // Load SpellVisualEffectName.dbc - ID → M2 path
    auto fxDbc = cachedAssetManager_->loadDBC("SpellVisualEffectName.dbc");
    if (!fxDbc || !fxDbc->isLoaded() || fxDbc->getFieldCount() <= fxFilePathField) {
        LOG_DEBUG("SpellVisual: SpellVisualEffectName.dbc unavailable (fc=",
                  fxDbc ? fxDbc->getFieldCount() : 0, ")");
        return;
    }
    std::unordered_map<uint32_t, std::string> effectPaths; // effectNameId → path
    for (uint32_t i = 0; i < fxDbc->getRecordCount(); ++i) {
        uint32_t id   = fxDbc->getUInt32(i, 0);
        std::string p = fxDbc->getString(i, fxFilePathField);
        if (id && !p.empty()) {
            // The DBC stores the extension the art was authored with; what
            // shipped is .m2.
            p = pipeline::modelPathToM2(p);
            effectPaths[id] = p;
        }
    }

    // Load SpellVisualKit.dbc - kitId → best SpellVisualEffectName ID
    // Probes all effect slots in priority order and keeps the first valid hit.
    auto kitDbc = cachedAssetManager_->loadDBC("SpellVisualKit.dbc");
    std::unordered_map<uint32_t, uint32_t> kitToEffectName; // kitId → effectNameId
    if (kitDbc && kitDbc->isLoaded()) {
        uint32_t fc = kitDbc->getFieldCount();
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            uint32_t kitId = kitDbc->getUInt32(i, 0);
            if (!kitId) continue;
            kitAnimations_[kitId] = kitDbc->getUInt32(i, 2);
            // Attachment semantics come from the kit slot, not the filename.
            static constexpr uint32_t attachments[] = {0, 0, 2, 1, 5, 11, 19, 0, 0};
            for (size_t k = 0; k < numKitFields; ++k) {
                if (kitFields[k] >= fc) continue;
                const uint32_t effectId = kitDbc->getUInt32(i, kitFields[k]);
                if (const auto path = effectPaths.find(effectId); path != effectPaths.end())
                    kitEffects_[kitId].push_back({path->second, attachments[k]});
            }
            uint32_t eff = 0;
            for (size_t k = 0; k < numKitFields && !eff; ++k) {
                if (kitFields[k] < fc)
                    eff = kitDbc->getUInt32(i, kitFields[k]);
            }
            if (eff) kitToEffectName[kitId] = eff;
        }
    }

    // Helper: resolve path for a given kit ID
    auto kitPath = [&](uint32_t kitId) -> std::string {
        if (!kitId) return {};
        auto kitIt = kitToEffectName.find(kitId);
        if (kitIt == kitToEffectName.end()) return {};
        auto fxIt = effectPaths.find(kitIt->second);
        return (fxIt != effectPaths.end()) ? fxIt->second : std::string{};
    };
    for (const auto& [kitId, animation] : kitAnimations_) {
        (void)animation;
        const auto path = kitPath(kitId);
        if (!path.empty()) kitPaths_[kitId] = path;
    }
    auto missilePath = [&](uint32_t effId) -> std::string {
        if (!effId) return {};
        auto fxIt = effectPaths.find(effId);
        return (fxIt != effectPaths.end()) ? fxIt->second : std::string{};
    };

    // Load SpellVisual.dbc - visualId → cast/impact M2 paths via kit chain
    auto svDbc = cachedAssetManager_->loadDBC("SpellVisual.dbc");
    if (!svDbc || !svDbc->isLoaded()) {
        LOG_DEBUG("SpellVisual: SpellVisual.dbc unavailable");
        return;
    }
    uint32_t svFc = svDbc->getFieldCount();
    assert(svLayout && "SpellVisual layout required for channel kits");
    uint32_t loadedPrecast = 0, loadedCast = 0, loadedImpact = 0, loadedMissile = 0;
    for (uint32_t i = 0; i < svDbc->getRecordCount(); ++i) {
        uint32_t vid = svDbc->getUInt32(i, 0);
        if (!vid) continue;
        const uint32_t channelField = (*svLayout)["ChannelKit"];
        if (channelField < svFc) channelKits_[vid] = svDbc->getUInt32(i, channelField);
        assert(svPrecastKitField < svFc);
        precastKits_[vid] = svDbc->getUInt32(i, svPrecastKitField);
        const uint32_t stateField = (*svLayout)["StateKit"];
        assert(stateField < svFc);
        stateKits_[vid] = svDbc->getUInt32(i, stateField);

        // Precast path: PrecastKit → SpecialEffect0/BaseEffect
        {
            std::string path;
            if (svPrecastKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svPrecastKitField));
            if (!path.empty()) { spellVisualPrecastPath_[vid] = path; ++loadedPrecast; }
        }
        // Cast path: CastKit → SpecialEffect0/BaseEffect
        {
            std::string path;
            if (svCastKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svCastKitField));
            if (!path.empty()) { spellVisualCastPath_[vid] = path; ++loadedCast; }
        }
        // Impact path: ImpactKit → SpecialEffect0/BaseEffect
        {
            std::string path;
            if (svImpactKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svImpactKitField));
            if (!path.empty()) { spellVisualImpactPath_[vid] = path; ++loadedImpact; }
        }
        // The missile flies from caster to target. It used to stand in for a
        // missing cast or impact kit instead, parked at one end, which is why a
        // Fireball - which has both kits - never showed its fireball at all.
        if (svMissileField < svFc) {
            std::string path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) { spellVisualMissilePath_[vid] = path; ++loadedMissile; }
        }
    }
    LOG_INFO("SpellVisual: loaded precast=", loadedPrecast, " cast=", loadedCast, " impact=", loadedImpact,
             " missile=", loadedMissile,
             " visual\u2192M2 mappings (of ", svDbc->getRecordCount(), " records)");
}

// ---------------------------------------------------------------------------
// Classify model path to a character attachment point for bone tracking
// ---------------------------------------------------------------------------
uint32_t SpellVisualSystem::classifyAttachmentId(const std::string& modelPath) {
    std::string lower = modelPath;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // "hand" effects track the right hand (attachment 1)
    if (lower.find("_hand") != std::string::npos || lower.find("hand_") != std::string::npos)
        return 1; // RightHand
    // "chest" effects track chest/torso (attachment 5 in M2 spec)
    if (lower.find("_chest") != std::string::npos || lower.find("chest_") != std::string::npos)
        return 5; // Chest
    // "head" effects track head (attachment 11)
    if (lower.find("_head") != std::string::npos || lower.find("head_") != std::string::npos)
        return 11; // Head
    return 0; // No bone tracking (static position or base effect)
}

// ---------------------------------------------------------------------------
// Height offset for spell effect placement (fallback when no bone tracking)
// ---------------------------------------------------------------------------
glm::vec3 SpellVisualSystem::applyEffectHeightOffset(const glm::vec3& basePos, const std::string& modelPath) {
    // Lowercase the path for case-insensitive matching
    std::string lower = modelPath;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // "hand" effects go at hand height (~0.8m above feet)
    if (lower.find("_hand") != std::string::npos || lower.find("hand_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 0.8f);
    }
    // "chest" effects go at chest height (~1.0m above feet)
    if (lower.find("_chest") != std::string::npos || lower.find("chest_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 1.0f);
    }
    // "head" effects go at head height (~1.6m above feet)
    if (lower.find("_head") != std::string::npos || lower.find("head_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 1.6f);
    }
    // "base" / "feet" / ground effects stay at ground level
    return basePos;
}

void SpellVisualSystem::playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                                uint32_t castTimeMs, uint32_t attachInstanceId) {
    LOG_INFO("SpellVisual: playSpellVisualPrecast visualId=", visualId,
             " pos=(", worldPosition.x, ",", worldPosition.y, ",", worldPosition.z,
             ") castTimeMs=", castTimeMs);
    if (!m2Renderer_ || visualId == 0) {
        LOG_WARNING("SpellVisual: playSpellVisualPrecast early-out: m2Renderer_=", (m2Renderer_ ? "yes" : "null"),
                    " visualId=", visualId);
        return;
    }

    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) { LOG_WARNING("SpellVisual: no AssetManager"); return; }

    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // An absent stage has no effect; do not substitute a different stage.
    auto pathIt = spellVisualPrecastPath_.find(visualId);
    if (pathIt == spellVisualPrecastPath_.end()) {
        return;
    }

    const std::string& modelPath = pathIt->second;
    LOG_INFO("SpellVisual: precast path resolved to: ", modelPath);

    // Get or assign a model ID for this path
    auto midIt = spellVisualModelIds_.find(modelPath);
    uint32_t modelId = 0;
    if (midIt != spellVisualModelIds_.end()) {
        modelId = midIt->second;
    } else {
        if (nextSpellVisualModelId_ >= 999800) {
            LOG_WARNING("SpellVisual: model ID pool exhausted");
            return;
        }
        modelId = nextSpellVisualModelId_++;
        spellVisualModelIds_[modelPath] = modelId;
    }

    if (spellVisualFailedModels_.count(modelId)) {
        LOG_WARNING("SpellVisual: precast model in failed-cache, skipping: ", modelPath);
        return;
    }

    if (!m2Renderer_->hasModel(modelId)) {
        auto m2Data = cachedAssetManager_->readFile(modelPath);
        if (m2Data.empty()) {
            LOG_WARNING("SpellVisual: could not read precast model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            // Fall back to cast kit
            playSpellVisual(visualId, worldPosition, false, attachInstanceId);
            return;
        }
        LOG_INFO("SpellVisual: precast M2 data read OK, size=", m2Data.size(), " bytes");
        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        if (model.name.empty()) model.name = modelPath;
        LOG_INFO("SpellVisual: precast M2 parsed: verts=", model.vertices.size(),
                 " bones=", model.bones.size(), " particles=", model.particleEmitters.size(),
                 " ribbons=", model.ribbonEmitters.size(),
                 " globalSeqs=", model.globalSequenceDurations.size(),
                 " sequences=", model.sequences.size());
        if (model.vertices.empty() && model.particleEmitters.empty()) {
            LOG_WARNING("SpellVisual: empty precast model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            playSpellVisual(visualId, worldPosition, false, attachInstanceId);
            return;
        }
        if (model.version >= 264) {
            std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skinData = cachedAssetManager_->readFile(skinPath);
            if (!skinData.empty()) {
                pipeline::M2Loader::loadSkin(skinData, model);
                LOG_INFO("SpellVisual: loaded skin, indices=", model.indices.size());
            }
        }
        if (!m2Renderer_->loadModel(model, modelId)) {
            LOG_WARNING("SpellVisual: failed to load precast model to GPU: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            playSpellVisual(visualId, worldPosition, false, attachInstanceId);
            return;
        }
        m2Renderer_->markModelAsSpellEffect(modelId);
        LOG_INFO("SpellVisual: loaded precast model id=", modelId, " path=", modelPath);
    }

    // Determine attachment point for bone tracking (hand/chest/head → follow
    // the CASTER's bones; attachInstanceId=0 means a non-tracked caster, so
    // the effect stays static at their world position).
    uint32_t attachId = classifyAttachmentId(modelPath);
    if (attachInstanceId == 0) attachId = 0;
    glm::vec3 spawnPos = worldPosition;
    if (attachId != 0 && renderer_) {
        auto* charRenderer = renderer_->getCharacterRenderer();
        if (charRenderer) {
            glm::mat4 attachMat;
            if (charRenderer->getAttachmentTransform(attachInstanceId, attachId, attachMat)) {
                spawnPos = glm::vec3(attachMat[3]);
            } else {
                spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
                attachId = 0;
            }
        } else {
            spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
            attachId = 0;
        }
    } else {
        spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
    }

    uint32_t instanceId = m2Renderer_->createInstance(modelId,
                                                       spawnPos,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: createInstance returned 0 for precast model=", modelPath);
        return;
    }
    m2Renderer_->restartInstanceAnimation(instanceId);

    // Duration: prefer server cast time if available (long casts like Hearthstone=10s),
    // otherwise fall back to M2 animation duration, then default.
    float duration;
    if (castTimeMs >= 500) {
        // Server cast time available - precast should last the full cast duration
        duration = std::clamp(static_cast<float>(castTimeMs) / 1000.0f, 0.5f, 30.0f);
    } else {
        float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
        duration = (animDurMs > 100.0f)
            ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
            : SPELL_VISUAL_DEFAULT_DURATION;
    }
    activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = duration, .isPrecast = true, .attachmentId = attachId, .attachInstanceId = attachInstanceId});
    LOG_INFO("SpellVisual: spawned precast visualId=", visualId, " instanceId=", instanceId,
             " duration=", duration, "s castTimeMs=", castTimeMs, " attach=", attachId,
             " model=", modelPath,
             " active=", activeSpellVisuals_.size());

    // Hand effects: spawn a mirror copy on the caster's left hand (attachment 2)
    if (attachId == 1 /* RightHand */) {
        glm::vec3 leftPos = worldPosition;
        if (renderer_) {
            auto* cr = renderer_->getCharacterRenderer();
            if (cr) {
                glm::mat4 lm;
                if (cr->getAttachmentTransform(attachInstanceId, 2, lm))
                    leftPos = glm::vec3(lm[3]);
            }
        }
        uint32_t leftId = m2Renderer_->createInstance(modelId, leftPos, glm::vec3(0.0f), 1.0f);
        if (leftId != 0) {
            m2Renderer_->restartInstanceAnimation(leftId);
            activeSpellVisuals_.push_back({.instanceId = leftId, .elapsed = 0.0f, .duration = duration, .isPrecast = true, .attachmentId = 2 /* LeftHand */, .attachInstanceId = attachInstanceId});
        }
    }
}

// The cast, impact and missile models load the same way: a model ID per path,
// a negative cache for paths that failed, and the skin beside a WotLK model.
uint32_t SpellVisualSystem::loadSpellModel(const std::string& modelPath) {
    // Get or assign a model ID for this path
    auto midIt = spellVisualModelIds_.find(modelPath);
    uint32_t modelId = 0;
    if (midIt != spellVisualModelIds_.end()) {
        modelId = midIt->second;
    } else {
        if (nextSpellVisualModelId_ >= 999800) {
            LOG_WARNING("SpellVisual: model ID pool exhausted");
            return 0;
        }
        modelId = nextSpellVisualModelId_++;
        spellVisualModelIds_[modelPath] = modelId;
    }

    // Skip models that have previously failed to load (avoid repeated I/O)
    if (spellVisualFailedModels_.count(modelId)) {
        LOG_WARNING("SpellVisual: model in failed-cache, skipping: ", modelPath);
        return 0;
    }

    // Load the M2 model if not already loaded
    if (!m2Renderer_->hasModel(modelId)) {
        auto m2Data = cachedAssetManager_->readFile(modelPath);
        if (m2Data.empty()) {
            LOG_WARNING("SpellVisual: could not read model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return 0;
        }
        LOG_INFO("SpellVisual: M2 data read OK, size=", m2Data.size(), " bytes");
        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        if (model.name.empty()) model.name = modelPath;
        LOG_INFO("SpellVisual: M2 parsed: verts=", model.vertices.size(),
                 " bones=", model.bones.size(), " particles=", model.particleEmitters.size(),
                 " ribbons=", model.ribbonEmitters.size());
        if (model.vertices.empty() && model.particleEmitters.empty()) {
            LOG_WARNING("SpellVisual: empty model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return 0;
        }
        // Load skin file for WotLK-format M2s
        if (model.version >= 264) {
            std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skinData = cachedAssetManager_->readFile(skinPath);
            if (!skinData.empty()) pipeline::M2Loader::loadSkin(skinData, model);
        }
        if (!m2Renderer_->loadModel(model, modelId)) {
            LOG_WARNING("SpellVisual: failed to load model to GPU: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return 0;
        }
        m2Renderer_->markModelAsSpellEffect(modelId);
        LOG_INFO("SpellVisual: loaded model id=", modelId, " path=", modelPath);
    }
    return modelId;
}

void SpellVisualSystem::playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                                         bool useImpactKit, uint32_t attachInstanceId) {
    LOG_INFO("SpellVisual: playSpellVisual visualId=", visualId, " impact=", useImpactKit,
             " pos=(", worldPosition.x, ",", worldPosition.y, ",", worldPosition.z, ")");
    if (!m2Renderer_ || visualId == 0) return;

    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;

    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // Cast and impact stages are independent, including deliberately empty kits.
    auto& primaryMap = useImpactKit ? spellVisualImpactPath_ : spellVisualCastPath_;
    auto pathIt = primaryMap.find(visualId);
    if (pathIt == primaryMap.end()) {
        return;
    }

    playModelEffect(visualId, pathIt->second, worldPosition, useImpactKit, attachInstanceId);
}

uint32_t SpellVisualSystem::channelAnimation(uint32_t visualId) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto kit = channelKits_.find(visualId);
    if (kit == channelKits_.end() || !kit->second) return 0xffffffffu;
    const auto animation = kitAnimations_.find(kit->second);
    return animation == kitAnimations_.end() ? 0xffffffffu : animation->second;
}

uint32_t SpellVisualSystem::precastAnimation(uint32_t visualId) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto kit = precastKits_.find(visualId);
    if (kit == precastKits_.end() || !kit->second) return 0xffffffffu;
    const auto animation = kitAnimations_.find(kit->second);
    return animation == kitAnimations_.end() ? 0xffffffffu : animation->second;
}

void SpellVisualSystem::startChannelVisual(uint32_t visualId, const glm::vec3& position,
                                          uint32_t durationMs, uint32_t attachInstanceId) {
    if (!m2Renderer_ || !attachInstanceId) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    updateChannelVisual(attachInstanceId, 0);
    const auto kit = channelKits_.find(visualId);
    if (kit == channelKits_.end() || !kit->second) return;
    const auto effects = kitEffects_.find(kit->second);
    if (effects == kitEffects_.end()) return;
    auto* characters = renderer_->getCharacterRenderer();
    for (const auto& effect : effects->second) {
        const uint32_t model = loadSpellModel(effect.path);
        if (!model) continue;
        glm::vec3 spawn = position;
        if (effect.attachment) {
            glm::mat4 transform;
            if (!characters || !characters->getAttachmentTransform(attachInstanceId, effect.attachment, transform)) {
                LOG_ERROR("Channel attachment unavailable: ", effect.attachment, " model=", effect.path);
                continue;
            }
            spawn = glm::vec3(transform[3]);
        }
        const uint32_t instance = m2Renderer_->createInstance(model, spawn, glm::vec3(0), 1.0f);
        if (!instance) { LOG_ERROR("Channel effect instance failed: ", effect.path); continue; }
        m2Renderer_->restartInstanceAnimation(instance);
        activeSpellVisuals_.push_back({instance, 0.0f, durationMs / 1000.0f,
            false, effect.attachment, attachInstanceId, true});
        LOG_INFO("Channel visual: kit=", kit->second, " attachment=", effect.attachment, " model=", effect.path);
    }
}

void SpellVisualSystem::syncAuraVisuals(uint32_t attachInstanceId,
                                       const std::vector<uint32_t>& visualIds,
                                       bool observerConcealed) {
    if (!m2Renderer_ || !attachInstanceId) return;
    auto found = unitAuraVisuals_.find(attachInstanceId);
    if (found != unitAuraVisuals_.end() && found->second.ids == visualIds &&
        found->second.observerConcealed == observerConcealed) return;
    if (found == unitAuraVisuals_.end() && visualIds.empty()) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto* characters = renderer_->getCharacterRenderer();
    glm::vec3 position;
    if (!characters || !characters->getInstancePosition(attachInstanceId, position)) return;

    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end();) {
        if (it->attachInstanceId == attachInstanceId && it->auraVisualId && !it->decaying &&
            (std::find(visualIds.begin(), visualIds.end(), it->auraVisualId) == visualIds.end() ||
             (it->concealmentDetectionMarker && !observerConcealed))) {
            const float decay = m2Renderer_->finishInstanceStateEffect(it->instanceId);
            if (decay > 0) {
                it->decaying = true;
                it->elapsed = 0;
                it->duration = decay;
                ++it;
            } else {
                m2Renderer_->removeInstance(it->instanceId);
                it = activeSpellVisuals_.erase(it);
            }
        } else ++it;
    }
    const auto previous = found == unitAuraVisuals_.end() ? std::vector<uint32_t>{} : found->second.ids;
    const bool revealMarkers = observerConcealed &&
        (found == unitAuraVisuals_.end() || !found->second.observerConcealed);
    for (uint32_t visual : visualIds) {
        const bool existing = std::find(previous.begin(), previous.end(), visual) != previous.end();
        if (existing && !revealMarkers) continue;
        const auto kit = stateKits_.find(visual);
        if (kit == stateKits_.end() || !kit->second) continue;
        const auto effects = kitEffects_.find(kit->second);
        if (effects == kitEffects_.end()) continue;
        for (const auto& effect : effects->second) {
            const bool marker = isConcealmentDetectionMarker(effect.path);
            if ((marker && !observerConcealed) || (existing && !marker)) continue;
            const uint32_t model = loadSpellModel(effect.path);
            if (!model) { LOG_ERROR("Aura model unavailable: ", effect.path); continue; }
            const uint32_t instance = m2Renderer_->createInstance(model, position, glm::vec3(0), 1.0f);
            if (!instance) { LOG_ERROR("Aura instance failed: ", effect.path); continue; }
            m2Renderer_->startInstanceStateEffect(instance);
            activeSpellVisuals_.push_back({instance, 0.0f, std::numeric_limits<float>::infinity(),
                false, effect.attachment, attachInstanceId, false, visual, marker});
            LOG_INFO("Aura visual: owner=", attachInstanceId, " visual=", visual,
                     " kit=", kit->second, " attachment=", effect.attachment, " model=", effect.path);
        }
    }
    if (visualIds.empty()) unitAuraVisuals_.erase(attachInstanceId);
    else unitAuraVisuals_[attachInstanceId] = {visualIds, observerConcealed};
}

void SpellVisualSystem::updateChannelVisual(uint32_t attachInstanceId, uint32_t remainingMs) {
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end();) {
        if (it->isChannel && it->attachInstanceId == attachInstanceId) {
            if (!remainingMs) {
                m2Renderer_->removeInstance(it->instanceId);
                it = activeSpellVisuals_.erase(it);
                continue;
            }
            it->duration = it->elapsed + remainingMs / 1000.0f;
        }
        ++it;
    }
}

void SpellVisualSystem::playSpellVisualKit(uint32_t kitId, const glm::vec3& worldPosition,
                                            uint32_t attachInstanceId) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto animation = kitAnimations_.find(kitId);
    if (animation == kitAnimations_.end()) {
        LOG_WARNING("Unknown SpellVisualKit: ", kitId);
        return;
    }
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (cr && attachInstanceId && animation->second != 0xffffffffu &&
        cr->hasAnimation(attachInstanceId, animation->second)) {
        uint32_t current = 0; float time = 0, duration = 0;
        cr->getAnimationState(attachInstanceId, current, time, duration);
        if (current == anim::SITTING || current == anim::SIT_GROUND_DOWN) {
            cr->setUpperBodyAnimation(attachInstanceId, 0);
            cr->setUpperBodyAnimation(attachInstanceId, animation->second, false);
        } else if (attachInstanceId == renderer_->getCharacterInstanceId()) {
            renderer_->getAnimationController()->playOneShotAnimation(animation->second);
        } else {
            cr->playAnimation(attachInstanceId, animation->second, false);
        }
    }
    LOG_DEBUG("SpellVisualKit: id=", kitId, " animation=", animation->second);
    if (auto path = kitPaths_.find(kitId); path != kitPaths_.end())
        playModelEffect(kitId, path->second, worldPosition, false, attachInstanceId);
}

void SpellVisualSystem::playModelEffect(uint32_t visualId, const std::string& modelPath,
                                       const glm::vec3& worldPosition, bool useImpactKit,
                                       uint32_t attachInstanceId) {
    if (!m2Renderer_) return;
    LOG_INFO("SpellVisual: ", (useImpactKit ? "impact" : "cast"), " path resolved to: ", modelPath);

    const uint32_t modelId = loadSpellModel(modelPath);
    if (modelId == 0) return;

    // Determine attachment point for bone tracking on cast effects. Only the
    // caster identified by attachInstanceId may be tracked - never default to
    // the local player (that glued every nearby unit's cast kit to the
    // player's hands).
    uint32_t attachId = 0;
    if (!useImpactKit && attachInstanceId != 0) {
        attachId = classifyAttachmentId(modelPath);
    }
    glm::vec3 spawnPos = worldPosition;
    if (attachId != 0 && renderer_) {
        auto* charRenderer = renderer_->getCharacterRenderer();
        if (charRenderer) {
            glm::mat4 attachMat;
            if (charRenderer->getAttachmentTransform(attachInstanceId, attachId, attachMat)) {
                spawnPos = glm::vec3(attachMat[3]);
            } else {
                spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
                attachId = 0;
            }
        } else {
            spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
            attachId = 0;
        }
    } else {
        spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
    }

    // Spawn instance at world position
    uint32_t instanceId = m2Renderer_->createInstance(modelId,
                                                       spawnPos,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: failed to create instance for visualId=", visualId);
        return;
    }
    m2Renderer_->restartInstanceAnimation(instanceId);
    // Determine lifetime from M2 animation duration (clamp to reasonable range)
    float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
    float duration = (animDurMs > 100.0f)
        ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
        : SPELL_VISUAL_DEFAULT_DURATION;
    activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = duration, .isPrecast = false, .attachmentId = attachId, .attachInstanceId = attachInstanceId});
    LOG_INFO("SpellVisual: spawned ", (useImpactKit ? "impact" : "cast"), " visualId=", visualId,
             " instanceId=", instanceId, " duration=", duration, "s animDurMs=", animDurMs,
             " attach=", attachId, " model=", modelPath, " active=", activeSpellVisuals_.size());

    // Hand effects: spawn a mirror copy on the caster's left hand (attachment 2)
    if (attachId == 1 /* RightHand */) {
        glm::vec3 leftPos = worldPosition;
        if (renderer_) {
            auto* cr = renderer_->getCharacterRenderer();
            if (cr) {
                glm::mat4 lm;
                if (cr->getAttachmentTransform(attachInstanceId, 2, lm))
                    leftPos = glm::vec3(lm[3]);
            }
        }
        uint32_t leftId = m2Renderer_->createInstance(modelId, leftPos, glm::vec3(0.0f), 1.0f);
        if (leftId != 0) {
            m2Renderer_->restartInstanceAnimation(leftId);
            activeSpellVisuals_.push_back({.instanceId = leftId, .elapsed = 0.0f, .duration = duration, .isPrecast = false, .attachmentId = 2 /* LeftHand */, .attachInstanceId = attachInstanceId});
        }
    }
}

bool SpellVisualSystem::hasSpellMissile(uint32_t visualId) {
    if (visualId == 0) return false;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    return spellVisualMissilePath_.count(visualId) != 0;
}

bool SpellVisualSystem::playSpellMissile(uint32_t visualId, const glm::vec3& start,
                                         const glm::vec3& end, float speed, bool impactAtEnd) {
    if (!m2Renderer_ || !hasSpellMissile(visualId)) return false;
    if (!cachedAssetManager_) return false;
    if (!std::isfinite(speed) || speed <= 0.0f) return false;

    const std::string& modelPath = spellVisualMissilePath_.at(visualId);
    const uint32_t modelId = loadSpellModel(modelPath);
    if (modelId == 0) return false;

    const uint32_t instanceId = m2Renderer_->createInstance(modelId, start, glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: failed to create missile instance for visualId=", visualId);
        return false;
    }
    m2Renderer_->restartInstanceAnimation(instanceId);

    // Speed is yards a second. A missile still in the air after a few seconds
    // has lost its target to a teleport or a corpse run; the damage landed long
    // ago, so it is put down rather than left crossing the map.
    const float distance = glm::length(end - start);
    const float duration = std::clamp(distance / speed, 0.05f, SPELL_MISSILE_MAX_DURATION);
    spellMissiles_.push_back({.instanceId = instanceId, .visualId = visualId, .start = start,
                              .end = end, .elapsed = 0.0f, .duration = duration,
                              .impactAtEnd = impactAtEnd});
    placeSpellMissile(spellMissiles_.back(), 0.0f);
    LOG_INFO("SpellVisual: missile visualId=", visualId, " model=", modelPath,
             " distance=", distance, " speed=", speed, " duration=", duration, "s");
    return true;
}

// The missile models point down +X. Turn it to face along its path, with the
// nose pitched up or down to follow a target above or below the caster.
void SpellVisualSystem::placeSpellMissile(const SpellMissile& missile, float t) {
    const glm::vec3 pos = glm::mix(missile.start, missile.end, t);
    const glm::vec3 path = missile.end - missile.start;
    const float len = glm::length(path);
    glm::vec3 rotation(0.0f);
    if (len > 1e-4f) {
        const glm::vec3 dir = path / len;
        rotation.z = std::atan2(dir.y, dir.x);
        rotation.y = -std::asin(std::clamp(dir.z, -1.0f, 1.0f));
    }
    m2Renderer_->setInstanceTransform(missile.instanceId, placementModelMatrix(pos, rotation, 1.0f));
}

void SpellVisualSystem::playPhysicalProjectile(const std::string& modelPath,
                                                const std::string& texturePath,
                                                const glm::vec3& start,
                                                const glm::vec3& end,
                                                float duration,
                                                bool spin) {
    if (!renderer_ || modelPath.empty()) return;
    auto* characterRenderer = renderer_->getCharacterRenderer();
    if (!characterRenderer) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;

    const std::string cacheKey = modelPath + "|" + texturePath;
    uint32_t modelId = 0;
    auto cached = projectileModelIds_.find(cacheKey);
    if (cached != projectileModelIds_.end()) {
        modelId = cached->second;
    } else {
        if (nextProjectileModelId_ >= 999000) return;
        modelId = nextProjectileModelId_++;
        projectileModelIds_[cacheKey] = modelId;
    }

    if (!characterRenderer->getModelData(modelId)) {
        auto bytes = cachedAssetManager_->readFile(modelPath);
        if (bytes.empty()) return;
        pipeline::M2Model model = pipeline::M2Loader::load(bytes);
        if (model.name.empty()) model.name = modelPath;
        if (model.version >= 264) {
            const std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skin = cachedAssetManager_->readFile(skinPath);
            if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, model);
        }
        if (model.vertices.empty() || !characterRenderer->loadModel(model, modelId)) return;
        if (!texturePath.empty()) {
            if (auto* texture = characterRenderer->loadTexture(texturePath))
                characterRenderer->setModelTexture(modelId, 0, texture);
        }
    }

    const glm::vec3 delta = end - start;
    const float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    glm::vec3 rotation(0.0f);
    rotation.z = std::atan2(delta.y, delta.x);
    rotation.y = -std::atan2(delta.z, std::max(horizontal, 0.001f));

    const uint32_t instanceId = characterRenderer->createInstance(modelId, start, rotation, 1.0f);
    if (instanceId == 0) return;
    physicalProjectiles_.push_back({.instanceId = instanceId, .start = start, .end = end, .rotation = rotation, .elapsed = 0.0f,
                                    .duration = std::max(duration, 0.05f), .spin = spin});
}

void SpellVisualSystem::update(float deltaTime) {
    if (auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
        std::erase_if(unitAuraVisuals_, [&](const auto& entry) {
            glm::vec3 position;
            return !characters->getInstancePosition(entry.first, position);
        });
    }
    if (activeSpellVisuals_.empty() && physicalProjectiles_.empty() && spellMissiles_.empty())
        return;

    // Collected first: the impact spawns into activeSpellVisuals_, and a
    // missile's arrival is the moment its impact kit belongs to.
    std::vector<std::pair<uint32_t, glm::vec3>> arrivals;
    for (auto it = spellMissiles_.begin(); it != spellMissiles_.end(); ) {
        it->elapsed += deltaTime;
        const float t = std::min(it->elapsed / it->duration, 1.0f);
        if (t >= 1.0f) {
            m2Renderer_->removeInstance(it->instanceId);
            if (it->impactAtEnd) arrivals.emplace_back(it->visualId, it->end);
            it = spellMissiles_.erase(it);
        } else {
            placeSpellMissile(*it, t);
            ++it;
        }
    }
    for (const auto& [visualId, pos] : arrivals)
        playSpellVisual(visualId, pos, /*useImpactKit=*/true);

    // Get character bone tracking context (once per frame)
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;

    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        if (it->auraVisualId) {
            glm::vec3 position;
            if (!charRenderer || !charRenderer->getInstancePosition(it->attachInstanceId, position)) {
                unitAuraVisuals_.erase(it->attachInstanceId);
                m2Renderer_->removeInstance(it->instanceId);
                it = activeSpellVisuals_.erase(it);
                continue;
            }
            if (!it->attachmentId) m2Renderer_->setInstancePosition(it->instanceId, position);
        }
        it->elapsed += deltaTime;
        if (it->elapsed >= it->duration) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            // Update position for bone-tracked effects - follow the CASTER's
            // hands/chest/head, not the local player's.
            if (it->attachmentId != 0 && it->attachInstanceId != 0 && charRenderer) {
                glm::mat4 attachMat;
                if (charRenderer->getAttachmentTransform(it->attachInstanceId, it->attachmentId, attachMat)) {
                    glm::vec3 bonePos = glm::vec3(attachMat[3]);
                    m2Renderer_->setInstancePosition(it->instanceId, bonePos);
                }
            }
            ++it;
        }
    }


    if (charRenderer) {
        for (auto it = physicalProjectiles_.begin(); it != physicalProjectiles_.end(); ) {
            it->elapsed += deltaTime;
            const float t = std::min(it->elapsed / it->duration, 1.0f);
            charRenderer->setInstancePosition(
                it->instanceId, glm::mix(it->start, it->end, t));
            if (it->spin) {
                glm::vec3 rotation = it->rotation;
                rotation.x += t * glm::two_pi<float>() * 2.0f;
                charRenderer->setInstanceRotation(it->instanceId, rotation);
            }
            if (t >= 1.0f) {
                charRenderer->removeInstance(it->instanceId);
                it = physicalProjectiles_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void SpellVisualSystem::cancelAllPrecastVisuals() {
    if (!m2Renderer_) return;
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        if (it->isPrecast) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            ++it;
        }
    }
}

void SpellVisualSystem::reset() {
    unitAuraVisuals_.clear();
    // Clear lingering spell visual instances from the previous map/combat session.
    // Without this, old effects could remain visible after teleport or map change.
    for (auto& sv : activeSpellVisuals_) {
        if (m2Renderer_) m2Renderer_->removeInstance(sv.instanceId);
    }
    activeSpellVisuals_.clear();
    for (const auto& missile : spellMissiles_) {
        if (m2Renderer_) m2Renderer_->removeInstance(missile.instanceId);
    }
    spellMissiles_.clear();
    if (renderer_ && renderer_->getCharacterRenderer()) {
        for (const auto& projectile : physicalProjectiles_)
            renderer_->getCharacterRenderer()->removeInstance(projectile.instanceId);
    }
    physicalProjectiles_.clear();
    // Reset the negative cache so models that failed during asset loading can retry.
    spellVisualFailedModels_.clear();
}

} // namespace rendering
} // namespace wowee
