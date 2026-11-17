#include "rendering/spell_visual_system.hpp"
#include "rendering/aura_marker_visibility.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera.hpp"
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
#include <cstdlib>
#include <limits>
#include <cstring>
#include <random>
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
    // Declared for TBC only: columns after ChannelKit were mapped from the
    // 2.4.3 data (MissileModel 7, InstantAreaKit 22 holds summon/portal kits).
    const uint32_t svInstantAreaKitField = svLayout ? svLayout->field("InstantAreaKit") : 0xFFFFFFFFu;
    const uint32_t svPersistentAreaKitField = svLayout ? svLayout->field("PersistentAreaKit") : 0xFFFFFFFFu;
    const uint32_t svMissileAttachmentField = svLayout ? svLayout->field("MissileAttachment") : 0xFFFFFFFFu;
    const uint32_t svMissileDestinationField =
        svLayout ? svLayout->field("MissileDestinationAttachment") : 0xFFFFFFFFu;
    uint32_t fxFilePathField  = fxLayout  ? (*fxLayout)["FilePath"]       : 2;

    // Kit effect fields to probe, in priority order.
    // SpecialEffect0 > BaseEffect > LeftHand > RightHand > Chest > Head > Breath
    struct KitField { const char* name; uint32_t fallback; };
    static constexpr KitField kitFieldDefs[] = {
        {.name = "SpecialEffect0",  .fallback = 11}, {.name = "BaseEffect",       .fallback = 5},
        {.name = "LeftHandEffect",   .fallback = 6}, {.name = "RightHandEffect",  .fallback = 7},
        {.name = "ChestEffect",      .fallback = 4}, {.name = "HeadEffect",       .fallback = 3},
        {.name = "BreathEffect",     .fallback = 8}, {.name = "SpecialEffect1",  .fallback = 12},
        {.name = "SpecialEffect2",  .fallback = 13}, {.name = "WorldEffect",     .fallback = 14},
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
    auto& effectPaths = effectPaths_;  // effectNameId → path
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

    // Load SpellVisualKit.dbc - every effect slot, with the attachment the
    // slot names.
    auto kitDbc = cachedAssetManager_->loadDBC("SpellVisualKit.dbc");
    // CharProc0-3 and the first parameter of each (CharParamZero0-3).
    uint32_t procFields[4], procParamFields[4];
    for (int k = 0; k < 4; ++k) {
        procFields[k] = kitLayout ? kitLayout->field("CharProc" + std::to_string(k)) : 0xFFFFFFFFu;
        procParamFields[k] = kitLayout ? kitLayout->field("CharParamZero" + std::to_string(k)) : 0xFFFFFFFFu;
    }
    if (kitDbc && kitDbc->isLoaded()) {
        uint32_t fc = kitDbc->getFieldCount();
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            uint32_t kitId = kitDbc->getUInt32(i, 0);
            if (!kitId) continue;
            kitAnimations_[kitId] = kitDbc->getUInt32(i, 2);
            // Attachment semantics come from the kit slot, not the filename.
            // HeadEffect uses the model's overhead anchor, not its animated
            // anatomical head. Mounted detection eyes and Dazed stars share
            // this slot and remain above the rider in the original client.
            // ChestEffect attaches to M2 attachment 34 (Chest); 5 is
            // ShoulderRight, which quillboar and most creatures lack, so
            // Sinister Strike's chest impact fell back to the unit's feet.
            // WorldEffect (314 TBC kits, e.g. kit 66 Entangling Roots'
            // EntanglingRoots_State) stands at the unit's position.
            // LeftHand/RightHand kit effects attach to the spell hand points
            // 21/22 (SpellLeftHand/SpellRightHand). Models with both sets put
            // 21 at the left hand (2) and 22 at the right (1); dragonwhelp.m2
            // has only 21/22, on the wings, so 2/1 found nothing and a
            // Chromatic Whelp's Lightning Bolt cast effect fell to the floor.
            static constexpr uint32_t attachments[] = {0, 0, 21, 22, 34, 18, kAttachBreath, 0, 0, 0};
            for (size_t k = 0; k < numKitFields; ++k) {
                if (kitFields[k] >= fc) continue;
                const uint32_t effectId = kitDbc->getUInt32(i, kitFields[k]);
                if (const auto path = effectPaths.find(effectId); path != effectPaths.end())
                    kitEffects_[kitId].push_back({path->second, attachments[k]});
            }
            for (int k = 0; k < 4; ++k) {
                if (procFields[k] >= fc || procParamFields[k] >= fc) continue;
                const uint32_t procedure = kitDbc->getUInt32(i, procFields[k]);
                const float param = kitDbc->getFloat(i, procParamFields[k]);
                if (procedure == kCharProcChain && param != 0.0f) {
                    assert(std::isfinite(param) && param > 0.0f && param < 65536.0f);
                    kitChainEffects_[kitId] = static_cast<uint32_t>(param);
                } else if (procedure == kCharProcColor) {
                    // 0xRRGGBB as a float: Ghost 0x8CB9FD, Frostbolt 0x3C3CFF.
                    assert(std::isfinite(param) && param >= 0.0f && param < 16777216.0f);
                    const auto rgb = static_cast<uint32_t>(param);
                    kitCharacterProcedures_[kitId].color = glm::vec3(
                        ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f);
                } else if (procedure == kCharProcOpacity) {
                    assert(std::isfinite(param) && param >= 0.0f && param <= 1.0f);
                    kitCharacterProcedures_[kitId].opacity = param;
                }
            }
        }
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
    uint32_t loadedMissile = 0;
    for (uint32_t i = 0; i < svDbc->getRecordCount(); ++i) {
        uint32_t vid = svDbc->getUInt32(i, 0);
        if (!vid) continue;
        const uint32_t channelField = (*svLayout)["ChannelKit"];
        if (channelField < svFc) channelKits_[vid] = svDbc->getUInt32(i, channelField);
        assert(svPrecastKitField < svFc);
        precastKits_[vid] = svDbc->getUInt32(i, svPrecastKitField);
        if (svCastKitField < svFc) castKits_[vid] = svDbc->getUInt32(i, svCastKitField);
        if (svImpactKitField < svFc) impactKits_[vid] = svDbc->getUInt32(i, svImpactKitField);
        if (svInstantAreaKitField < svFc) instantAreaKits_[vid] = svDbc->getUInt32(i, svInstantAreaKitField);
        if (svPersistentAreaKitField < svFc)
            persistentAreaKits_[vid] = svDbc->getUInt32(i, svPersistentAreaKitField);
        const uint32_t stateField = (*svLayout)["StateKit"];
        assert(stateField < svFc);
        stateKits_[vid] = svDbc->getUInt32(i, stateField);

        // The missile flies from caster to target. It used to stand in for a
        // missing cast or impact kit instead, parked at one end, which is why a
        // Fireball - which has both kits - never showed its fireball at all.
        if (svMissileField < svFc) {
            std::string path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) { spellVisualMissilePath_[vid] = path; ++loadedMissile; }
            if (!path.empty() && svMissileAttachmentField < svFc && svMissileDestinationField < svFc) {
                missileAttachments_[vid] = {
                    .launch = static_cast<int32_t>(svDbc->getUInt32(i, svMissileAttachmentField)),
                    .destination = svDbc->getUInt32(i, svMissileDestinationField)};
            }
        }
    }
    LOG_INFO("SpellVisual: loaded ", kitEffects_.size(), " kits with effects, missile=", loadedMissile,
             " (of ", svDbc->getRecordCount(), " visual records)");
    loadChainEffects();
}

