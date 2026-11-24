#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace wowee {
namespace rendering {

struct Ray {
    glm::vec3 origin;
    glm::vec3 direction;
};

class Camera {
public:
    Camera();

    void setPosition(const glm::vec3& pos) {
        // Reject NaN/inf - would produce a NaN view matrix and freeze the
        // GPU in some drivers, or produce garbage frustum culling.
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return;
        position = pos; updateViewMatrix();
    }
    void setRotation(float yaw, float pitch) {
        if (!std::isfinite(yaw) || !std::isfinite(pitch)) return;
        externalPose = false;
        this->yaw = yaw; this->pitch = pitch; updateViewMatrix();
    }
    void setAspectRatio(float aspect) {
        // glm::perspective with aspect <= 0 produces NaN in the projection.
        if (!std::isfinite(aspect) || aspect <= 0.0f) return;
        aspectRatio = aspect; updateProjectionMatrix();
    }
    void setFov(float fov) {
        // glm::perspective(0) is degenerate; >180 wraps the trig.
        if (!std::isfinite(fov) || fov <= 0.0f || fov >= 180.0f) return;
        this->fov = fov; updateProjectionMatrix();
    }

    // Explicit comparison/debug camera. Angles and FOV are degrees.
    void setExternalPose(const glm::vec3& pos, const glm::vec3& forward,
                         const glm::vec3& up, float verticalFov);
    void clearExternalPose() { externalPose = false; updateViewMatrix(); }
    [[nodiscard]] bool hasExternalPose() const { return externalPose; }

    [[nodiscard]] const glm::vec3& getPosition() const { return position; }
    [[nodiscard]] const glm::mat4& getViewMatrix() const { return viewMatrix; }
    [[nodiscard]] const glm::mat4& getProjectionMatrix() const { return projectionMatrix; }
    [[nodiscard]] const glm::mat4& getUnjitteredProjectionMatrix() const { return unjitteredProjectionMatrix; }
    [[nodiscard]] glm::mat4 getViewProjectionMatrix() const { return projectionMatrix * viewMatrix; }
    [[nodiscard]] glm::mat4 getUnjitteredViewProjectionMatrix() const { return unjitteredProjectionMatrix * viewMatrix; }
    [[nodiscard]] float getAspectRatio() const { return aspectRatio; }
    [[nodiscard]] float getFovDegrees() const { return fov; }
    [[nodiscard]] float getNearPlane() const { return nearPlane; }
    [[nodiscard]] float getFarPlane() const { return farPlane; }

    // Sub-pixel jitter for temporal upscaling (FSR 2)
    void setJitter(float jx, float jy);
    void clearJitter();
    [[nodiscard]] glm::vec2 getJitter() const { return jitterOffset; }

    [[nodiscard]] glm::vec3 getForward() const;
    [[nodiscard]] glm::vec3 getRight() const;
    [[nodiscard]] glm::vec3 getUp() const;

    [[nodiscard]] Ray screenToWorldRay(float screenX, float screenY, float screenW, float screenH) const;

private:
    void updateViewMatrix();
    void updateProjectionMatrix();

    bool externalPose = false;
    glm::vec3 externalForward{1, 0, 0};
    glm::vec3 externalUp{0, 0, 1};
    glm::vec3 position = glm::vec3(0.0f);
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fov = 45.0f;
    float aspectRatio = 16.0f / 9.0f;
    float nearPlane = 0.22f;     // Matches Native TBC 2.4.3 (~0.222 yd = 8/36 yd); avoids close-up mesh clipping with D32_SFLOAT depth
    float farPlane = 30000.0f;   // Improves depth precision vs extremely large far clip

    glm::mat4 viewMatrix = glm::mat4(1.0f);
    glm::mat4 projectionMatrix = glm::mat4(1.0f);
    glm::mat4 unjitteredProjectionMatrix = glm::mat4(1.0f);
    glm::vec2 jitterOffset = glm::vec2(0.0f);  // NDC jitter (applied to projection)
};

/// Continuous scripted camera orbit/zoom state driven by TBC Lua bindings:
/// MoveView{Right,Left,Up,Down,In,Out}{Start,Stop} and FlipCameraYaw.
struct ScriptedViewMotion {
    float rightSpeed = 0.0f;
    float leftSpeed = 0.0f;
    float upSpeed = 0.0f;
    float downSpeed = 0.0f;
    float inSpeed = 0.0f;
    float outSpeed = 0.0f;

    [[nodiscard]] bool active() const {
        return rightSpeed != 0.0f || leftSpeed != 0.0f ||
               upSpeed != 0.0f || downSpeed != 0.0f ||
               inSpeed != 0.0f || outSpeed != 0.0f;
    }

    bool apply(std::string_view op, float value, float& ioYawDegrees) {
        if (!std::isfinite(value)) return false;
        if (op == "right") { rightSpeed = value; return false; }
        if (op == "left")  { leftSpeed = value; return false; }
        if (op == "up")    { upSpeed = value; return false; }
        if (op == "down")  { downSpeed = value; return false; }
        if (op == "in")    { inSpeed = value; return false; }
        if (op == "out")   { outSpeed = value; return false; }
        if (op == "flip_yaw") {
            ioYawDegrees += value;
            return true;
        }
        return false;
    }

    void advance(float deltaTime,
                 float yawMoveSpeed, float pitchMoveSpeed, float distanceMoveSpeed,
                 float minPitch, float maxPitch, float maxDistance,
                 float& ioYaw, float& ioPitch, float& ioDistance,
                 bool& outAnglesChanged, bool& outDistanceChanged) const {
        outAnglesChanged = false;
        outDistanceChanged = false;
        if (deltaTime <= 0.0f || !active()) return;
        const float yawRate = (rightSpeed - leftSpeed) * yawMoveSpeed;
        const float pitchRate = (upSpeed - downSpeed) * pitchMoveSpeed;
        const float zoomRate = (outSpeed - inSpeed) * distanceMoveSpeed;
        if (yawRate != 0.0f || pitchRate != 0.0f) {
            ioYaw -= yawRate * deltaTime;
            ioPitch = std::clamp(ioPitch + pitchRate * deltaTime, minPitch, maxPitch);
            outAnglesChanged = true;
        }
        if (zoomRate != 0.0f) {
            ioDistance = std::clamp(ioDistance + zoomRate * deltaTime, 0.0f, maxDistance);
            outDistanceChanged = true;
        }
    }
};

} // namespace rendering
} // namespace wowee
