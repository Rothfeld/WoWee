#include "rendering/spell_missile_motion.hpp"

#include "core/logger.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace wowee::rendering {

namespace {

constexpr float kDegToRad = glm::pi<float>() / 180.0f;

// SpellMissileMotion.dbc scripts call sin(deg) and cos(deg) in degrees.
int luaDegSin(lua_State* L) {
    const double deg = luaL_checknumber(L, 1);
    lua_pushnumber(L, std::sin(deg * static_cast<double>(kDegToRad)));
    return 1;
}

int luaDegCos(lua_State* L) {
    const double deg = luaL_checknumber(L, 1);
    lua_pushnumber(L, std::cos(deg * static_cast<double>(kDegToRad)));
    return 1;
}

void setGlobalNumber(lua_State* L, const char* name, float value) {
    lua_pushnumber(L, static_cast<lua_Number>(value));
    lua_setglobal(L, name);
}

float getGlobalNumber(lua_State* L, const char* name, float fallback) {
    lua_getglobal(L, name);
    float result = fallback;
    if (lua_isnumber(L, -1)) {
        const float v = static_cast<float>(lua_tonumber(L, -1));
        if (std::isfinite(v)) result = v;
    }
    lua_pop(L, 1);
    return result;
}

}  // namespace

MissileBasis computeMissileBasis(const glm::vec3& chord) {
    MissileBasis basis;
    const float len = glm::length(chord);
    if (len > 1e-4f && std::isfinite(len)) {
        basis.forward = chord / len;
    }
    const glm::vec3 refUp = (std::abs(basis.forward.z) > 0.999f)
        ? glm::vec3(1.0f, 0.0f, 0.0f)
        : glm::vec3(0.0f, 0.0f, 1.0f);
    basis.right = glm::normalize(glm::cross(basis.forward, refUp));
    basis.up = glm::normalize(glm::cross(basis.right, basis.forward));
    return basis;
}

glm::vec3 applyMissileMotion(const glm::vec3& start, const glm::vec3& end,
                             float progress, const MissileMotionOutput& out) {
    const float t = std::clamp(progress, 0.0f, 1.0f);
    const glm::vec3 chord = end - start;
    const glm::vec3 basePos = glm::mix(start, end, t);
    const MissileBasis basis = computeMissileBasis(chord);

    const float rad = out.transAngle * kDegToRad;
    const glm::vec3 polarOffset =
        basis.up * (std::cos(rad) * out.transMag) +
        basis.right * (std::sin(rad) * out.transMag);
    const glm::vec3 offset =
        basis.forward * out.transFront +
        basis.right * out.transRight +
        basis.up * out.transUp +
        polarOffset;
    return basePos + offset;
}

MissileMotionOutput defaultParabolicMotion(float startDistance, float progress) {
    MissileMotionOutput out;
    const float u = progress * 2.0f - 1.0f;
    out.transMag = (1.0f - u * u) * std::max(0.0f, startDistance) * 0.15f;
    return out;
}

SpellMissileMotionTable::SpellMissileMotionTable() = default;

SpellMissileMotionTable::~SpellMissileMotionTable() {
    clear();
}

SpellMissileMotionTable::SpellMissileMotionTable(SpellMissileMotionTable&& other) noexcept
    : L_(other.L_), motions_(std::move(other.motions_)), loaded_(other.loaded_) {
    other.L_ = nullptr;
    other.loaded_ = false;
}

SpellMissileMotionTable& SpellMissileMotionTable::operator=(SpellMissileMotionTable&& other) noexcept {
    if (this != &other) {
        clear();
        L_ = other.L_;
        motions_ = std::move(other.motions_);
        loaded_ = other.loaded_;
        other.L_ = nullptr;
        other.loaded_ = false;
    }
    return *this;
}

void SpellMissileMotionTable::ensureLuaState() {
    if (L_) return;
    L_ = luaL_newstate();
    if (!L_) return;
    // Sandbox: expose only degree-based sin and cos; no os/io/package/debug libraries.
    lua_register(L_, "sin", luaDegSin);
    lua_register(L_, "cos", luaDegCos);
}

void SpellMissileMotionTable::releaseLuaRefs() {
    if (L_) {
        for (auto& [id, entry] : motions_) {
            if (entry.luaFuncRef != LUA_NOREF) {
                luaL_unref(L_, LUA_REGISTRYINDEX, entry.luaFuncRef);
                entry.luaFuncRef = LUA_NOREF;
            }
        }
        lua_close(L_);
        L_ = nullptr;
    }
    motions_.clear();
}

void SpellMissileMotionTable::clear() {
    releaseLuaRefs();
    loaded_ = false;
}