void SpellVisualSystem::loadChainEffects() {
    auto dbc = cachedAssetManager_->loadDBC("SpellChainEffects.dbc");
    if (!dbc || !dbc->isLoaded()) {
        LOG_WARNING("SpellVisual: SpellChainEffects.dbc unavailable; channel beams are not drawn");
        return;
    }
    if (dbc->getRecordSize() != 173) {
        LOG_WARNING("SpellVisual: SpellChainEffects.dbc records are ", dbc->getRecordSize(),
                    " bytes, not the TBC 2.4.3 173; channel beams are not drawn");
        return;
    }
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        const uint8_t* record = dbc->getRecord(i);
        ChainEffect effect;
        effect.avgSegLen = dbc->getFloat(i, 1);
        effect.width = dbc->getFloat(i, 2);
        effect.texCoordScale = dbc->getFloat(i, 4);
        effect.texture = dbc->getString(i, 7);
        effect.waveHeight = dbc->getFloat(i, 19);
        effect.waveFreq = dbc->getFloat(i, 20);
        effect.waveSpeed = dbc->getFloat(i, 21);
        effect.minWaveAngle = dbc->getFloat(i, 22);
        effect.maxWaveAngle = dbc->getFloat(i, 23);
        effect.minWaveSpin = dbc->getFloat(i, 24);
        effect.maxWaveSpin = dbc->getFloat(i, 25);
        effect.alpha = record[156] / 255.0f;
        effect.color = glm::vec3(record[157], record[158], record[159]) / 255.0f;
        effect.blendMode = record[160];
        chainEffects_[dbc->getUInt32(i, 0)] = std::move(effect);
    }
    LOG_INFO("SpellVisual: loaded ", chainEffects_.size(), " chain effects, ", kitChainEffects_.size(),
             " kits draw one");
}

