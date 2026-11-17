// pick_mask: the screen pixels at which WoWee's unit picking rule would pick a
// creature, for a given CreatureDisplayInfo row and camera - headless, from the
// extracted game data, through the same code the client picks with
// (rendering/unit_pick.hpp). For comparing against cursor sweeps of the
// original client (#79/#90).
//
//   build/bin/pick_mask spec.json mask.pgm [sphere.pgm]
//
// spec.json (camera in the native sample shape of tools/playtester, canonical
// world coordinates; fov_radians converted as paired_sync does):
//   {"data_dir": "/workspace/azerothcore/client/wowee-data/expansions/tbc",
//    "display_id": 2338, "width": 1280, "height": 720,
//    "unit": {"position": [x, y, z], "orientation": 0.0, "object_scale": 1.0,
//             "animation": 0, "time_ms": 0},
//    "camera": {"position": [x, y, z], "forward": [..], "up": [..], "fov_radians": 1.5708},
//    "geosets": []}
// mask.pgm: 255 where the rule picks the unit, 0 elsewhere. sphere.pgm: the
// broad-phase sphere alone. A JSON summary goes to stdout.
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/unit_pick.hpp"
#include "rendering/visual_mesh_pick.hpp"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace wowee;

namespace {

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}

pipeline::DBCFile loadDbc(const fs::path& dataDir, const char* name) {
    pipeline::DBCFile dbc;
    if (!dbc.load(readFile(dataDir / "dbfilesclient" / name)))
        throw std::runtime_error(std::string("bad DBC ") + name);
    return dbc;
}

// Extracted trees store paths lower-cased with '/' and the shipped .m2.
fs::path dataPath(const fs::path& dataDir, std::string path) {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
    });
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos && (path.substr(dot) == ".mdx" || path.substr(dot) == ".mdl"))
        path = path.substr(0, dot) + ".m2";
    return dataDir / path;
}

glm::vec3 vec3(const nlohmann::json& j) { return {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()}; }

// WoWee's render space swaps canonical x and y (core::coords::canonicalToRender).
glm::vec3 toRender(const glm::vec3& v) { return {v.y, v.x, v.z}; }

