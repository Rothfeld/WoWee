// ============================================================
// CombatUI - extracted from GameScreen
// Owns all combat-related UI rendering: cast bar, cooldown tracker,
// raid warning overlay, combat text, DPS meter, buff bar,
// battleground score HUD, combat log, threat window, BG scoreboard.
// ============================================================
#include "ui/combat_ui.hpp"
#include "ui/interface_fonts.hpp"
#include "ui/floating_combat_text.hpp"
#include "core/entity_spawner.hpp"
#include "rendering/character_renderer.hpp"
#include "addons/lua_api_registrations.hpp"
#include "ui/buff_bar_layout.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/settings_panel.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/ui_colors.hpp"
#include "ui/ui_helpers.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "core/coordinates.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "game/game_handler.hpp"
#include "game/bg_score_defs.hpp"
#include "pipeline/asset_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/ui_sound_manager.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
    using namespace wowee::ui::colors;
    using namespace wowee::ui::helpers;
} // anonymous namespace

namespace wowee {
namespace ui {


// ============================================================
// Cast Bar
// ============================================================


// ============================================================
// Raid Warning / Boss Emote Center-Screen Overlay
// ============================================================

// ============================================================================
// The sound a whisper makes
// ============================================================================
//
// What is left of the raid warning overlay. RaidWarningFrame is FrameXML's and
// answers CHAT_MSG_RAID_WARNING itself, so the banner went; the walk through
// new chat lines stayed, because the whisper sound was being played from
// inside it and belongs to no element - FrameXML plays none for a whisper.
void CombatUI::playSoundsForNewChat(game::GameHandler& gameHandler) {
    const auto& chatHistory = gameHandler.getChatHistory();
    const size_t newCount = chatHistory.size();
    if (newCount <= raidWarnChatSeenCount_) return;

    for (size_t i = raidWarnChatSeenCount_; i < newCount; ++i) {
        if (chatHistory[i].type != game::ChatType::WHISPER) continue;
        if (auto* ac = services_.audioCoordinator) {
            if (auto* ui = ac->getUiSoundManager()) ui->playWhisperReceived();
        }
    }
    raidWarnChatSeenCount_ = newCount;
}


// ============================================================
// Floating Combat Text
// ============================================================

// World damage/XP numbers are separate from optional incoming combat text.
// TBC build 8606 keeps damage for 1.5 seconds and XP for 4.5 seconds.
void CombatUI::renderCombatText(game::GameHandler& gameHandler) {
    const auto& entries = gameHandler.getCombatText();
    if (entries.empty()) return;

    auto* window = services_.window;
    if (!window) return;
    const float screenW = static_cast<float>(window->getWidth());
    const float screenH = static_cast<float>(window->getHeight());

    // Camera for world-space projection
    auto* appRenderer = services_.renderer;
    rendering::Camera* camera = appRenderer ? appRenderer->getCamera() : nullptr;
    glm::mat4 viewProj;
    if (camera) viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ImFont* font = interfaceFace("frizqt__");
    if (!font) { LOG_ERROR("Combat text requires Fonts\\FRIZQT__.TTF"); return; }
    const float baseFontSize = 22.0f * screenH / 600.0f;

    const float uiScale = screenH / 768.0f;
    struct WorldText {
        uint64_t guid;
        fct::Box box;
        std::string text;
        ImVec4 color;
        float alpha;
    };
    std::vector<WorldText> worldTexts;
    auto& app = core::Application::getInstance();
    const auto nameAnchorFor = [&](uint64_t guid, glm::vec3& out) {
        auto* spawner = app.getEntitySpawner();
        auto* characters = appRenderer ? appRenderer->getCharacterRenderer() : nullptr;
        if (!spawner || !characters) return false;
        const auto entity = gameHandler.getEntityManager().getEntity(guid);
        if (!entity || !entity->isUnit()) return false;
        const auto& unit = static_cast<const game::Unit&>(*entity);
        const auto instanceId = guid == gameHandler.getPlayerGuid() ? appRenderer->getCharacterInstanceId()
                                                                    : spawner->characterInstanceIdForGuid(guid);
        return characters->getInstanceNamePosition(instanceId, unit.getMountDisplayId() != 0, out);
    };

    for (const auto& entry : entries) {
        const bool outgoing = entry.isPlayerSource;
        const bool experience = entry.type == game::CombatTextEntry::XP_GAIN;
        const bool reward = experience || entry.type == game::CombatTextEntry::HONOR_GAIN;
        // Lines that land on the player are Blizzard_CombatText's, which
        // hears them as COMBAT_TEXT_UPDATE (CombatHandler::addCombatText);
        // build 8606 draws nothing of its own for them, and nothing for a
        // proc either.
        if (!outgoing && !reward) continue;
        if (entry.type == game::CombatTextEntry::PROC_TRIGGER) continue;
        const bool isCrit = entry.type == game::CombatTextEntry::CRIT_DAMAGE ||
                            entry.type == game::CombatTextEntry::CRIT_HEAL;
        // Native fade thresholds: ordinary hits 150/760/1500 ms;
        // XP 500/2000/4500 ms. Keep full opacity between fade intervals.
        const float fadeIn = reward ? 0.5f : 0.15f;
        const float fadeOut = reward ? 2.0f : isCrit ? 1.0f : 0.76f;
        const bool worldNumber = outgoing && !reward && entry.type != game::CombatTextEntry::PROC_TRIGGER;
        const float alpha = worldNumber ? fct::alpha(entry.age, isCrit) :
            std::clamp(std::min(entry.age / fadeIn,
            (entry.lifetime() - entry.age) / (entry.lifetime() - fadeOut)), 0.0f, 1.0f);

        // --- Format text and color (identical logic for both world and HUD paths) ---
        ImVec4 color;
        char text[128];
        switch (entry.type) {
            case game::CombatTextEntry::MELEE_DAMAGE:
            case game::CombatTextEntry::SPELL_DAMAGE:
                snprintf(text, sizeof(text), outgoing ? "%d" : "-%d", entry.amount);
                color = outgoing ?
                    (entry.type == game::CombatTextEntry::MELEE_DAMAGE ?
                     ImVec4(1,1,1,alpha) : ImVec4(1,222.0f/255.0f,0,alpha)) :
                    ImVec4(1.0f, 0.3f, 0.3f, alpha);
                break;
            case game::CombatTextEntry::CRIT_DAMAGE:
                snprintf(text, sizeof(text), outgoing ? "%d" : "-%d", entry.amount);
                color = outgoing ?
                    (entry.spellId ? ImVec4(1,222.0f/255.0f,0,alpha) : ImVec4(1,1,1,alpha)) :
                    ImVec4(1.0f, 0.5f, 0.0f, alpha);
                break;
            case game::CombatTextEntry::HEAL:
                snprintf(text, sizeof(text), "+%d", entry.amount);
                color = ImVec4(0.3f, 1.0f, 0.3f, alpha);
                break;
            case game::CombatTextEntry::CRIT_HEAL:
                snprintf(text, sizeof(text), "+%d!", entry.amount);
                color = ImVec4(0.3f, 1.0f, 0.3f, alpha);
                break;
            case game::CombatTextEntry::MISS:
                snprintf(text, sizeof(text), "Miss");
                color = ImVec4(1,1,1,alpha);
                break;
            case game::CombatTextEntry::DODGE:
                snprintf(text, sizeof(text), outgoing ? "Dodge" : "You Dodge");
                color = outgoing ? ImVec4(1,1,1,alpha)
                                 : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::PARRY:
                snprintf(text, sizeof(text), outgoing ? "Parry" : "You Parry");
                color = outgoing ? ImVec4(1,1,1,alpha)
                                 : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::BLOCK:
                if (entry.amount > 0)
                    snprintf(text, sizeof(text), outgoing ? "Block %d" : "You Block %d", entry.amount);
                else
                    snprintf(text, sizeof(text), outgoing ? "Block" : "You Block");
                color = outgoing ? ImVec4(1,1,1,alpha)
                                 : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::EVADE:
                snprintf(text, sizeof(text), outgoing ? "Evade" : "You Evade");
                color = outgoing ? ImVec4(1,1,1,alpha)
                                 : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::PERIODIC_DAMAGE:
                snprintf(text, sizeof(text), "-%d", entry.amount);
                color = outgoing ?
                    ImVec4(1.0f, 0.9f, 0.3f, alpha) :
                    ImVec4(1.0f, 0.4f, 0.4f, alpha);
                break;
            case game::CombatTextEntry::PERIODIC_HEAL:
                snprintf(text, sizeof(text), "+%d", entry.amount);
                color = ImVec4(0.4f, 1.0f, 0.5f, alpha);
                break;
            case game::CombatTextEntry::ENVIRONMENTAL: {
                const char* envLabel = "";
                switch (entry.powerType) {
                    case 0: envLabel = "Fatigue "; break;
                    case 1: envLabel = "Drowning "; break;
                    case 2: envLabel = ""; break;
                    case 3: envLabel = "Lava "; break;
                    case 4: envLabel = "Slime "; break;
                    case 5: envLabel = "Fire "; break;
                    default: envLabel = ""; break;
                }
                snprintf(text, sizeof(text), "%s-%d", envLabel, entry.amount);
                color = ImVec4(0.9f, 0.5f, 0.2f, alpha);
                break;
            }
            case game::CombatTextEntry::ENERGIZE:
                snprintf(text, sizeof(text), "+%d", entry.amount);
                switch (entry.powerType) {
                    case 1:  color = ImVec4(1.0f, 0.2f, 0.2f, alpha); break;
                    case 2:  color = ImVec4(1.0f, 0.6f, 0.1f, alpha); break;
                    case 3:  color = ImVec4(1.0f, 0.9f, 0.2f, alpha); break;
                    case 6:  color = ImVec4(0.3f, 0.9f, 0.8f, alpha); break;
                    default: color = ImVec4(0.3f, 0.6f, 1.0f, alpha); break;
                }
                break;
            case game::CombatTextEntry::POWER_DRAIN:
                snprintf(text, sizeof(text), "-%d", entry.amount);
                switch (entry.powerType) {
                    case 1:  color = ImVec4(1.0f, 0.35f, 0.35f, alpha); break;
                    case 2:  color = ImVec4(1.0f, 0.7f, 0.2f, alpha); break;
                    case 3:  color = ImVec4(1.0f, 0.95f, 0.35f, alpha); break;
                    case 6:  color = ImVec4(0.45f, 0.95f, 0.85f, alpha); break;
                    default: color = ImVec4(0.45f, 0.75f, 1.0f, alpha); break;
                }
                break;
            case game::CombatTextEntry::XP_GAIN:
                snprintf(text, sizeof(text), "XP: %d", entry.amount);
                color = ImVec4(0.6f, 0.0f, 0.6f, alpha);
                break;
            case game::CombatTextEntry::IMMUNE:
                snprintf(text, sizeof(text), "Immune");
                color = ImVec4(0.9f, 0.9f, 0.9f, alpha);
                break;
            case game::CombatTextEntry::ABSORB:
                if (entry.amount > 0)
                    snprintf(text, sizeof(text), "Absorbed %d", entry.amount);
                else
                    snprintf(text, sizeof(text), "Absorbed");
                color = ImVec4(0.5f, 0.8f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::RESIST:
                if (entry.amount > 0)
                    snprintf(text, sizeof(text), "Resisted %d", entry.amount);
                else
                    snprintf(text, sizeof(text), "Resisted");
                color = ImVec4(1,1,1,alpha);
                break;
            case game::CombatTextEntry::DEFLECT:
                snprintf(text, sizeof(text), outgoing ? "Deflect" : "You Deflect");
                color = outgoing ? ImVec4(0.7f, 0.7f, 0.7f, alpha)
                                 : ImVec4(0.5f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::REFLECT: {
                const std::string& reflectName = entry.spellId ? gameHandler.getSpellName(entry.spellId) : "";
                if (!reflectName.empty())
                    snprintf(text, sizeof(text), outgoing ? "Reflected: %s" : "Reflect: %s", reflectName.c_str());
                else
                    snprintf(text, sizeof(text), outgoing ? "Reflected" : "You Reflect");
                color = outgoing ? ImVec4(0.85f, 0.75f, 1.0f, alpha)
                                 : ImVec4(0.75f, 0.85f, 1.0f, alpha);
                break;
            }
            case game::CombatTextEntry::PROC_TRIGGER: {
                const std::string& procName = entry.spellId ? gameHandler.getSpellName(entry.spellId) : "";
                if (!procName.empty())
                    snprintf(text, sizeof(text), "%s!", procName.c_str());
                else
                    snprintf(text, sizeof(text), "PROC!");
                color = ImVec4(1.0f, 0.85f, 0.0f, alpha);
                break;
            }
            case game::CombatTextEntry::DISPEL:
                if (entry.spellId != 0) {
                    const std::string& dispelledName = gameHandler.getSpellName(entry.spellId);
                    if (!dispelledName.empty())
                        snprintf(text, sizeof(text), "Dispel %s", dispelledName.c_str());
                    else
                        snprintf(text, sizeof(text), "Dispel");
                } else {
                    snprintf(text, sizeof(text), "Dispel");
                }
                color = ImVec4(0.6f, 0.9f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::STEAL:
                if (entry.spellId != 0) {
                    const std::string& stolenName = gameHandler.getSpellName(entry.spellId);
                    if (!stolenName.empty())
                        snprintf(text, sizeof(text), "Spellsteal %s", stolenName.c_str());
                    else
                        snprintf(text, sizeof(text), "Spellsteal");
                } else {
                    snprintf(text, sizeof(text), "Spellsteal");
                }
                color = ImVec4(0.8f, 0.7f, 1.0f, alpha);
                break;
            case game::CombatTextEntry::INTERRUPT: {
                const std::string& interruptedName = entry.spellId ? gameHandler.getSpellName(entry.spellId) : "";
                if (!interruptedName.empty())
                    snprintf(text, sizeof(text), "Interrupt %s", interruptedName.c_str());
                else
                    snprintf(text, sizeof(text), "Interrupt");
                color = ImVec4(1.0f, 0.6f, 0.9f, alpha);
                break;
            }
            case game::CombatTextEntry::INSTAKILL:
                snprintf(text, sizeof(text), outgoing ? "Kill!" : "Killed!");
                color = outgoing ? ImVec4(1.0f, 0.25f, 0.25f, alpha)
                                 : ImVec4(1.0f, 0.1f, 0.1f, alpha);
                break;
            case game::CombatTextEntry::HONOR_GAIN:
                snprintf(text, sizeof(text), "+%d Honor", entry.amount);
                color = ImVec4(1.0f, 0.85f, 0.0f, alpha);
                break;
            case game::CombatTextEntry::GLANCING:
                snprintf(text, sizeof(text), "%d", entry.amount);
                color = outgoing ?
                    ImVec4(1,1,1,alpha) :
                    ImVec4(0.75f, 0.35f, 0.35f, alpha);
                break;
            case game::CombatTextEntry::CRUSHING:
                snprintf(text, sizeof(text), "%d", entry.amount);
                color = outgoing ?
                    ImVec4(1.0f, 0.55f, 0.1f, alpha) :
                    ImVec4(1.0f, 0.15f, 0.15f, alpha);
                break;
            default:
                snprintf(text, sizeof(text), "%d", entry.amount);
                color = ImVec4(1.0f, 1.0f, 1.0f, alpha);
                break;
        }

        // --- Rendering style ---
        const float progress = entry.age / entry.lifetime();
        const float critPulse = progress < 0.1f ? 0.1f + 19.0f * progress :
            progress < 0.2f ? 3.0f - 10.0f * progress : 1.0f;
        float renderFontSize = isCrit ? baseFontSize * 1.5f * critPulse : baseFontSize;

        ImU32 shadowCol = IM_COL32(0, 0, 0, static_cast<int>(alpha * 180));
        ImU32 textCol   = ImGui::ColorConvertFloat4ToU32(color);

        if (worldNumber && camera && entry.dstGuid != 0) {
            // Numbers start at the bottom of the unit's plain name line
            // (name attachment + 0.25 yd): ordinary ones sit on it and climb,
            // critical ones hang below it and stay. Laid out per unit after every
            // entry is known.
            glm::vec3 anchor;
            if (nameAnchorFor(entry.dstGuid, anchor)) {
                anchor.z += fct::kNameLineOffset;
                if (!isCrit) anchor.z += fct::kRiseYardsPerSecond * entry.age;
                const glm::vec4 clipPos = viewProj * glm::vec4(anchor, 1.0f);
                if (clipPos.w > 0.01f) {
                    const glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                    const float fontPx = fct::kFontHeight * uiScale *
                        (isCrit ? fct::kCritScale * fct::critPulse(entry.age) : 1.0f);
                    fct::Box box;
                    box.naturalX = (ndc.x * 0.5f + 0.5f) * screenW;
                    box.naturalTop = (ndc.y * 0.5f + 0.5f) * screenH - (isCrit ? 0.0f : fontPx);
                    box.fontPx = fontPx;
                    box.width = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, text).x;
                    worldTexts.push_back({entry.dstGuid, box, text, color, alpha});
                }
            }
            continue;
        }
        if (reward && camera) {
            // Experience above the player.
            glm::vec3 renderPos;
            if (core::Application::getInstance().getRenderPositionForGuid(gameHandler.getPlayerGuid(), renderPos)) {
                renderPos.z += 2.0f;
                glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
                if (clipPos.w > 0.01f) {
                    glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                    if (ndc.x >= -1.5f && ndc.x <= 1.5f && ndc.y >= -1.5f && ndc.y <= 1.5f) {
                        float sx = (ndc.x * 0.5f + 0.5f) * screenW;
                        float sy = (ndc.y * 0.5f + 0.5f) * screenH;
                        ImVec2 ts = font->CalcTextSizeA(renderFontSize, FLT_MAX, 0.0f, text);
                        sx -= ts.x * 0.5f;
                        sx = std::max(2.0f, std::min(sx, screenW - ts.x - 2.0f));
                        drawList->AddText(font, renderFontSize,
                                          ImVec2(sx + 1.0f, sy + 1.0f), shadowCol, text);
                        drawList->AddText(font, renderFontSize,
                                          ImVec2(sx, sy), textCol, text);
                    }
                }
            }
        }

    }

    // Entries are oldest first; layout wants each unit's newest first.
    std::stable_sort(worldTexts.begin(), worldTexts.end(),
        [](const WorldText& a, const WorldText& b) { return a.guid < b.guid; });
    for (size_t begin = 0; begin < worldTexts.size();) {
        size_t end = begin;
        while (end < worldTexts.size() && worldTexts[end].guid == worldTexts[begin].guid) ++end;
        std::vector<fct::Box> boxes;
        for (size_t i = end; i-- > begin;) boxes.push_back(worldTexts[i].box);
        fct::layout(boxes);
        for (auto& box : boxes) fct::clampToTop(box, uiScale);
        for (size_t i = begin; i < end; ++i) worldTexts[i].box = boxes[end - 1 - i];
        begin = end;
    }
    for (const auto& t : worldTexts) {
        const float x = t.box.x - 0.5f * t.box.width;
        const ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(t.alpha * fct::kShadowAlpha * 255.0f));
        drawList->AddText(font, t.box.fontPx,
            ImVec2(x + fct::kShadowX * uiScale, t.box.top + fct::kShadowY * uiScale), shadow, t.text.c_str());
        drawList->AddText(font, t.box.fontPx, ImVec2(x, t.box.top),
            ImGui::ColorConvertFloat4ToU32(t.color), t.text.c_str());
    }
}

} // namespace ui
} // namespace wowee