void SpellVisualSystem::playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                                uint32_t castTimeMs, uint32_t attachInstanceId) {
    LOG_INFO("SpellVisual: playSpellVisualPrecast visualId=", visualId,
             " pos=(", worldPosition.x, ",", worldPosition.y, ",", worldPosition.z,
             ") castTimeMs=", castTimeMs);
    if (!m2Renderer_ || visualId == 0) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) { LOG_WARNING("SpellVisual: no AssetManager"); return; }
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // An absent stage has no effect; do not substitute a different stage.
    const auto kit = precastKits_.find(visualId);
    if (kit == precastKits_.end() || !kit->second) return;
    // A precast lasts the server cast time when there is one.
    const float duration = castTimeMs >= 500
        ? std::clamp(static_cast<float>(castTimeMs) / 1000.0f, 0.5f, 30.0f) : -1.0f;
    spawnKitEffects(kit->second, worldPosition, attachInstanceId, duration, /*isPrecast=*/true);
}

// Every effect a kit names, each at the attachment its SpellVisualKit slot
// names: LeftHand 21, RightHand 22, Chest 34, Head 18 (overhead), Breath 17
// (oriented), Base and Special slots at the unit's position.
void SpellVisualSystem::spawnKitEffects(uint32_t kitId, const glm::vec3& worldPosition,
                                        uint32_t attachInstanceId, float duration, bool isPrecast) {
    const auto effects = kitEffects_.find(kitId);
    if (effects == kitEffects_.end()) return;
    auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    for (const auto& effect : effects->second) {
        const uint32_t modelId = loadSpellModel(effect.path);
        if (modelId == 0) continue;
        uint32_t attachment = attachInstanceId ? effect.attachment : 0;
        glm::mat4 attachMat(1.0f);
        if (attachment != 0 &&
            (!characters || !characters->getAttachmentTransform(attachInstanceId, attachment, attachMat))) {
            LOG_WARNING("SpellVisual: kit=", kitId, " attachment ", attachment,
                        " unavailable on instance ", attachInstanceId, "; placed at unit position, model=",
                        effect.path);
            attachment = 0;
        }
        const bool breath = attachment == kAttachBreath;
        const glm::vec3 spawn = attachment ? glm::vec3(attachMat[3]) : worldPosition;
        const uint32_t instanceId = breath
            ? m2Renderer_->createInstanceWithMatrix(modelId, attachMat, spawn)
            : m2Renderer_->createInstance(modelId, spawn, glm::vec3(0.0f), 1.0f);
        if (instanceId == 0) {
            LOG_WARNING("SpellVisual: instance failed for kit=", kitId, " model=", effect.path);
            continue;
        }
        m2Renderer_->restartInstanceAnimation(instanceId);
        // Without a stage length (cast time, aura, channel) the effect lives
        // one play of the sequence it shows. Authored lengths run from 0.33 s
        // to 31 s across TBC kit models (Moonfire_Impact_Base and
        // Flamestrike_Impact_Base 12 s); no cap or floor is in the data.
        float lifetime = duration;
        if (lifetime <= 0.0f) {
            const float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
            if (animDurMs <= 0.0f) {
                // Static props (BookSmall01, BreadSlice) have a zero-length
                // Stand; nothing says how long they stay.
                LOG_WARNING("SpellVisual: kit=", kitId, " model ", effect.path,
                            " has no animation length; not shown");
            }
            lifetime = animDurMs / 1000.0f;
        }
        activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = lifetime,
            .isPrecast = isPrecast, .attachmentId = attachment, .attachInstanceId = attachInstanceId,
            .orientWithAttachment = breath});
        if (!breath && attachInstanceId) placeOnUnit(activeSpellVisuals_.back(), characters, spawn);
        LOG_INFO("SpellVisual: kit=", kitId, " slot attachment=", effect.attachment, " used=", attachment,
                 " instanceId=", instanceId, " duration=", lifetime, "s model=", effect.path);
    }
}

