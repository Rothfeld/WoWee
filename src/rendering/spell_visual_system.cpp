#include "rendering/spell_visual_system.hpp"
#include "rendering/aura_marker_visibility.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/freeze_anim_policy.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/chain_effect.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/placement_transform.hpp"
#include "rendering/projected_texture.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entries.hpp"
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

namespace {
// SpellVisualKit CharProc 9's CharParamZero, by index. The same names stand in
// this order in the 2.4.3 executable's string data, and the DBC agrees: kits
// with this procedure name the model of their index as their BaseEffect too -
// 0 Blizzard kit 9350 Blizzard_Impact_Base, 1 Rain of Fire 9352
// RainOfFire_Impact_Base, 2 Revelations 9261 CallLightning_Impact, 4 Death &
// Decay 9359 DeathAndDecay_Area_Base, 5 Volley 9356 ArcaneShot_Area, 6 Starfall
// 9364 StarShards_Impact_Base, 7 Rain of Chaos 9383 Fel_RainOfFire_Impact_Base,
// 8 Debris 9110 HellfireRaid_Dust_Impact_Base (Cave In 9373 drops these rocks
// in the original). No TBC kit uses 3.
constexpr const char* kAreaImpactModels[] = {
    "Spells\\Blizzard_Impact_Base.m2",
    "Spells\\RainOfFire_Impact_Base.m2",
    "Spells\\CallLightning_Impact.m2",
    "Spells\\FlamestrikeSmall_Impact_Base.m2",
    "Spells\\DeathAndDecay_Area_Base.m2",
    "Spells\\ArcaneShot_Area.m2",
    "Spells\\StarShards_Impact_Base.m2",
    "Spells\\Fel_RainOfFire_Impact_Base.m2",
    "Spells\\HellfireRaid_Dust_Impact_Base.m2",
};

// The size of a unit attachment's frame: the unit's model scale (object x
// display x model scale) times the authored scale of the attachment's bone.
float attachmentScale(const glm::mat4& attachment) {
    return std::cbrt(std::abs(glm::determinant(glm::mat3(attachment))));
}
}  // namespace