void writePgm(const fs::path& path, int width, int height, const std::vector<uint8_t>& pixels) {
    std::ofstream out(path, std::ios::binary);
    out << "P5\n" << width << ' ' << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: pick_mask spec.json mask.pgm [sphere.pgm]\n";
        return 2;
    }
    const auto spec = nlohmann::json::parse(readFile(argv[1]));
    const fs::path dataDir = spec.at("data_dir").get<std::string>();
    const uint32_t displayId = spec.at("display_id");
    const int width = spec.at("width"), height = spec.at("height");

    // CreatureDisplayInfo: 1 ModelID, 4 CreatureModelScale. CreatureModelData:
    // 2 ModelPath, 4 ModelScale (the TBC dbc_layouts.json columns).
    const auto displays = loadDbc(dataDir, "creaturedisplayinfo.dbc");
    const auto models = loadDbc(dataDir, "creaturemodeldata.dbc");
    const int32_t displayRow = displays.findRecordById(displayId);
    if (displayRow < 0) throw std::runtime_error("display not in CreatureDisplayInfo.dbc");
    const uint32_t modelId = displays.getUInt32(static_cast<uint32_t>(displayRow), 1);
    const int32_t modelRow = models.findRecordById(modelId);
    if (modelRow < 0) throw std::runtime_error("model not in CreatureModelData.dbc");
    const std::string modelPath = models.getString(static_cast<uint32_t>(modelRow), 2);
    const auto& unit = spec.at("unit");
    const float scale = displays.getFloat(static_cast<uint32_t>(displayRow), 4) *
                        models.getFloat(static_cast<uint32_t>(modelRow), 4) *
                        unit.value("object_scale", 1.0f);

    const auto model = pipeline::M2Loader::load(readFile(dataPath(dataDir, modelPath)));
    if (!model.isValid()) throw std::runtime_error("bad M2 " + modelPath);
    const uint32_t animation = unit.value("animation", 0u);
    int sequence = -1;
    for (size_t i = 0; i < model.sequences.size(); ++i) {
        if (model.sequences[i].id != animation) continue;
        if (sequence < 0 || model.sequences[i].variationIndex == 0) sequence = static_cast<int>(i);
        if (model.sequences[i].variationIndex == 0) break;
    }
    if (sequence < 0) throw std::runtime_error("model has no sequence for the animation");
    const float timeMs = unit.value("time_ms", 0.0f);
    const auto bones = rendering::poseModelBones(model, sequence, timeMs, timeMs);

    std::unordered_set<uint16_t> geosets;
    for (const auto& g : spec.value("geosets", nlohmann::json::array())) geosets.insert(g.get<uint16_t>());
    const rendering::UnitPickState state{.bones = bones, .activeGeosets = &geosets, .opacity = 1.0f,
                                         .sequenceIndex = sequence, .sampleTimeMs = timeMs,
                                         .globalTimeMs = timeMs};

    // Instance matrix as CharacterRenderer::getModelMatrix builds it: render
    // position, render yaw = canonical facing + 90 degrees, uniform scale.
    glm::mat4 world = glm::translate(glm::mat4(1.0f), toRender(vec3(unit.at("position"))));
    world = glm::rotate(world, unit.value("orientation", 0.0f) + glm::radians(90.0f), glm::vec3(0, 0, 1));
    world = glm::scale(world, glm::vec3(scale));
    const glm::mat4 inverse = glm::inverse(world);

    // Camera rays in canonical space (right = forward x up there), then into
    // render space. Vertical field of view as tools/playtester/paired_sync.py.
    const auto& camera = spec.at("camera");
    const glm::vec3 eye = vec3(camera.at("position"));
    const glm::vec3 forward = glm::normalize(vec3(camera.at("forward")));
    const glm::vec3 right = glm::normalize(glm::cross(forward, vec3(camera.at("up"))));
    const glm::vec3 up = glm::cross(right, forward);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float vertical = camera.at("fov_radians").get<float>() / std::sqrt(1.0f + aspect * aspect);
    const float tanHalf = std::tan(vertical * 0.5f);
    const glm::vec3 localOrigin(inverse * glm::vec4(toRender(eye), 1.0f));
    const rendering::ModelPickSphere sphere = rendering::modelPickSphere(model, sequence);

    std::vector<uint8_t> mask(static_cast<size_t>(width) * height, 0), sphereMask(mask.size(), 0);
    size_t picked = 0, inSphere = 0;
    int minX = width, minY = height, maxX = -1, maxY = -1;
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            const float x = (2.0f * (px + 0.5f) / width - 1.0f) * tanHalf * aspect;
            const float y = (1.0f - 2.0f * (py + 0.5f) / height) * tanHalf;
            const glm::vec3 direction = forward + right * x + up * y;
            const glm::vec3 localDirection(inverse * glm::vec4(toRender(direction), 0.0f));
            const size_t index = static_cast<size_t>(py) * width + px;
            const glm::vec3 oc = localOrigin - sphere.center;
            const float a = glm::dot(localDirection, localDirection);
            const float b = glm::dot(oc, localDirection);
            const float disc = b * b - a * (glm::dot(oc, oc) - sphere.radius * sphere.radius);
            if (disc >= 0.0f && (-b + std::sqrt(disc)) / a >= 0.0f) { sphereMask[index] = 255; ++inSphere; }
            const float hit = rendering::pickUnitModel(model, state, localOrigin, localDirection,
                                                       std::numeric_limits<float>::infinity());
            if (!std::isfinite(hit)) continue;
            mask[index] = 255;
            ++picked;
            minX = std::min(minX, px); maxX = std::max(maxX, px);
            minY = std::min(minY, py); maxY = std::max(maxY, py);
        }
    }
    writePgm(argv[2], width, height, mask);
    if (argc > 3) writePgm(argv[3], width, height, sphereMask);
    nlohmann::json summary = {
        {"display_id", displayId}, {"model", modelPath}, {"scale", scale},
        {"sequence", sequence}, {"animation", animation}, {"time_ms", timeMs},
        {"picked_pixels", picked}, {"sphere_pixels", inSphere},
        {"bbox", picked ? nlohmann::json{minX, minY, maxX, maxY} : nlohmann::json(nullptr)},
        {"sphere_center_model", {sphere.center.x, sphere.center.y, sphere.center.z}},
        {"sphere_radius_model", sphere.radius}};
    std::cout << summary.dump() << '\n';
    return 0;
}