// One SpellVisualEffectName model at one attachment of a unit, oriented by
// the attachment and following it for the model's own animation length.
void SpellVisualSystem::playEffectAtAttachment(uint32_t effectNameId, uint32_t attachInstanceId,
                                               uint32_t attachment) {
    assert(effectNameId != 0 && attachInstanceId != 0 && attachment != 0);
    if (!m2Renderer_) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto path = effectPaths_.find(effectNameId);
    if (path == effectPaths_.end()) {
        LOG_ERROR("SpellVisualEffectName ", effectNameId, " not in SpellVisualEffectName.dbc");
        return;
    }
    auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    glm::mat4 attachMat(1.0f);
    if (!characters || !characters->getAttachmentTransform(attachInstanceId, attachment, attachMat)) {
        LOG_WARNING("SpellVisual: attachment ", attachment, " missing on instance ", attachInstanceId,
                    "; effect ", effectNameId, " not shown: ", path->second);
        return;
    }
    const uint32_t modelId = loadSpellModel(path->second);
    if (modelId == 0) return;
    const uint32_t instanceId = m2Renderer_->createInstanceWithMatrix(modelId, attachMat, glm::vec3(attachMat[3]));
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: instance failed for effect ", effectNameId, " model=", path->second);
        return;
    }
    m2Renderer_->restartInstanceAnimation(instanceId);
    // One play of the model's sequence, as for kit effects.
    const float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
    if (animDurMs <= 0.0f)
        LOG_WARNING("SpellVisual: effect ", effectNameId, " model ", path->second, " has no animation length");
    const float lifetime = animDurMs / 1000.0f;
    activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = lifetime,
        .isPrecast = false, .attachmentId = attachment, .attachInstanceId = attachInstanceId,
        .orientWithAttachment = true});
    LOG_DEBUG("SpellVisual: effect=", effectNameId, " attachment=", attachment, " instance=",
              attachInstanceId, " duration=", lifetime, "s model=", path->second);
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
    const auto& kits = useImpactKit ? impactKits_ : castKits_;
    const auto kit = kits.find(visualId);
    if (kit == kits.end() || !kit->second) return;
    // An impact kit's animation plays on the unit it lands on, as a kit sent
    // by SMSG_PLAY_SPELL_IMPACT does: General Drakkisath's Knockdown (16790)
    // and Backhand (6253) impact kit 348 throws the target down with
    // Knockdown (121).
    if (useImpactKit) {
        playSpellVisualKit(kit->second, worldPosition, attachInstanceId);
        return;
    }
    spawnKitEffects(kit->second, worldPosition, attachInstanceId, -1.0f, /*isPrecast=*/false);
}

uint32_t SpellVisualSystem::kitAnimation(const std::unordered_map<uint32_t, uint32_t>& kits,
                                         uint32_t visualId) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto kit = kits.find(visualId);
    if (kit == kits.end() || !kit->second) return kNoAnimation;
    const auto animation = kitAnimations_.find(kit->second);
    return animation == kitAnimations_.end() ? kNoAnimation : animation->second;
}

