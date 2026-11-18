// ChatBubbleManager - 3D-projected chat bubbles above entities.
// Moved from ChatPanel::renderBubbles / setupCallbacks (Phase 1.4).
#include "ui/chat/chat_bubble_manager.hpp"
#include "game/game_handler.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "core/coordinates.hpp"
#include "core/window.hpp"
#include "core/application.hpp"
#include "core/entity_spawner.hpp"
#include "rendering/character_renderer.hpp"
#include <imgui.h>
#include "ui/chat/chat_utils.hpp"
#include "ui/sight_cache.hpp"
#include "ui/ui_texture_load.hpp"

#include <glm/glm.hpp>

namespace wowee { namespace ui {

void ChatBubbleManager::addBubble(uint64_t senderGuid, const std::string& message, bool isYell) {
    float duration = 8.0f + static_cast<float>(message.size()) * 0.06f;
    if (isYell) duration += 2.0f;
    if (duration > 15.0f) duration = 15.0f;

    // Replace existing bubble for same sender
    for (auto& b : bubbles_) {
        if (b.senderGuid == senderGuid) {
            b.message = message;
            b.timeRemaining = duration;
            b.totalDuration = duration;
            b.isYell = isYell;
            return;
        }
    }
    // Evict oldest if too many
    if (bubbles_.size() >= kMaxBubbles) {
        bubbles_.erase(bubbles_.begin());
    }
    bubbles_.push_back({.senderGuid = senderGuid, .message = message, .timeRemaining = duration, .totalDuration = duration, .isYell = isYell});
}

void ChatBubbleManager::render(game::GameHandler& gameHandler, const UIServices& services) {
    // Off means off: the queue is dropped as well, so turning the setting off
    // clears what is on screen rather than leaving it to fade for five seconds.
    if (!show_) { bubbles_.clear(); return; }
    if (bubbles_.empty()) return;

    auto* renderer = services.renderer;
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    if (!camera) return;

    auto* window = services.window;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    // Get delta time from ImGui
    float dt = ImGui::GetIO().DeltaTime;

    glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    // Update and render bubbles
    for (int i = static_cast<int>(bubbles_.size()) - 1; i >= 0; --i) {
        auto& bubble = bubbles_[i];
        bubble.timeRemaining -= dt;
        if (bubble.timeRemaining <= 0.0f) {
            bubbles_.erase(bubbles_.begin() + i);
            continue;
        }

        // Get entity position
        auto entity = gameHandler.getEntityManager().getEntity(bubble.senderGuid);
        if (!entity) continue;

        // The frame's bottom edge sits on the speaker's name attachment and
        // its tail hangs below. Build 8606, one unit saying and yelling under
        // several displays (pair 7, _experiments/20261118T074350Z.plates_fct
        // bubble.py): box bottom at 2.25 yd for a human (attachment 2.21), 8.69
        // for the Mountain Giant (8.59), tail tip 0.2-0.3 yd lower; a yell is
        // placed the same way.
        glm::vec3 renderPos;
        {
            auto& app = core::Application::getInstance();
            auto* spawner = app.getEntitySpawner();
            auto* characters = renderer->getCharacterRenderer();
            const uint32_t instanceId = bubble.senderGuid == gameHandler.getPlayerGuid()
                ? renderer->getCharacterInstanceId()
                : (spawner ? spawner->characterInstanceIdForGuid(bubble.senderGuid) : 0);
            const bool mounted = entity->isUnit() &&
                static_cast<const game::Unit&>(*entity).getMountDisplayId() != 0;
            if (!characters || instanceId == 0 ||
                !characters->getInstanceNamePosition(instanceId, mounted, renderPos))
                continue;  // The model has not spawned yet.
        }

        // Not through a wall. A bubble carries a name as well as a line, so
        // one drawn over the building someone is standing behind says they are
        // in front of it - the same fault the nameplates had, answered by the
        // same query.
        static SightCache sight;
        if (sight.blocked(renderer->getWMORenderer(), bubble.senderGuid,
                          camera->getPosition(), renderPos)) {
            continue;
        }

        // Project to screen
        glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
        if (clipPos.w <= 0.0f) continue;  // Behind camera

        glm::vec2 ndc(clipPos.x / clipPos.w, clipPos.y / clipPos.w);
        float screenX = (ndc.x * 0.5f + 0.5f) * screenW;
        // Camera bakes the Vulkan Y-flip into the projection matrix:
        // NDC y=-1 is top, y=1 is bottom - same convention as nameplate/minimap projection.
        float screenY = (ndc.y * 0.5f + 0.5f) * screenH;

        // Skip if off-screen
        if (screenX < -200.0f || screenX > screenW + 200.0f ||
            screenY < -100.0f || screenY > screenH + 100.0f) continue;

        // Fade alpha over last 2 seconds
        float alpha = 1.0f;
        if (bubble.timeRemaining < 2.0f) {
            alpha = bubble.timeRemaining / 2.0f;
        }

        // Into the background draw list, which is where the interface is
        // drawn - and drawn into before it, so the interface covers this.
        //
        // These were ImGui windows, and an ImGui window is above every draw
        // list the interface uses, whatever the interface is doing: a bubble
        // sat on top of the auction house, the bags, the map, anything open.
        // Nothing about the position was wrong; a window was simply the one
        // thing that cannot go underneath.
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImFont* font = ImGui::GetFont();
        const float scale = screenH / 768.0f;
        const float fontSize = 15.0f * scale;
        const float kWrapWidth = 300.0f * scale;
        const ImVec2 padding(16.0f * scale, 12.0f * scale);

        const ImVec2 textSize =
            font->CalcTextSizeA(fontSize, FLT_MAX, kWrapWidth, bubble.message.c_str());
        const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
        // The point is where the speaker's head is, and the bubble sits above
        // it, centred - which is what the window's pivot of (0.5, 1) did.
        const ImVec2 topLeft(screenX - size.x * 0.5f, screenY - size.y);
        // The tail is drawn below this point (see the tail quad).

        static const auto background = uploadUiTextureFromBlp(services.assetManager,
            "Interface/Tooltips/ChatBubble-Background.blp", window);
        static const auto border = uploadUiTextureFromBlp(services.assetManager,
            "Interface/Tooltips/ChatBubble-Backdrop.blp", window);
        static const auto tail = uploadUiTextureFromBlp(services.assetManager,
            "Interface/Tooltips/ChatBubble-Tail.blp", window);
        if (!background || !border || !tail)
            throw std::runtime_error("Missing authored chat bubble textures");
        const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(255 * alpha));
        const float x0 = topLeft.x, y0 = topLeft.y;
        const float x1 = x0 + size.x, y1 = y0 + size.y;
        const float edge = 16.0f * scale;
        dl->AddImage((ImTextureID)(uintptr_t)background, ImVec2(x0 + 3 * scale, y0 + 3 * scale),
            ImVec2(x1 - 3 * scale, y1 - 3 * scale), ImVec2(0, 0), ImVec2(1, 1), tint);
        const auto piece = [&](int index, float left, float top, float right, float bottom, bool rotated = false) {
            const float u0 = index / 8.0f, u1 = (index + 1) / 8.0f;
            dl->AddImageQuad((ImTextureID)(uintptr_t)border,
                ImVec2(left, top), ImVec2(right, top), ImVec2(right, bottom), ImVec2(left, bottom),
                ImVec2(u0, 0), rotated ? ImVec2(u0, 1) : ImVec2(u1, 0),
                ImVec2(u1, 1), rotated ? ImVec2(u1, 0) : ImVec2(u0, 1), tint);
        };
        piece(4, x0, y0, x0 + edge, y0 + edge);
        piece(5, x1 - edge, y0, x1, y0 + edge);
        piece(6, x0, y1 - edge, x0 + edge, y1);
        piece(7, x1 - edge, y1 - edge, x1, y1);
        piece(0, x0, y0 + edge, x0 + edge, y1 - edge);
        piece(1, x1 - edge, y0 + edge, x1, y1 - edge);
        piece(2, x0 + edge, y0, x1 - edge, y0 + edge, true);
        piece(3, x0 + edge, y1 - edge, x1 - edge, y1, true);
        dl->AddImage((ImTextureID)(uintptr_t)tail, ImVec2(screenX - edge, y1 - edge * .5f),
            ImVec2(screenX + edge, y1 + edge * 1.5f), ImVec2(0, 0), ImVec2(1, 1), tint);

        const ImVec4 textColor = bubble.isYell
            ? ImVec4(1.0f, 0.2f, 0.2f, alpha)
            : ImVec4(1.0f, 1.0f, 1.0f, alpha);
        dl->AddText(font, fontSize, ImVec2(topLeft.x + padding.x, topLeft.y + padding.y),
                    ImGui::ColorConvertFloat4ToU32(textColor), bubble.message.c_str(), nullptr,
                    kWrapWidth);
    }
}

void ChatBubbleManager::setupCallback(game::GameHandler& gameHandler) {
    if (!callbackSet_) {
        // Resolve $-tokens here rather than at draw time: a bubble is built once
        // and drawn every frame. The chat panel already does this for the same
        // text, which is why an NPC line read correctly in the log and showed
        // its raw $r/$c/$g markers in the bubble over their head.
        gameHandler.setChatBubbleCallback(
            [this, &gameHandler](uint64_t guid, const std::string& msg, bool isYell) {
                addBubble(guid, chat_utils::replaceGenderPlaceholders(msg, gameHandler), isYell);
            });
        callbackSet_ = true;
    }
}

} // namespace ui
} // namespace wowee
