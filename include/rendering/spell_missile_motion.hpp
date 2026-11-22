#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>

struct lua_State;

namespace wowee {
namespace pipeline {
class DBCFile;
}

namespace rendering {

/// Per-frame inputs supplied to a TBC 2.4.3 SpellMissileMotion.dbc Lua script.
struct MissileMotionInput {
    float progress = 0.0f;         // [0, 1] normalized flight progress
    float time = 0.0f;             // seconds elapsed since launch
    float startDistance = 0.0f;    // initial launch-to-destination distance (yards)
    float totalDistance = 0.0f;    // current launch-to-destination distance (yards)
    int missileIndex = 0;          // 0 .. missileCount - 1
    int missileCount = 1;          // SpellMissileMotion.MissileCount (>= 1)
    float rand1 = 0.5f;            // per-missile uniform [0, 1]
    float rand2 = 0.5f;            // per-missile uniform [0, 1]
    float rand3 = 0.5f;            // per-missile uniform [0, 1]
};

/// Outputs written by a SpellMissileMotion.dbc Lua script.
struct MissileMotionOutput {
    float transFront = 0.0f;       // yards along flight forward axis
    float transRight = 0.0f;       // yards along flight right axis
    float transUp = 0.0f;          // yards along flight up axis
    float transAngle = 0.0f;       // degrees around flight forward axis (0 = +up, 90 = +right)
    float transMag = 0.0f;         // yards radial offset at transAngle
    float modelPitch = 0.0f;       // degrees added to model pitch
    float modelYaw = 0.0f;         // degrees added to model yaw
    float modelRoll = 0.0f;        // degrees added to model roll
    float scale = 1.0f;            // multiplicative model scale
    float speedScalar = 1.0f;      // flight speed multiplier
};

/// Orthonormal basis aligned with a missile's straight-line chord (start -> end).
struct MissileBasis {
    glm::vec3 forward{1.0f, 0.0f, 0.0f};
    glm::vec3 right{0.0f, -1.0f, 0.0f};
    glm::vec3 up{0.0f, 0.0f, 1.0f};
};

/// Build an orthonormal basis from chord vector (end - start) in right-handed Z-up world space.
MissileBasis computeMissileBasis(const glm::vec3& chord);

/// Evaluate a SpellMissileMotionOutput displacement into world space along start -> end.
glm::vec3 applyMissileMotion(const glm::vec3& start, const glm::vec3& end,
                             float progress, const MissileMotionOutput& out);

/// Default parabolic arc (transUp = 4 * t * (1 - t) * startDistance * 0.15) used by
/// SpellVisual.dbc rows with MissilePathType == 2 and MissileMotion == 0.
MissileMotionOutput defaultParabolicMotion(float startDistance, float progress);

/// Loads and evaluates TBC 2.4.3 SpellMissileMotion.dbc Lua motion scripts in a
/// minimal sandboxed Lua 5.1 state (exposing only degree-based sin/cos and script globals).
class SpellMissileMotionTable {
public:
    SpellMissileMotionTable();
    ~SpellMissileMotionTable();

    SpellMissileMotionTable(const SpellMissileMotionTable&) = delete;
    SpellMissileMotionTable& operator=(const SpellMissileMotionTable&) = delete;
    SpellMissileMotionTable(SpellMissileMotionTable&& other) noexcept;
    SpellMissileMotionTable& operator=(SpellMissileMotionTable&& other) noexcept;

    bool loadFromDbc(const pipeline::DBCFile& dbc);
    void clear();

    bool isLoaded() const { return loaded_; }
    size_t count() const { return motions_.size(); }
    bool hasMotion(uint32_t motionId) const;
    uint32_t missileCount(uint32_t motionId) const;

    /// Execute the precompiled Lua motion script for motionId.
    /// Returns false if motionId is unknown or fails to run (out stays default-initialized).
    bool evaluate(uint32_t motionId, const MissileMotionInput& input, MissileMotionOutput& out) const;

private:
    struct MotionEntry {
        std::string name;
        int luaFuncRef = -2;  // LUA_NOREF
        uint32_t flags = 0;
        uint32_t missileCount = 1;
    };

    void ensureLuaState();
    void releaseLuaRefs();

    lua_State* L_ = nullptr;
    std::unordered_map<uint32_t, MotionEntry> motions_;
    bool loaded_ = false;
};

}  // namespace rendering
}  // namespace wowee