uint32_t SpellVisualSystem::precastAnimation(uint32_t visualId) { return kitAnimation(precastKits_, visualId); }
uint32_t SpellVisualSystem::castAnimation(uint32_t visualId) { return kitAnimation(castKits_, visualId); }
uint32_t SpellVisualSystem::channelAnimation(uint32_t visualId) { return kitAnimation(channelKits_, visualId); }

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
        glm::mat4 transform(1.0f);
        if (effect.attachment) {
            if (!characters || !characters->getAttachmentTransform(attachInstanceId, effect.attachment, transform)) {
                LOG_ERROR("Channel attachment unavailable: ", effect.attachment, " model=", effect.path);
                continue;
            }
            spawn = glm::vec3(transform[3]);
        }
        const bool breath = effect.attachment == kAttachBreath;
        const uint32_t instance = breath
            ? m2Renderer_->createInstanceWithMatrix(model, transform, spawn)
            : m2Renderer_->createInstance(model, spawn, glm::vec3(0), 1.0f);
        if (!instance) { LOG_ERROR("Channel effect instance failed: ", effect.path); continue; }
        m2Renderer_->restartInstanceAnimation(instance);
        activeSpellVisuals_.push_back({.instanceId = instance, .elapsed = 0.0f,
            .duration = durationMs / 1000.0f, .isPrecast = false, .attachmentId = effect.attachment,
            .attachInstanceId = attachInstanceId, .isChannel = true, .orientWithAttachment = breath});
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
            placeOnUnit(activeSpellVisuals_.back(), renderer_ ? renderer_->getCharacterRenderer() : nullptr,
                        position);
            LOG_INFO("Aura visual: owner=", attachInstanceId, " visual=", visual,
                     " kit=", kit->second, " attachment=", effect.attachment, " model=", effect.path);
        }
    }
    std::vector<uint32_t> applied;
    if (found != unitAuraVisuals_.end()) {
        for (uint32_t visual : found->second.applied)
            if (std::find(visualIds.begin(), visualIds.end(), visual) != visualIds.end())
                applied.push_back(visual);
    }
    for (uint32_t visual : visualIds)
        if (std::find(applied.begin(), applied.end(), visual) == applied.end())
            applied.push_back(visual);
    applyCharacterProcedures(attachInstanceId, applied);
    if (visualIds.empty()) unitAuraVisuals_.erase(attachInstanceId);
    else unitAuraVisuals_[attachInstanceId] = {visualIds, observerConcealed, std::move(applied)};
}

uint32_t SpellVisualSystem::auraStateAnimation(uint32_t attachInstanceId) const {
    const auto unit = unitAuraVisuals_.find(attachInstanceId);
    if (unit == unitAuraVisuals_.end()) return kNoAnimation;
    for (auto visual = unit->second.applied.rbegin(); visual != unit->second.applied.rend(); ++visual) {
        const auto kit = stateKits_.find(*visual);
        if (kit == stateKits_.end() || !kit->second) continue;
        const auto animation = kitAnimations_.find(kit->second);
        if (animation != kitAnimations_.end() && animation->second != kNoAnimation)
            return animation->second;
    }
    return kNoAnimation;
}

