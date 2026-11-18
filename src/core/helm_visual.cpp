#include "core/helm_visual.hpp"

#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"

#include <cassert>

#include <algorithm>

namespace wowee {
namespace core {

namespace {

constexpr const char* kHeadDir = "Item\\ObjectComponents\\Head\\";

std::string stripExtension(std::string name) {
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    return name;
}

} // namespace

HelmVisual resolveHelmVisual(pipeline::AssetManager& assets,
                             uint32_t itemDisplayInfoId,
                             uint8_t raceId,
                             uint8_t genderId) {
    HelmVisual out;
    if (itemDisplayInfoId == 0) return out;

    auto dbc = assets.loadDBC("ItemDisplayInfo.dbc");
    if (!dbc) return out;
    const int32_t row = dbc->findRecordById(itemDisplayInfoId);
    if (row < 0) return out;

    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const uint32_t modelField = layout ? (*layout)["LeftModel"] : 1u;
    const uint32_t textureField = layout ? (*layout)["LeftModelTexture"] : 3u;

    const std::string modelName =
        stripExtension(dbc->getString(static_cast<uint32_t>(row), modelField));
    if (modelName.empty()) return out;

    const std::string suffix = raceGenderSuffix(raceId, genderId);
    if (!suffix.empty()) out.racialModelPath = kHeadDir + modelName + suffix + ".m2";
    out.baseModelPath = kHeadDir + modelName + ".m2";

    const std::string textureName = dbc->getString(static_cast<uint32_t>(row), textureField);
    if (!textureName.empty()) {
        if (!suffix.empty()) {
            const std::string racial = kHeadDir + textureName + suffix + ".blp";
            if (assets.fileExists(racial)) out.texturePath = racial;
        }
        if (out.texturePath.empty()) out.texturePath = kHeadDir + textureName + ".blp";
    }
    return out;
}


std::vector<uint16_t> helmHiddenGeosetGroups(pipeline::AssetManager& assets,
                                             uint32_t itemDisplayInfoId,
                                             uint8_t raceId,
                                             uint8_t genderId) {
    std::vector<uint16_t> groups;
    if (itemDisplayInfoId == 0) return groups;

    const auto* active = pipeline::getActiveDBCLayout();
    assert(active && "helmHiddenGeosetGroups: no active DBC layout");
    const auto* idiLayout = active->getLayout("ItemDisplayInfo");
    const auto* visLayout = active->getLayout("HelmetGeosetVisData");
    if (!idiLayout || !visLayout) {
        static bool reported = false;
        if (!reported) LOG_ERROR("helmHiddenGeosetGroups: dbc_layouts.json lacks ",
                  idiLayout ? "HelmetGeosetVisData" : "ItemDisplayInfo",
                  "; helms hide nothing");
        reported = true;
        return groups;
    }
    const uint32_t visColumn =
        (*idiLayout)[genderId == 0 ? "HelmetGeosetVisMale" : "HelmetGeosetVisFemale"];
    if (visColumn == 0xFFFFFFFFu) return groups;  // noteMissingLayoutField reported it

    auto displayInfo = assets.loadDBC("ItemDisplayInfo.dbc");
    auto visData = assets.loadDBC("HelmetGeosetVisData.dbc");
    if (!displayInfo || !visData) {
        LOG_ERROR("helmHiddenGeosetGroups: ItemDisplayInfo.dbc or HelmetGeosetVisData.dbc missing");
        return groups;
    }
    const int32_t row = displayInfo->findRecordById(itemDisplayInfoId);
    if (row < 0) {
        LOG_WARNING("helmHiddenGeosetGroups: ItemDisplayInfo ", itemDisplayInfoId, " not found");
        return groups;
    }
    const uint32_t visId = displayInfo->getUInt32(static_cast<uint32_t>(row), visColumn);
    if (visId == 0) return groups;

    const int32_t visRow = visData->findRecordById(visId);
    if (visRow < 0) {
        LOG_ERROR("helmHiddenGeosetGroups: ItemDisplayInfo ", itemDisplayInfoId,
                  " names HelmetGeosetVisData ", visId, ", which the file lacks");
        return groups;
    }
    for (const auto& column : kHelmHideColumns) {
        const uint32_t field = (*visLayout)[column.layoutName];
        if (field == 0xFFFFFFFFu) continue;  // noteMissingLayoutField reported it
        if (helmMaskCoversRace(visData->getUInt32(static_cast<uint32_t>(visRow), field), raceId)) {
            groups.push_back(column.geosetGroup);
        }
    }
    for (const char* name : kHelmHideUnmappedColumns) {
        const uint32_t field = visLayout->tryField(name);
        if (field == 0xFFFFFFFFu) continue;
        if (helmMaskCoversRace(visData->getUInt32(static_cast<uint32_t>(visRow), field), raceId)) {
            LOG_WARNING("helmHiddenGeosetGroups: HelmetGeosetVisData ", visId, " ", name,
                        " covers race ", static_cast<int>(raceId),
                        "; no character geoset group is known for it, not applied");
        }
    }
    return groups;
}

} // namespace core
} // namespace wowee