bool SpellMissileMotionTable::loadFromDbc(const pipeline::DBCFile& dbc) {
    clear();
    if (!dbc.isLoaded() || dbc.getFieldCount() < 5) return false;
    ensureLuaState();
    if (!L_) return false;

    uint32_t compiled = 0;
    uint32_t failed = 0;
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        const uint32_t id = dbc.getUInt32(i, 0);
        if (id == 0) continue;
        const std::string name = dbc.getString(i, 1);
        const std::string script = dbc.getString(i, 2);
        const uint32_t flags = dbc.getUInt32(i, 3);
        const uint32_t count = std::max(1u, dbc.getUInt32(i, 4));
        if (script.empty()) continue;

        const std::string chunkName = "SpellMissileMotion:" + std::to_string(id);
        if (luaL_loadbuffer(L_, script.data(), script.size(), chunkName.c_str()) != 0) {
            const char* err = lua_tostring(L_, -1);
            LOG_WARNING("SpellMissileMotion: compile failed id=", id, " name='", name,
                        "' err=", err ? err : "unknown");
            lua_pop(L_, 1);
            ++failed;
            continue;
        }
        const int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
        motions_[id] = MotionEntry{
            .name = name,
            .luaFuncRef = ref,
            .flags = flags,
            .missileCount = count,
        };
        ++compiled;
    }

    loaded_ = true;
    LOG_INFO("SpellMissileMotion: compiled ", compiled, " motion scripts (failed=", failed, ")");
    return true;
}

bool SpellMissileMotionTable::hasMotion(uint32_t motionId) const {
    return motionId != 0 && motions_.find(motionId) != motions_.end();
}

uint32_t SpellMissileMotionTable::missileCount(uint32_t motionId) const {
    const auto it = motions_.find(motionId);
    return (it != motions_.end()) ? it->second.missileCount : 1u;
}

bool SpellMissileMotionTable::evaluate(uint32_t motionId, const MissileMotionInput& input,
                                       MissileMotionOutput& out) const {
    out = MissileMotionOutput{};
    if (!L_ || motionId == 0) return false;
    const auto it = motions_.find(motionId);
    if (it == motions_.end() || it->second.luaFuncRef == LUA_NOREF) return false;

    const float clampedProgress = std::clamp(input.progress, 0.0f, 1.0f);
    const float safeDist = std::max(0.0f, input.totalDistance);
    const float safeStartDist = std::max(0.0f, input.startDistance);
    const float distToFire = clampedProgress * safeDist;
    const float distToImpact = (1.0f - clampedProgress) * safeDist;

    // Reset output globals so every script starts from clean defaults.
    setGlobalNumber(L_, "transFront", 0.0f);
    setGlobalNumber(L_, "transRight", 0.0f);
    setGlobalNumber(L_, "transUp", 0.0f);
    setGlobalNumber(L_, "transAngle", 0.0f);
    setGlobalNumber(L_, "transMag", 0.0f);
    setGlobalNumber(L_, "modelPitch", 0.0f);
    setGlobalNumber(L_, "modelYaw", 0.0f);
    setGlobalNumber(L_, "modelRoll", 0.0f);
    setGlobalNumber(L_, "scale", 1.0f);
    setGlobalNumber(L_, "speedScalar", 1.0f);

    // Populate input globals read by TBC SpellMissileMotion.dbc scripts.
    setGlobalNumber(L_, "progress", clampedProgress);
    setGlobalNumber(L_, "time", std::max(0.0f, input.time));
    setGlobalNumber(L_, "startDistance", safeStartDist);
    setGlobalNumber(L_, "totalDistance", safeDist);
    setGlobalNumber(L_, "distanceToFirePos", distToFire);
    setGlobalNumber(L_, "distanceToImpactPos", distToImpact);
    setGlobalNumber(L_, "distanceFromImpactPos", distToImpact);
    setGlobalNumber(L_, "missileIndex", static_cast<float>(input.missileIndex));
    setGlobalNumber(L_, "missileCount", static_cast<float>(std::max(1, input.missileCount)));
    setGlobalNumber(L_, "rand1", input.rand1);
    setGlobalNumber(L_, "rand2", input.rand2);
    setGlobalNumber(L_, "rand3", input.rand3);

    lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second.luaFuncRef);
    if (lua_pcall(L_, 0, 0, 0) != 0) {
        const char* err = lua_tostring(L_, -1);
        LOG_WARNING("SpellMissileMotion: runtime error id=", motionId, " err=", err ? err : "unknown");
        lua_pop(L_, 1);
        return false;
    }

    out.transFront = getGlobalNumber(L_, "transFront", 0.0f);
    out.transRight = getGlobalNumber(L_, "transRight", 0.0f);
    out.transUp = getGlobalNumber(L_, "transUp", 0.0f);
    out.transAngle = getGlobalNumber(L_, "transAngle", 0.0f);
    out.transMag = getGlobalNumber(L_, "transMag", 0.0f);
    out.modelPitch = getGlobalNumber(L_, "modelPitch", 0.0f);
    out.modelYaw = getGlobalNumber(L_, "modelYaw", 0.0f);
    out.modelRoll = getGlobalNumber(L_, "modelRoll", 0.0f);
    out.scale = std::max(0.01f, getGlobalNumber(L_, "scale", 1.0f));
    out.speedScalar = std::clamp(getGlobalNumber(L_, "speedScalar", 1.0f), 0.05f, 10.0f);
    return true;
}

}  // namespace wowee::rendering
