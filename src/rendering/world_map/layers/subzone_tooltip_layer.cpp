// subzone_tooltip_layer.cpp - Overlay area hover labels in zone view.
// Extracted from WorldMap::renderImGuiOverlay (Phase 8 of refactoring plan).
#include "rendering/world_map/layers/subzone_tooltip_layer.hpp"
#include <imgui.h>
#include <limits>

namespace wowee {
namespace rendering {
namespace world_map {

int SubzoneTooltipLayer::overlayAt(const Zone& zone, float u, float v) {
    if (u < 0 || u > 1 || v < 0 || v > 1) return -1;
    // The visible map crops the padded 1024x768 texture to 1002x668.
    const float x = u * 1002.0f, y = v * 668.0f;
    int selected = -1;
    float bestArea = std::numeric_limits<float>::max();
    for (size_t i = 0; i < zone.overlays.size(); ++i) {
        const auto& overlay = zone.overlays[i];
        if (overlay.hitRectRight <= overlay.hitRectLeft || overlay.hitRectBottom <= overlay.hitRectTop)
            continue;
        if (x < overlay.hitRectLeft || x > overlay.hitRectRight ||
            y < overlay.hitRectTop || y > overlay.hitRectBottom) continue;
        const float area = static_cast<float>(overlay.hitRectRight - overlay.hitRectLeft) *
                           (overlay.hitRectBottom - overlay.hitRectTop);
        if (area < bestArea) { bestArea = area; selected = static_cast<int>(i); }
    }
    return selected;
}

void SubzoneTooltipLayer::render(const LayerContext& ctx) {
    if (ctx.frameXmlHosted || ctx.viewLevel != ViewLevel::ZONE) return;
    if (ctx.currentZoneIdx < 0 || !ctx.zones) return;

    ImVec2 mp = ImGui::GetIO().MousePos;
    if (mp.x < ctx.imgMin.x || mp.x > ctx.imgMin.x + ctx.displayW ||
        mp.y < ctx.imgMin.y || mp.y > ctx.imgMin.y + ctx.displayH)
        return;

    float mu = (mp.x - ctx.imgMin.x) / ctx.displayW;
    float mv = (mp.y - ctx.imgMin.y) / ctx.displayH;

    const auto& zone = (*ctx.zones)[ctx.currentZoneIdx];
    std::string hoveredName;
    bool hoveredExplored = false;
    const int oi = overlayAt(zone, mu, mv);
    if (oi >= 0) {
        for (const auto areaId : zone.overlays[oi].areaIDs) {
            if (!ctx.areaNameByAreaId) break;
            const auto found = ctx.areaNameByAreaId->find(areaId);
            if (found != ctx.areaNameByAreaId->end()) { hoveredName = found->second; break; }
        }
        hoveredExplored = ctx.exploredOverlays && ctx.exploredOverlays->count(oi) > 0;
    }

    if (!hoveredName.empty()) {
        std::string label = hoveredName;
        if (!hoveredExplored)
            label += "  (Unexplored)";

        ImVec2 labelSz = ImGui::CalcTextSize(label.c_str());
        float lx = ctx.imgMin.x + (ctx.displayW - labelSz.x) * 0.5f;
        float ly = ctx.imgMin.y + 6.0f;
        ImU32 labelCol = hoveredExplored
            ? IM_COL32(255, 230, 150, 240)
            : IM_COL32(160, 160, 160, 200);
        ctx.drawList->AddText(ImVec2(lx + 1.0f, ly + 1.0f),
                              IM_COL32(0, 0, 0, 200), label.c_str());
        ctx.drawList->AddText(ImVec2(lx, ly), labelCol, label.c_str());
    }
}

} // namespace world_map
} // namespace rendering
} // namespace wowee
