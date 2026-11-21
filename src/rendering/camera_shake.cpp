#include "rendering/camera_shake.hpp"

#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"

#include <cmath>

namespace wowee {
namespace rendering {

bool CameraShakeTable::load(const pipeline::DBCFile* cameraShakesDbc,
                            const pipeline::DBCFile* spellEffectShakesDbc,
                            const pipeline::DBCFile* creatureModelDataDbc) {
    shakes_.clear();
    spellEffectShakes_.clear();
    footstepShakeByModel_.clear();
    deathThudShakeByModel_.clear();

    if (!cameraShakesDbc || !cameraShakesDbc->isLoaded() || cameraShakesDbc->getFieldCount() < 8) {
        return false;
    }

    for (uint32_t i = 0; i < cameraShakesDbc->getRecordCount(); ++i) {
        const uint32_t id = cameraShakesDbc->getUInt32(i, 0);
        if (id == 0) continue;
        CameraShakeEntry entry;
        entry.id = id;
        entry.shakeType = cameraShakesDbc->getUInt32(i, 1);
        entry.axis = cameraShakesDbc->getUInt32(i, 2);
        entry.amplitude = cameraShakesDbc->getFloat(i, 3);
        entry.frequency = cameraShakesDbc->getFloat(i, 4);
        entry.duration = cameraShakesDbc->getFloat(i, 5);
        entry.phase = cameraShakesDbc->getFloat(i, 6);
        entry.coefficient = cameraShakesDbc->getFloat(i, 7);
        if (!std::isfinite(entry.amplitude) || !std::isfinite(entry.frequency) ||
            !std::isfinite(entry.duration) || !std::isfinite(entry.phase) ||
            !std::isfinite(entry.coefficient)) {
            continue;
        }
        if (entry.amplitude <= 0.0f || entry.frequency <= 0.0f || entry.duration <= entry.phase) {
            continue;
        }
        shakes_[id] = entry;
    }

    if (spellEffectShakesDbc && spellEffectShakesDbc->isLoaded() &&
        spellEffectShakesDbc->getFieldCount() >= 4) {
        for (uint32_t i = 0; i < spellEffectShakesDbc->getRecordCount(); ++i) {
            const uint32_t id = spellEffectShakesDbc->getUInt32(i, 0);
            if (id == 0) continue;
            spellEffectShakes_[id] = {
                spellEffectShakesDbc->getUInt32(i, 1),
                spellEffectShakesDbc->getUInt32(i, 2),
                spellEffectShakesDbc->getUInt32(i, 3),
            };
        }
    }

    if (creatureModelDataDbc && creatureModelDataDbc->isLoaded() &&
        creatureModelDataDbc->getFieldCount() > 12) {
        const auto* cmdLayout = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("CreatureModelData")
            : nullptr;
        uint32_t idCol = 0;
        uint32_t footCol = 11;
        uint32_t deathCol = 12;
        if (cmdLayout) {
            if (const uint32_t c = (*cmdLayout)["ID"]; c != 0xFFFFFFFF && c < creatureModelDataDbc->getFieldCount()) {
                idCol = c;
            }
            if (const uint32_t c = (*cmdLayout)["FootstepShakeSize"];
                c != 0xFFFFFFFF && c < creatureModelDataDbc->getFieldCount()) {
                footCol = c;
            }
            if (const uint32_t c = (*cmdLayout)["DeathThudShakeSize"];
                c != 0xFFFFFFFF && c < creatureModelDataDbc->getFieldCount()) {
                deathCol = c;
            }
        }
        for (uint32_t i = 0; i < creatureModelDataDbc->getRecordCount(); ++i) {
            const uint32_t modelId = creatureModelDataDbc->getUInt32(i, idCol);
            if (modelId == 0) continue;
            const uint32_t footShake = creatureModelDataDbc->getUInt32(i, footCol);
            const uint32_t deathShake = creatureModelDataDbc->getUInt32(i, deathCol);
            if (footShake != 0 && shakes_.count(footShake) > 0) {
                footstepShakeByModel_[modelId] = footShake;
            }
            if (deathShake != 0 && shakes_.count(deathShake) > 0) {
                deathThudShakeByModel_[modelId] = deathShake;
            }
        }
    }

    return !shakes_.empty();
}

} // namespace rendering
} // namespace wowee
