#pragma once

#include "rendering/camera.hpp"
#include "core/logger.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace wowee::rendering {

// Opt-in local comparison protocol. Writers replace the request atomically.
// No file polling or camera changes without WOWEE_VIEW_SYNC.
inline void updateViewSync(Camera& camera) {
    static const char* path = std::getenv("WOWEE_VIEW_SYNC");
    if (!path) return;
    static std::filesystem::file_time_type lastWrite{};
    static bool held = false;
    static glm::vec3 position{}, forward{}, up{};
    static float fov = 0, savedFov = 0;
    static std::string token;
    bool acknowledge = false;
    try {
        if (std::filesystem::exists(path)) {
            const auto modified = std::filesystem::last_write_time(path);
            if (modified != lastWrite) {
                lastWrite = modified;
                std::ifstream input(path);
                input.exceptions(std::ios::badbit);
                int version;
                std::string mode, extra;
                if (!(input >> version >> token >> mode) || version != 1 || token.empty() ||
                    token.size() > 64 || token.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos)
                    throw std::runtime_error("Invalid view-sync header");
                if (mode == "hold") {
                    if (!(input >> position.x >> position.y >> position.z >> forward.x >> forward.y >> forward.z
                                >> up.x >> up.y >> up.z >> fov) || (input >> extra))
                        throw std::runtime_error("Invalid view-sync pose payload");
                    if (!held) savedFov = camera.getFovDegrees();
                    camera.setExternalPose(position, forward, up, fov);
                    held = true;
                } else if (mode == "release") {
                    if (input >> extra) throw std::runtime_error("Unexpected release payload");
                    if (held) camera.setFov(savedFov);
                    camera.clearExternalPose();
                    held = false;
                } else throw std::runtime_error("Unknown view-sync operation");
                acknowledge = true;
            }
        }
        if (held) camera.setExternalPose(position, forward, up, fov);
        if (acknowledge) {
            const std::string temporary = std::string(path) + ".ack.tmp";
            std::ofstream output(temporary);
            output.exceptions(std::ios::badbit | std::ios::failbit);
            const auto p = camera.getPosition();
            output << std::setprecision(9) << token << " OK " << (held ? "hold" : "release") << ' '
                   << p.x << ' ' << p.y << ' ' << p.z << ' ' << camera.getFovDegrees();
            const auto appliedForward = camera.getForward(), appliedUp = camera.getUp();
            output << ' ' << appliedForward.x << ' ' << appliedForward.y << ' ' << appliedForward.z
                   << ' ' << appliedUp.x << ' ' << appliedUp.y << ' ' << appliedUp.z << '\n';
            output.close();
            std::filesystem::rename(temporary, std::string(path) + ".ack");
        }
    } catch (const std::exception& error) {
        LOG_ERROR("View sync failed: ", error.what());
        if (held) camera.setFov(savedFov);
        held = false;
        camera.clearExternalPose();
        // A failed request cannot accidentally acknowledge a previous pose.
        std::ofstream output(std::string(path) + ".ack");
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << token << " ERROR " << error.what() << '\n';
    }
}
}