void SpellVisualSystem::applyCharacterProcedures(uint32_t attachInstanceId,
                                                 const std::vector<uint32_t>& applied) {
    glm::vec3 color(1.0f);
    float opacity = 1.0f;
    bool colored = false, faded = false;
    for (auto visual = applied.rbegin(); visual != applied.rend() && !(colored && faded); ++visual) {
        const auto kit = stateKits_.find(*visual);
        if (kit == stateKits_.end() || !kit->second) continue;
        const auto procedures = kitCharacterProcedures_.find(kit->second);
        if (procedures == kitCharacterProcedures_.end()) continue;
        if (!colored && procedures->second.color) { color = *procedures->second.color; colored = true; }
        if (!faded && procedures->second.opacity) { opacity = *procedures->second.opacity; faded = true; }
    }
    LOG_DEBUG("Kit appearance: owner=", attachInstanceId, " visuals=", applied.size(),
              " color=(", color.r, ",", color.g, ",", color.b, ") opacity=", opacity);
    renderer_->getCharacterRenderer()->setInstanceKitAppearance(attachInstanceId, color, opacity);
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

void SpellVisualSystem::syncChannels(const std::vector<UnitChannel>& channels) {
    if (!m2Renderer_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    if (!spellVisualDbcLoaded_) return;

    // ChannelKit effects of channels seen only through the fields. A unit
    // whose MSG_CHANNEL_START was received already has them, for that
    // channel's duration; startChannelVisual replaces held ones.
    std::unordered_map<uint32_t, uint32_t> current;
    for (const auto& channel : channels)
        if (channel.casterInstanceId) current[channel.casterInstanceId] = channel.visualId;
    for (auto it = fieldChannels_.begin(); it != fieldChannels_.end();) {
        const auto now = current.find(it->first);
        if (now == current.end() || now->second != it->second) {
            if (heldChannelKits_.erase(it->first)) updateChannelVisual(it->first, 0);
            it = fieldChannels_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& channel : channels) {
        if (!channel.casterInstanceId || fieldChannels_.count(channel.casterInstanceId)) continue;
        fieldChannels_[channel.casterInstanceId] = channel.visualId;
        const bool playing = std::any_of(activeSpellVisuals_.begin(), activeSpellVisuals_.end(),
            [&](const SpellVisualInstance& v) { return v.isChannel && v.attachInstanceId == channel.casterInstanceId; });
        if (playing) continue;
        startChannelVisual(channel.visualId, channel.casterPosition, std::numeric_limits<uint32_t>::max(),
                           channel.casterInstanceId);
        heldChannelKits_.insert(channel.casterInstanceId);
    }

    std::vector<M2Renderer::ChainStrip> strips;
    std::unordered_set<uint64_t> live;
    static thread_local std::mt19937 random{std::random_device{}()};
    for (const auto& channel : channels) {
        if (!channel.hasTarget) continue;
        const auto kit = channelKits_.find(channel.visualId);
        if (kit == channelKits_.end() || !kit->second) continue;
        const auto chainId = kitChainEffects_.find(kit->second);
        if (chainId == kitChainEffects_.end()) continue;
        const auto found = chainEffects_.find(chainId->second);
        assert(found != chainEffects_.end());
        const ChainEffect& effect = found->second;
        if (effect.blendMode != 2 && effect.blendMode != 3 && effect.blendMode != 4) {
            static std::unordered_set<uint32_t> said;
            if (said.insert(chainId->second).second)
                LOG_WARNING("SpellVisual: chain effect ", chainId->second, " blend mode ", int(effect.blendMode),
                            " is not drawn (only 2, 3 and 4 are implemented)");
            continue;
        }
        live.insert(channel.casterGuid);
        auto [state, created] = beams_.try_emplace(channel.casterGuid);
        if (created) {
            state->second.age = beamClock_;
            state->second.waveAngle = std::uniform_real_distribution<float>(
                std::min(effect.minWaveAngle, effect.maxWaveAngle), std::max(effect.minWaveAngle, effect.maxWaveAngle))(random);
            state->second.waveSpin = std::uniform_real_distribution<float>(
                std::min(effect.minWaveSpin, effect.maxWaveSpin), std::max(effect.minWaveSpin, effect.maxWaveSpin))(random);
            LOG_INFO("Channel beam: caster=0x", std::hex, channel.casterGuid, std::dec, " visual=", channel.visualId,
                     " kit=", kit->second, " chain=", chainId->second, " texture=", effect.texture);
        }
        const float t = beamClock_ - state->second.age;
        const glm::vec3 axis = channel.end - channel.start;
        const float length = glm::length(axis);
        if (length < 1e-3f) continue;
        const glm::vec3 dir = axis / length;
        // The wave's plane: world up made perpendicular to the beam, turned
        // about the beam by the wave angle (and its spin over time).
        glm::vec3 up = glm::vec3(0, 0, 1) - dir * dir.z;
        if (glm::length(up) < 1e-4f) up = glm::vec3(1, 0, 0) - dir * dir.x;
        up = glm::normalize(up);
        const float angle = state->second.waveAngle + state->second.waveSpin * t;
        const glm::vec3 across = glm::normalize(up * std::cos(angle) + glm::cross(dir, up) * std::sin(angle));
        const int segments = std::max(1, static_cast<int>(std::ceil(length / std::max(effect.avgSegLen, 0.05f))));
        M2Renderer::ChainStrip strip;
        strip.texturePath = effect.texture;
        strip.additive = effect.blendMode != 2;
        strip.color = effect.color;
        strip.alpha = effect.alpha;
        // Width is half the strip: Green Beam (0.25) measured 6.5 px across
        // in the original against 3.1 px drawn 0.25 wide (close0 capture).
        strip.halfWidth = effect.width;
        for (int i = 0; i <= segments; ++i) {
            const float s = length * static_cast<float>(i) / static_cast<float>(segments);
            const float wave = effect.waveHeight * std::sin(effect.waveFreq * s - effect.waveSpeed * t);
            strip.spine.push_back(channel.start + dir * s + across * wave);
            // TexCoordScale repeats of the texture over the whole beam
            // (Green Beam -2: SoulBeam.blp twice, reversed). Repeating it
            // every TextureLength (2) yards showed the texture's own ripple
            // as a zigzag the original does not have.
            strip.u.push_back(effect.texCoordScale * s / length);
        }
        strips.push_back(std::move(strip));
    }
    std::erase_if(beams_, [&](const auto& entry) { return !live.count(entry.first); });
    m2Renderer_->setChainStrips(std::move(strips));
}

void SpellVisualSystem::playInstantAreaKit(uint32_t visualId, const glm::vec3& casterPosition,
                                           uint32_t casterInstanceId) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto kit = instantAreaKits_.find(visualId);
    if (kit == instantAreaKits_.end() || !kit->second) return;
    LOG_DEBUG("SpellVisual: instant area kit ", kit->second, " of visual ", visualId,
              " at caster instance ", casterInstanceId);
    // Unit slots attach to the caster; Base/World slots stand where it stood.
    spawnKitEffects(kit->second, casterPosition, casterInstanceId, /*duration=*/-1.0f,
                    /*isPrecast=*/false);
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
            renderer_->getAnimationController()->playKitAnimation(animation->second);
        } else {
            cr->playAnimation(attachInstanceId, animation->second, false);
        }
    }
    LOG_DEBUG("SpellVisualKit: id=", kitId, " animation=", animation->second);
    spawnKitEffects(kitId, worldPosition, attachInstanceId, -1.0f, /*isPrecast=*/false);
}