// SpellVisualEffectName.Scale: the size an effect row is drawn at. One model
// has several rows at different sizes - Shock_Missile is 1, 4 and 8 (Void
// Reaver's Arcane Orb, 34172 visual 8159, uses the 8), icebarrier_state 0.5,
// 1, 2 and 4.
float SpellVisualSystem::effectScale(uint32_t effectNameId) const {
    const auto found = effectScales_.find(effectNameId);
    return found != effectScales_.end() ? found->second : 1.0f;
}

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
    // TBC columns 13/14 (Burn 43217's visual 9727: kit 8255 on the caster,
    // 213 on each unit hit). Absent from other layouts.
    const uint32_t svCasterImpactKitField = svLayout ? svLayout->field("CasterImpactKit") : 0xFFFFFFFFu;
    const uint32_t svTargetImpactKitField = svLayout ? svLayout->field("TargetImpactKit") : 0xFFFFFFFFu;
    const uint32_t svImpactAreaKitField = svLayout ? svLayout->field("ImpactAreaKit") : 0xFFFFFFFFu;
    const uint32_t svMissileAttachmentField = svLayout ? svLayout->field("MissileAttachment") : 0xFFFFFFFFu;
    const uint32_t svMissileDestinationField =
        svLayout ? svLayout->field("MissileDestinationAttachment") : 0xFFFFFFFFu;
    uint32_t fxFilePathField  = fxLayout  ? (*fxLayout)["FilePath"]       : 2;
    const uint32_t fxScaleField = fxLayout ? fxLayout->field("Scale") : 0xFFFFFFFFu;
    if (fxScaleField == 0xFFFFFFFFu)
        LOG_WARNING("SpellVisual: SpellVisualEffectName layout lacks Scale; every effect drawn at 1");

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
            if (fxScaleField < fxDbc->getFieldCount()) {
                const float scale = fxDbc->getFloat(i, fxScaleField);
                assert(std::isfinite(scale) && scale > 0.0f);
                effectScales_[id] = scale;
            }
        }
    }

    // Load SpellVisualKit.dbc - every effect slot, with the attachment the
    // slot names.
    auto kitDbc = cachedAssetManager_->loadDBC("SpellVisualKit.dbc");
    // CharProc0-3 and their parameters (CharParamZero0-3..CharParamThree0-3).
    uint32_t procFields[4], procParamFields[4], procParamOneFields[4],
        procParamTwoFields[4], procParamThreeFields[4];
    for (int k = 0; k < 4; ++k) {
        procFields[k] = kitLayout ? kitLayout->field("CharProc" + std::to_string(k)) : 0xFFFFFFFFu;
        procParamFields[k] = kitLayout ? kitLayout->field("CharParamZero" + std::to_string(k)) : 0xFFFFFFFFu;
        procParamOneFields[k] = kitLayout ? kitLayout->field("CharParamOne" + std::to_string(k)) : 0xFFFFFFFFu;
        procParamTwoFields[k] = kitLayout ? kitLayout->tryField("CharParamTwo" + std::to_string(k)) : 0xFFFFFFFFu;
        procParamThreeFields[k] = kitLayout ? kitLayout->tryField("CharParamThree" + std::to_string(k)) : 0xFFFFFFFFu;
    }
    if (kitDbc && kitDbc->isLoaded()) {
        uint32_t fc = kitDbc->getFieldCount();
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            uint32_t kitId = kitDbc->getUInt32(i, 0);
            if (!kitId) continue;
            kitAnimations_[kitId] = kitDbc->getUInt32(i, 2);
            if (fc > 15) {
                if (const uint32_t soundId = kitDbc->getUInt32(i, 15); soundId != 0)
                    kitSounds_[kitId] = soundId;
            }
            if (fc > 16) {
                if (const uint32_t shakeId = kitDbc->getUInt32(i, 16); shakeId != 0)
                    kitShakes_[kitId] = shakeId;
            }
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
            // SpecialEffect0-2 attach to M2 Special1-3 (23-25): drake.m2 carries
            // 23 at its head, where native starts Ebonroc's Flame Breath (kit
            // 5851 SpecialEffect0, NefarianFlameBreath.m2); the unit position
            // put it at his feet.
            // BaseEffect attaches to M2 attachment 19 (Base). Its bone carries
            // an authored scale that the original applies to the effect:
            // GnomeMale 0.5, HumanMale 1, Infernal 1.8, Dragon 5.5. Ice
            // Block's cube (BaseEffect icebarrier_state) measured about 4 yd
            // times that scale times the unit's own scale on each of them.
            static constexpr uint32_t attachments[] = {23, 19, 21, 22, 34, 18, kAttachBreath, 24, 25, 0};
            for (size_t k = 0; k < numKitFields; ++k) {
                if (kitFields[k] >= fc) continue;
                const uint32_t effectId = kitDbc->getUInt32(i, kitFields[k]);
                if (const auto path = effectPaths.find(effectId); path != effectPaths.end())
                    kitEffects_[kitId].push_back({path->second, attachments[k], effectScale(effectId)});
            }
            for (int k = 0; k < 4; ++k) {
                if (procFields[k] >= fc || procParamFields[k] >= fc) continue;
                const uint32_t procedure = kitDbc->getUInt32(i, procFields[k]);
                const float param = kitDbc->getFloat(i, procParamFields[k]);
                if ((procedure == kCharProcChain || procedure == kCharProcCastChain) && param != 0.0f) {
                    assert(std::isfinite(param) && param > 0.0f && param < 65536.0f);
                    const bool first = kitChainEffects_.emplace(kitId, static_cast<uint32_t>(param)).second;
                    assert(first && "one beam per kit");
                    (void)first;
                } else if (procedure == kCharProcColor) {
                    // 0xRRGGBB as a float: Ghost 0x8CB9FD, Frostbolt 0x3C3CFF.
                    assert(std::isfinite(param) && param >= 0.0f && param < 16777216.0f);
                    const auto rgb = static_cast<uint32_t>(param);
                    kitCharacterProcedures_[kitId].color = glm::vec3(
                        ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f);
                } else if (procedure == kCharProcLight) {
                    // Shadow Image Visual kit 7441: 0x000000 for 1e11 s; Chain
                    // Lightning impact kit 282: 0x6C6CFF for 0.5 s. Kits 7003 and
                    // 6957 give 0 s, which lights nothing.
                    assert(std::isfinite(param) && param >= 0.0f && param < 16777216.0f);
                    assert(procParamOneFields[k] < fc);
                    const float seconds = kitDbc->getFloat(i, procParamOneFields[k]);
                    assert(std::isfinite(seconds) && seconds >= 0.0f);
                    const auto rgb = static_cast<uint32_t>(param);
                    if (seconds > 0.0f)
                        kitLights_[kitId] = {.color = glm::vec3(((rgb >> 16) & 0xFF) / 255.0f,
                                                                ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f),
                                             .seconds = seconds};
                } else if (procedure == kCharProcOpacity) {
                    assert(std::isfinite(param) && param >= 0.0f && param <= 1.0f);
                    kitCharacterProcedures_[kitId].opacity = param;
                } else if (charProcFreezesAnimation(procedure)) {
                    kitCharacterProcedures_[kitId].freezeAnimation = true;
                } else if (procedure == kCharProcAreaImpacts) {
                    if (procParamOneFields[k] >= fc) {
                        static bool said = false;
                        if (!said) LOG_WARNING("SpellVisual: SpellVisualKit layout lacks CharParamOne", k,
                                               "; area impacts (Blizzard shards, Cave In rocks) are not drawn");
                        said = true;
                        continue;
                    }
                    const float perSecond = kitDbc->getFloat(i, procParamOneFields[k]);
                    assert(param >= 0.0f && param < std::size(kAreaImpactModels) && param == std::floor(param));
                    assert(std::isfinite(perSecond) && perSecond > 0.0f);
                    kitAreaImpacts_[kitId] = {.path = kAreaImpactModels[static_cast<size_t>(param)],
                                              .perSecond = perSecond};
                } else if (procedure == kCharProcWeaponTrail) {
                    if (procParamOneFields[k] >= fc || procParamTwoFields[k] >= fc || procParamThreeFields[k] >= fc) {
                        static bool said = false;
                        if (!said) LOG_WARNING("SpellVisual: SpellVisualKit layout lacks CharParamOne/Two/Three", k,
                                               "; weapon swing trails (CharProc 8) are not drawn");
                        said = true;
                        continue;
                    }
                    const float p1 = kitDbc->getFloat(i, procParamOneFields[k]);
                    const float p2 = kitDbc->getFloat(i, procParamTwoFields[k]);
                    const float p3 = kitDbc->getFloat(i, procParamThreeFields[k]);
                    if (const auto spec = parseWeaponTrailCharProc(param, p1, p2, p3)) {
                        kitWeaponTrails_[kitId] = *spec;
                    }
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
        if (svImpactAreaKitField < svFc) impactAreaKits_[vid] = svDbc->getUInt32(i, svImpactAreaKitField);
        if (svCasterImpactKitField < svFc) casterImpactKits_[vid] = svDbc->getUInt32(i, svCasterImpactKitField);
        if (svTargetImpactKitField < svFc) targetImpactKits_[vid] = svDbc->getUInt32(i, svTargetImpactKitField);
        if (svPersistentAreaKitField < svFc)
            persistentAreaKits_[vid] = svDbc->getUInt32(i, svPersistentAreaKitField);
        const uint32_t stateField = (*svLayout)["StateKit"];
        assert(stateField < svFc);
        stateKits_[vid] = svDbc->getUInt32(i, stateField);

        if (svMissileAttachmentField < svFc)
            launchAttachments_[vid] = static_cast<int32_t>(svDbc->getUInt32(i, svMissileAttachmentField));

        // The missile flies from caster to target. It used to stand in for a
        // missing cast or impact kit instead, parked at one end, which is why a
        // Fireball - which has both kits - never showed its fireball at all.
        if (svMissileField < svFc) {
            std::string path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) {
                spellVisualMissilePath_[vid] = path;
                spellVisualMissileScale_[vid] = effectScale(svDbc->getUInt32(i, svMissileField));
                ++loadedMissile;
            }
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
    loadCameraShakes();
}

void SpellVisualSystem::loadCameraShakes() {
    auto effDbc = cachedAssetManager_->loadDBC("SpellEffectCameraShakes.dbc");
    if (effDbc && effDbc->isLoaded() && effDbc->getFieldCount() >= 4) {
        for (uint32_t i = 0; i < effDbc->getRecordCount(); ++i) {
            const uint32_t id = effDbc->getUInt32(i, 0);
            if (!id) continue;
            spellEffectShakes_[id] = {effDbc->getUInt32(i, 1), effDbc->getUInt32(i, 2), effDbc->getUInt32(i, 3)};
        }
    }
    auto csDbc = cachedAssetManager_->loadDBC("CameraShakes.dbc");
    if (csDbc && csDbc->isLoaded() && csDbc->getFieldCount() >= 6) {
        for (uint32_t i = 0; i < csDbc->getRecordCount(); ++i) {
            const uint32_t id = csDbc->getUInt32(i, 0);
            if (!id) continue;
            const float amp = csDbc->getFloat(i, 3);
            const float freq = csDbc->getFloat(i, 4);
            const float dur = csDbc->getFloat(i, 5);
            if (std::isfinite(amp) && std::isfinite(freq) && std::isfinite(dur) && amp > 0.0f && dur > 0.0f) {
                cameraShakes_[id] = {.amplitude = amp, .frequency = freq, .duration = dur};
            }
        }
    }
    LOG_INFO("SpellVisual: loaded ", spellEffectShakes_.size(), " effect shakes, ",
             cameraShakes_.size(), " camera shakes, ", kitShakes_.size(), " kits shake");
}

void SpellVisualSystem::playKitShake(uint32_t kitId, const glm::vec3& worldPosition) {
    if (!renderer_) return;
    auto* camCtrl = renderer_->getCameraController();
    if (!camCtrl) return;
    const auto ksIt = kitShakes_.find(kitId);
    if (ksIt == kitShakes_.end()) return;
    const auto esIt = spellEffectShakes_.find(ksIt->second);
    if (esIt == spellEffectShakes_.end()) return;

    float maxAmp = 0.0f;
    float bestFreq = 0.0f;
    float maxDur = 0.0f;
    for (uint32_t csId : esIt->second) {
        if (!csId) continue;
        const auto csIt = cameraShakes_.find(csId);
        if (csIt == cameraShakes_.end()) continue;
        if (csIt->second.amplitude > maxAmp) {
            maxAmp = csIt->second.amplitude;
            bestFreq = csIt->second.frequency;
        }
        maxDur = std::max(maxDur, csIt->second.duration);
    }
    if (maxAmp <= 0.0f || maxDur <= 0.0f) return;

    // Attenuate spell camera shakes with distance from the effect origin so
    // distant AoE stomps do not shake the observer's camera at full strength.
    constexpr float kMaxShakeRange = 50.0f;
    float atten = 1.0f;
    if (const auto* cam = renderer_->getCamera()) {
        const float dist = glm::length(cam->getPosition() - worldPosition);
        atten = std::clamp(1.0f - dist / kMaxShakeRange, 0.0f, 1.0f);
    }
    if (atten <= 0.0f) return;

    const float magnitude = std::min(maxAmp * 0.03f, 0.45f) * atten;
    const float frequency = bestFreq > 0.0f ? bestFreq : 6.0f;
    camCtrl->triggerShake(magnitude, frequency, maxDur);
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
        effect.noiseScale = dbc->getFloat(i, 3);
        effect.texCoordScale = dbc->getFloat(i, 4);
        effect.segDurationMs = dbc->getUInt32(i, 5);
        effect.segDelayMs = dbc->getUInt32(i, 6);
        effect.flags = dbc->getUInt32(i, 8);
        effect.jointSmoothness = dbc->getFloat(i, 16);
        assert(effect.jointSmoothness >= 0.0f && effect.jointSmoothness <= 1.0f);
        effect.minDurBetweenJoints = dbc->getFloat(i, 17);
        effect.pulseSpeed = dbc->getFloat(i, 36);
        effect.pulseOnLength = dbc->getFloat(i, 37);
        effect.pulseFadeLength = dbc->getFloat(i, 38);
        effect.joints = {.count = dbc->getUInt32(i, 9), .perMinor = dbc->getUInt32(i, 11),
                         .minorPerMajor = dbc->getUInt32(i, 12), .radius = dbc->getFloat(i, 10),
                         .minorScale = dbc->getFloat(i, 13), .majorScale = dbc->getFloat(i, 14)};
        effect.jointMoveSpeed = dbc->getFloat(i, 15);
        effect.majorJointMoveSpeed = dbc->getFloat(i, 18);
        effect.flickerOn = {dbc->getFloat(i, 32), dbc->getFloat(i, 33)};
        effect.flickerOff = {dbc->getFloat(i, 34), dbc->getFloat(i, 35)};
        effect.arcHeight = dbc->getFloat(i, 26);
        effect.minArcAngle = dbc->getFloat(i, 27);
        effect.maxArcAngle = dbc->getFloat(i, 28);
        effect.minArcSpin = dbc->getFloat(i, 29);
        effect.maxArcSpin = dbc->getFloat(i, 30);
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
        // After BlendMode: Combo (string offset) at +161, RenderLayer +165,
        // TextureLength (float) +169. TextureLength (2.0 on 317 of 320 rows)
        // is not read: native draws one texture copy over the beam
        // (chainTexCoord).
        uint32_t combo = 0;
        std::memcpy(&combo, record + 161, sizeof(combo));
        if (combo) effect.strands = chainComboStrands(dbc->getStringViewByOffset(combo), dbc->getUInt32(i, 0));
        chainEffects_[dbc->getUInt32(i, 0)] = std::move(effect);
    }
    // Combo entries naming a row the file does not have (215 names 89) draw nothing.
    for (auto& [id, effect] : chainEffects_) {
        std::erase_if(effect.strands, [&, id = id](uint32_t strand) {
            const bool missing = !chainEffects_.count(strand);
            if (missing) LOG_DEBUG("SpellVisual: chain effect ", id, " combo names missing row ", strand);
            return missing;
        });
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
    // A precast lasts the server cast time plus pushbacks and ends on
    // SMSG_SPELL_GO / SMSG_SPELL_FAILURE / cancel; a small grace window
    // prevents premature expiry from network jitter before SMSG_SPELL_GO.
    const float duration = precastDurationSeconds(castTimeMs);
    if (attachInstanceId != 0) cancelPrecastVisuals(attachInstanceId);
    if (const uint32_t snd = playKitSound(kit->second, worldPosition, attachInstanceId, /*stoppable=*/attachInstanceId != 0);
        snd != 0 && attachInstanceId != 0) {
        activePrecastSounds_[attachInstanceId] = snd;
    }
    spawnKitEffects(kit->second, worldPosition, attachInstanceId, duration, KitStage::Precast);
    startKitWeaponTrail(kit->second, attachInstanceId);
}

void SpellVisualSystem::delayPrecastVisuals(uint32_t attachInstanceId, uint32_t delayMs) {
    if (!attachInstanceId || delayMs == 0) return;
    const float delaySec = static_cast<float>(delayMs) / 1000.0f;
    for (auto& visual : activeSpellVisuals_) {
        if (visual.isPrecast && visual.attachInstanceId == attachInstanceId) {
            visual.duration = std::max(visual.duration + delaySec,
                                       visual.elapsed + delaySec + kPrecastGraceSec);
        }
    }
}

void SpellVisualSystem::extendPrecastVisualsToRemaining(uint32_t attachInstanceId, uint32_t remainingMs) {
    if (!attachInstanceId || remainingMs == 0) return;
    const float remainSec = static_cast<float>(remainingMs) / 1000.0f;
    for (auto& visual : activeSpellVisuals_) {
        if (visual.isPrecast && visual.attachInstanceId == attachInstanceId) {
            visual.duration = std::max(visual.duration, visual.elapsed + remainSec + kPrecastGraceSec);
        }
    }
}

uint32_t SpellVisualSystem::playKitSound(uint32_t kitId, const glm::vec3& worldPosition,
                                         uint32_t attachInstanceId, bool stoppable) {
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_ || !audio::AudioEngine::instance().isInitialized()) return 0;
    const auto sndIt = kitSounds_.find(kitId);
    if (sndIt == kitSounds_.end() || sndIt->second == 0) return 0;

    float sfxScale = 1.0f;
    if (auto* ac = renderer_ ? renderer_->getAudioCoordinator()
                             : core::Application::getInstance().getAudioCoordinator()) {
        sfxScale = ac->getSfxVolumeScale();
    }
    if (sfxScale <= 0.0f) return 0;

    const bool isLocalPlayer = renderer_ && attachInstanceId != 0 &&
                               attachInstanceId == renderer_->getCharacterInstanceId();
    auto& table = audio::SoundEntryTable::instance();
    if (isLocalPlayer) {
        if (stoppable) return table.play2DStoppable(cachedAssetManager_, sndIt->second, sfxScale);
        return table.play2D(cachedAssetManager_, sndIt->second, sfxScale) ? 1u : 0u;
    }
    if (stoppable) return table.play3DStoppable(cachedAssetManager_, sndIt->second, worldPosition, sfxScale);
    return table.play3D(cachedAssetManager_, sndIt->second, worldPosition, sfxScale) ? 1u : 0u;
}

void SpellVisualSystem::stopPrecastSound(uint32_t attachInstanceId) {
    if (auto it = activePrecastSounds_.find(attachInstanceId); it != activePrecastSounds_.end()) {
        audio::AudioEngine::instance().stopSound(it->second);
        activePrecastSounds_.erase(it);
    }
}

void SpellVisualSystem::stopChannelSound(uint32_t attachInstanceId) {
    if (auto it = activeChannelSounds_.find(attachInstanceId); it != activeChannelSounds_.end()) {
        audio::AudioEngine::instance().stopSound(it->second);
        activeChannelSounds_.erase(it);
    }
}

// Every effect a kit names, each at the attachment its SpellVisualKit slot
// names: LeftHand 21, RightHand 22, Chest 34, Head 18 (overhead), Breath 17,
// Base 19, Special 23-25 (17, 21-22, and 23-25 oriented by their bone); World
// at the unit's position.
float SpellVisualSystem::spawnKitEffects(uint32_t kitId, const glm::vec3& worldPosition,
                                         uint32_t attachInstanceId, float duration, KitStage stage) {
    const auto effects = kitEffects_.find(kitId);
    if (effects == kitEffects_.end()) return 0.0f;
    float longest = 0.0f;
    auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    for (const auto& effect : effects->second) {
        const uint32_t modelId = loadSpellModel(effect.path);
        if (modelId == 0) continue;
        uint32_t attachment = attachInstanceId ? effect.attachment : 0;
        glm::mat4 attachMat(1.0f);
        // A LeftHand/RightHand effect on a model without SpellLeftHand/
        // SpellRightHand (21/22) is not shown: build 8606 draws Bloodlust's
        // hand glow (2825, BloodLust_State_Hand) on HumanMale but nothing on
        // Zombie, HUMSMerchant, HumanMaleNoble, Turtle, CryptFiend,
        // WarpStalker or Dryder, which lack 21/22 - not at their hands 1/2,
        // not at their feet (_experiments/20261118T114913Z.hand_effect_attachments).
        // BreathEffect (17) likewise: Soul Scream 41545's FlameBreath_Blue on
        // the Essence of Anger (WellofSoulsPain.m2, no 17) is not drawn by
        // build 8606; at the unit's feet it was a blue fire on the ground
        // (_experiments/20261118T124000Z.astromancer_glow scream-pairs.jpg).
        if (slotSkippedWhenAbsent(attachment) &&
            (!characters || !characters->getAttachmentTransform(attachInstanceId, attachment, attachMat))) {
            LOG_DEBUG("SpellVisual: kit=", kitId, " attachment ", attachment, " absent on instance ",
                      attachInstanceId, "; not shown: ", effect.path);
            continue;
        }
        if (attachment != 0 &&
            (!characters || !characters->getAttachmentTransform(attachInstanceId, attachment, attachMat))) {
            LOG_WARNING("SpellVisual: kit=", kitId, " attachment ", attachment,
                        " unavailable on instance ", attachInstanceId, "; placed at unit position, model=",
                        effect.path);
            attachment = 0;
        }
        const bool breath = slotFollowsBone(attachment);
        const glm::vec3 spawn = attachment ? glm::vec3(attachMat[3]) : worldPosition;
        const uint32_t instanceId = breath
            ? m2Renderer_->createInstanceWithMatrix(modelId, glm::scale(attachMat, glm::vec3(effect.scale)), spawn)
            : m2Renderer_->createInstance(modelId, spawn, glm::vec3(0.0f), effect.scale);
        if (instanceId == 0) {
            LOG_WARNING("SpellVisual: instance failed for kit=", kitId, " model=", effect.path);
            continue;
        }
        m2Renderer_->setInstanceIsSpellVisual(instanceId);
        if (stage == KitStage::Precast) {
            m2Renderer_->startInstanceStateEffect(instanceId);
        } else {
            m2Renderer_->restartInstanceAnimation(instanceId);
        }
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
        longest = std::max(longest, lifetime);
        activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = lifetime,
            .isPrecast = stage == KitStage::Precast, .attachmentId = attachment, .attachInstanceId = attachInstanceId,
            .orientWithAttachment = breath, .effectScale = effect.scale, .isCast = stage == KitStage::Cast});
        if (!breath && attachInstanceId)
            placeOnUnit(activeSpellVisuals_.back(), characters, spawn, attachment ? attachmentScale(attachMat) : 1.0f);
        LOG_INFO("SpellVisual: kit=", kitId, " slot attachment=", effect.attachment, " used=", attachment,
                 " instanceId=", instanceId, " duration=", lifetime, "s model=", effect.path);
    }
    return longest;
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
    const float scale = effectScale(effectNameId);
    const uint32_t instanceId = m2Renderer_->createInstanceWithMatrix(
        modelId, glm::scale(attachMat, glm::vec3(scale)), glm::vec3(attachMat[3]));
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: instance failed for effect ", effectNameId, " model=", path->second);
        return;
    }
    m2Renderer_->setInstanceIsSpellVisual(instanceId);
    m2Renderer_->restartInstanceAnimation(instanceId);
    // One play of the model's sequence, as for kit effects.
    const float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
    if (animDurMs <= 0.0f)
        LOG_WARNING("SpellVisual: effect ", effectNameId, " model ", path->second, " has no animation length");
    const float lifetime = animDurMs / 1000.0f;
    activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = lifetime,
        .isPrecast = false, .attachmentId = attachment, .attachInstanceId = attachInstanceId,
        .orientWithAttachment = true, .effectScale = scale});
    LOG_DEBUG("SpellVisual: effect=", effectNameId, " attachment=", attachment, " instance=",
              attachInstanceId, " duration=", lifetime, "s model=", path->second);
}

