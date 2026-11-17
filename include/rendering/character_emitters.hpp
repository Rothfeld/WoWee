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

/// The alpha the emitters are drawn at; 0 means the owner is not drawn and
/// its emitters are dropped.
inline float characterEmitterFade(bool visible, float opacity, float cameraOpacity) {
    return visible ? opacity * cameraOpacity : 0.0f;
}

/// The M2Renderer model id for a character model's emitters. Hashed from the
/// model path and the character model id, as world doodads hash their path.
inline uint32_t characterEmitterModelId(const std::string& modelName, uint32_t characterModelId) {
    return static_cast<uint32_t>(std::hash<std::string>{}(
        "character-emitters:" + modelName + ":" + std::to_string(characterModelId)));
}

} // namespace wowee::rendering