// Flamestrike 2120: visual 10383, PersistentAreaKit 9357 BaseEffect
// Flamestrike_Impact_Base, the burning ground the original keeps under the
// area for the DynamicObject's life (8 s). The model loops its own sequence;
// it is removed with the object, not on a timer.
void SpellVisualSystem::playPersistentArea(uint64_t key, uint32_t visualId, const glm::vec3& worldPosition) {
    if (!m2Renderer_ || visualId == 0) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    stopPersistentArea(key);
    const auto kit = persistentAreaKits_.find(visualId);
    if (kit == persistentAreaKits_.end() || kit->second == 0) return;
    const auto effects = kitEffects_.find(kit->second);
    if (effects == kitEffects_.end()) {
        LOG_WARNING("SpellVisual: persistent area kit ", kit->second, " of visual ", visualId, " has no models");
        return;
    }
    auto& instances = persistentAreas_[key];
    for (const auto& effect : effects->second) {
        const uint32_t modelId = loadSpellModel(effect.path);
        if (modelId == 0) continue;
        const uint32_t instanceId = m2Renderer_->createInstance(modelId, worldPosition, glm::vec3(0.0f), 1.0f);
        if (instanceId == 0) {
            LOG_WARNING("SpellVisual: persistent area instance failed, model=", effect.path);
            continue;
        }
        m2Renderer_->restartInstanceAnimation(instanceId);
        instances.push_back(instanceId);
        LOG_INFO("SpellVisual: persistent area 0x", std::hex, key, std::dec, " visual=", visualId,
                 " kit=", kit->second, " model=", effect.path);
    }
}

void SpellVisualSystem::stopPersistentArea(uint64_t key) {
    const auto it = persistentAreas_.find(key);
    if (it == persistentAreas_.end()) return;
    if (m2Renderer_)
        for (uint32_t instanceId : it->second) m2Renderer_->removeInstance(instanceId);
    persistentAreas_.erase(it);
}

bool SpellVisualSystem::hasSpellMissile(uint32_t visualId) {
    if (visualId == 0) return false;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    return spellVisualMissilePath_.count(visualId) != 0;
}