void SpellVisualSystem::playResistImmuneEffect(const glm::vec3& impactPosition,
                                               const glm::vec3& incomingDirection) {
    if (!m2Renderer_) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // SpellVisualEffectName.dbc row 3207: 'HARDCODED Resist Spell' -> spells\resist_immune_effect.mdx
    static constexpr uint32_t kEffectResistImmune = 3207;
    const auto path = effectPaths_.find(kEffectResistImmune);
    if (path == effectPaths_.end()) {
        LOG_DEBUG("SpellVisual: HARDCODED Resist Spell (3207) absent from SpellVisualEffectName.dbc");
        return;
    }
    const uint32_t modelId = loadSpellModel(path->second);
    if (modelId == 0) return;

    // resist_immune_effect.m2 authors its curved shockwave dome at X=[-1.45, -0.84],
    // so aligning +X along incomingDirection (caster -> victim) places the dome in
    // front of the victim facing the incoming missile (verified at 30 Hz on build 8606).
    glm::vec3 rotation(0.0f);
    const float len = glm::length(incomingDirection);
    if (len > 1e-4f) {
        const glm::vec3 dir = incomingDirection / len;
        rotation.z = std::atan2(dir.y, dir.x);
        rotation.y = -std::asin(std::clamp(dir.z, -1.0f, 1.0f));
    }
    const float scale = effectScale(kEffectResistImmune);
    const uint32_t instanceId = m2Renderer_->createInstanceWithMatrix(
        modelId, placementModelMatrix(impactPosition, rotation, scale), impactPosition);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: instance failed for resist/immune effect model=", path->second);
        return;
    }
    m2Renderer_->setInstanceIsSpellVisual(instanceId);
    m2Renderer_->restartInstanceAnimation(instanceId);
    const float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
    const float lifetime = animDurMs > 0.0f ? (animDurMs / 1000.0f) : 0.5f;
    activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = lifetime,
        .isPrecast = false, .attachmentId = 0, .attachInstanceId = 0, .effectScale = scale});
    LOG_DEBUG("SpellVisual: resist/immune effect instance=", instanceId, " duration=", lifetime, "s");
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
    if (useImpactKit) {
        if (attachInstanceId != 0)
            recentUnitImpacts_[attachInstanceId][visualId] = beamClock_;
        // A spell landing on a unit plays its ImpactKit and its TargetImpactKit
        // there. Burn 43217 (Zul'jin's Column of Fire) has only the latter:
        // kit 213, Fire ImpactDD Low Chest on each player it burns.
        // An impact kit's animation plays on the unit it lands on, as a kit
        // sent by SMSG_PLAY_SPELL_IMPACT does: General Drakkisath's Knockdown
        // (16790) and Backhand (6253) impact kit 348 throws the target down
        // with Knockdown (121).
        for (const auto* kits : {&impactKits_, &targetImpactKits_}) {
            const auto kit = kits->find(visualId);
            if (kit != kits->end() && kit->second)
                playSpellVisualKit(kit->second, worldPosition, attachInstanceId);
        }
        return;
    }
    const auto kit = castKits_.find(visualId);
    if (kit == castKits_.end() || !kit->second) return;
    if (attachInstanceId != 0) stopPrecastSound(attachInstanceId);
    playKitSound(kit->second, worldPosition, attachInstanceId, /*stoppable=*/false);
    playKitShake(kit->second, worldPosition);
    spawnKitEffects(kit->second, worldPosition, attachInstanceId, -1.0f, KitStage::Cast);
    startKitWeaponTrail(kit->second, attachInstanceId);
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

bool SpellVisualSystem::hasPrecastSound(uint32_t visualId) {
    if (visualId == 0) return false;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto it = precastKits_.find(visualId);
    return it != precastKits_.end() && it->second != 0 && kitSounds_.count(it->second) != 0;
}

bool SpellVisualSystem::hasCastSound(uint32_t visualId) {
    if (visualId == 0) return false;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto check = [&](const std::unordered_map<uint32_t, uint32_t>& kits) {
        const auto it = kits.find(visualId);
        return it != kits.end() && it->second != 0 && kitSounds_.count(it->second) != 0;
    };
    return check(castKits_) || check(casterImpactKits_) || check(instantAreaKits_) || check(channelKits_);
}

bool SpellVisualSystem::hasImpactSound(uint32_t visualId) {
    if (visualId == 0) return false;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto check = [&](const std::unordered_map<uint32_t, uint32_t>& kits) {
        const auto it = kits.find(visualId);
        return it != kits.end() && it->second != 0 && kitSounds_.count(it->second) != 0;
    };
    return check(impactKits_) || check(targetImpactKits_) || check(impactAreaKits_);
}

void SpellVisualSystem::startChannelVisual(uint32_t visualId, const glm::vec3& position,
                                          uint32_t durationMs, uint32_t attachInstanceId) {
    if (!m2Renderer_ || !attachInstanceId) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    updateChannelVisual(attachInstanceId, 0);
    const auto kit = channelKits_.find(visualId);
    if (kit == channelKits_.end() || !kit->second) return;
    const float remaining = durationMs == std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<float>::infinity()
        : durationMs / 1000.0f;
    unitChannelVisuals_[attachInstanceId] = {visualId, remaining};
    startKitLight(kit->second, attachInstanceId);
    startKitWeaponTrail(kit->second, attachInstanceId);
    applyCharacterProcedures(attachInstanceId);
    if (const uint32_t snd = playKitSound(kit->second, position, attachInstanceId, /*stoppable=*/true);
        snd != 0) {
        activeChannelSounds_[attachInstanceId] = snd;
    }
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
        const bool breath = slotFollowsBone(effect.attachment);
        const uint32_t instance = breath
            ? m2Renderer_->createInstanceWithMatrix(model, glm::scale(transform, glm::vec3(effect.scale)), spawn)
            : m2Renderer_->createInstance(model, spawn, glm::vec3(0), effect.scale);
        if (!instance) { LOG_ERROR("Channel effect instance failed: ", effect.path); continue; }
        m2Renderer_->setInstanceIsSpellVisual(instance);
        // A channel effect plays Stand once, then loops Hold, as a state
        // effect does (27 of 65 TBC ChannelKit models author both). Dark
        // Channeling 21157 on Magister Kalendris (Shadow_Snare_High_base:
        // Stand 3 s with the black streaks of #0 on; Hold 4 s with #0 off and
        // the dark rings of #1 fading out and back in) shows a green glow
        // with faint rings in the original; looping Stand drew a burst of
        // black spikes round him.
        m2Renderer_->startInstanceStateEffect(instance);
        activeSpellVisuals_.push_back({.instanceId = instance, .elapsed = 0.0f,
            .duration = durationMs / 1000.0f, .isPrecast = false, .attachmentId = effect.attachment,
            .attachInstanceId = attachInstanceId, .isChannel = true, .orientWithAttachment = breath,
            .effectScale = effect.scale});
        if (!breath)
            placeOnUnit(activeSpellVisuals_.back(), characters, spawn,
                        effect.attachment ? attachmentScale(transform) : 1.0f);
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
                m2Renderer_->retireInstance(it->instanceId);
                it = activeSpellVisuals_.erase(it);
            }
        } else ++it;
    }
    const auto previous = found == unitAuraVisuals_.end() ? std::vector<uint32_t>{} : found->second.ids;
    // A newly applied aura's state kit starts its CharProc 13 light; the light
    // then runs its own time, removal of the aura does not end it.
    for (uint32_t visual : visualIds) {
        if (std::find(previous.begin(), previous.end(), visual) != previous.end()) continue;
        if (const auto kit = stateKits_.find(visual); kit != stateKits_.end() && kit->second) {
            startKitLight(kit->second, attachInstanceId);
            startKitWeaponTrail(kit->second, attachInstanceId);
            playKitSound(kit->second, position, attachInstanceId, /*stoppable=*/false);
        }
    }
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
            // A hand or breath slot on a model without it is not shown (as spawnKitEffects).
            glm::mat4 handMat(1.0f);
            if (slotSkippedWhenAbsent(effect.attachment) &&
                !characters->getAttachmentTransform(attachInstanceId, effect.attachment, handMat)) {
                LOG_DEBUG("Aura visual: hand attachment ", effect.attachment, " absent on instance ",
                          attachInstanceId, "; not shown: ", effect.path);
                continue;
            }
            const uint32_t model = loadSpellModel(effect.path);
            if (!model) { LOG_ERROR("Aura model unavailable: ", effect.path); continue; }
            const uint32_t instance = m2Renderer_->createInstance(model, position, glm::vec3(0), 1.0f);
            if (!instance) { LOG_ERROR("Aura instance failed: ", effect.path); continue; }
            m2Renderer_->setInstanceIsSpellVisual(instance);
            m2Renderer_->startInstanceStateEffect(instance);
            // As spawnKitEffects: a model without the slot's attachment (40 of
            // 769 TBC creature models lack Base 19, e.g. HumSCitizenMid) has
            // the effect at its position, at the unit's own size.
            uint32_t attachment = effect.attachment;
            glm::mat4 attachMat(1.0f);
            if (attachment && !characters->getAttachmentTransform(attachInstanceId, attachment, attachMat)) {
                LOG_WARNING("Aura visual: attachment ", attachment, " unavailable on instance ", attachInstanceId,
                            "; placed at unit position, model=", effect.path);
                attachment = 0;
            }
            activeSpellVisuals_.push_back({instance, 0.0f, std::numeric_limits<float>::infinity(),
                false, attachment, attachInstanceId, false, visual, marker});
            activeSpellVisuals_.back().effectScale = effect.scale;
            activeSpellVisuals_.back().orientWithAttachment = slotFollowsBone(attachment);
            if (activeSpellVisuals_.back().orientWithAttachment)
                m2Renderer_->setInstanceTransform(instance, glm::scale(attachMat, glm::vec3(effect.scale)));
            else
                placeOnUnit(activeSpellVisuals_.back(), characters, attachment ? glm::vec3(attachMat[3]) : position,
                            attachment ? attachmentScale(attachMat) : 1.0f);
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
    if (visualIds.empty()) unitAuraVisuals_.erase(attachInstanceId);
    else unitAuraVisuals_[attachInstanceId] = {visualIds, observerConcealed, std::move(applied)};
    applyCharacterProcedures(attachInstanceId);
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

void SpellVisualSystem::applyCharacterProcedures(uint32_t attachInstanceId) {
    auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!characters || !characters->hasInstance(attachInstanceId)) return;
    glm::vec3 color(1.0f);
    float opacity = 1.0f;
    bool colored = false, faded = false, frozen = false;
    auto applyKit = [&](uint32_t kitId) {
        if (!kitId) return;
        const auto procedures = kitCharacterProcedures_.find(kitId);
        if (procedures == kitCharacterProcedures_.end()) return;
        if (!colored && procedures->second.color) { color = *procedures->second.color; colored = true; }
        if (!faded && procedures->second.opacity) { opacity = *procedures->second.opacity; faded = true; }
        if (procedures->second.freezeAnimation) frozen = true;
    };
    if (const auto chan = unitChannelVisuals_.find(attachInstanceId); chan != unitChannelVisuals_.end()) {
        if (const auto kit = channelKits_.find(chan->second.visualId); kit != channelKits_.end())
            applyKit(kit->second);
    }
    if (const auto auras = unitAuraVisuals_.find(attachInstanceId);
        auras != unitAuraVisuals_.end() && !(colored && faded && frozen)) {
        for (auto visual = auras->second.applied.rbegin();
             visual != auras->second.applied.rend() && !(colored && faded && frozen); ++visual) {
            const auto kit = stateKits_.find(*visual);
            if (kit != stateKits_.end()) applyKit(kit->second);
        }
    }
    LOG_DEBUG("Kit appearance: owner=", attachInstanceId,
              " channel=", unitChannelVisuals_.count(attachInstanceId),
              " color=(", color.r, ",", color.g, ",", color.b, ") opacity=", opacity,
              " frozen=", frozen);
    characters->setInstanceKitAppearance(attachInstanceId, color, opacity, frozen);
}

