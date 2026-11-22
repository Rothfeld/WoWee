#pragma once

#include "ui/chat/chat_bubble_layout.hpp"
#include "ui/ui_services.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace game { class GameHandler; }
namespace ui {

/**
 * Manages 3D-projected chat bubbles above entities.
 *
 * Extracted from ChatPanel (Phase 1.4 of chat_panel_ref.md).
 * Owns bubble lifecycle: add, update (tick), render (ImGui overlay).
 */
class ChatBubbleManager {
public:
    /** Add or replace a bubble for the given entity. */
    void addBubble(uint64_t senderGuid, const std::string& message,
                   game::ChatType chatType = game::ChatType::SAY);

    /** Render and tick all active bubbles (projects to screen via camera). */
    void render(game::GameHandler& gameHandler, const UIServices& services);

    /** Register the chat-bubble callback on GameHandler (call once per session). */
    void setupCallback(game::GameHandler& gameHandler);

    [[nodiscard]] bool empty() const { return bubbles_.empty(); }

    /// The chatBubbles CVar (SAY/YELL/MONSTER_*). Turning it off drops queued
    /// non-party bubbles immediately rather than leaving them to fade.
    [[nodiscard]] bool bubblesShown() const { return show_; }
    void setBubblesShown(bool shown) { show_ = shown; }

    /// The chatBubblesParty CVar (PARTY/RAID).
    [[nodiscard]] bool partyBubblesShown() const { return showParty_; }
    void setPartyBubblesShown(bool shown) { showParty_ = shown; }

private:
    struct ChatBubble {
        uint64_t senderGuid = 0;
        std::string message;
        float timeRemaining = 0.0f;
        float totalDuration = 0.0f;
        game::ChatType chatType = game::ChatType::SAY;
    };
    std::vector<ChatBubble> bubbles_;
    bool callbackSet_ = false;
    bool show_ = true;
    bool showParty_ = false;

    static constexpr size_t kMaxBubbles = 10;
};

} // namespace ui
} // namespace wowee
