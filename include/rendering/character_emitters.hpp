#pragma once

// Particle and ribbon emitters of models CharacterRenderer draws.
//
// CharacterRenderer skins creatures, players and their attached weapons and
// enchant models, but has no particle system: every authored emitter on those
// models went undrawn (wisp/wisp.m2 has 11, fireelemental/fireelemental.m2
// 34; 248 of 481 TBC creature model directories carry emitters, all with
// named type-0 textures). M2Renderer owns the particle and ribbon path. Each
// such character gets an emitter-only copy of its model loaded there and one
// externally posed instance, fed every frame with the character's own model
// matrix, skinned bone matrices, sequence and clocks, so emitters ride the
// animated skeleton exactly as the mesh does.

#include "pipeline/m2_loader.hpp"

#include <cassert>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace wowee::rendering {

/// Whether a model has anything for the emitter path to run.
inline bool hasEffectEmitters(const pipeline::M2Model& model) {
    return !model.particleEmitters.empty() || !model.ribbonEmitters.empty();
}

/// The model with its drawable and collision geometry removed. Bones,
/// sequences, global sequences, textures, materials and emitters stay: the
/// emitters index the bones and textures, and their tracks the sequences.
inline pipeline::M2Model emitterOnlyModel(const pipeline::M2Model& model) {
    pipeline::M2Model effects = model;
    effects.vertices.clear();
    effects.indices.clear();
    effects.batches.clear();
    effects.collisionVertices.clear();
    effects.collisionIndices.clear();
    effects.collisionNormals.clear();
    return effects;
}

/// A ParticleColor.dbc row: ARGB start[3], mid[3], end[3]; slot k serves
/// emitters with colour index 11 + k. Rows 355/356 carry real alpha
/// (0x00 -> 0x3c -> 0x00) as an emitter's own colour words do.
using ParticleColorRow = std::array<uint32_t, 9>;

/// The emitter-only model recoloured by its display's ParticleColor row.
inline void applyParticleColors(pipeline::M2Model& model, const ParticleColorRow& row) {
    const auto rgb = [](uint32_t argb) {
        return glm::vec3((argb >> 16) & 255u, (argb >> 8) & 255u, argb & 255u) / 255.0f;
    };
    const auto alpha = [](uint32_t argb) { return static_cast<float>(argb >> 24) / 255.0f; };
    for (auto& emitter : model.particleEmitters) {
        if (emitter.particleColorIndex == 0) continue;
        const size_t k = emitter.particleColorIndex - 11u;
        assert(k < 3);
        assert(emitter.particleColor.vec3Values.size() == 3 && emitter.particleAlpha.floatValues.size() == 3);
        emitter.particleColor.vec3Values = {rgb(row[k]), rgb(row[3 + k]), rgb(row[6 + k])};
        emitter.particleAlpha.floatValues = {alpha(row[k]), alpha(row[3 + k]), alpha(row[6 + k])};
    }
}

/// The alpha the emitters are drawn at; 0 means the owner is not drawn and
/// its emitters are dropped.
inline float characterEmitterFade(bool visible, float opacity, float cameraOpacity) {
    return visible ? opacity * cameraOpacity : 0.0f;
}

/// Whether the owner holds a finished one-shot sequence on its last frame
/// (a corpse at the end of Death). Its animation clock has stopped and its
/// emitters bear no particles: ElementalEarth's Death bursts keep rate 100
/// from 27701 ms to a key at 33333 ms, past Death's end at 30000 ms, and the
/// original client leaves the Stone Rumbler's corpse without them.
inline bool characterEmitterClockHeld(bool loop, uint32_t sequenceDurationMs, float timeMs) {
    return !loop && sequenceDurationMs > 0 && timeMs >= static_cast<float>(sequenceDurationMs);
}

/// The M2Renderer model id for a character model's emitters. Hashed from the
/// model path and the character model id, as world doodads hash their path.
inline uint32_t characterEmitterModelId(const std::string& modelName, uint32_t characterModelId,
                                        uint32_t particleColorId) {
    return static_cast<uint32_t>(std::hash<std::string>{}(
        "character-emitters:" + modelName + ":" + std::to_string(characterModelId) + ":" +
        std::to_string(particleColorId)));
}

} // namespace wowee::rendering