void SpellVisualSystem::updateChannelVisual(uint32_t attachInstanceId, uint32_t remainingMs) {
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end();) {
        if (it->isChannel && it->attachInstanceId == attachInstanceId) {
            if (!remainingMs) {
                retireEffect(*it);
                it = activeSpellVisuals_.erase(it);
                continue;
            }
            it->duration = it->elapsed + remainingMs / 1000.0f;
        }
        ++it;
    }
    if (!remainingMs) {
        stopChannelSound(attachInstanceId);
        if (unitChannelVisuals_.erase(attachInstanceId))
            applyCharacterProcedures(attachInstanceId);
    } else if (const auto chan = unitChannelVisuals_.find(attachInstanceId);
               chan != unitChannelVisuals_.end()) {
        chan->second.remaining = remainingMs / 1000.0f;
    }
}

SpellVisualSystem::BeamState SpellVisualSystem::newBeamState(uint32_t chainId) {
    static thread_local std::mt19937 random{std::random_device{}()};
    auto between = [&](float a, float b) {
        return std::uniform_real_distribution<float>(std::min(a, b), std::max(a, b))(random);
    };
    BeamState state;
    state.age = beamClock_;
    std::vector<uint32_t> rows{chainId};
    const auto& strands = chainEffects_.at(chainId).strands;
    rows.insert(rows.end(), strands.begin(), strands.end());
    for (uint32_t row : rows) {
        const ChainEffect& effect = chainEffects_.at(row);
        state.strands.push_back({.chainId = row,
            .waveAngle = between(effect.minWaveAngle, effect.maxWaveAngle),
            .waveSpin = between(effect.minWaveSpin, effect.maxWaveSpin),
            .arcAngle = between(effect.minArcAngle, effect.maxArcAngle),
            .arcSpin = between(effect.minArcSpin, effect.maxArcSpin)});
    }
    return state;
}

// A beam ends at its unit's attachment (the chest, 34, unless it is the
// caster's start) while the unit is drawn, at the chest when the model
// lacks that point; otherwise at the position recorded when it started.
glm::vec3 SpellVisualSystem::beamEnd(const ChainEnd& end) {
    auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    glm::mat4 point(1.0f);
    if (end.instanceId && characters && characters->getAttachmentTransform(end.instanceId, end.attachment, point))
        return glm::vec3(point[3]);
    if (end.instanceId && end.attachment != 34 && characters &&
        characters->getAttachmentTransform(end.instanceId, 34, point))
        return glm::vec3(point[3]);
    return end.position;
}

void SpellVisualSystem::appendBeamStrip(std::vector<M2Renderer::ChainStrip>& strips, const glm::vec3& start,
                                        const glm::vec3& end, float t, BeamState& state) {
    static thread_local std::mt19937 random{std::random_device{}()};
    assert(!state.strands.empty());
    const glm::vec3 axis = end - start;
    const float length = glm::length(axis);
    if (length < 1e-3f) return;
    const glm::vec3 dir = axis / length;
    // The wave's and arc's planes: world up made perpendicular to the beam,
    // turned about the beam by their angle (and its spin over time).
    glm::vec3 up = glm::vec3(0, 0, 1) - dir * dir.z;
    if (glm::length(up) < 1e-4f) up = glm::vec3(1, 0, 0) - dir * dir.x;
    up = glm::normalize(up);
    const glm::vec3 side = glm::cross(dir, up);
    auto turned = [&](float angle) { return glm::normalize(up * std::cos(angle) + side * std::sin(angle)); };
    // The row, then each row its Combo names, every one drawn whole from its
    // own record between the same ends: native Sunshatter 45215 (chain 215)
    // shows 209's blue bolt, 211's broad white-purple wave and 217's thin
    // purple bolts beside its green one; Tear of Azzinoth Summon Channel 39857
    // (237) three green SoulBeam waves: 237 and 239 twice (WaveSpeed 15, 5, 5).
    for (StrandState& strand : state.strands) {
        const ChainEffect& effect = chainEffects_.at(strand.chainId);
        if (effect.blendMode != 2 && effect.blendMode != 3 && effect.blendMode != 4) {
            static std::unordered_set<uint32_t> said;
            if (said.insert(strand.chainId).second)
                LOG_WARNING("SpellVisual: chain effect ", strand.chainId, " blend mode ", int(effect.blendMode),
                            " is not drawn (only 2, 3 and 4 are implemented)");
            continue;
        }
        const int segments = chainSegmentCount(length, effect.avgSegLen);
        auto drawNoise = [&](std::vector<glm::vec2>& out) {
            // Uniform in the disc across the beam; the ends stay on their units.
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            out.assign(segments + 1, glm::vec2(0.0f));
            for (int i = 1; i < segments; ++i) {
                const float r = std::sqrt(unit(random)), a = glm::two_pi<float>() * unit(random);
                out[i] = glm::vec2(std::cos(a), std::sin(a)) * r;
            }
        };
        // Pulse (chainPulse): rows with Flags 0x20 light a window sweeping
        // along the beam; the row's Combo copies share it (Fel Consumption's
        // three strands are lit over the same stretch). Their flicker fields
        // do not hide them: native Fel Crystal Visual (292, flicker on
        // 0.48-0.66 s, off 0.26-0.55 s) sweeps unbroken bands 2 s long from
        // each crystal to Selin; hidden by the flicker they came and went in
        // half-second pieces.
        const bool pulsing = (effect.flags & 0x20u) && effect.pulseSpeed != 0.0f;
        // Flicker: a row with Min/MaxFlickerOn/Off is drawn for a random
        // on-time, hidden for a random off-time, again and again (not
        // compared with native on a row without pulse). Open Portal from
        // Shattrath 46801 (chain 411 eight times, Flags 0x36) pulses: native
        // strands lit first at the caster, reaching the target in 0.15-0.2 s
        // (PulseSpeed 100) and showing one to four at a time are its windows.
        if (!pulsing && (effect.flickerOn.y > 0.0f || effect.flickerOff.y > 0.0f)) {
            auto between = [&](glm::vec2 range) {
                return std::uniform_real_distribution<float>(std::min(range.x, range.y), std::max(range.x, range.y))(random);
            };
            if (strand.flickerUntil == 0.0f) {
                strand.flickerOn = std::uniform_real_distribution<float>(0.0f, 1.0f)(random) < 0.5f;
                strand.flickerUntil = beamClock_ + between(strand.flickerOn ? effect.flickerOn : effect.flickerOff) *
                                      std::uniform_real_distribution<float>(0.0f, 1.0f)(random);
            }
            while (beamClock_ >= strand.flickerUntil) {
                strand.flickerOn = !strand.flickerOn;
                strand.flickerUntil += std::max(between(strand.flickerOn ? effect.flickerOn : effect.flickerOff), 0.01f);
            }
            if (!strand.flickerOn) continue;
        }
        const bool redraw = effect.minDurBetweenJoints > 0.0f && beamClock_ >= strand.nextJoints;
        // Noise (chainNoiseStep): a new beam starts at a full random draw;
        // then every frame each vertex keeps JointSmoothness of its offset
        // and moves the rest toward a random offset drawn anew every frame,
        // or every MinDurBetweenJoints when that is set (Sunshatter, 0.08 s:
        // native's kinks drift and are gone within a few tenths of a
        // second). When the units move and the segment count changes, the
        // offsets are read at the same fractions of the beam: drawing a new
        // full set made a beam between moving units (High Priestess Jeklik's
        // Mind Flay, chain 4) a jagged rod jumping every frame.
        if (strand.noise.empty()) {
            drawNoise(strand.noise);
            drawNoise(strand.noiseTarget);
            strand.nextJoints = beamClock_ + effect.minDurBetweenJoints;
        } else {
            if (strand.noise.size() != static_cast<size_t>(segments + 1)) {
                strand.noise = chainResample(strand.noise, segments + 1);
                strand.noiseTarget = chainResample(strand.noiseTarget, segments + 1);
            }
            if (effect.minDurBetweenJoints <= 0.0f || redraw) {
                drawNoise(strand.noiseTarget);
                strand.nextJoints = beamClock_ + effect.minDurBetweenJoints;
            }
            for (size_t i = 0; i < strand.noise.size(); ++i)
                strand.noise[i] = chainNoiseStep(strand.noise[i], strand.noiseTarget[i], effect.jointSmoothness);
        }
        // Joints: JointCount points evenly between the ends, each straying
        // up to chainJointRadius yards; the beam runs straight from joint to
        // joint. Each walks toward a random spot in its disc at JointMoveSpeed
        // (MajorJointMoveSpeed for major joints) yards a second and picks the
        // next one on arrival, at most every MinDurBetweenJoints; without a
        // speed it jumps there every MinDurBetweenJoints.
        const ChainJoints& jointSpec = effect.joints;
        auto jointTarget = [&](uint32_t i) {
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            const float r = std::sqrt(unit(random)) * chainJointRadius(jointSpec, i);
            const float a = glm::two_pi<float>() * unit(random);
            return glm::vec2(std::cos(a), std::sin(a)) * r;
        };
        if (strand.joints.size() != jointSpec.count) {
            strand.joints.clear();
            strand.jointTargets.clear();
            for (uint32_t i = 1; i <= jointSpec.count; ++i) {
                strand.joints.push_back(jointTarget(i));
                strand.jointTargets.push_back(jointTarget(i));
            }
            strand.jointsMoved = beamClock_;
        }
        if (jointSpec.count && beamClock_ > strand.jointsMoved) {
            const float dt = beamClock_ - strand.jointsMoved;
            for (uint32_t i = 1; i <= jointSpec.count; ++i) {
                glm::vec2& at = strand.joints[i - 1];
                glm::vec2& target = strand.jointTargets[i - 1];
                const bool major = jointSpec.perMinor && jointSpec.minorPerMajor &&
                                   i % (jointSpec.perMinor * jointSpec.minorPerMajor) == 0;
                const float speed = major ? effect.majorJointMoveSpeed : effect.jointMoveSpeed;
                if (speed > 0.0f) {
                    const glm::vec2 step = target - at;
                    const float left = glm::length(step);
                    if (left <= speed * dt) {
                        at = target;
                        if (redraw) target = jointTarget(i);
                    } else {
                        at += step / left * speed * dt;
                    }
                } else if (redraw) {
                    at = target;
                    target = jointTarget(i);
                }
            }
            strand.jointsMoved = beamClock_;
        }
        const glm::vec3 across = turned(strand.waveAngle + strand.waveSpin * t);
        // ArcHeight yards at the middle, half a sine from end to end: native
        // Sunshatter (1.1) bows about a yard above the line at 24 yd.
        const glm::vec3 bow = turned(strand.arcAngle + strand.arcSpin * t) * effect.arcHeight;
        const float noise = chainNoiseRadius(effect.noiseScale, length);
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
            const glm::vec2 o = strand.noise[i] * noise +
                                chainJointOffsetAt(strand.joints, s / length);
            strip.spine.push_back(start + dir * s + across * wave + bow * std::sin(glm::pi<float>() * s / length) +
                                  up * o.x + side * o.y);
            strip.u.push_back(chainTexCoord(s, length, effect.texCoordScale, t));
            if (pulsing)
                strip.fade.push_back(chainPulse(s, length, effect.pulseSpeed, effect.pulseOnLength,
                                                effect.pulseFadeLength, t));
        }
        strips.push_back(std::move(strip));
    }
}

// Build 8606, Scholomance Dark Summoner channelling Sunshatter 45215 at the
// player, killed with `.die`: MSG_CHANNEL_UPDATE 0 and SMSG_SPELL_FAILURE
// arrive 0.30 s before the object update clearing UNIT_CHANNEL_SPELL; native's
// beam is gone 0.06-0.13 s after the first, WoWee's went with the fields
// (_experiments/20261119T1300Z.channel_end, bmdie9_sheet.jpg).
void SpellVisualSystem::endChannel(uint64_t casterGuid) { endedChannels_.insert(casterGuid); }

void SpellVisualSystem::startChannel(uint64_t casterGuid) { endedChannels_.erase(casterGuid); }