std::optional<SpellVisualSystem::MissileAttachments> SpellVisualSystem::missileAttachments(uint32_t visualId) {
    if (!hasSpellMissile(visualId)) return std::nullopt;
    const auto it = missileAttachments_.find(visualId);
    if (it == missileAttachments_.end()) return std::nullopt;
    return it->second;
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

// An effect on a unit faces the way the unit faces: its model's +X turns with
// the unit's yaw. Placed by position alone, every effect faced world +X, so an
// asymmetric one (Scarlet Myrmidon's enrage, a breath cone) pointed sideways
// for most headings.
void SpellVisualSystem::placeOnUnit(const SpellVisualInstance& visual, CharacterRenderer* characters,
                                    const glm::vec3& position) {
    float yaw = 0.0f;
    if (!characters || !visual.attachInstanceId ||
        !characters->getInstanceYaw(visual.attachInstanceId, yaw)) {
        m2Renderer_->setInstancePosition(visual.instanceId, position);
        return;
    }
    const glm::mat4 transform = glm::rotate(glm::translate(glm::mat4(1.0f), position), yaw,
                                            glm::vec3(0.0f, 0.0f, 1.0f));
    m2Renderer_->setInstanceTransform(visual.instanceId, transform);
}

void SpellVisualSystem::update(float deltaTime) {
    beamClock_ += deltaTime;
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
            if (!it->attachmentId) placeOnUnit(*it, charRenderer, position);
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
                    if (it->orientWithAttachment) m2Renderer_->setInstanceTransform(it->instanceId, attachMat);
                    else placeOnUnit(*it, charRenderer, bonePos);
                    static const bool traceAttachments = std::getenv("WOWEE_TRACE_AURA_ATTACHMENT") != nullptr;
                    if (traceAttachments && it->auraVisualId &&
                        std::floor(it->elapsed) != std::floor(it->elapsed - deltaTime)) {
                        glm::vec3 ownerPosition;
                        glm::mat4 above;
                        glm::mat4 head;
                        const auto* camera = renderer_->getCamera();
                        if (camera && charRenderer->getInstancePosition(it->attachInstanceId, ownerPosition) &&
                            charRenderer->getAttachmentTransform(it->attachInstanceId, 18, above) &&
                            charRenderer->getAttachmentTransform(it->attachInstanceId, 11, head)) {
                            const auto viewProjection = camera->getProjectionMatrix() * camera->getViewMatrix();
                            const auto headClip = viewProjection * head[3];
                            const auto aboveClip = viewProjection * above[3];
                            LOG_INFO("Aura attachment trace: owner=", it->attachInstanceId,
                                " visual=", it->auraVisualId, " attachment=", it->attachmentId,
                                " elapsed=", it->elapsed,
                                " ownerXYZ=", ownerPosition.x, ",", ownerPosition.y, ",", ownerPosition.z,
                                " anchorXYZ=", bonePos.x, ",", bonePos.y, ",", bonePos.z,
                                " headXYZ=", head[3].x, ",", head[3].y, ",", head[3].z,
                                " aboveXYZ=", above[3].x, ",", above[3].y, ",", above[3].z,
                                " headClip=", headClip.x, ",", headClip.y, ",", headClip.z, ",", headClip.w,
                                " aboveClip=", aboveClip.x, ",", aboveClip.y, ",", aboveClip.z, ",", aboveClip.w,
                                " anchorScale=", glm::length(glm::vec3(attachMat[0])), ",",
                                    glm::length(glm::vec3(attachMat[1])), ",", glm::length(glm::vec3(attachMat[2])));
                        }
                    }
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
    beams_.clear();
    fieldChannels_.clear();
    heldChannelKits_.clear();
    if (m2Renderer_) m2Renderer_->setChainStrips({});
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
    for (const auto& [key, instances] : persistentAreas_) {
        if (m2Renderer_)
            for (uint32_t instanceId : instances) m2Renderer_->removeInstance(instanceId);
    }
    persistentAreas_.clear();
    // Reset the negative cache so models that failed during asset loading can retry.
    spellVisualFailedModels_.clear();
}

} // namespace rendering
} // namespace wowee
