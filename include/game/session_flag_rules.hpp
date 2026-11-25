#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/entity.hpp"
#include "game/protocol_constants.hpp"

namespace wowee::game {

// ============================================================================
// Tutorial bitfield rules (SMSG_TUTORIAL_FLAGS / CMSG_TUTORIAL_FLAG / CLEAR / RESET)
// ============================================================================
// FrameXML passes 1-based tutorial IDs (1..256). CMaNGOS WorldSession::HandleTutorialFlagOpcode
// reads a 0-based bit index `iFlag` (`wInt = iFlag / 32`, `rInt = iFlag % 32`).

[[nodiscard]] inline constexpr bool isTutorialBitSet(
        const std::array<uint32_t, 8>& words, int tutorialId) {
    if (tutorialId < 1 || tutorialId > 256) return false;
    const uint32_t idx = static_cast<uint32_t>(tutorialId - 1);
    return (words[idx / 32u] & (1u << (idx % 32u))) != 0u;
}

/// Sets the 1-based tutorial bit and returns the 0-based wire index for
/// CMSG_TUTORIAL_FLAG when newly set, or std::nullopt if out of range or already set.
inline constexpr std::optional<uint32_t> setTutorialBit(
        std::array<uint32_t, 8>& words, int tutorialId) {
    if (tutorialId < 1 || tutorialId > 256) return std::nullopt;
    const uint32_t idx = static_cast<uint32_t>(tutorialId - 1);
    const uint32_t mask = (1u << (idx % 32u));
    if ((words[idx / 32u] & mask) != 0u) return std::nullopt;
    words[idx / 32u] |= mask;
    return idx;
}

[[nodiscard]] inline std::vector<int> flaggedTutorialIds(
        const std::array<uint32_t, 8>& words) {
    std::vector<int> ids;
    for (uint32_t w = 0; w < 8u; ++w) {
        const uint32_t bits = words[w];
        if (bits == 0u) continue;
        for (uint32_t b = 0; b < 32u; ++b) {
            if ((bits & (1u << b)) != 0u) {
                ids.push_back(static_cast<int>(w * 32u + b + 1u));
            }
        }
    }
    return ids;
}

// ============================================================================
// Server, Defense, and Kick message formatting rules
// ============================================================================

struct FormattedServerMessage {
    std::string chatLine;
    std::string uiError;
};

/// CMaNGOS ServerMessageType (World.h:55-62):
///   1 = SERVER_MSG_SHUTDOWN_TIME
///   2 = SERVER_MSG_RESTART_TIME
///   3 = SERVER_MSG_CUSTOM
///   4 = SERVER_MSG_SHUTDOWN_CANCELLED (World::ShutdownCancel sends text = "")
///   5 = SERVER_MSG_RESTART_CANCELLED  (World::ShutdownCancel sends text = "")
[[nodiscard]] inline FormattedServerMessage formatServerMessage(
        uint32_t msgType, std::string_view text) {
    FormattedServerMessage out;
    switch (msgType) {
        case 1:
            if (!text.empty()) {
                out.chatLine = "[Shutdown] " + std::string(text);
                out.uiError = "Server shutdown: " + std::string(text);
            }
            break;
        case 2:
            if (!text.empty()) {
                out.chatLine = "[Restart] " + std::string(text);
                out.uiError = "Server restart: " + std::string(text);
            }
            break;
        case 4:
            out.chatLine = text.empty()
                ? "[Shutdown cancelled]"
                : ("[Shutdown cancelled] " + std::string(text));
            out.uiError = text.empty()
                ? "Server shutdown cancelled."
                : ("Server shutdown cancelled: " + std::string(text));
            break;
        case 5:
            out.chatLine = text.empty()
                ? "[Restart cancelled]"
                : ("[Restart cancelled] " + std::string(text));
            out.uiError = text.empty()
                ? "Server restart cancelled."
                : ("Server restart cancelled: " + std::string(text));
            break;
        default:
            if (!text.empty()) {
                out.chatLine = "[Server] " + std::string(text);
            }
            break;
    }
    return out;
}

[[nodiscard]] inline std::string formatDefenseMessage(
        std::string_view zoneName, std::string_view message) {
    if (message.empty()) return {};
    if (!zoneName.empty()) {
        return "[LocalDefense - " + std::string(zoneName) + "] " + std::string(message);
    }
    return "[Defense] " + std::string(message);
}

[[nodiscard]] inline std::string formatTbcKickReason(
        uint8_t reasonCode, std::string_view text) {
    if (!text.empty()) {
        return "Disconnected by server: " + std::string(text);
    }
    return "Disconnected by server (reason " + std::to_string(reasonCode) + ").";
}

// ============================================================================
// Part B Bitflag Policies (CreatureTypeFlags, UnitFlags, UnitFlags2, UnitDynFlags, NPCFlags)
// ============================================================================

// --- CreatureTypeFlags (Entities/CreatureDefines.h:282-316) ---

/// When the player is a ghost, Spirit Healers/Guides and creatures carrying
/// CREATURE_TYPEFLAG_VISIBLE_TO_GHOSTS (0x02) remain visible; ordinary living
/// creatures whose template is known and lacks VISIBLE_TO_GHOSTS are hidden.
[[nodiscard]] inline constexpr bool creatureVisibleToPlayer(
        bool isPlayerGhost, bool hasCachedTemplate, uint32_t npcFlags, uint32_t creatureTypeFlags) {
    if (!isPlayerGhost) return true;
    if ((npcFlags & (NPC_FLAG_SPIRIT_HEALER | NPC_FLAG_SPIRIT_GUIDE)) != 0u) return true;
    if (!hasCachedTemplate) return true;
    return (creatureTypeFlags & CREATURE_TYPEFLAG_VISIBLE_TO_GHOSTS) != 0u;
}

[[nodiscard]] inline constexpr bool creatureInteractableWhileDead(uint32_t creatureTypeFlags) {
    return (creatureTypeFlags & CREATURE_TYPEFLAG_INTERACT_WHILE_DEAD) != 0u;
}

[[nodiscard]] inline constexpr bool creatureShowsFactionTooltip(uint32_t creatureTypeFlags) {
    return (creatureTypeFlags & CREATURE_TYPEFLAG_NO_FACTION_TOOLTIP) == 0u;
}

[[nodiscard]] inline constexpr bool creatureAllowsNameplate(uint32_t creatureTypeFlags) {
    return (creatureTypeFlags & CREATURE_TYPEFLAG_NO_NAME_PLATE) == 0u;
}

[[nodiscard]] inline constexpr bool creatureCastsShadowBlob(uint32_t creatureTypeFlags) {
    return (creatureTypeFlags & CREATURE_TYPEFLAG_HAS_NO_SHADOW_BLOB) == 0u;
}

[[nodiscard]] inline constexpr bool creatureRendersObjectName(uint32_t creatureTypeFlags) {
    return (creatureTypeFlags & CREATURE_TYPEFLAG_DO_NOT_RENDER_OBJECT_NAME) == 0u;
}

// --- UnitFlags (Entities/Unit.h:480-514) ---

inline constexpr uint32_t kUnitFlagsLossOfControlMask =
    UNIT_FLAG_CLIENT_CONTROL_LOST |
    UNIT_FLAG_STUNNED |
    UNIT_FLAG_CONFUSED |
    UNIT_FLAG_FLEEING |
    UNIT_FLAG_TAXI_FLIGHT;

[[nodiscard]] inline constexpr bool playerHasFullControl(uint32_t unitFlags, uint64_t charmedByGuid = 0) {
    return (unitFlags & kUnitFlagsLossOfControlMask) == 0u && charmedByGuid == 0u;
}

[[nodiscard]] inline constexpr bool unitIsSilenced(uint32_t unitFlags) {
    return (unitFlags & UNIT_FLAG_SILENCED) != 0u;
}

[[nodiscard]] inline constexpr bool unitIsPacified(uint32_t unitFlags) {
    return (unitFlags & UNIT_FLAG_PACIFIED) != 0u;
}

[[nodiscard]] inline constexpr bool unitIsDisarmed(uint32_t unitFlags) {
    return (unitFlags & UNIT_FLAG_DISARMED) != 0u;
}

/// UNIT_FLAG_PREVENT_ANIM (0x20000000, set by SPELL_ATTR_EX_PREVENTS_ANIM auras
/// such as Dazed 1604 and Feign Death 5384) blocks server-side chat emotes and
/// stand-state changes. It does NOT freeze the M2 skeletal animation clock
/// (only SpellVisualKit CharProc 11 freezes skeletal animation playback).
[[nodiscard]] inline constexpr bool unitPreventsAnim(uint32_t unitFlags) {
    return (unitFlags & UNIT_FLAG_PREVENT_ANIM) != 0u;
}

/// UNIT_FLAG_SHEATHE (0x40000000) forces weapons into sheathed state (0) even if
/// UNIT_FIELD_BYTES_2 byte 0 holds 1 or 2.
[[nodiscard]] inline constexpr uint8_t effectiveUnitSheathState(uint8_t rawSheathByte, uint32_t unitFlags) {
    if ((unitFlags & UNIT_FLAG_SHEATHE) != 0u) return 0u;
    return rawSheathByte;
}

// --- UnitFlags2 (Entities/Unit.h:517-531) ---

[[nodiscard]] inline constexpr bool unitHidesBody(uint32_t unitFlags2) {
    return (unitFlags2 & UNIT_FLAG2_HIDE_BODY) != 0u;
}

[[nodiscard]] inline constexpr bool unitSkipsSpawnFadeIn(uint32_t unitFlags2) {
    return (unitFlags2 & UNIT_FLAG2_DO_NOT_FADE_IN) != 0u;
}

// --- UnitDynFlags (Globals/SharedDefines.h:1530-1540) ---

[[nodiscard]] inline constexpr bool unitTrackedOnMinimap(uint32_t dynamicFlags) {
    return (dynamicFlags & UNIT_DYNFLAG_TRACK_UNIT) != 0u;
}

[[nodiscard]] inline constexpr bool unitExposesSpecialInfo(uint32_t dynamicFlags) {
    return (dynamicFlags & UNIT_DYNFLAG_SPECIALINFO) != 0u;
}

// --- NPCFlags (Entities/Unit.h:537-562) ---

enum class DirectNpcServiceAction : uint8_t {
    GOSSIP_HELLO = 0,
    BATTLEMASTER_HELLO,
    GUILD_BANKER_ACTIVATE,
    TABARDVENDOR_ACTIVATE,
    PETITION_SHOWLIST,
    LIST_STABLED_PETS,
    SPELLCLICK,
};

/// Determines the primary wire action when right-clicking an interactable NPC.
/// NPCs that also carry GOSSIP or QUESTGIVER open their gossip menu first
/// (matching 2.4.3), except BATTLEMASTER without GOSSIP and dedicated single-service NPCs.
[[nodiscard]] inline constexpr DirectNpcServiceAction resolveDirectNpcServiceAction(
        uint32_t npcFlags, bool isPreWotlk) {
    if (!isPreWotlk && (npcFlags & NPC_FLAG_SPELLCLICK) != 0u &&
        (npcFlags & (NPC_FLAG_GOSSIP | NPC_FLAG_QUESTGIVER)) == 0u) {
        return DirectNpcServiceAction::SPELLCLICK;
    }
    if ((npcFlags & NPC_FLAG_BATTLEMASTER) != 0u &&
        (npcFlags & (NPC_FLAG_GOSSIP | NPC_FLAG_QUESTGIVER)) == 0u) {
        return DirectNpcServiceAction::BATTLEMASTER_HELLO;
    }
    if ((npcFlags & (NPC_FLAG_GOSSIP | NPC_FLAG_QUESTGIVER)) != 0u) {
        return DirectNpcServiceAction::GOSSIP_HELLO;
    }
    if ((npcFlags & NPC_FLAG_GUILD_BANKER) != 0u) {
        return DirectNpcServiceAction::GUILD_BANKER_ACTIVATE;
    }
    if ((npcFlags & NPC_FLAG_TABARDDESIGNER) != 0u) {
        return DirectNpcServiceAction::TABARDVENDOR_ACTIVATE;
    }
    if ((npcFlags & NPC_FLAG_PETITIONER) != 0u) {
        return DirectNpcServiceAction::PETITION_SHOWLIST;
    }
    if ((npcFlags & NPC_FLAG_STABLEMASTER) != 0u) {
        return DirectNpcServiceAction::LIST_STABLED_PETS;
    }
    return DirectNpcServiceAction::GOSSIP_HELLO;
}

} // namespace wowee::game