void SpellVisualSystem::syncChannels(const std::vector<UnitChannel>& fieldChannels) {
    if (!m2Renderer_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    if (!spellVisualDbcLoaded_) return;
    // Channels the server has ended whose fields are not yet cleared are over.
    std::erase_if(endedChannels_, [&](uint64_t guid) {
        return std::none_of(fieldChannels.begin(), fieldChannels.end(),
                            [&](const UnitChannel& channel) { return channel.casterGuid == guid; });
    });
    std::vector<UnitChannel> channels;
    for (const auto& channel : fieldChannels)
        if (!endedChannels_.contains(channel.casterGuid)) channels.push_back(channel);

    // ChannelKit effects of channels seen only through the fields. A unit
    // whose MSG_CHANNEL_START was received already has them, for that
    // channel's duration; startChannelVisual replaces held ones.
    std::unordered_map<uint32_t, uint32_t> current;
    for (const auto& channel : channels)
        if (channel.casterInstanceId) current[channel.casterInstanceId] = channel.visualId;
    for (auto it = fieldChannels_.begin(); it != fieldChannels_.end();) {
        const auto now = current.find(it->first);
        if (now == current.end() || now->second != it->second) {
            if (heldChannelKits_.erase(it->first) || unitChannelVisuals_.count(it->first))
                updateChannelVisual(it->first, 0);
            it = fieldChannels_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& channel : channels) {
        if (!channel.casterInstanceId || fieldChannels_.count(channel.casterInstanceId)) continue;
        fieldChannels_[channel.casterInstanceId] = channel.visualId;
        const bool playing = unitChannelVisuals_.count(channel.casterInstanceId) != 0 ||
            std::any_of(activeSpellVisuals_.begin(), activeSpellVisuals_.end(),
                [&](const SpellVisualInstance& v) { return v.isChannel && v.attachInstanceId == channel.casterInstanceId; });
        if (playing) continue;
        startChannelVisual(channel.visualId, channel.casterPosition, std::numeric_limits<uint32_t>::max(),
                           channel.casterInstanceId);
        heldChannelKits_.insert(channel.casterInstanceId);
    }

    std::vector<M2Renderer::ChainStrip> strips;
    std::unordered_set<uint64_t> live;
    for (const auto& channel : channels) {
        if (!channel.hasTarget) continue;
        // The ChannelKit's beam, else the StateKit's: Shared Bonds 41363
        // (visual 8244, no ChannelKit, StateKit 7030 -> chain 2 HealBeam),
        // channelled by Black Temple's Sister of Pain and Sister of Pleasure
        // at each other, is a beam between the two natively (build 8606,
        // pair 15); the channel's target holds the aura.
        auto kitChain = [&](const std::unordered_map<uint32_t, uint32_t>& kits)
            -> std::pair<uint32_t, uint32_t> {
            const auto kit = kits.find(channel.visualId);
            if (kit == kits.end() || !kit->second) return {0, 0};
            const auto chain = kitChainEffects_.find(kit->second);
            return chain == kitChainEffects_.end() ? std::pair<uint32_t, uint32_t>{0, 0}
                                                   : std::pair{kit->second, chain->second};
        };
        auto [kitId, chain] = kitChain(channelKits_);
        if (!chain) std::tie(kitId, chain) = kitChain(stateKits_);
        if (!chain) continue;
        live.insert(channel.casterGuid);
        auto [state, created] = beams_.try_emplace(channel.casterGuid);
        if (created) {
            state->second = newBeamState(chain);
            LOG_INFO("Channel beam: caster=0x", std::hex, channel.casterGuid, std::dec, " visual=", channel.visualId,
                     " kit=", kitId, " chain=", chain, " texture=", chainEffects_.at(chain).texture);
        }
        appendBeamStrip(strips, channel.start, channel.end, beamClock_ - state->second.age,
                        state->second);
        // A jumping channel runs on from its target to each further one.
        auto& hops = hopBeams_[channel.casterGuid];
        while (hops.size() < channel.hops.size()) hops.push_back(newBeamState(chain));
        hops.resize(channel.hops.size());
        glm::vec3 from = channel.end;
        for (size_t i = 0; i < channel.hops.size(); ++i) {
            appendBeamStrip(strips, from, channel.hops[i], beamClock_ - hops[i].age, hops[i]);
            from = channel.hops[i];
        }
    }
    // Cast kit beams, from startAt for their SegDuration (or fallback lifetime).
    std::erase_if(castBeams_, [&](const CastBeam& beam) {
        return beamClock_ >= beam.startAt + beam.lifetime;
    });
    for (auto& beam : castBeams_) {
        if (beamClock_ < beam.startAt) continue;
        appendBeamStrip(strips, beamEnd(beam.start), beamEnd(beam.end), beamClock_ - beam.startAt, beam.state);
    }
    std::erase_if(beams_, [&](const auto& entry) { return !live.count(entry.first); });
    std::erase_if(hopBeams_, [&](const auto& entry) { return !live.count(entry.first); });
    // Targets stay until the caster channels something else or they are
    // replaced: SMSG_SPELL_GO can arrive before the channel fields.
    for (const auto& channel : channels) {
        const auto targets = channelTargets_.find(channel.casterGuid);
        if (targets != channelTargets_.end() && targets->second.spellId != channel.spellId)
            channelTargets_.erase(targets);
    }
    m2Renderer_->setChainStrips(std::move(strips));
}

void SpellVisualSystem::setChannelTargets(uint64_t casterGuid, uint32_t spellId, std::vector<uint64_t> targets) {
    channelTargets_[casterGuid] = {spellId, std::move(targets)};
}

std::vector<uint64_t> SpellVisualSystem::channelTargets(uint64_t casterGuid, uint32_t spellId) const {
    const auto found = channelTargets_.find(casterGuid);
    if (found == channelTargets_.end() || found->second.spellId != spellId) return {};
    return found->second.targets;
}

void SpellVisualSystem::playCastKit(uint32_t visualId, const glm::vec3& casterPosition,
                                    uint32_t casterInstanceId, const std::vector<ChainEnd>& ends,
                                    bool chained, float spellSeconds) {
    if (!m2Renderer_ || visualId == 0) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto kit = castKits_.find(visualId);
    if (kit == castKits_.end() || !kit->second) return;
    if (casterInstanceId != 0) stopPrecastSound(casterInstanceId);
    playKitSound(kit->second, casterPosition, casterInstanceId, /*stoppable=*/false);
    playKitShake(kit->second, casterPosition);
    const float models = spawnKitEffects(kit->second, casterPosition, casterInstanceId, -1.0f, KitStage::Cast);
    startKitWeaponTrail(kit->second, casterInstanceId);
    const auto chainId = kitChainEffects_.find(kit->second);
    if (chainId == kitChainEffects_.end() || ends.empty()) return;
    const bool hasModels = kitEffects_.count(kit->second) != 0;
    const float fallback = hasModels ? models : spellSeconds;
    const auto chainIt = chainEffects_.find(chainId->second);
    if (chainIt == chainEffects_.end()) return;
    const auto firstTiming = castBeamTiming(chainIt->second.segDurationMs, chainIt->second.segDelayMs,
                                            fallback, 0, chained);
    if (firstTiming.duration <= 0.0f) {
        static std::unordered_set<uint32_t> said;
        if (said.insert(kit->second).second)
            LOG_WARNING("SpellVisual: cast kit ", kit->second, " draws chain ", chainId->second,
                        " but neither SegDuration, an effect model, nor a spell duration times it; beam not drawn");
        return;
    }
    ChainEnd from{casterInstanceId, casterPosition, beamStartAttachment(visualId)};
    for (size_t hop = 0; hop < ends.size(); ++hop) {
        const auto& end = ends[hop];
        const auto timing = castBeamTiming(chainIt->second.segDurationMs, chainIt->second.segDelayMs,
                                           fallback, hop, chained);
        castBeams_.push_back({.chainId = chainId->second, .start = from, .end = end,
                              .startAt = beamClock_ + timing.delay, .lifetime = timing.duration,
                              .state = newBeamState(chainId->second)});
        if (chained) from = end;
    }
    LOG_INFO("SpellVisual: cast kit ", kit->second, " beam chain ", chainId->second, " to ", ends.size(),
             " end(s) for ", firstTiming.duration, " s (delay ", firstTiming.delay, " s)",
             chained ? ", hopping" : "");
}

void SpellVisualSystem::playCasterImpactKit(uint32_t visualId, const glm::vec3& casterPosition,
                                            uint32_t casterInstanceId) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto kit = casterImpactKits_.find(visualId);
    if (kit == casterImpactKits_.end() || !kit->second) return;
    LOG_INFO("SpellVisual: caster impact kit ", kit->second, " of visual ", visualId);
    playSpellVisualKit(kit->second, casterPosition, casterInstanceId);
}

void SpellVisualSystem::playInstantAreaKit(uint32_t visualId, const glm::vec3& destination) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto kit = instantAreaKits_.find(visualId);
    if (kit == instantAreaKits_.end() || !kit->second) return;
    LOG_INFO("SpellVisual: instant area kit ", kit->second, " of visual ", visualId, " at (",
             destination.x, ",", destination.y, ",", destination.z, ")");
    playKitSound(kit->second, destination, /*attachInstanceId=*/0, /*stoppable=*/false);
    playKitShake(kit->second, destination);
    spawnKitEffects(kit->second, destination, /*attachInstanceId=*/0, /*duration=*/-1.0f,
                    KitStage::Other);
}

void SpellVisualSystem::playImpactAreaKit(uint32_t visualId, const glm::vec3& destination) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto kit = impactAreaKits_.find(visualId);
    if (kit == impactAreaKits_.end() || !kit->second) return;
    LOG_INFO("SpellVisual: impact area kit ", kit->second, " of visual ", visualId, " at (",
             destination.x, ",", destination.y, ",", destination.z, ")");
    playKitSound(kit->second, destination, /*attachInstanceId=*/0, /*stoppable=*/false);
    playKitShake(kit->second, destination);
    spawnKitEffects(kit->second, destination, /*attachInstanceId=*/0, /*duration=*/-1.0f,
                    KitStage::Other);
}

void SpellVisualSystem::scheduleImpactAreaKit(uint32_t visualId, const glm::vec3& destination, float delay) {
    assert(std::isfinite(delay) && delay >= 0.0f);
    pendingAreaImpacts_.push_back({.visualId = visualId, .destination = destination, .remaining = delay});
    LOG_INFO("SpellVisual: impact area of visual ", visualId, " lands in ", delay, "s at (",
             destination.x, ",", destination.y, ",", destination.z, ")");
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
    // A corpse keeps its death pose (CharacterRenderer::isInstanceDead).
    if (cr && attachInstanceId && animation->second != 0xffffffffu &&
        !cr->isInstanceDead(attachInstanceId) && cr->hasAnimation(attachInstanceId, animation->second)) {
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
    startKitLight(kitId, attachInstanceId);
    startKitWeaponTrail(kitId, attachInstanceId);
    playKitSound(kitId, worldPosition, attachInstanceId, /*stoppable=*/false);
    playKitShake(kitId, worldPosition);
    spawnKitEffects(kitId, worldPosition, attachInstanceId, -1.0f, KitStage::Other);
}

void SpellVisualSystem::startKitLight(uint32_t kitId, uint32_t attachInstanceId) {
    if (!attachInstanceId) return;
    const auto light = kitLights_.find(kitId);
    if (light == kitLights_.end()) return;
    auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!characters) return;
    characters->startInstanceKitLight(attachInstanceId, light->second.color, light->second.seconds);
    LOG_DEBUG("Kit light: owner=", attachInstanceId, " kit=", kitId, " color=(", light->second.color.r, ",",
              light->second.color.g, ",", light->second.color.b, ") seconds=", light->second.seconds);
}

void SpellVisualSystem::startKitWeaponTrail(uint32_t kitId, uint32_t attachInstanceId) {
    if (!attachInstanceId) return;
    const auto trailIt = kitWeaponTrails_.find(kitId);
    if (trailIt == kitWeaponTrails_.end()) return;
    const auto& spec = trailIt->second;

    ActiveUnitWeaponTrail entry;
    entry.ownerInstanceId = attachInstanceId;
    entry.spec = spec;
    entry.elapsed = 0.0f;

    if (auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
        const auto spans = characters->getDrawnMeleeWeaponSpans(attachInstanceId);
        for (const auto& span : spans) {
            WeaponTrailChannel ch;
            ch.weaponInstanceId = span.weaponInstanceId;
            ch.edges.push_back({span.baseWorld, span.tipWorld, spec.alpha});
            entry.channels.push_back(std::move(ch));
        }
    }

    auto existing = std::find_if(activeWeaponTrails_.begin(), activeWeaponTrails_.end(),
                                 [&](const ActiveUnitWeaponTrail& t) { return t.ownerInstanceId == attachInstanceId; });
    if (existing != activeWeaponTrails_.end()) {
        *existing = std::move(entry);
    } else {
        activeWeaponTrails_.push_back(std::move(entry));
    }
    LOG_DEBUG("Weapon trail: owner=", attachInstanceId, " kit=", kitId,
              " color=(", spec.color.r, ",", spec.color.g, ",", spec.color.b,
              ") alpha=", spec.alpha, " fadeOutRate=", spec.fadeOutRate,
              " duration=", spec.durationSec, "s");
}

