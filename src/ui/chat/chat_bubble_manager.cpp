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
#include "ui/interface_fonts.hpp"
#include "ui/sight_cache.hpp"
#include "ui/ui_texture_load.hpp"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace wowee { namespace ui {

namespace {
struct RenderedBubbleLine {
    std::string text;
    float widthPx = 0.0f;
};

std::string trimTrailingWhitespace(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}
} // namespace

void ChatBubbleManager::addBubble(uint64_t senderGuid, const std::string& message,
                                  game::ChatType chatType) {
    const bool isYell = (chatType == game::ChatType::YELL ||
                         chatType == game::ChatType::MONSTER_YELL);
    float duration = 8.0f + static_cast<float>(message.size()) * 0.06f;
    if (isYell) duration += 2.0f;
    if (duration > 15.0f) duration = 15.0f;

    // Replace existing bubble for same sender
    for (auto& b : bubbles_) {
        if (b.senderGuid == senderGuid) {
            b.message = message;
            b.timeRemaining = duration;
            b.totalDuration = duration;
            b.chatType = chatType;
            return;
        }
    }
    // Evict oldest if too many
    if (bubbles_.size() >= kMaxBubbles) {
        bubbles_.erase(bubbles_.begin());
    }
    bubbles_.push_back({
        .senderGuid = senderGuid,
        .message = message,
        .timeRemaining = duration,
        .totalDuration = duration,
        .chatType = chatType,
    });
}

void ChatBubbleManager::render(game::GameHandler& gameHandler, const UIServices& services) {
    // Off means off: drop disabled bubble categories immediately rather than
    // leaving them on screen to fade out.
    if (!show_ && !showParty_) { bubbles_.clear(); return; }
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
        const bool isParty = isPartyChatBubbleType(bubble.chatType);
        if ((isParty && !showParty_) || (!isParty && !show_)) {
            bubbles_.erase(bubbles_.begin() + i);
            continue;
        }
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
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImFont* font = interfaceFaceOrDefault("frizqt__");
        const ChatBubbleMetrics m = chatBubbleMetrics(screenH);
        const float fontSize = static_cast<float>(m.pixelHeight);

        std::vector<RenderedBubbleLine> wrappedLines;
        float maxLineW = 0.0f;
        if (const auto nativeRows = interfaceWrap(bubble.message, "frizqt__",
                                                  kBubbleFontHeightUi, m.wrapWidthUi,
                                                  false, m.scale);
            !nativeRows.empty()) {
            const NativeTextMeasure measure = interfaceTextMeasure("frizqt__", m.pixelHeight);
            for (const auto& row : nativeRows) {
                std::string lineText;
                for (const auto& run : row.runs) {
                    lineText += run.text;
                }
                lineText = trimTrailingWhitespace(std::move(lineText));
                const float w = (measure.face && !lineText.empty())
                    ? static_cast<float>(measure.width(lineText))
                    : row.width;
                maxLineW = std::max(maxLineW, w);
                wrappedLines.push_back({std::move(lineText), w});
            }
        } else {
            const char* lineStart = bubble.message.c_str();
            const char* textEnd = lineStart + bubble.message.size();
            while (lineStart < textEnd) {
                const char* lineEnd = font->CalcWordWrapPosition(fontSize, lineStart, textEnd, m.wrapWidthPx);
                if (lineEnd == lineStart) {
                    lineEnd = lineStart + 1;
                }
                const char* trimEnd = lineEnd;
                while (trimEnd > lineStart && (trimEnd[-1] == ' ' || trimEnd[-1] == '\n' || trimEnd[-1] == '\r')) {
                    --trimEnd;
                }
                std::string lineText(lineStart, trimEnd);
                const float w = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lineStart, trimEnd).x;
                maxLineW = std::max(maxLineW, w);
                wrappedLines.push_back({std::move(lineText), w});
                lineStart = lineEnd;
                while (lineStart < textEnd && (*lineStart == ' ' || *lineStart == '\n' || *lineStart == '\r')) {
                    ++lineStart;
                }
            }
        }
        if (wrappedLines.empty()) continue;

        const ChatBubbleBox box = computeChatBubbleBox(
            screenX, screenY, maxLineW, static_cast<int>(wrappedLines.size()), m);

        static const auto background = uploadUiTextureFromBlp(services.assetManager,
            "Interface/Tooltips/ChatBubble-Background.blp", window);
        static const auto border = uploadUiTextureFromBlp(services.assetManager,
            "Interface/Tooltips/ChatBubble-Backdrop.blp", window);
        static const auto tail = uploadUiTextureFromBlp(services.assetManager,
            "Interface/Tooltips/ChatBubble-Tail.blp", window);
        if (!background || !border || !tail)
            throw std::runtime_error("Missing authored chat bubble textures");
        const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(255 * alpha));
        const float x0 = box.x0, y0 = box.y0;
        const float x1 = box.x1, y1 = box.y1;
        const float edge = m.edge;
        if (box.bgX1 > box.bgX0 && box.bgY1 > box.bgY0) {
            dl->AddImage((ImTextureID)(uintptr_t)background,
                ImVec2(box.bgX0, box.bgY0), ImVec2(box.bgX1, box.bgY1),
                ImVec2(0, 0), ImVec2(1, 1), tint);
        }
        const auto piece = [&](int index, float left, float top, float right, float bottom, bool rotated = false) {
            if (right <= left || bottom <= top) return;
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
        dl->AddImage((ImTextureID)(uintptr_t)tail,
            ImVec2(box.tailX0, box.tailY0), ImVec2(box.tailX1, box.tailY1),
            ImVec2(0, 0), ImVec2(1, 1), tint);

        const ChatBubbleColor col = chatBubbleColor(bubble.chatType);
        const ImU32 textColU32 = ImGui::ColorConvertFloat4ToU32(
            ImVec4(col.r, col.g, col.b, alpha));
        // Center-align each wrapped line inside the bubble, matching Native TBC 2.4.3.
        float lineY = std::floor(y0 + m.padY);
        const float boxInnerW = box.width - m.padX * 2.0f;
        for (const auto& line : wrappedLines) {
            const float lineX = std::floor(x0 + m.padX + std::max(0.0f, (boxInnerW - line.widthPx) * 0.5f));
            if (!line.text.empty()) {
                dl->AddText(font, fontSize, ImVec2(lineX, lineY), textColU32,
                            line.text.c_str(), line.text.c_str() + line.text.size());
            }
            lineY += fontSize;
        }
    }
}

void ChatBubbleManager::setupCallback(game::GameHandler& gameHandler) {
    if (!callbackSet_) {
        // The line as the chat frame has it: 2.4.3 resolves no $-token in what
        // an NPC says (game/text_tokens.hpp).
        gameHandler.setChatBubbleCallback(
            [this](uint64_t guid, const std::string& msg, game::ChatType chatType) {
                addBubble(guid, msg, chatType);
            });
        callbackSet_ = true;
    }
}

} // namespace ui
} // namespace wowee