void SpellVisualSystem::updateWeaponTrails(float deltaTime) {
    if (!m2Renderer_) return;
    if (activeWeaponTrails_.empty()) {
        m2Renderer_->setWeaponTrailStrips({});
        return;
    }

    auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    std::vector<M2Renderer::WeaponTrailStrip> strips;

    for (auto it = activeWeaponTrails_.begin(); it != activeWeaponTrails_.end();) {
        it->elapsed += deltaTime;
        const bool emitting = it->elapsed < it->spec.durationSec;

        std::vector<CharacterRenderer::DrawnWeaponSpan> spans;
        if (emitting && characters) {
            spans = characters->getDrawnMeleeWeaponSpans(it->ownerInstanceId);
            for (const auto& span : spans) {
                const bool known = std::any_of(it->channels.begin(), it->channels.end(),
                                               [&](const WeaponTrailChannel& ch) {
                                                   return ch.weaponInstanceId == span.weaponInstanceId;
                                               });
                if (!known) {
                    WeaponTrailChannel ch;
                    ch.weaponInstanceId = span.weaponInstanceId;
                    it->channels.push_back(std::move(ch));
                }
            }
        }

        for (auto chIt = it->channels.begin(); chIt != it->channels.end();) {
            std::optional<std::pair<glm::vec3, glm::vec3>> bladeWorld;
            if (emitting) {
                for (const auto& span : spans) {
                    if (span.weaponInstanceId == chIt->weaponInstanceId) {
                        bladeWorld = std::make_pair(span.baseWorld, span.tipWorld);
                        break;
                    }
                }
            }
            stepWeaponTrailChannel(*chIt, it->spec, deltaTime, emitting, bladeWorld);

            if (!emitting && isWeaponTrailChannelDead(*chIt)) {
                chIt = it->channels.erase(chIt);
                continue;
            }
            if (chIt->edges.size() >= 2) {
                M2Renderer::WeaponTrailStrip strip;
                strip.color = it->spec.color;
                strip.edges.reserve(chIt->edges.size());
                for (const auto& e : chIt->edges) {
                    strip.edges.push_back({e.base, e.tip, e.alpha});
                }
                strips.push_back(std::move(strip));
            }
            ++chIt;
        }

        if (!emitting && it->channels.empty()) {
            it = activeWeaponTrails_.erase(it);
        } else {
            ++it;
        }
    }

    m2Renderer_->setWeaponTrailStrips(std::move(strips));
}

// Flamestrike 2120: visual 10383, PersistentAreaKit 9357 BaseEffect
// Flamestrike_Impact_Base, the burning ground the original keeps under the
// area for the DynamicObject's life (8 s). The model loops its own sequence;
// it is removed with the object, not on a timer.
void SpellVisualSystem::playPersistentArea(uint64_t key, uint32_t visualId, const glm::vec3& worldPosition,
                                           float radius) {
    if (!m2Renderer_ || visualId == 0) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    stopPersistentArea(key);
    const auto kit = persistentAreaKits_.find(visualId);
    if (kit == persistentAreaKits_.end() || kit->second == 0) return;
    if (const uint32_t snd = playKitSound(kit->second, worldPosition, /*attachInstanceId=*/0, /*stoppable=*/true);
        snd != 0) {
        activeAreaSounds_[key] = snd;
    }
    // Cave In 36240 (kit 9373): Smash_Impact_Chest glows at the centre, a
    // 1 s loop, while HellfireRaid_Dust_Impact_Base rock falls land around it
    // (original 2.4.3, pair 8). Rain of Fire 5740 (kit 9352, 5 a second)
    // drops about five fireballs a second across its 8 yards.
    if (const auto impacts = kitAreaImpacts_.find(kit->second); impacts != kitAreaImpacts_.end()) {
        if (radius > 0.0f) {
            activeAreaImpacts_[key] = {.impacts = impacts->second, .center = worldPosition, .radius = radius};
        } else {
            LOG_WARNING("SpellVisual: persistent area 0x", std::hex, key, std::dec, " kit ", kit->second,
                        " drops area impacts but its spell names no area radius; none drawn");
        }
    }
    const auto effects = kitEffects_.find(kit->second);
    if (effects == kitEffects_.end()) {
        LOG_WARNING("SpellVisual: persistent area kit ", kit->second, " of visual ", visualId, " has no models");
        return;
    }
    auto& instances = persistentAreas_[key];
    for (const auto& effect : effects->second) {
        const uint32_t modelId = loadSpellModel(effect.path);
        if (modelId == 0) continue;
        const uint32_t instanceId = m2Renderer_->createInstance(modelId, worldPosition, glm::vec3(0.0f), effect.scale);
        if (instanceId == 0) {
            LOG_WARNING("SpellVisual: persistent area instance failed, model=", effect.path);
            continue;
        }
        m2Renderer_->setInstanceIsSpellVisual(instanceId);
        // PersistentAreaKit models author Stand (0), Hold (158) and optional
        // Decay (159) state sequences (LightningStorm_Cloud_State, FlameCircleEffect).
        m2Renderer_->startInstanceStateEffect(instanceId);
        instances.push_back(instanceId);
        LOG_INFO("SpellVisual: persistent area 0x", std::hex, key, std::dec, " visual=", visualId,
                 " kit=", kit->second, " model=", effect.path);
    }
}

void SpellVisualSystem::stopPersistentArea(uint64_t key) {
    // Impacts already falling finish their fall.
    activeAreaImpacts_.erase(key);
    if (auto sndIt = activeAreaSounds_.find(key); sndIt != activeAreaSounds_.end()) {
        audio::AudioEngine::instance().stopSound(sndIt->second);
        activeAreaSounds_.erase(sndIt);
    }
    const auto it = persistentAreas_.find(key);
    if (it == persistentAreas_.end()) return;
    if (m2Renderer_) {
        for (uint32_t instanceId : it->second) {
            const float decay = m2Renderer_->finishInstanceStateEffect(instanceId);
            if (decay > 0.0f) {
                activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = decay,
                    .isPrecast = false, .attachmentId = 0, .attachInstanceId = 0, .decaying = true});
            } else {
                m2Renderer_->retireInstance(instanceId);
            }
        }
    }
    persistentAreas_.erase(it);
}

// A beam leaves the caster where a missile of the same SpellVisual would:
// MissileAttachment -1 (Fel Energy Cosmetic 44574, Fel Crystal Cosmetic
// 44374, Mind Flay, Drain Life, Chain Lightning) at the hand the pose
// raises toward the target, M2 attachment 2 (see the missile launch in
// spell_handler.cpp). Build 8606, Magisters' Terrace Observation Grounds,
// both clients in one instance: the Sunblade Warlock's green beam leaves
// its outstretched hand; WoWee started it at the chest (34). A human
// player channelling Drain Life 689 or 44374 on the Shimmering Flats, arm
// raised away from the target, starts both natively and at attachment 2
// next to the chest. Other MissileAttachment values (Blue Beam 38034: 3,
// Fel Crystal Visual 44355: 1) keep the chest, which matched native for 38034
// (_experiments/20261119T1530Z.beam_caster_end).
uint32_t SpellVisualSystem::beamStartAttachment(uint32_t visualId) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto it = launchAttachments_.find(visualId);
    return it != launchAttachments_.end() && it->second == -1 ? 2 : 34;
}

SpellLaunchOrigin SpellVisualSystem::beamStartOrigin(uint32_t visualId, const pipeline::M2Model* casterModel) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto it = launchAttachments_.find(visualId);
    const int32_t missileAttachment = it != launchAttachments_.end() ? it->second : 0;
    const uint32_t chanAnim = channelAnimation(visualId);
    return resolveBeamStartOrigin(casterModel, chanAnim, missileAttachment);
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
                                         const glm::vec3& end, float speed, float launchDelay,
                                         bool impactAtEnd, bool areaImpactAtEnd,
                                         bool resistEffectAtEnd, std::function<void()> onArrive,
                                         uint32_t casterInstanceId, SpellLaunchOrigin launchOrigin,
                                         uint32_t targetInstanceId, uint32_t destinationAttachment,
                                         MissileTargetResolver targetResolver) {
    if (!m2Renderer_ || !hasSpellMissile(visualId)) return false;
    if (!cachedAssetManager_) return false;
    if (!std::isfinite(speed) || speed <= 0.0f) return false;
    assert(std::isfinite(launchDelay) && launchDelay >= 0.0f);

    const std::string& modelPath = spellVisualMissilePath_.at(visualId);
    const uint32_t modelId = loadSpellModel(modelPath);
    if (modelId == 0) return false;

    glm::vec3 resolvedEnd = end;
    uint32_t targetAttachInstanceId = 0;
    if (targetResolver) {
        if (const auto sample = targetResolver()) {
            resolvedEnd = sample->position;
            targetAttachInstanceId = sample->attachInstanceId;
        }
    }

    // Speed is yards a second and fixes the landing, as the server's hit delay
    // does. The original keeps that landing however late the missile leaves the
    // hand: Fireball 133 at 16.5 and 32 yards, Frostbolt 116 at 16.4, Shadow Bolt
    // 686 at 31.8 all reached the target within 0.03 s of SPELL_GO + distance /
    // speed, after leaving 0.17-0.31 s late. Long flights are not cut short:
    // Hyjal's Infernal 32148 (speed 20) from a relay 116 yd up falls for about
    // 6 s in the original.
    const glm::vec3 delta = resolvedEnd - start;
    const float distance = glm::length(delta);
    const float duration = distance / speed;
    const glm::vec3 initDir = (distance > 1e-4f) ? (delta / distance) : glm::vec3(1.0f, 0.0f, 0.0f);
    const float flightSpeed = missileFlightSpeed(distance, speed, launchDelay);
    spellMissiles_.push_back({.instanceId = 0, .modelId = modelId, .visualId = visualId, .start = start,
                              .end = resolvedEnd, .pos = start, .dir = initDir, .elapsed = 0.0f,
                              .launchAt = launchDelay, .duration = duration, .flightSpeed = flightSpeed,
                              .distanceTraveled = 0.0f, .targetAttachInstanceId = targetAttachInstanceId,
                              .impactAtEnd = impactAtEnd, .scale = spellVisualMissileScale_.at(visualId),
                              .areaImpactAtEnd = areaImpactAtEnd, .resistEffectAtEnd = resistEffectAtEnd,
                              .onArrive = std::move(onArrive), .casterInstanceId = casterInstanceId,
                              .launchOrigin = launchOrigin, .targetInstanceId = targetInstanceId,
                              .destinationAttachment = destinationAttachment,
                              .targetResolver = std::move(targetResolver)});
    LOG_INFO("SpellVisual: missile visualId=", visualId, " model=", modelPath,
             " distance=", distance, " speed=", speed, " lands=", duration, "s leaves=", launchDelay, "s");
    return true;
}

// The missile models point down +X. Turn it to face along its path, with the
// nose pitched up or down to follow a target above or below the caster.
void SpellVisualSystem::placeSpellMissileAt(const SpellMissile& missile, const glm::vec3& pos,
                                            const glm::vec3& rotation, float animProgress) {
    m2Renderer_->setInstanceTransform(missile.instanceId, placementModelMatrix(pos, rotation, missile.scale));
    // The model's sequence plays once over the flight, whatever its length.
    // Arcane_Missile_Lvl3.m2 (Darkmaster Gandling's Arcane Missiles 15791,
    // speed 20) spreads its three missiles to a third of its 1 s Stand and
    // brings them together at its end. Build 8606, Ras casting 15791: at
    // 7.3 yd (0.36 s) three trails part and meet at the target's chest; at
    // 16 yd (0.81 s) they part wider and meet at the target too. Played at
    // the clock, the first landed at full spread, 2.5 yd apart, and a melee
    // range one barely parted.
    m2Renderer_->setInstanceAnimationProgress(missile.instanceId, animProgress);
}

void SpellVisualSystem::placeSpellMissile(const SpellMissile& missile, float t) {
    const glm::vec3 pos = glm::mix(missile.start, missile.end, t);
    placeSpellMissileAt(missile, pos, missileDirectionToEuler(missile.end - missile.start), t);
}

void SpellVisualSystem::playPhysicalProjectile(const std::string& modelPath,
                                                const std::string& texturePath,
                                                const glm::vec3& start,
                                                const glm::vec3& end,
                                                float duration,
                                                bool spin,
                                                MissileTargetResolver targetResolver,
                                                float speed) {
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

    glm::vec3 resolvedEnd = end;
    if (targetResolver) {
        if (const auto sample = targetResolver()) resolvedEnd = sample->position;
    }
    const glm::vec3 delta = resolvedEnd - start;
    const float dist = glm::length(delta);
    const glm::vec3 initDir = (dist > 1e-4f) ? (delta / dist) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 rotation = missileDirectionToEuler(initDir);

    const uint32_t instanceId = characterRenderer->createInstance(modelId, start, rotation, 1.0f);
    if (instanceId == 0) return;
    physicalProjectiles_.push_back({.instanceId = instanceId, .start = start, .end = resolvedEnd,
                                    .pos = start, .dir = initDir, .rotation = rotation, .elapsed = 0.0f,
                                    .duration = std::max(duration, 0.05f),
                                    .speed = (std::isfinite(speed) && speed > 0.0f) ? speed : 0.0f,
                                    .distanceTraveled = 0.0f, .spin = spin,
                                    .targetResolver = std::move(targetResolver)});
}

// An effect on a unit faces the way the unit faces: its model's +X turns with
// the unit's yaw. Placed by position alone, every effect faced world +X, so an
// asymmetric one (Scarlet Myrmidon's enrage, a breath cone) pointed sideways
// for most headings.
// An attached effect is as large as its attachment: the unit's model scale
// times the attachment bone's authored scale (attachmentScale). Measured in
// the original 2.4.3 with Ice Block on one creature morphed and scaled, cube
// width / (unit scale x Base bone scale): Infernal at display scale 0.5, 1, 4
// and `.modify scale 2` 3.9-4.1 yd, human 3.8, gnome 4.4, dragon 4.0,
// Vaelastrasz (display 13992, 1.5) 4.1. His Corruption glow (chest bone 4.5,
// base bone 5.5) is therefore several times a human's. Effects at the unit's
// position (WorldEffect) keep 1: Entangling Roots measured equal on a gnome
// and a human but much larger on a dragon; that rule is not established.
void SpellVisualSystem::placeOnUnit(const SpellVisualInstance& visual, CharacterRenderer* characters,
                                    const glm::vec3& position, float scale) {
    assert(std::isfinite(scale) && scale > 0.0f);
    float yaw = 0.0f;
    if (!characters || !visual.attachInstanceId ||
        !characters->getInstanceYaw(visual.attachInstanceId, yaw)) {
        m2Renderer_->setInstanceTransform(visual.instanceId,
            glm::scale(glm::translate(glm::mat4(1.0f), position), glm::vec3(visual.effectScale)));
        return;
    }
    const glm::mat4 transform = glm::scale(
        glm::rotate(glm::translate(glm::mat4(1.0f), position), yaw, glm::vec3(0.0f, 0.0f, 1.0f)),
        glm::vec3(scale * visual.effectScale));
    m2Renderer_->setInstanceTransform(visual.instanceId, transform);
}

// Each impact is one play of the model at a uniformly random point of the
// area's disc, placed on the terrain or WMO floor at its own (x, y) so shards
// and bolts follow slopes rather than clipping into uphill ground or bursting
// in midair on the downhill side (build 8606, pair 8, Shimmering Flats rim).
void SpellVisualSystem::dropAreaImpacts(float deltaTime) {
    if (activeAreaImpacts_.empty() || !m2Renderer_) return;
    static thread_local std::mt19937 random{std::random_device{}()};
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    auto* terrain = renderer_ ? renderer_->getTerrainManager() : nullptr;
    auto* wmo = renderer_ ? renderer_->getWMORenderer() : nullptr;
    for (auto& [key, area] : activeAreaImpacts_) {
        area.due += deltaTime * area.impacts.perSecond;
        const float reach = area.radius * 2.0f;
        while (area.due >= 1.0f) {
            area.due -= 1.0f;
            const uint32_t modelId = loadSpellModel(area.impacts.path);
            if (modelId == 0) continue;
            const float distance = area.radius * std::sqrt(unit(random));
            const float angle = glm::two_pi<float>() * unit(random);
            glm::vec3 at = area.center + glm::vec3(distance * std::cos(angle), distance * std::sin(angle), 0.0f);
            const auto terH = terrain ? terrain->getHeightAt(at.x, at.y) : std::nullopt;
            const auto wmoH = wmo
                ? wmo->getFloorHeight(at.x, at.y, area.center.z + reach, nullptr, area.center.z)
                : std::nullopt;
            if (const auto floorZ = projected_texture::nearestSurfaceZ(area.center.z, reach, terH, wmoH)) {
                at.z = *floorZ;
            }
            const uint32_t instanceId = m2Renderer_->createInstance(modelId, at, glm::vec3(0.0f), 1.0f);
            if (instanceId == 0) {
                LOG_WARNING("SpellVisual: area impact instance failed, model=", area.impacts.path);
                continue;
            }
            m2Renderer_->setInstanceIsSpellVisual(instanceId);
            m2Renderer_->restartInstanceAnimation(instanceId);
            const float lifetime = m2Renderer_->getInstanceAnimDuration(instanceId) / 1000.0f;
            assert(lifetime > 0.0f);
            activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = lifetime,
                .isPrecast = false, .attachmentId = 0, .attachInstanceId = 0});
        }
    }
}

void SpellVisualSystem::update(float deltaTime) {
    beamClock_ += deltaTime;
    dropAreaImpacts(deltaTime);
    if (auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
        std::erase_if(unitAuraVisuals_, [&](const auto& entry) {
            glm::vec3 position;
            return !characters->getInstancePosition(entry.first, position);
        });
        for (auto it = recentUnitImpacts_.begin(); it != recentUnitImpacts_.end(); ) {
            glm::vec3 position;
            if (!characters->getInstancePosition(it->first, position)) {
                it = recentUnitImpacts_.erase(it);
                continue;
            }
            std::erase_if(it->second, [&](const auto& stamp) {
                return (beamClock_ - stamp.second) > kRecentImpactDedupSeconds;
            });
            if (it->second.empty()) it = recentUnitImpacts_.erase(it);
            else ++it;
        }
        for (auto it = unitChannelVisuals_.begin(); it != unitChannelVisuals_.end();) {
            glm::vec3 position;
            if (!characters->getInstancePosition(it->first, position)) {
                it = unitChannelVisuals_.erase(it);
                continue;
            }
            if (std::isfinite(it->second.remaining)) {
                it->second.remaining -= deltaTime;
                if (it->second.remaining <= 0.0f) {
                    const uint32_t owner = it->first;
                    stopChannelSound(owner);
                    it = unitChannelVisuals_.erase(it);
                    applyCharacterProcedures(owner);
                    continue;
                }
            }
            ++it;
        }
    }
    for (auto it = pendingAreaImpacts_.begin(); it != pendingAreaImpacts_.end(); ) {
        it->remaining -= deltaTime;
        if (it->remaining > 0.0f) {
            ++it;
            continue;
        }
        const PendingAreaImpact landed = *it;
        it = pendingAreaImpacts_.erase(it);
        playImpactAreaKit(landed.visualId, landed.destination);
    }
    if (activeSpellVisuals_.empty() && physicalProjectiles_.empty() && spellMissiles_.empty())
        return;

    // Collected first: the impact spawns into activeSpellVisuals_, and a
    // missile's arrival is the moment its impact/resist kit belongs to.
    struct ImpactArrival { uint32_t visualId; glm::vec3 pos; uint32_t attachInstanceId; };
    std::vector<ImpactArrival> arrivals;
    std::vector<std::pair<uint32_t, glm::vec3>> areaArrivals;
    std::vector<std::pair<glm::vec3, glm::vec3>> resistArrivals;
    std::vector<std::function<void()>> arriveCallbacks;
    for (auto it = spellMissiles_.begin(); it != spellMissiles_.end(); ) {
        it->elapsed += deltaTime;
        // Refresh live target position for unit-targeted homing missiles; if the
        // target despawned mid-flight, preserve the last resolved position.
        if (it->targetResolver) {
            if (const auto sample = it->targetResolver()) {
                it->end = sample->position;
                it->targetAttachInstanceId = sample->attachInstanceId;
            } else {
                it->targetAttachInstanceId = 0;
            }
        }
        // A missile is drawn from the hand at its release even when its landing
        // time has already passed; it then crosses in the next frame. Native
        // Frostbolt Volley 8398 from Ras Frostwhisper (release 0.37 s, speed 28):
        // at 6.8 yd (landing 0.24 s) the bolt shows near the target with its
        // trail back to the hand in one frame, at 7.8 yd it crosses in about
        // 0.1 s, at 15.7 yd it flies from the release to SPELL_GO + distance /
        // speed. Dropped before launch, a close target saw nothing at all.
        // It lands the frame after it was drawn at the end, and its trail
        // lives out where it lies.
        if (it->landed) {
            if (it->impactAtEnd) arrivals.push_back({it->visualId, it->end, it->targetAttachInstanceId});
            if (it->areaImpactAtEnd) areaArrivals.emplace_back(it->visualId, it->end);
            if (it->resistEffectAtEnd) resistArrivals.emplace_back(it->end, it->dir);
            if (it->onArrive) arriveCallbacks.push_back(std::move(it->onArrive));
            m2Renderer_->retireMissileInstance(it->instanceId);
            it = spellMissiles_.erase(it);
            continue;
        }
        if (it->elapsed < it->launchAt) {
            ++it;
            continue;
        }
        if (!it->instanceId) {
            // Sample live animated launch/target coordinates at the release moment
            // (t = launchAt), not the stale Precast/Stand pose from SMSG_SPELL_GO.
            if (auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
                if (it->casterInstanceId != 0) {
                    glm::mat4 mat(1.0f);
                    if (it->launchOrigin.source == SpellLaunchOrigin::Source::EventBone &&
                        characters->getBonePointTransform(it->casterInstanceId, it->launchOrigin.bone,
                                                          it->launchOrigin.localOffset, mat)) {
                        it->start = glm::vec3(mat[3]);
                    } else if (it->launchOrigin.source == SpellLaunchOrigin::Source::Attachment &&
                               characters->getAttachmentTransform(it->casterInstanceId,
                                                                  it->launchOrigin.attachmentId, mat)) {
                        it->start = glm::vec3(mat[3]);
                    }
                }
                if (!it->targetResolver && it->targetInstanceId != 0) {
                    glm::mat4 mat(1.0f);
                    if (characters->getAttachmentTransform(it->targetInstanceId,
                                                           it->destinationAttachment, mat)) {
                        it->end = glm::vec3(mat[3]);
                    }
                }
            }
            it->instanceId = m2Renderer_->createInstance(it->modelId, it->start, glm::vec3(0.0f), it->scale);
            if (!it->instanceId) {
                LOG_WARNING("SpellVisual: failed to create missile instance for visualId=", it->visualId);
                if (it->impactAtEnd) arrivals.push_back({it->visualId, it->end, it->targetAttachInstanceId});
                if (it->areaImpactAtEnd) areaArrivals.emplace_back(it->visualId, it->end);
                if (it->resistEffectAtEnd) resistArrivals.emplace_back(it->end, it->dir);
                if (it->onArrive) arriveCallbacks.push_back(std::move(it->onArrive));
                it = spellMissiles_.erase(it);
                continue;
            }
            m2Renderer_->setInstanceIsSpellVisual(it->instanceId);
            it->pos = it->start;
            const glm::vec3 toEnd = it->end - it->start;
            if (const float len = glm::length(toEnd); len > 1e-4f) it->dir = toEnd / len;
            placeSpellMissileAt(*it, it->pos, missileDirectionToEuler(it->dir), 0.0f);
            ++it;
            continue;
        }
        if (it->targetResolver) {
            const float stepSpeed = (it->elapsed >= missileMaxFlightDuration(it->duration)) ? 0.0f : it->flightSpeed;
            const HomingStepResult step = stepHomingMissile(
                it->pos, it->end, it->dir, stepSpeed, deltaTime, it->distanceTraveled);
            it->pos = step.position;
            it->dir = step.direction;
            placeSpellMissileAt(*it, it->pos, step.rotation, step.animProgress);
            it->landed = step.arrived;
        } else {
            const float dist = glm::length(it->end - it->start);
            const float flight = (it->flightSpeed > 0.0f) ? (dist / it->flightSpeed) : 0.0f;
            const float t = flight > 0.0f ? std::min(1.0f, (it->elapsed - it->launchAt) / flight) : 1.0f;
            placeSpellMissile(*it, t);
            it->landed = t >= 1.0f;
        }
        ++it;
    }
    for (const auto& arrival : arrivals)
        playSpellVisual(arrival.visualId, arrival.pos, /*useImpactKit=*/true, arrival.attachInstanceId);
    for (const auto& [visualId, pos] : areaArrivals)
        playImpactAreaKit(visualId, pos);
    for (const auto& [pos, dir] : resistArrivals)
        playResistImmuneEffect(pos, dir);
    for (auto& cb : arriveCallbacks)
        cb();

    // Get character bone tracking context (once per frame)
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;

    std::erase_if(drainingCasterEffects_,
                  [&](const DrainingEffect& effect) { return !m2Renderer_->hasInstance(effect.instanceId); });
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        // Effects on a unit hidden in an unseen WMO room are hidden with it.
        if (it->attachInstanceId && charRenderer)
            m2Renderer_->setInstanceFade(it->instanceId, charRenderer->isInstanceRoomHidden(it->attachInstanceId) ? 0.0f : 1.0f);
        if (it->auraVisualId) {
            glm::vec3 position;
            if (!charRenderer || !charRenderer->getInstancePosition(it->attachInstanceId, position)) {
                unitAuraVisuals_.erase(it->attachInstanceId);
                m2Renderer_->removeInstance(it->instanceId);
                it = activeSpellVisuals_.erase(it);
                continue;
            }
            if (!it->attachmentId) placeOnUnit(*it, charRenderer, position, 1.0f);
        } else if ((it->isPrecast || it->isChannel) && it->attachInstanceId != 0) {
            glm::vec3 position;
            if (!charRenderer || !charRenderer->getInstancePosition(it->attachInstanceId, position)) {
                if (it->isPrecast) stopPrecastSound(it->attachInstanceId);
                if (it->isChannel) stopChannelSound(it->attachInstanceId);
                m2Renderer_->removeInstance(it->instanceId);
                it = activeSpellVisuals_.erase(it);
                continue;
            }
        }
        it->elapsed += deltaTime;
        if (it->elapsed >= it->duration) {
            retireEffect(*it);
            it = activeSpellVisuals_.erase(it);
        } else {
            // Update position for bone-tracked effects - follow the CASTER's
            // hands/chest/head, not the local player's.
            if (it->attachmentId != 0 && it->attachInstanceId != 0 && charRenderer) {
                glm::mat4 attachMat;
                if (charRenderer->getAttachmentTransform(it->attachInstanceId, it->attachmentId, attachMat)) {
                    glm::vec3 bonePos = glm::vec3(attachMat[3]);
                    if (it->orientWithAttachment)
                        m2Renderer_->setInstanceTransform(it->instanceId,
                                                          glm::scale(attachMat, glm::vec3(it->effectScale)));
                    else placeOnUnit(*it, charRenderer, bonePos, attachmentScale(attachMat));
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
            bool arrived = false;
            if (it->targetResolver && it->speed > 0.0f) {
                if (const auto sample = it->targetResolver()) it->end = sample->position;
                const float stepSpeed = (it->elapsed >= missileMaxFlightDuration(it->duration)) ? 0.0f : it->speed;
                const HomingStepResult step = stepHomingMissile(
                    it->pos, it->end, it->dir, stepSpeed, deltaTime, it->distanceTraveled);
                it->pos = step.position;
                it->dir = step.direction;
                it->rotation = step.rotation;
                charRenderer->setInstancePosition(it->instanceId, it->pos);
                glm::vec3 rotation = it->rotation;
                if (it->spin) rotation.x += step.animProgress * glm::two_pi<float>() * 2.0f;
                charRenderer->setInstanceRotation(it->instanceId, rotation);
                arrived = step.arrived;
            } else {
                const float t = std::min(it->elapsed / it->duration, 1.0f);
                charRenderer->setInstancePosition(
                    it->instanceId, glm::mix(it->start, it->end, t));
                if (it->spin) {
                    glm::vec3 rotation = it->rotation;
                    rotation.x += t * glm::two_pi<float>() * 2.0f;
                    charRenderer->setInstanceRotation(it->instanceId, rotation);
                }
                arrived = t >= 1.0f;
            }
            if (arrived) {
                charRenderer->removeInstance(it->instanceId);
                it = physicalProjectiles_.erase(it);
            } else {
                ++it;
            }
        }
    }
    updateWeaponTrails(deltaTime);
}

// Death is not an interrupt: build 8606, Scholomance Dark Summoner killed
// (`.die`) 0.4 s into the CastKit of Summon Frail Skeleton 16531
// (SummonPet_Cast_Impact_Base, its rising sparks 0.3 s lives) while
// channelling Evocation 12051 (CycloneWater_State, smoke 3 s lives): cone,
// smoke and sparks all gone between two 15 Hz frames as the Death animation
// starts. An Evocation interrupted by moving leaves its smoke 0.8 s, and so
// does the interrupt arriving with the death (SMSG_SPELL_FAILURE first), so
// the death removes the rest. Sunshatter 45215's channel beam goes with them.
// Evidence: _experiments/20261119T1300Z.channel_end (evdie7_fine.jpg,
// bmdie7_sheet.jpg, go8_sheet.jpg for the same CastKit playing out alive).
void SpellVisualSystem::removeCasterStages(uint32_t attachInstanceId) {
    if (!m2Renderer_ || !attachInstanceId) return;
    stopPrecastSound(attachInstanceId);
    stopChannelSound(attachInstanceId);
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end();) {
        if (it->attachInstanceId == attachInstanceId && (it->isPrecast || it->isCast || it->isChannel)) {
            LOG_DEBUG("SpellVisual: caster died, effect instance ", it->instanceId, " removed");
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(castBeams_, [&](const CastBeam& beam) { return beam.start.instanceId == attachInstanceId; });
    std::erase_if(drainingCasterEffects_, [&](const DrainingEffect& effect) {
        if (effect.attachInstanceId != attachInstanceId) return false;
        m2Renderer_->removeInstance(effect.instanceId);
        return true;
    });
    if (unitChannelVisuals_.erase(attachInstanceId))
        applyCharacterProcedures(attachInstanceId);
    std::erase_if(activeWeaponTrails_, [&](const ActiveUnitWeaponTrail& trail) {
        return trail.ownerInstanceId == attachInstanceId;
    });
}

void SpellVisualSystem::retireEffect(const SpellVisualInstance& effect) {
    m2Renderer_->retireInstance(effect.instanceId);
    if (effect.attachInstanceId && (effect.isPrecast || effect.isCast || effect.isChannel) &&
        m2Renderer_->hasInstance(effect.instanceId))
        drainingCasterEffects_.push_back({effect.attachInstanceId, effect.instanceId});
}

// As any kit's end: mesh and ribbons go, emitters stop, particles out live
// out. Build 8606, Scholomance Dark Summoner killed (`.die`) 7 s into Summon
// Frail Skeleton 16531 (precast kit 137: Summon_PreCast_Hand glows,
// SummonPet_Impact_Base circle): hand glows gone the next frame, circle and
// sparks fade within 0.3 s; WoWee played the 10 s precast out
// (_experiments/20261119T1300Z.channel_end, die1_kill.jpg).
void SpellVisualSystem::cancelPrecastVisuals(uint32_t attachInstanceId) {
    if (!m2Renderer_ || !attachInstanceId) return;
    stopPrecastSound(attachInstanceId);
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        if (it->isPrecast && it->attachInstanceId == attachInstanceId) {
            retireEffect(*it);
            it = activeSpellVisuals_.erase(it);
        } else {
            ++it;
        }
    }
}

bool SpellVisualSystem::hasRecentImpactVisual(uint32_t attachInstanceId, uint32_t visualId) const {
    if (!attachInstanceId || !visualId) return false;
    const auto unitIt = recentUnitImpacts_.find(attachInstanceId);
    if (unitIt == recentUnitImpacts_.end()) return false;
    const auto visIt = unitIt->second.find(visualId);
    if (visIt == unitIt->second.end()) return false;
    return (beamClock_ - visIt->second) <= kRecentImpactDedupSeconds;
}

void SpellVisualSystem::transferUnitVisuals(uint32_t fromInstanceId, uint32_t toInstanceId) {
    if (!fromInstanceId || !toInstanceId || fromInstanceId == toInstanceId) return;
    auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    glm::vec3 newPos(0.0f);
    const bool havePos = characters && characters->getInstancePosition(toInstanceId, newPos);
    for (auto& visual : activeSpellVisuals_) {
        if (visual.attachInstanceId != fromInstanceId) continue;
        visual.attachInstanceId = toInstanceId;
        if (!characters || !m2Renderer_) continue;
        if (visual.attachmentId != 0) {
            glm::mat4 attachMat(1.0f);
            if (characters->getAttachmentTransform(toInstanceId, visual.attachmentId, attachMat)) {
                if (visual.orientWithAttachment)
                    m2Renderer_->setInstanceTransform(visual.instanceId,
                                                      glm::scale(attachMat, glm::vec3(visual.effectScale)));
                else
                    placeOnUnit(visual, characters, glm::vec3(attachMat[3]), attachmentScale(attachMat));
            } else if (!slotSkippedWhenAbsent(visual.attachmentId) && havePos) {
                visual.attachmentId = 0;
                placeOnUnit(visual, characters, newPos, 1.0f);
            }
        } else if (havePos) {
            placeOnUnit(visual, characters, newPos, 1.0f);
        }
    }
    if (auto node = unitAuraVisuals_.extract(fromInstanceId)) {
        node.key() = toInstanceId;
        unitAuraVisuals_.erase(toInstanceId);
        unitAuraVisuals_.insert(std::move(node));
    }
    if (auto node = unitChannelVisuals_.extract(fromInstanceId)) {
        node.key() = toInstanceId;
        unitChannelVisuals_.erase(toInstanceId);
        unitChannelVisuals_.insert(std::move(node));
    }
    if (auto node = activePrecastSounds_.extract(fromInstanceId)) {
        node.key() = toInstanceId;
        activePrecastSounds_.erase(toInstanceId);
        activePrecastSounds_.insert(std::move(node));
    }
    if (auto node = activeChannelSounds_.extract(fromInstanceId)) {
        node.key() = toInstanceId;
        activeChannelSounds_.erase(toInstanceId);
        activeChannelSounds_.insert(std::move(node));
    }
    if (auto node = fieldChannels_.extract(fromInstanceId)) {
        node.key() = toInstanceId;
        fieldChannels_.erase(toInstanceId);
        fieldChannels_.insert(std::move(node));
    }
    if (heldChannelKits_.erase(fromInstanceId)) heldChannelKits_.insert(toInstanceId);
    if (auto node = recentUnitImpacts_.extract(fromInstanceId)) {
        auto& dst = recentUnitImpacts_[toInstanceId];
        for (const auto& [vis, stamp] : node.mapped()) {
            auto [it, inserted] = dst.emplace(vis, stamp);
            if (!inserted) it->second = std::max(it->second, stamp);
        }
    }
    for (auto& draining : drainingCasterEffects_)
        if (draining.attachInstanceId == fromInstanceId) draining.attachInstanceId = toInstanceId;
    for (auto& beam : castBeams_) {
        if (beam.start.instanceId == fromInstanceId) beam.start.instanceId = toInstanceId;
        if (beam.end.instanceId == fromInstanceId) beam.end.instanceId = toInstanceId;
    }
    applyCharacterProcedures(toInstanceId);
}

void SpellVisualSystem::reset() {
    for (const auto& [inst, snd] : activePrecastSounds_) audio::AudioEngine::instance().stopSound(snd);
    for (const auto& [inst, snd] : activeChannelSounds_) audio::AudioEngine::instance().stopSound(snd);
    for (const auto& [key, snd] : activeAreaSounds_) audio::AudioEngine::instance().stopSound(snd);
    activePrecastSounds_.clear();
    activeChannelSounds_.clear();
    activeAreaSounds_.clear();
    unitAuraVisuals_.clear();
    unitChannelVisuals_.clear();
    recentUnitImpacts_.clear();
    beams_.clear();
    hopBeams_.clear();
    channelTargets_.clear();
    castBeams_.clear();
    fieldChannels_.clear();
    heldChannelKits_.clear();
    endedChannels_.clear();
    drainingCasterEffects_.clear();
    activeWeaponTrails_.clear();
    if (m2Renderer_) {
        m2Renderer_->setChainStrips({});
        m2Renderer_->setWeaponTrailStrips({});
    }
    // Clear lingering spell visual instances from the previous map/combat session.
    // Without this, old effects could remain visible after teleport or map change.
    for (auto& sv : activeSpellVisuals_) {
        if (m2Renderer_) m2Renderer_->removeInstance(sv.instanceId);
    }
    activeSpellVisuals_.clear();
    for (const auto& missile : spellMissiles_) {
        if (m2Renderer_ && missile.instanceId) m2Renderer_->removeInstance(missile.instanceId);
    }
    spellMissiles_.clear();
    pendingAreaImpacts_.clear();
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
    activeAreaImpacts_.clear();
    // Reset the negative cache so models that failed during asset loading can retry.
    spellVisualFailedModels_.clear();
}

} // namespace rendering
} // namespace wowee
