#include <cassert>
#include "game/chat_handler.hpp"
#include "game/bg_score_defs.hpp"
#include "game/group_defines.hpp"
#include "game/cast_result_text.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <set>
#include "addons/lua_api_registrations.hpp"
#include "game/chat_filters.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/opcode_table.hpp"
#include "network/world_socket.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "core/logger.hpp"
#include "core/app_clock.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <ctime>
#include "core/local_time.hpp"

namespace wowee {
namespace game {

ChatHandler::ChatHandler(GameHandler& owner)
    : owner_(owner) {
    initializeChatLog();
}

namespace {

bool isTruthyEnvValue(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back(static_cast<char>(std::tolower(c)));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool isFalsyEnvValue(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back(static_cast<char>(std::tolower(c)));
    return lower.empty() || lower == "0" || lower == "false" || lower == "no" || lower == "off";
}

std::string escapeChatLogField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string formatChatLogTimestamp(std::chrono::system_clock::time_point timestamp) {
    auto time = std::chrono::system_clock::to_time_t(timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()) % 1000;

    std::tm tm = core::localTime(time);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string formatChatLogGuid(uint64_t guid) {
    if (guid == 0) return "";
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex
        << std::setfill('0') << std::setw(16) << guid;
    return oss.str();
}

std::string chatParticipant(const std::string& name, uint64_t guid, const char* fallback) {
    if (!name.empty()) return name;
    if (guid != 0) return formatChatLogGuid(guid);
    return fallback ? fallback : "";
}

bool chatPacketDiagEnabled() {
    static const bool enabled = [] {
        const char* raw = std::getenv("WOWEE_CHAT_PACKET_DIAG");
        if (!raw) return false;
        return !isFalsyEnvValue(raw);
    }();
    return enabled;
}

} // namespace

void ChatHandler::initializeChatLog() {
    const char* enabledRaw = std::getenv("WOWEE_CHAT_LOG");
    if (!enabledRaw) return;

    std::string enabledValue(enabledRaw);
    if (isFalsyEnvValue(enabledValue)) return;

    const char* pathRaw = std::getenv("WOWEE_CHAT_LOG_PATH");
    const bool hasPathOverride = pathRaw && *pathRaw;
    chatLogPath_ = hasPathOverride ? pathRaw : "logs/chat.log";
    if (!hasPathOverride && !isTruthyEnvValue(enabledValue) && !enabledValue.empty()) {
        chatLogPath_ = enabledValue;
    }

    std::filesystem::path path(chatLogPath_);
    if (!path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    chatLogStream_.open(path, std::ios::out | std::ios::app);
    if (!chatLogStream_.is_open()) {
        LOG_WARNING("Chat external log requested but could not open: ", path.string());
        return;
    }

    chatLogEnabled_ = true;
    chatLogInitialized_ = true;
    chatLogStream_ << "# WoWee chat log started " << formatChatLogTimestamp(std::chrono::system_clock::now()) << '\n';
    chatLogStream_ << "# timestamp\tsource\ttype\tsender\tsender_guid\treceiver\treceiver_guid\tchannel\tmessage\n";
    chatLogStream_.flush();
    LOG_INFO("External chat logging enabled: ", path.string());
}

void ChatHandler::logChatMessage(const MessageChatData& msg, const char* source) {
    if (!chatLogEnabled_ || !chatLogInitialized_ || !chatLogStream_.is_open()) return;
    if (msg.message.empty()) return;

    const std::string sender = chatParticipant(msg.senderName, msg.senderGuid, "System");
    const std::string receiver = chatParticipant(msg.receiverName, msg.receiverGuid, "");
    chatLogStream_
        << formatChatLogTimestamp(msg.timestamp) << '\t'
        << (source ? source : "") << '\t'
        << getChatTypeString(msg.type) << '\t'
        << escapeChatLogField(sender) << '\t'
        << formatChatLogGuid(msg.senderGuid) << '\t'
        << escapeChatLogField(receiver) << '\t'
        << formatChatLogGuid(msg.receiverGuid) << '\t'
        << escapeChatLogField(msg.channelName) << '\t'
        << escapeChatLogField(msg.message) << '\n';
    chatLogStream_.flush();
}

void ChatHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleMessageChat(packet);
    };
    table[Opcode::SMSG_GM_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        // TBC (CMaNGOS ChatHandler::BuildChatPacket, build 8606) writes the
        // ordinary SMSG_MESSAGECHAT body and appends the GM's name after the
        // chat tag, so the message parser reads it unchanged and leaves the
        // trailing name unread; the sender's name comes from its guid as for
        // any player line. A GM's own say comes back this way while the GM
        // chat tag is on.
        if (isPreWotlk()) {
            handleMessageChat(packet);
            return;
        }
        // WotLK SMSG_GM_MESSAGECHAT has the same header as SMSG_MESSAGECHAT
        // (type[1]+lang[4]+senderGuid[8]+unk[4] = 17 bytes) followed by an
        // extra gmNameLen[4]+gmName[N] before the type-specific body.
        // Strip the GM name field to produce standard SMSG_MESSAGECHAT format.
        if (!packet.hasRemaining(21)) return; // 17 header + 4 gmNameLen min
        uint8_t  type       = packet.readUInt8();
        uint32_t lang       = packet.readUInt32();
        uint64_t senderGuid = packet.readUInt64();
        uint32_t unk        = packet.readUInt32();
        uint32_t gmNameLen  = packet.readUInt32();
        if (!packet.hasRemaining(gmNameLen)) return;
        packet.setReadPos(packet.getReadPos() + gmNameLen); // skip gmName

        // Rebuild as regular SMSG_MESSAGECHAT (header + remaining body)
        network::Packet regular(0);
        regular.writeUInt8(type);
        regular.writeUInt32(lang);
        regular.writeUInt64(senderGuid);
        regular.writeUInt32(unk);
        const auto& raw = packet.getData();
        size_t pos = packet.getReadPos();
        if (pos < raw.size())
            regular.writeBytes(raw.data() + pos, raw.size() - pos);
        handleMessageChat(regular);
    };
    table[Opcode::SMSG_TEXT_EMOTE] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleTextEmote(packet);
    };
    table[Opcode::SMSG_EMOTE] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        if (!packet.hasRemaining(12)) return;
        uint32_t emoteId    = packet.readUInt32();
        uint64_t sourceGuid = packet.readUInt64();
        uint32_t animId = rendering::AnimationController::getEmoteAnimByEmotesId(emoteId);
        // Emotes.dbc EmoteSpecProc distinguishes persistent STATE_ emotes from
        // one-shots. Emote 0 (ONESHOT_NONE) cancels a one-shot but must not
        // clear a UNIT_NPC_EMOTESTATE work loop, so it is forwarded as non-state.
        const bool isState = emoteId != 0 &&
            rendering::AnimationController::isStateEmoteById(emoteId);
        if (sourceGuid != 0 && (animId != 0 || emoteId == 0)) {
            owner_.playServerEmote(sourceGuid, animId, isState, emoteId);
        } else if (emoteId != 0 && animId == 0) {
            LOG_DEBUG("SMSG_EMOTE emoteId=", emoteId, " had no Emotes.dbc animation mapping");
        }
    };
    table[Opcode::SMSG_CHANNEL_NOTIFY] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD ||
            owner_.getState() == WorldState::ENTERING_WORLD)
            handleChannelNotify(packet);
    };
    table[Opcode::SMSG_CHAT_PLAYER_NOT_FOUND] = [this](network::Packet& packet) {
        std::string name = packet.readString();
        if (!name.empty()) owner_.systemGlobal("ERR_CHAT_PLAYER_NOT_FOUND_S", {name});
    };
    table[Opcode::SMSG_CHAT_PLAYER_AMBIGUOUS] = [this](network::Packet& packet) {
        std::string name = packet.readString();
        if (!name.empty()) owner_.systemGlobal("ERR_CHAT_PLAYER_AMBIGUOUS_S", {name});
    };
    table[Opcode::SMSG_CHAT_WRONG_FACTION] = [this](network::Packet& /*packet*/) {
        owner_.systemGlobal("ERR_CHAT_WRONG_FACTION");
    };
    table[Opcode::SMSG_CHAT_NOT_IN_PARTY] = [this](network::Packet& /*packet*/) {
        owner_.systemGlobal("ERR_NOT_IN_GROUP");
    };
    table[Opcode::SMSG_CHAT_RESTRICTED] = [this](network::Packet& /*packet*/) {
        owner_.systemGlobal("ERR_CHAT_RESTRICTED");
    };

    // ---- Channel list ----

    // ---- Server / defense / area-trigger messages (moved from GameHandler) ----
    table[Opcode::SMSG_DEFENSE_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(5)) {
            /*uint32_t zoneId =*/ packet.readUInt32();
            std::string defMsg = packet.readString();
            if (!defMsg.empty()) addSystemChatMessage("[Defense] " + defMsg);
        }
    };
    // Server messages
    table[Opcode::SMSG_SERVER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t msgType = packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) {
                std::string prefix;
                switch (msgType) {
                    case 1: prefix = "[Shutdown] ";   owner_.addUIError("Server shutdown: " + msg);  break;
                    case 2: prefix = "[Restart] ";    owner_.addUIError("Server restart: " + msg);   break;
                    case 4: prefix = "[Shutdown cancelled] "; break;
                    case 5: prefix = "[Restart cancelled] ";  break;
                    default: prefix = "[Server] "; break;
                }
                addSystemChatMessage(prefix + msg);
            }
        }
    };
    table[Opcode::SMSG_CHAT_SERVER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            /*uint32_t msgType =*/ packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) addSystemChatMessage("[Announcement] " + msg);
        }
    };
    table[Opcode::SMSG_AREA_TRIGGER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            /*uint32_t len =*/ packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) {
                owner_.addUIError(msg);
                addSystemChatMessage(msg);
            }
        }
    };

    table[Opcode::SMSG_CHANNEL_LIST] = [this](network::Packet& p) { handleChannelList(p); };
    table[Opcode::SMSG_CHANNEL_MEMBER_COUNT] = [this](network::Packet& p) { handleChannelMemberCount(p); };
    // Add and update carry the same fields and mean the same thing to a
    // roster that keys on the guid.
    table[Opcode::SMSG_USERLIST_ADD] = [this](network::Packet& p) { handleUserlistAdd(p); };
    table[Opcode::SMSG_USERLIST_UPDATE] = [this](network::Packet& p) { handleUserlistAdd(p); };
    table[Opcode::SMSG_USERLIST_REMOVE] = [this](network::Packet& p) { handleUserlistRemove(p); };
}

void ChatHandler::sendChatMessage(ChatType type, const std::string& message, const std::string& target) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot send chat in state: ", static_cast<int>(owner_.getState()));
        return;
    }

    // Auto Clear AFK: speaking while away clears the flag first, as 2.4.3
    // does ("/afk" then "/say hello" printed "You are no longer AFK." before
    // the line, pair 6). The realm only clears it on request - an empty
    // CHAT_MSG_AFK - and the line itself comes from PLAYER_FLAGS
    // (onAwayFlags).
    const bool away = type == ChatType::AFK || type == ChatType::DND;
    if (!away && owner_.afkStatusRef() &&
        addons::storedCVarValue("autoClearAFK", "1") != "0") {
        sendChatMessage(ChatType::AFK, "", "");
    }

    // An empty AFK or DND is the request to clear it, the way SlashCmdList
    // CHAT_AFK sends a bare "/afk".
    if (message.empty() && !away) {
        LOG_WARNING("Cannot send empty chat message");
        return;
    }
    // The text the "You are now AFK: %s" line shows once the flag arrives.
    if (type == ChatType::AFK) owner_.afkMessageRef() = message;
    if (type == ChatType::DND) owner_.dndMessageRef() = message;

    // Party chat without a party is refused before sending, in chat: "/p
    // hello" printed "You aren't in a party." (pair 6); the realm drops it
    // silently.
    if (type == ChatType::PARTY && !owner_.isInGroup()) {
        owner_.systemGlobal("ERR_NOT_IN_GROUP");
        return;
    }

    // A corpse cannot speak in public. Build 8606 refuses say, yell and emote
    // with ERR_CHAT_WHILE_DEAD and sends nothing; whisper, party, guild and
    // channel still go out (observed on the original client, dead, not yet
    // released). GM commands typed as say are refused the same way.
    if (owner_.isPlayerDead() && !owner_.isPlayerGhost() &&
        (type == ChatType::SAY || type == ChatType::YELL || type == ChatType::EMOTE)) {
        owner_.addUIError("You can't chat when you're dead!");
        return;
    }

    LOG_INFO("OUTGOING CHAT: type=", static_cast<int>(type),
             " (", getChatTypeString(type), ") target='", target, "' msg='", message.substr(0, 60), "'");

    // Use the player's faction language. AzerothCore rejects wrong language.
    // Alliance races: Human(1), Dwarf(3), NightElf(4), Gnome(7), Draenei(11) → COMMON (7)
    // Horde races: Orc(2), Undead(5), Tauren(6), Troll(8), BloodElf(10) → ORCISH (1)
    uint8_t race = owner_.getPlayerRace();
    bool isHorde = (race == 2 || race == 5 || race == 6 || race == 8 || race == 10);
    ChatLanguage language = isHorde ? ChatLanguage::ORCISH : ChatLanguage::COMMON;

    auto packet = MessageChatPacket::build(type, language, message, target);
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG TX CMSG_MESSAGECHAT type=", getChatTypeString(type),
                    " target='", target, "' bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }
    owner_.getSocket()->send(packet);

    // No local echo. Build 8606 prints nothing when a line is sent: the row
    // comes from the server's SMSG_MESSAGECHAT, sent back to the speaker as
    // to everyone else (say, yell, party, guild, channel), or as
    // CHAT_MSG_WHISPER_INFORM for a whisper. Anything the server consumes
    // instead - a GM command such as ".gps" or ".go xyz", which
    // ChatHandler::ParseCommands answers with system messages only - or
    // refuses (muted, unknown whisper target) therefore never shows as said.
}

void ChatHandler::sendAddonMessage(ChatType type, const std::string& message, const std::string& target) {
    if (owner_.getState() != WorldState::IN_WORLD || message.empty()) return;

    auto packet = MessageChatPacket::build(type, ChatLanguage::ADDON, message, target);
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG TX CMSG_MESSAGECHAT addon type=", getChatTypeString(type),
                    " target='", target, "' bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }
    owner_.getSocket()->send(packet);
}

void ChatHandler::handleMessageChat(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_MESSAGECHAT");
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG RX SMSG_MESSAGECHAT bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }

    MessageChatData data;
    if (!owner_.getPacketParsers()->parseMessageChat(packet, data)) {
        const auto& raw = packet.getData();
        LOG_WARNING("Failed to parse SMSG_MESSAGECHAT, size=", packet.getSize(),
                    " data=[", raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
        return;
    }
    if (chatPacketDiagEnabled()) {
        LOG_WARNING("CHAT PACKET DIAG PARSED SMSG_MESSAGECHAT type=", getChatTypeString(data.type),
                    " sender=", formatChatLogGuid(data.senderGuid), " senderName='", data.senderName,
                    "' receiver=", formatChatLogGuid(data.receiverGuid), " receiverName='", data.receiverName,
                    "' channel='", data.channelName, "' msg='", data.message, "'");
    }
    LOG_DEBUG("INCOMING CHAT: type=", static_cast<int>(data.type),
             " (", getChatTypeString(data.type), ") sender=0x", std::hex, data.senderGuid, std::dec,
             " '", data.senderName, "' msg='", data.message.substr(0, 60), "'");

    deliverChatMessage(std::move(data), /*alreadyWaited=*/false);
}

void ChatHandler::deliverChatMessage(MessageChatData data, bool alreadyWaited) {
    // Resolve sender name from entity/cache if not already set by parser
    if (data.senderName.empty() && data.senderGuid != 0) {
        auto nameIt = owner_.getPlayerNameCache().find(data.senderGuid);
        if (nameIt != owner_.getPlayerNameCache().end()) {
            data.senderName = nameIt->second;
        } else {
            auto entity = owner_.getEntityManager().getEntity(data.senderGuid);
            if (entity) {
                if (entity->getType() == ObjectType::PLAYER) {
                    auto player = std::dynamic_pointer_cast<Player>(entity);
                    if (player && !player->getName().empty()) {
                        data.senderName = player->getName();
                    }
                } else if (entity->getType() == ObjectType::UNIT) {
                    auto unit = std::dynamic_pointer_cast<Unit>(entity);
                    if (unit && !unit->getName().empty()) {
                        data.senderName = unit->getName();
                    }
                }
            }
        }

        if (data.senderName.empty()) {
            const auto& partyData = owner_.getPartyData();
            for (const auto& member : partyData.members) {
                if (member.guid == data.senderGuid && !member.name.empty()) {
                    data.senderName = member.name;
                    break;
                }
            }
        }

        // The guild roster, which was not among the places looked and is the
        // one that holds the names this most often needs.
        //
        // Every other source here is someone nearby or in the party. A
        // guildmate is characteristically neither: they are in another zone,
        // which is the whole point of guild chat. So a guild line from anyone
        // not recently met arrived with no name at all - "[Guild] : message" -
        // and a guild achievement, which is announced by whoever earned it
        // wherever they are, read "[] has earned the achievement".
        //
        // The roster is already held and already carries the name beside the
        // guid; nothing had asked it. It is only populated once the roster has
        // been requested, so this is a source rather than a guarantee, and the
        // query below still runs when it misses.
        if (data.senderName.empty()) {
            for (const auto& member : owner_.getGuildRoster().members) {
                if (member.guid == data.senderGuid && !member.name.empty()) {
                    data.senderName = member.name;
                    break;
                }
            }
        }

        if (data.senderName.empty()) {
            owner_.queryPlayerName(data.senderGuid);
            // Hold the line rather than showing it with nobody's name on it.
            //
            // SMSG_MESSAGECHAT carries a guid and no name for a player line,
            // and every source above is somebody nearby, in the party or in
            // the guild. A whisper is characteristically from none of those -
            // that is what makes it a whisper - so it missed all of them, went
            // out with an empty name, and the interface printed it with no
            // sender. The backfill that follows a name query reaches this
            // client's own history and not what the interface has already
            // drawn, so the name has to be there the first time.
            if (!alreadyWaited) {
                const uint64_t waitingOn = data.senderGuid;
                chatAwaitingName_.push_back({std::move(data), waitingOn,
                                             core::appTimeSeconds() + kNameWaitSeconds});
                return;
            }
        }
    }

    if (data.message.empty()) {
        return;
    }

    // CHAT_MSG_BG_SYSTEM_* (AB/AV/WSG/EotS node claims, assaults, defenses)
    // carry "$n" / "$N" in the message and the acting player's GUID in
    // receiverGuid (targetGuid on the wire, mangos-tbc BattleGround2ChatBuilder).
    if ((data.type == ChatType::BG_SYSTEM_NEUTRAL ||
         data.type == ChatType::BG_SYSTEM_ALLIANCE ||
         data.type == ChatType::BG_SYSTEM_HORDE) &&
        (data.message.find("$n") != std::string::npos ||
         data.message.find("$N") != std::string::npos)) {
        const uint64_t subjectGuid = data.receiverGuid != 0 ? data.receiverGuid : data.senderGuid;
        if (subjectGuid != 0) {
            std::string subjectName;
            auto nameIt = owner_.getPlayerNameCache().find(subjectGuid);
            if (nameIt != owner_.getPlayerNameCache().end() && !nameIt->second.empty()) {
                subjectName = nameIt->second;
            }
            if (subjectName.empty()) {
                if (auto entity = owner_.getEntityManager().getEntity(subjectGuid)) {
                    if (entity->getType() == ObjectType::PLAYER) {
                        auto player = std::static_pointer_cast<Player>(entity);
                        if (!player->getName().empty()) subjectName = player->getName();
                    }
                }
            }
            if (subjectName.empty()) {
                for (const auto& member : owner_.getPartyData().members) {
                    if (member.guid == subjectGuid && !member.name.empty()) {
                        subjectName = member.name;
                        break;
                    }
                }
            }
            if (subjectName.empty()) {
                owner_.queryPlayerName(subjectGuid);
                if (!alreadyWaited) {
                    chatAwaitingName_.push_back({std::move(data), subjectGuid,
                                                 core::appTimeSeconds() + kNameWaitSeconds});
                    return;
                }
            } else {
                substituteBgSystemPlayerToken(data.message, subjectName);
            }
        }
    }

    if (data.type == ChatType::ACHIEVEMENT ||
        data.type == ChatType::GUILD_ACHIEVEMENT) {
        auto replaceAll = [](std::string& text, const std::string& marker,
                             const std::string& replacement) {
            size_t pos = 0;
            while ((pos = text.find(marker, pos)) != std::string::npos) {
                text.replace(pos, marker.size(), replacement);
                pos += replacement.size();
            }
        };

        if (data.achievementId != 0) {
            owner_.ensureAchievementNamesLoaded();
            std::string achievementName = owner_.getAchievementName(data.achievementId);
            if (achievementName.empty()) {
                achievementName = "Achievement #" + std::to_string(data.achievementId);
            }
            std::ostringstream link;
            link << "|cffffff00|Hachievement:" << data.achievementId
                 << ":0:0:0:0:0:0:0:0:0|h[" << achievementName << "]|h|r";
            replaceAll(data.message, "$a", link.str());
        }
    }

    // Addon messages use the same chat types as player messages, but belong to
    // CHAT_MSG_ADDON and must never be inserted into visible chat history.
    std::string addonPrefix;
    std::string addonPayload;
    if (decodeAddonChatPayload(data, addonPrefix, addonPayload)) {
        if (owner_.addonEventCallbackRef() && !addonPrefix.empty()) {
            owner_.addonEventCallbackRef()(
                "CHAT_MSG_ADDON",
                {addonPrefix, addonPayload, getChatTypeString(data.type), data.senderName});
        }
        return;
    }

    // Server monster messages use %s as a placeholder for the creature's name.
    if (!data.senderName.empty() && (
            data.type == ChatType::MONSTER_SAY || data.type == ChatType::MONSTER_YELL ||
            data.type == ChatType::MONSTER_EMOTE || data.type == ChatType::MONSTER_WHISPER ||
            data.type == ChatType::MONSTER_PARTY ||
            data.type == ChatType::RAID_BOSS_EMOTE || data.type == ChatType::RAID_BOSS_WHISPER)) {
        size_t pos = data.message.find("%s");
        while (pos != std::string::npos) {
            data.message.replace(pos, 2, data.senderName);
            pos = data.message.find("%s", pos + data.senderName.size());
        }
    }

    // Filter BG/Arena queue announcer spam (server-side modules on
    // ChromieCraft/AzerothCore). Common formats:
    //   |cffff0000[BG Queue Announcer]:|r ...
    //   |cffff0000[Arena Queue Announcer]:|r ...
    //   |cFFFFA500<player> joined : |cFF00FFFF2x2|r
    //   |cFFFFA500<player> exited |cFF00FFFF3x3|r
    // The third/fourth forms drop the "Queue Announcer" prefix entirely, so
    // we also detect the announcer-shaped pattern: a colored message that
    // names an arena/BG bracket suffix like "2x2|r" / "3v3|r".
    {
        const auto& msg = data.message;
        auto containsCI = [&](const char* needle) {
            const size_t nlen = std::strlen(needle);
            if (msg.size() < nlen) return false;
            const size_t last = msg.size() - nlen;
            for (size_t i = 0; i <= last; ++i) {
                bool match = true;
                for (size_t j = 0; j < nlen; ++j) {
                    unsigned char a = static_cast<unsigned char>(msg[i + j]);
                    unsigned char b = static_cast<unsigned char>(needle[j]);
                    if (std::tolower(a) != std::tolower(b)) { match = false; break; }
                }
                if (match) return true;
            }
            return false;
        };
        if (containsCI("queue announcer") || containsCI("queue status")) {
            return;
        }
        // Pattern-based catch for prefix-less variants. Require the message to
        // contain a color code (server-formatted) AND an arena/BG bracket token
        // immediately followed by |r AND a verb word ("joined", "exited",
        // "left", "entered"). Plain player chat won't hit all three.
        const bool hasColor = msg.find("|c") != std::string::npos;
        if (hasColor) {
            static const char* kBracketTokens[] = {
                "2x2|r", "3x3|r", "5x5|r",
                "2v2|r", "3v3|r", "5v5|r",
                "2X2|r", "3X3|r", "5X5|r",
                "2V2|r", "3V3|r", "5V5|r",
            };
            bool hasBracket = false;
            for (const char* t : kBracketTokens) {
                if (msg.find(t) != std::string::npos) { hasBracket = true; break; }
            }
            if (hasBracket && (containsCI("joined") || containsCI("exited") ||
                               containsCI(" left ") || containsCI("entered") ||
                               containsCI("queue"))) {
                return;
            }
        }
    }

    // Filter officer chat if player doesn't have officer chat permission.
    // Some servers send officer chat to all guild members regardless of rank.
    // WoW guild right bit 0x40 = GR_RIGHT_OFFCHATSPEAK, 0x80 = GR_RIGHT_OFFCHATLISTEN
    if (data.type == ChatType::OFFICER) {
        // Through the shared lookup, which answers zero rights when the roster
        // has not arrived - the same "say nothing about it" this had before,
        // since the test below only hides chat when a rank is known and lacks
        // the bit.
        const uint32_t idx = owner_.getPlayerGuildRankIndex();
        if (idx != 0xFFFFFFFFu && idx < owner_.getGuildRoster().ranks.size()) {
            if (!(owner_.getPlayerGuildRankRights() & 0x80)) { // GR_RIGHT_OFFCHATLISTEN
                return; // Don't show officer chat to non-officers
            }
        }
    }

    // Addon whispers are told apart by their envelope below (LANG_ADDON or the
    // prefix<TAB>payload form, decodeAddonChatPayload), not by what the text
    // starts with: a prefix list here dropped any player whisper that began
    // "oRA", "AVR", "GS_", "DVNE" or "BWVQ", or with a non-ASCII letter.

    // An NPC's line keeps its $-tokens. Build 8606 prints ".npc say" text with
    // "$N", "$c" and "$gm:f;" in it exactly as sent - in the chat frame and in
    // CHAT_MSG_MONSTER_SAY/YELL/EMOTE/WHISPER's arg1 - whoever the receiver is
    // (_experiments/20261118T203000Z.text_tokens). Its chat frame fills the
    // "%s" with the speaker; that is FrameXML's.

    // Add to chat history
    data.uid = ++chatUidCounter_;
    chatHistory_.push_back(data);
    if (chatHistory_.size() > maxChatHistory_) {
        chatHistory_.erase(chatHistory_.begin());
    }
    logChatMessage(data, "server");
    // No fireChatEvent here. This function announces the message itself,
    // further down and with the fuller argument list - the channel number and
    // short name, the line id, the sender's guid. Calling fireChatEvent as
    // well fired CHAT_MSG_* twice for every line the server sent, and the
    // interface drew both: every whisper, say, guild and channel message
    // appeared in the window doubled.
    //
    // The note that stood here said the event was fired only on the local
    // path, which was true when it was written and stopped being true when
    // this function grew its own. fireChatEvent stays for
    // addLocalChatMessage's callers, which have no richer arguments to give.

    // Track whisper sender for /r command
    if (data.type == ChatType::WHISPER) {
        // Always store GUID so getLastWhisperSender() can resolve the name
        // from the player name cache even if name wasn't available yet
        if (data.senderGuid != 0)
            owner_.lastWhisperSenderGuidRef() = data.senderGuid;
        if (!data.senderName.empty())
            owner_.lastWhisperSenderRef() = data.senderName;

        // No reply from here while away: the realm answers a whisper to an
        // AFK or DND player itself (Player::Whisper sends autoReplyMsg back as
        // CHAT_MSG_AFK/DND), so a client reply was a second, invented one.
    }

    // The Social panel's two filters, before anything shows this line: the
    // bubble below, the CHAT_MSG_ event, and the log all take the text from
    // here, so filtering at one point covers all three.
    //
    // Only what a player typed. A quest giver, a boss emote or a system notice
    // is not somebody spamming, and masking words in them would edit the
    // game's own writing.
    const bool fromAPlayer =
        data.type == ChatType::SAY || data.type == ChatType::YELL ||
        data.type == ChatType::PARTY || data.type == ChatType::RAID ||
        data.type == ChatType::GUILD || data.type == ChatType::OFFICER ||
        data.type == ChatType::WHISPER || data.type == ChatType::EMOTE ||
        data.type == ChatType::CHANNEL;

    // The player's own lines are what was typed, echoed by the server: the
    // filter is for somebody else repeating themselves.
    const bool ownLine = data.senderGuid == owner_.getPlayerGuid();

    if (fromAPlayer && !ownLine) {
        // Disable Spam Filter, so the CVar being on means filtering happens.
        if (addons::storedCVarValue("spamFilter", "1") != "0") {
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (repeatsRecentLine(recentChatLines_, data.senderGuid, data.message, now)) {
                return;
            }
            recentChatLines_.push_back({.senderGuid = data.senderGuid, .text = data.message, .at = now});
            // A short memory is the point: this is looking for a line pasted
            // again a moment later, not keeping a record of the conversation.
            while (recentChatLines_.size() > 64) recentChatLines_.pop_front();
        }
    }

    emitChatBubble(data);

    // Log the message
    std::string senderInfo;
    if (!data.senderName.empty()) {
        senderInfo = data.senderName;
    } else if (data.senderGuid != 0) {
        senderInfo = "Unknown-" + std::to_string(data.senderGuid);
    } else {
        senderInfo = "System";
    }

    std::string channelInfo;
    if (!data.channelName.empty()) {
        channelInfo = "[" + data.channelName + "] ";
    }

    LOG_DEBUG("[", getChatTypeString(data.type), "] ", channelInfo, senderInfo, ": ", data.message);

    // Fire CHAT_MSG_* addon events
    if (owner_.addonEventCallbackRef()) {
        std::string eventName = "CHAT_MSG_";
        eventName += getChatTypeString(data.type);
        // The language's *name*, not its id. ChatFrame_MessageEventHandler
        // does
        //
        //     if strlen(arg3) > 0 and arg3 ~= "Universal"
        //                          and arg3 ~= self.defaultLanguage then
        //         languageHeader = "["..arg3.."] "
        //
        // and GetDefaultLanguage answers "Common" or "Orcish". A number never
        // matches either, so every line in the player's own language was
        // printed with its id in front of it - guild chat in Common came out
        // as "[7] Name: hello". Universal is the id the file does not carry
        // and the word the test above looks for.
        // A system line carries no language: 2.4.3 hands CHAT_MSG_SYSTEM an
        // empty arg3 ("Edrick added to friends.", "DB table ... reloaded.",
        // pair 11), where the packet's language is Universal.
        const bool noLanguage = data.type == ChatType::SYSTEM ||
            data.type == ChatType::BG_SYSTEM_NEUTRAL ||
            data.type == ChatType::BG_SYSTEM_ALLIANCE ||
            data.type == ChatType::BG_SYSTEM_HORDE;
        const std::string lang = noLanguage
            ? std::string() : owner_.getLanguageName(static_cast<uint32_t>(data.language));
        char guidBuf[32];
        snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)data.senderGuid);
        // arg8 is the channel's number: the handler builds "CHANNEL"..arg8
        // and looks it up in ChatTypeInfo, so a zero there (CHANNEL0, which no
        // table has) drops the line before it reaches the window.
        // A line id of its own, remembered against the sender. FrameXML builds
        // this into the player link on the name - "|Hplayer:Name:lineId:..." -
        // so a right-click on that name hands it back and the client is
        // expected to know which message it was. Zero for every line is one id
        // shared by all of them, which is what made Report Spam unofferable:
        // CanComplainChat is asked about the id before the entry is shown.
        const uint32_t lineId = nextChatLineId_++;
        if (data.senderGuid != 0) {
            chatLineSenders_.emplace_back(lineId, data.senderGuid);
            // Recent lines only. A session's chat has no end, and a message old
            // enough to have fallen out of this is old enough not to be
            // reportable.
            constexpr size_t kRememberedLines = 512;
            while (chatLineSenders_.size() > kRememberedLines)
                chatLineSenders_.pop_front();
        }
        // 2.4.3 on a General line (pair 15): arg4 "1. General - Tanaris",
        // arg7 1 (the ChatChannels id, 0 for a custom channel), arg8 1, arg9
        // "General - Tanaris". ChatFrame prints arg4 up to " - " as the
        // "[1. General]" in front of the line, and keeps it when its arg7 is
        // the window's zone channel id or arg9 its channel name.
        const int channelNumber = getChannelIndex(data.channelName);
        const std::string numberedChannel = channelNumber > 0
            ? std::to_string(channelNumber) + ". " + data.channelName : data.channelName;
        // arg5 is the *target*, not the sender. It was the sender's own name,
        // which is only read by CHANNEL_NOTICE_USER - and read as "there are
        // two names in this notice", so every kick and ban was formatted as
        // though someone had done it to someone else. receiverName is what the
        // packet carries for it, empty on an ordinary message.
        //
        // arg6 is the flag beside the name: FrameXML looks up CHAT_FLAG_<arg6>
        // and prints <Away>, <Busy> or <GM>. The tag is on the packet and was
        // being dropped, so nothing ever showed as away or as a game master.
        // AzerothCore's values are a bitmask - AFK 0x01, DND 0x02, GM 0x04,
        // COM 0x08, DEV 0x10 - and the interface takes one word, so the most
        // significant is the one to name.
        const char* chatFlag = (data.chatTag & 0x04) ? "GM"
                             : (data.chatTag & 0x10) ? "DEV"
                             : (data.chatTag & 0x02) ? "DND"
                             : (data.chatTag & 0x01) ? "AFK"
                                                     : "";
        // Says a line reached the interface at all. A blank chat window is
        // either nothing arriving or something arriving and not being drawn,
        // and those have opposite causes with the same appearance.
        //
        // Once per event name, not once every two seconds: what is being
        // asked is whether a SAY, a YELL, a WHISPER reaches the interface,
        // and the second SAY answers nothing the first did. Rate limiting it
        // wrote one of these every two seconds for as long as anyone in the
        // zone was talking.
        {
            static std::set<std::string> saidFor;
            if (saidFor.size() < 32 && saidFor.insert(eventName).second) {
                LOG_WARNING("Chat: fired ", eventName, " to the interface from '",
                            data.senderName, "' - said once per event name");
            }
        }
        // arg2 is the name the interface prints, and for an outgoing whisper
        // that is the person written to rather than the one writing:
        // CHAT_WHISPER_INFORM_GET is "To %s: " and takes the same argument
        // CHAT_WHISPER_GET does. Passing the sender there addressed every
        // whisper the player sent to the player themselves.
        const std::string& shownName =
            (data.type == ChatType::WHISPER_INFORM && !data.receiverName.empty())
                ? data.receiverName
                : data.senderName;
        owner_.addonEventCallbackRef()(eventName, {
            data.message,
            shownName,
            lang,
            numberedChannel,
            data.receiverName,
            chatFlag,
            std::to_string(joinedChannelId(data.channelName)),
            std::to_string(channelNumber),
            data.channelName,
            "0",
            std::to_string(lineId),
            guidBuf
        });
    }
}

void ChatHandler::sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = TextEmotePacket::build(textEmoteId, targetGuid);
    owner_.getSocket()->send(packet);
}

std::string ChatHandler::knownPlayerName(uint64_t guid) const {
    if (guid == 0) return {};
    if (const std::string& name = owner_.lookupName(guid); !name.empty()) return name;
    for (const auto& member : owner_.getPartyData().members) {
        if (member.guid == guid && !member.name.empty()) return member.name;
    }
    for (const auto& member : owner_.getGuildRoster().members) {
        if (member.guid == guid && !member.name.empty()) return member.name;
    }
    return {};
}

void ChatHandler::flushChatAwaitingName(uint64_t guid) {
    if (chatAwaitingName_.empty() || guid == 0) return;
    std::vector<MessageChatData> ready;
    for (auto it = chatAwaitingName_.begin(); it != chatAwaitingName_.end();) {
        if (it->guid == guid) {
            ready.push_back(std::move(it->data));
            it = chatAwaitingName_.erase(it);
        } else {
            ++it;
        }
    }
    // Delivered outside the walk: each one runs the whole chat path, which can
    // hold another line of its own.
    for (auto& data : ready) deliverChatMessage(std::move(data), /*alreadyWaited=*/true);
    flushNoticesAwaitingName(guid, false);
}

void ChatHandler::flushNoticesAwaitingName(uint64_t guid, bool expired) {
    if (noticesAwaitingName_.empty()) return;
    const double now = core::appTimeSeconds();
    std::vector<ChannelNoticeAwaitingName> ready;
    for (auto it = noticesAwaitingName_.begin(); it != noticesAwaitingName_.end();) {
        if (expired ? now >= it->deadline : it->guid == guid) {
            if (expired) LOG_WARNING("Channel notice: no name for ", formatChatLogGuid(it->guid),
                                     " within ", kNameWaitSeconds, "s - showing it without one");
            ready.push_back(std::move(*it));
            it = noticesAwaitingName_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& n : ready) fireChannelNotice(n.data, n.number, true, false);
}

void ChatHandler::expireChatAwaitingName() {
    flushNoticesAwaitingName(0, true);
    if (chatAwaitingName_.empty()) return;
    const double now = core::appTimeSeconds();
    std::vector<MessageChatData> ready;
    for (auto it = chatAwaitingName_.begin(); it != chatAwaitingName_.end();) {
        if (now >= it->deadline) {
            LOG_WARNING("Chat: no name for ", formatChatLogGuid(it->guid),
                        " within ", kNameWaitSeconds, "s - showing the line without one");
            ready.push_back(std::move(it->data));
            it = chatAwaitingName_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& data : ready) deliverChatMessage(std::move(data), /*alreadyWaited=*/true);
}

void ChatHandler::handleTextEmote(network::Packet& packet) {
    const bool legacyFormat = isClassicLikeExpansion();
    TextEmoteData data;
    if (!TextEmoteParser::parse(packet, data, legacyFormat)) {
        LOG_WARNING("Failed to parse SMSG_TEXT_EMOTE");
        return;
    }

    if (data.senderGuid == owner_.getPlayerGuid() && data.senderGuid != 0) {
        return;
    }

    std::string senderName = owner_.lookupName(data.senderGuid);
    // SMSG_TEXT_EMOTE is the narrated chat line. The server sends the actual
    // visual separately in SMSG_EMOTE; replaying an animation here restarts
    // one-shots and replaces correctly resolved STATE_* loops such as /dance.
    if (senderName.empty()) {
        const auto& partyData = owner_.getPartyData();
        for (const auto& member : partyData.members) {
            if (member.guid == data.senderGuid && !member.name.empty()) {
                senderName = member.name;
                break;
            }
        }
    }

    if (senderName.empty()) {
        owner_.queryPlayerName(data.senderGuid);
        LOG_DEBUG("Deferred chat text for unresolved SMSG_TEXT_EMOTE sender=0x",
                  std::hex, data.senderGuid, std::dec,
                  " emoteId=", data.textEmoteId);
        return;
    }

    const std::string* targetPtr = data.targetName.empty() ? nullptr : &data.targetName;
    std::string emoteText = rendering::AnimationController::getEmoteTextByDbcId(data.textEmoteId, senderName, targetPtr);
    if (emoteText.empty()) {
        emoteText = data.targetName.empty()
            ? senderName + " performs an emote."
            : senderName + " performs an emote at " + data.targetName + ".";
    }

    MessageChatData chatMsg;
    chatMsg.type = ChatType::TEXT_EMOTE;
    chatMsg.language = ChatLanguage::COMMON;
    chatMsg.senderGuid = data.senderGuid;
    chatMsg.senderName = senderName;
    chatMsg.message = emoteText;

    addLocalChatMessage(chatMsg);

    LOG_INFO("TEXT_EMOTE from ", senderName, " (emoteId=", data.textEmoteId, ")");
}

void ChatHandler::joinChannel(const std::string& channelName, const std::string& password, uint32_t channelId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildJoinChannel(channelName, password, channelId)
        : JoinChannelPacket::build(channelName, password);
    owner_.getSocket()->send(packet);
    LOG_INFO("Requesting to join channel: ", channelName);
}

void ChatHandler::requestChannelList(const std::string& channelName, bool print) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (print) printChannelLists_.insert(channelName);
    // The channel name and nothing else. SMSG_CHANNEL_LIST was parsed all
    // along and nothing ever asked for one, so the roster only existed in
    // theory.
    network::Packet packet(wireOpcode(Opcode::CMSG_CHANNEL_LIST));
    packet.writeString(channelName);
    owner_.getSocket()->send(packet);
    LOG_INFO("Requesting member list for channel: ", channelName);
}

void ChatHandler::leaveChannel(const std::string& channelName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildLeaveChannel(channelName)
        : LeaveChannelPacket::build(channelName);
    owner_.getSocket()->send(packet);
    LOG_INFO("Requesting to leave channel: ", channelName);
}

void ChatHandler::takeChannelNumber(const std::string& channelName) {
    if (std::find(joinedChannels_.begin(), joinedChannels_.end(), channelName) != joinedChannels_.end()) return;
    const auto free = std::find(joinedChannels_.begin(), joinedChannels_.end(), std::string());
    if (free != joinedChannels_.end()) *free = channelName;
    else joinedChannels_.push_back(channelName);
}

std::string ChatHandler::getChannelByIndex(int index) const {
    if (index < 1 || index > static_cast<int>(joinedChannels_.size())) return "";
    return joinedChannels_[index - 1];
}

int ChatHandler::getChannelIndex(const std::string& channelName) const {
    if (channelName.empty()) return 0;
    for (int i = 0; i < static_cast<int>(joinedChannels_.size()); ++i) {
        if (joinedChannels_[i] == channelName) return i + 1;
    }
    return 0;
}

const std::vector<ChatHandler::BuiltinChannel>& ChatHandler::builtinChannels() const {
    if (!builtinChannels_.empty()) return builtinChannels_;
    auto* am = owner_.services().assetManager;
    auto dbc = am ? am->loadDBC("ChatChannels.dbc") : nullptr;
    assert(dbc && dbc->isLoaded() && dbc->getFieldCount() >= 21 && "ChatChannels.dbc unreadable");
    // ID, flags, faction group, the pattern in 16 locales plus a mask
    // (column 3 on), then the name in the same shape: column 20.
    constexpr uint32_t kPatternColumn = 3;
    constexpr uint32_t kNameColumn = 3 + 17;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        builtinChannels_.push_back({dbc->getUInt32(i, 0), dbc->getUInt32(i, 1),
                                    dbc->getString(i, kPatternColumn), dbc->getString(i, kNameColumn)});
    }
    return builtinChannels_;
}

std::string ChatHandler::builtinChannelName(uint32_t id) const {
    if (id == 0) return {};
    for (const auto& channel : builtinChannels())
        if (channel.id == id) return channel.name;
    return {};
}

uint32_t ChatHandler::builtinChannelFlags(uint32_t id) const {
    for (const auto& channel : builtinChannels())
        if (channel.id == id) return channel.flags;
    return 0;
}

uint32_t ChatHandler::builtinChannelId(const std::string& shortName) const {
    for (const auto& channel : builtinChannels())
        if (channel.name == shortName) return channel.id;
    return 0;
}

std::vector<ChatHandler::ChannelDisplayRow> ChatHandler::channelDisplayRows() const {
    // Measured on 2.4.3 in Stormwind with five channels joined (pair 11):
    // Group, World, 1. General, 2. Trade, 3. LocalDefense, 4. LookingForGroup,
    // Custom, 5. socialaudit - each half in chat number order, the built-in
    // ones by their ChatChannels name without the zone.
    std::vector<ChannelDisplayRow> world, custom;
    for (size_t i = 0; i < joinedChannels_.size(); ++i) {
        const std::string& name = joinedChannels_[i];
        if (name.empty()) continue;
        const auto id = joinedChannelIds_.find(name);
        const uint32_t dbcId = id == joinedChannelIds_.end() ? 0 : id->second;
        ChannelDisplayRow row;
        row.channel = name;
        row.number = static_cast<int>(i) + 1;
        if (dbcId != 0) {
            row.label = builtinChannelName(dbcId);
            row.category = "CHANNEL_CATEGORY_WORLD";
            world.push_back(std::move(row));
        } else {
            row.label = name;
            row.category = "CHANNEL_CATEGORY_CUSTOM";
            custom.push_back(std::move(row));
        }
    }
    std::vector<ChannelDisplayRow> rows;
    auto section = [&](const char* label, std::vector<ChannelDisplayRow>& children) {
        ChannelDisplayRow header;
        header.label = label;
        header.header = true;
        header.collapsed = collapsedChannelHeaders_.count(label) != 0;
        header.children = static_cast<int>(children.size());
        rows.push_back(header);
        if (header.collapsed) return;
        for (auto& c : children) rows.push_back(std::move(c));
    };
    // In a group its own channel sits under Group: "Raid (2)" in a raid of
    // two, with no chat number (pair 11), "Party" in a party.
    std::vector<ChannelDisplayRow> group;
    const auto& party = owner_.getPartyData();
    if (!party.members.empty()) {
        ChannelDisplayRow row;
        row.label = groupIsRaid(party.groupType) ? "Raid" : "Party";
        row.channel = kGroupChannel;
        row.category = "CHANNEL_CATEGORY_GROUP";
        group.push_back(std::move(row));
    }
    section("Group", group);
    section("World", world);
    section("Custom", custom);
    return rows;
}

int ChatHandler::channelDisplayIndex(const std::string& channelName) const {
    const auto rows = channelDisplayRows();
    for (size_t i = 0; i < rows.size(); ++i)
        if (!rows[i].header && rows[i].channel == channelName) return static_cast<int>(i) + 1;
    return 0;
}

void ChatHandler::setChannelHeaderCollapsed(int displayIndex, bool collapsed) {
    const auto rows = channelDisplayRows();
    if (displayIndex < 1 || displayIndex > static_cast<int>(rows.size())) return;
    const auto& row = rows[static_cast<size_t>(displayIndex) - 1];
    if (!row.header) return;
    if (collapsed) collapsedChannelHeaders_.insert(row.label);
    else collapsedChannelHeaders_.erase(row.label);
}

std::string ChatHandler::resolveChannelName(const std::string& name) const {
    for (const auto& joined : joinedChannels_)
        if (joined == name) return joined;
    for (const auto& [joined, id] : joinedChannelIds_)
        if (id != 0 && builtinChannelName(id) == name) return joined;
    return {};
}

std::vector<ChannelMember> ChatHandler::channelPanelRoster(const std::string& channel) const {
    if (channel != kGroupChannel) return getChannelRoster(channel);
    std::vector<ChannelMember> roster;
    const auto& party = owner_.getPartyData();
    if (party.members.empty()) return roster;
    auto add = [&](uint64_t guid, const std::string& name) {
        ChannelMember m;
        m.guid = guid;
        m.name = name;
        m.owner = m.moderator = guid == party.leaderGuid;
        roster.push_back(std::move(m));
    };
    if (const auto* self = owner_.getActiveCharacter()) add(owner_.getPlayerGuid(), self->name);
    for (const auto& member : party.members) add(member.guid, member.name);
    std::stable_sort(roster.begin(), roster.end(),
                     [](const ChannelMember& a, const ChannelMember& b) { return a.name < b.name; });
    return roster;
}

std::optional<uint32_t> ChatHandler::channelMemberCount(const std::string& channelName) const {
    if (channelName == kGroupChannel)
        return static_cast<uint32_t>(channelPanelRoster(channelName).size());
    const auto it = channelMemberCounts_.find(channelName);
    if (it == channelMemberCounts_.end()) return std::nullopt;
    return it->second;
}

void ChatHandler::requestChannelMemberCounts() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    for (const auto& name : joinedChannels_) {
        if (name.empty()) continue;
        network::Packet packet(wireOpcode(Opcode::CMSG_GET_CHANNEL_MEMBER_COUNT));
        packet.writeString(name);
        owner_.getSocket()->send(packet);
    }
}

void ChatHandler::requestChannelDisplayList(const std::string& channelName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_CHANNEL_DISPLAY_LIST));
    packet.writeString(channelName);
    owner_.getSocket()->send(packet);
}

void ChatHandler::onPlayerName(uint64_t guid, const std::string& name) {
    for (auto& [channel, roster] : channelRosters_) {
        bool named = false;
        for (auto& m : roster) {
            if (m.guid != guid || m.name == name) continue;
            m.name = name;
            named = true;
        }
        if (!named) continue;
        std::stable_sort(roster.begin(), roster.end(),
                         [](const ChannelMember& a, const ChannelMember& b) { return a.name < b.name; });
        if (const int row = channelDisplayIndex(channel); row > 0)
            owner_.fireAddonEvent("CHANNEL_ROSTER_UPDATE", {std::to_string(row), std::to_string(roster.size())});
    }
}

/// SMSG_CHANNEL_MEMBER_COUNT: name, flags, count (CMaNGOS
/// HandleGetChannelMemberCountOpcode). 2.4.3 fires CHANNEL_COUNT_UPDATE with
/// the channel's panel row and the count (pair 11: 6, 27 for LookingForGroup).
void ChatHandler::handleChannelMemberCount(network::Packet& packet) {
    const std::string name = packet.readString();
    if (!packet.hasRemaining(5)) return;
    packet.readUInt8();   // the channel's flags
    const uint32_t count = packet.readUInt32();
    channelMemberCounts_[name] = count;
    if (const int row = channelDisplayIndex(name); row > 0)
        owner_.fireAddonEvent("CHANNEL_COUNT_UPDATE", {std::to_string(row), std::to_string(count)});
}

namespace {

// The notice's name as FrameXML reads it: ChatFrame prints
// format(CHAT_<name>_NOTICE, ...) for CHAT_MSG_CHANNEL_NOTICE(_USER).
// nullptr: no line (JOINED/LEFT of others, MODE_CHANGE, voice in TBC).
struct ChannelNotice { const char* name; bool user; };
ChannelNotice channelNotice(ChannelNotifyType type) {
    using T = ChannelNotifyType;
    switch (type) {
        case T::YOU_JOINED:            return {"YOU_JOINED", false};
        case T::YOU_LEFT:              return {"YOU_LEFT", false};
        case T::WRONG_PASSWORD:        return {"WRONG_PASSWORD", false};
        case T::NOT_MEMBER:            return {"NOT_MEMBER", false};
        case T::NOT_MODERATOR:         return {"NOT_MODERATOR", false};
        case T::PASSWORD_CHANGED:      return {"PASSWORD_CHANGED", true};
        case T::OWNER_CHANGED:         return {"OWNER_CHANGED", true};
        case T::PLAYER_NOT_FOUND:      return {"PLAYER_NOT_FOUND", true};
        case T::NOT_OWNER:             return {"NOT_OWNER", false};
        case T::CHANNEL_OWNER:         return {"CHANNEL_OWNER", true};
        case T::ANNOUNCEMENTS_ON:      return {"ANNOUNCEMENTS_ON", true};
        case T::ANNOUNCEMENTS_OFF:     return {"ANNOUNCEMENTS_OFF", true};
        case T::MODERATION_ON:         return {"MODERATION_ON", true};
        case T::MODERATION_OFF:        return {"MODERATION_OFF", true};
        case T::MUTED:                 return {"MUTED", false};
        case T::PLAYER_KICKED:         return {"PLAYER_KICKED", true};
        case T::BANNED:                return {"BANNED", false};
        case T::PLAYER_BANNED:         return {"PLAYER_BANNED", true};
        case T::PLAYER_UNBANNED:       return {"PLAYER_UNBANNED", true};
        case T::PLAYER_NOT_BANNED:     return {"PLAYER_NOT_BANNED", true};
        case T::PLAYER_ALREADY_MEMBER: return {"PLAYER_ALREADY_MEMBER", true};
        case T::INVITE:                return {"INVITE", true};
        case T::INVITE_WRONG_FACTION:  return {"INVITE_WRONG_FACTION", false};
        case T::WRONG_FACTION:         return {"WRONG_FACTION", false};
        case T::INVALID_NAME:          return {"INVALID_NAME", false};
        case T::NOT_MODERATED:         return {"NOT_MODERATED", false};
        case T::PLAYER_INVITED:        return {"PLAYER_INVITED", true};
        case T::PLAYER_INVITE_BANNED:  return {"PLAYER_INVITE_BANNED", true};
        case T::THROTTLED:             return {"THROTTLED", false};
        case T::NOT_IN_AREA:           return {"NOT_IN_AREA", false};
        case T::NOT_IN_LFG:            return {"NOT_IN_LFG", false};
        default:                       return {nullptr, false};
    }
}

} // namespace

// Channel notices are the interface's to word, as in 2.4.3: the client fires
// CHAT_MSG_CHANNEL_NOTICE (or _USER when a player is named) with the notice
// name in arg1, "N. Channel" in arg4, the player in arg2 and a second one in
// arg5, and ChatFrame formats CHAT_<name>_NOTICE - "Joined Channel: [1.
// General - Elwynn Forest]" (pair 6, native at login). arg7 is the
// ChatChannels id, arg8 the channel number and arg9 the full name ("General -
// Tanaris", pair 15); ChatFrame keeps the line when arg7 is one of the
// window's zone channel ids or arg9 one of its channel names.
void ChatHandler::fireChannelNotice(const ChannelNotifyData& data, int number, bool waited, bool changed) {
    ChannelNotice notice = channelNotice(data.notifyType);
    if (changed) notice.name = "YOU_CHANGED";
    if (!notice.name) {
        LOG_DEBUG("Channel notify type ", static_cast<int>(data.notifyType), " for channel ", data.channelName);
        return;
    }
    if (!owner_.addonEventCallbackRef()) return;
    auto name = [this](uint64_t guid) { return guid ? knownPlayerName(guid) : std::string{}; };
    // A player named only by guid ("Owner changed to %s") waits for the name
    // query, as a chat line from a stranger does: printed at once it read
    // "[1. General] Owner changed to ."
    if (!waited) {
        for (const uint64_t guid : {data.senderGuid, data.sourceGuid}) {
            if (guid == 0 || !data.playerName.empty() || !name(guid).empty()) continue;
            owner_.queryPlayerName(guid);
            noticesAwaitingName_.push_back({data, number, guid, core::appTimeSeconds() + kNameWaitSeconds});
            return;
        }
    }
    std::string player = data.playerName.empty() ? name(data.senderGuid) : data.playerName;
    std::string second = name(data.sourceGuid);
    if (notice.user && player.empty())
        LOG_WARNING("Channel notice ", notice.name, " on ", data.channelName, ": player name unknown (guid ",
                    data.senderGuid, ")");
    const std::string shown = number > 0 ? std::to_string(number) + ". " + data.channelName : data.channelName;
    // The realm names the id only in YOU_JOINED and YOU_LEFT; any other
    // notice is about a channel the client holds, with the id it joined under.
    const uint32_t channelId = data.channelId != 0 ? data.channelId : joinedChannelId(data.channelName);
    owner_.addonEventCallbackRef()(notice.user ? "CHAT_MSG_CHANNEL_NOTICE_USER" : "CHAT_MSG_CHANNEL_NOTICE", {
        notice.name, player, "", shown, second, "", std::to_string(channelId),
        std::to_string(number), data.channelName, "0", "0", "0x0000000000000000"
    });
}

void ChatHandler::handleChannelNotify(network::Packet& packet) {
    ChannelNotifyData data;
    if (!ChannelNotifyParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_CHANNEL_NOTIFY");
        return;
    }

    switch (data.notifyType) {
        case ChannelNotifyType::YOU_JOINED: {
            // A built-in channel with the id of one already held is the zone
            // moving it: it takes the old one's number and the original says
            // "Changed Channel: [1. General - Stormwind City]" (pair 6); the
            // realm sends nothing for the one it replaces.
            bool changed = false;
            if (data.channelId != 0) {
                for (auto& name : joinedChannels_) {
                    const auto it = joinedChannelIds_.find(name);
                    if (it == joinedChannelIds_.end() || it->second != data.channelId || name == data.channelName)
                        continue;
                    ownedChannels_.erase(name);
                    joinedChannelIds_.erase(it);
                    name = data.channelName;
                    changed = true;
                    break;
                }
            }
            if (!changed) takeChannelNumber(data.channelName);
            joinedChannelIds_[data.channelName] = data.channelId;
            LOG_INFO(changed ? "Changed channel: " : "Joined channel: ", data.channelName);
            // The channel panel's rows moved; it redraws on this. 2.4.3 fires
            // it before the notice (pair 15).
            owner_.fireAddonEvent("CHANNEL_UI_UPDATE", {});
            fireChannelNotice(data, getChannelIndex(data.channelName), false, changed);
            break;
        }
        case ChannelNotifyType::YOU_LEFT: {
            // Numbered as it was, before it goes from the list.
            const int number = getChannelIndex(data.channelName);
            ownedChannels_.erase(data.channelName);
            joinedChannelIds_.erase(data.channelName);
            // Its number stays free, not closed up (see getJoinedChannels).
            std::replace(joinedChannels_.begin(), joinedChannels_.end(), data.channelName, std::string());
            while (!joinedChannels_.empty() && joinedChannels_.back().empty()) joinedChannels_.pop_back();
            LOG_INFO("Left channel: ", data.channelName);
            // 2.4.3: CHANNEL_UI_UPDATE, the notice, CHANNEL_UI_UPDATE (pair 15).
            owner_.fireAddonEvent("CHANNEL_UI_UPDATE", {});
            fireChannelNotice(data, number);
            owner_.fireAddonEvent("CHANNEL_UI_UPDATE", {});
            break;
        }
        case ChannelNotifyType::PLAYER_ALREADY_MEMBER:
            // Server confirms we're in this channel but our local list doesn't have it yet -
            // can happen after reconnect or if the join notification was missed.
            if (std::find(joinedChannels_.begin(), joinedChannels_.end(), data.channelName) == joinedChannels_.end()) {
                takeChannelNumber(data.channelName);
                LOG_INFO("Already in channel: ", data.channelName);
            }
            fireChannelNotice(data, getChannelIndex(data.channelName));
            break;
        case ChannelNotifyType::OWNER_CHANGED:
            // The guid beside the name is the new owner's -
            // Channel::MakeOwnerChanged writes _ownerGUID into it. Kept so the
            // unit menu can offer the moderator entries, which FrameXML hides
            // behind IsDisplayChannelOwner: the verbs behind them were built
            // and could not be reached.
            if (data.senderGuid != 0 && data.senderGuid == owner_.getPlayerGuid()) {
                ownedChannels_.insert(data.channelName);
            } else {
                ownedChannels_.erase(data.channelName);
            }
            fireChannelNotice(data, getChannelIndex(data.channelName));
            break;
        default:
            fireChannelNotice(data, getChannelIndex(data.channelName));
            break;
    }
}

void ChatHandler::autoJoinDefaultChannels() {
    // The built-in channels are named after the zone ("General - Elwynn
    // Forest"), which SMSG_INIT_WORLD_STATES names after world entry; they
    // are joined from there (joinZoneChannels).
    zoneChannelsPending_ = true;
    zoneChannelsInterfaceReady_ = false;
}

// After the interface loads, as 2.4.3's "Joined Channel" lines follow the
// login message of the day; before it they reach no chat frame.
void ChatHandler::onInterfaceReady(uint32_t liveZoneId) {
    zoneChannelsInterfaceReady_ = true;
    zoneChannelsLiveZone_ = liveZoneId;
    tryJoinZoneChannels();
}

// SMSG_INIT_WORLD_STATES can come before or after world entry's request;
// the zone it names is kept either way.
void ChatHandler::joinZoneChannels(uint32_t zoneId) {
    (void)zoneId;  // the same value GameHandler keeps; read there
    tryJoinZoneChannels();
}

uint32_t ChatHandler::channelZoneId() const {
    // The zone under the player, as the client's own map data has it; the
    // realm's SMSG_INIT_WORLD_STATES zone when that is not known yet. A quick
    // reconnect to a session still in the world gets no INIT_WORLD_STATES.
    return zoneChannelsLiveZone_ ? zoneChannelsLiveZone_ : owner_.getWorldStateZoneId();
}

std::string ChatHandler::builtinChannelJoinName(const BuiltinChannel& channel, uint32_t zoneId) const {
    if (channel.pattern.find("%s") == std::string::npos) return channel.pattern;
    const uint32_t zone = owner_.getAreaZoneId(zoneId);
    // A city-only channel (Trade, GuildRecruitment) joined in a capital is
    // named after AreaTable's "City" row (3459): 2.4.3 in Stormwind joins
    // "[2. Trade - City]" beside "[1. General - Stormwind City]" (pair 11),
    // in Shattrath "Trade - City" and "GuildRecruitment - City" (pair 15),
    // and in Goldshire "Trade - Elwynn Forest" (pair 6).
    constexpr uint32_t kAreaCity = 3459;
    std::string place = owner_.getAreaName(zone);
    if ((channel.flags & kChannelCityOnly) && owner_.isCapitalZone(zone)) {
        place = owner_.getAreaName(kAreaCity);
        assert(!place.empty() && "AreaTable has no City row");
    }
    if (place.empty()) return {};
    return formatGlobalString(channel.pattern, {place});
}

// SetGuildRecruitmentMode(1) on 2.4.3 in Tanaris joins "[4. GuildRecruitment
// - Tanaris]" (id 25, the lowest free number) and SetGuildRecruitmentMode(0)
// leaves it; neither touches the guildRecruitmentChannel CVar (pair 15).
void ChatHandler::joinBuiltinChannels(uint32_t flag) {
    const uint32_t zoneId = channelZoneId();
    for (const auto& channel : builtinChannels()) {
        if (!(channel.flags & flag)) continue;
        bool joined = false;
        for (const auto& [name, id] : joinedChannelIds_) joined = joined || id == channel.id;
        if (joined) continue;
        const std::string name = builtinChannelJoinName(channel, zoneId);
        if (name.empty()) {
            LOG_ERROR("No name for zone ", zoneId, ": channel ", channel.pattern, " not joined");
            continue;
        }
        joinChannel(name, "", channel.id);
    }
}

void ChatHandler::leaveBuiltinChannels(uint32_t flag) {
    std::vector<std::string> leaving;
    for (const auto& [name, id] : joinedChannelIds_) {
        for (const auto& channel : builtinChannels())
            if (channel.id == id && (channel.flags & flag)) leaving.push_back(name);
    }
    for (const auto& name : leaving) leaveChannel(name);
}

// 2.4.3 at login (pair 6, Goldshire): "Joined Channel: [1. General - Elwynn
// Forest]", "[2. Trade - Elwynn Forest]", "[3. LocalDefense - Elwynn
// Forest]", "[4. GuildRecruitment - Elwynn Forest]", "[5. LookingForGroup]";
// at a Blackwing Lair login "General - Blackwing Lair"; in Tanaris "4.
// GuildRecruitment - Tanaris" too (pair 15, not in a guild). ChatChannels.dbc
// supplies the rows: the id the realm keys a built-in channel on, flag 0x1
// for the ones joined at login (WorldDefense, without it, was not joined),
// and the name pattern, "%s" being the zone's AreaTable name.
// GuildRecruitment has no 0x1: it carries 0x20000 (not for guild members)
// and is joined when the guildRecruitmentChannel CVar is on (its default,
// GetCVarDefault "1" on 2.4.3) and the character is in no guild. The realm
// moves zone channels on a zone change itself (Player::UpdateLocalChannels).
void ChatHandler::tryJoinZoneChannels() {
    const uint32_t zoneId = channelZoneId();
    LOG_INFO("Zone channels: pending=", zoneChannelsPending_, " zone=", zoneId,
             " interface=", zoneChannelsInterfaceReady_);
    if (!zoneChannelsPending_ || zoneId == 0 || !zoneChannelsInterfaceReady_) return;
    zoneChannelsPending_ = false;
    LOG_INFO("Joining built-in channels for zone ", zoneId, " '",
             owner_.getAreaName(owner_.getAreaZoneId(zoneId)), "'");
    for (const auto& channel : builtinChannels()) {
        bool wanted = (channel.flags & kChannelInitial) != 0;
        if (channel.flags & kChannelNoGuild)
            wanted = addons::storedCVarValue("guildRecruitmentChannel", "1") != "0" && !owner_.isInGuild();
        switch (channel.id) {
            case 1:  wanted = wanted && chatAutoJoin.general; break;
            case 2:  wanted = wanted && chatAutoJoin.trade; break;
            case 22: wanted = wanted && chatAutoJoin.localDefense; break;
            case 26: wanted = wanted && chatAutoJoin.lfg; break;
            default: break;
        }
        if (!wanted || channel.pattern.empty()) continue;
        const std::string name = builtinChannelJoinName(channel, zoneId);
        if (name.empty()) {
            LOG_ERROR("No name for zone ", zoneId, ": channel ", channel.pattern, " not joined");
            continue;
        }
        joinChannel(name, "", channel.id);
    }
}

void ChatHandler::emitChatBubble(const MessageChatData& data) {
    if (owner_.chatBubbleCallbackRef() && data.senderGuid != 0) {
        bool bubble = (data.type == ChatType::SAY || data.type == ChatType::YELL ||
                       data.type == ChatType::MONSTER_SAY || data.type == ChatType::MONSTER_YELL ||
                       data.type == ChatType::MONSTER_PARTY);
        // Party Chat Bubbles, a separate box beside the one for say and yell,
        // and read by nothing until now: party lines never floated over anyone
        // whichever way it was set. Off unless asked for, as it ships, since a
        // five-person group talking is a lot of text over the fight.
        if (!bubble && (data.type == ChatType::PARTY || data.type == ChatType::RAID)) {
            bubble = addons::storedCVarValue("chatBubblesParty", "0") != "0";
        }
        if (bubble) {
            bool isYell = (data.type == ChatType::YELL || data.type == ChatType::MONSTER_YELL);
            owner_.chatBubbleCallbackRef()(data.senderGuid, data.message, isYell);
        }
    }
}

void ChatHandler::addLocalChatMessage(const MessageChatData& msg) {
    chatHistory_.push_back(msg);
    chatHistory_.back().uid = ++chatUidCounter_;
    if (chatHistory_.size() > maxChatHistory_) {
        chatHistory_.pop_front();
    }
    logChatMessage(msg, "local");

    // A local line is a client notice; the player's own speech is not made
    // here but arrives from the server and is bubbled on that path.
    emitChatBubble(msg);
    fireChatEvent(msg);
}

void ChatHandler::fireChatEvent(const MessageChatData& msg) {
    if (!owner_.addonEventCallbackRef()) return;
    std::string eventName = "CHAT_MSG_";
    eventName += getChatTypeString(msg.type);
    const Character* ac = owner_.getActiveCharacter();
    // The client's own notices are nobody's: 2.4.3's CHAT_MSG_SYSTEM has an
    // empty arg2 and arg3 for "Edrikk added to friends." (pair 11), where
    // this sent the player's name and "Universal". So are the skill, loot and
    // trade skill lines: 2.4.3 fires CHAT_MSG_SKILL "Your skill in First Aid
    // has increased to 2.", CHAT_MSG_LOOT "You create: ..." and
    // CHAT_MSG_TRADESKILLS "You create Linen Bandage." with '' and '' (pair 8).
    const bool system = msg.type == ChatType::SYSTEM || msg.type == ChatType::SKILL ||
                        msg.type == ChatType::LOOT || msg.type == ChatType::TRADESKILLS;
    std::string senderName = system ? std::string{} : msg.senderName.empty()
        ? (ac ? ac->name : std::string{}) : msg.senderName;
    char guidBuf[32];
    snprintf(guidBuf, sizeof(guidBuf), "0x%016llX",
             (unsigned long long)(msg.senderGuid != 0 ? msg.senderGuid : owner_.getPlayerGuid()));
    // The name, for the reason handleMessageChat gives: a number here is
    // printed as a language header in front of every line.
    {
        // Once per event name, for the reason the remote path above gives.
        static std::set<std::string> saidForLocal;
        if (saidForLocal.size() < 32 && saidForLocal.insert(eventName).second) {
            LOG_WARNING("Chat: fired ", eventName, " to the interface (local)"
                        " - said once per event name");
        }
    }
    // The channel's index, which the chat frame builds "CHANNEL"..arg8 out of
    // and looks up in ChatTypeInfo - a zero there is CHANNEL0, which no table
    // has, and the line is dropped before it reaches the window. This is the
    // one thing the callback that used to announce these as well did better,
    // and it is here now so nothing was lost when that went.
    const int channelIndex = getChannelIndex(msg.channelName);
    // arg2 is the name the interface prints, and for a whisper the player
    // sent that is who it went to, not who wrote it: CHAT_WHISPER_INFORM_GET
    // is "To %s: " and reads the same argument CHAT_WHISPER_GET does. The
    // other announce path does the same for the server's WHISPER_INFORM.
    const std::string& shownName =
        (msg.type == ChatType::WHISPER_INFORM && !msg.receiverName.empty())
            ? msg.receiverName
            : senderName;
    owner_.addonEventCallbackRef()(eventName, {
        msg.message, shownName,
        system ? std::string() : owner_.getLanguageName(static_cast<uint32_t>(msg.language)),
        msg.channelName, msg.receiverName, "", "0", std::to_string(channelIndex),
        msg.channelName, "0", "0", guidBuf
    });
}

void ChatHandler::addLocalChatLine(ChatType type, const std::string& message) {
    if (message.empty()) return;
    MessageChatData msg;
    msg.type = type;
    msg.language = ChatLanguage::UNIVERSAL;
    msg.message = message;
    addLocalChatMessage(msg);
}

void ChatHandler::addSystemChatMessage(const std::string& message) {
    addLocalChatLine(ChatType::SYSTEM, message);
}

void ChatHandler::toggleAfk(const std::string& message) {
    sendChatMessage(ChatType::AFK, message, "");
}

void ChatHandler::toggleDnd(const std::string& message) {
    sendChatMessage(ChatType::DND, message, "");
}

// The away lines follow PLAYER_FLAGS, as 2.4.3's do (pair 6, Edrikf):
//   /afk          "You are now AFK: Away from Keyboard"   MARKED_AFK_MESSAGE
//   /afk again    "You are no longer AFK."                CLEARED_AFK
//   /dnd          "You are now DND: Do not Disturb."      MARKED_DND
//   /dnd again    "You are no longer marked DND."         CLEARED_DND
//   AFK -> /dnd   "You are no longer AFK." then "You are now DND: ..."
//   DND -> /afk   "You are now AFK: ..." only - no CLEARED_DND
//   /afk x while AFK with a message: the realm only replaces the reply text,
//   the flags do not move and nothing is printed.
// Without a message of its own the line names DEFAULT_AFK_MESSAGE or
// DEFAULT_DND_MESSAGE, the text the realm answers whispers with.
void ChatHandler::onAwayFlags(bool afk, bool dnd, bool announce) {
    const bool wasAfk = owner_.afkStatusRef();
    const bool wasDnd = owner_.dndStatusRef();
    owner_.afkStatusRef() = afk;
    owner_.dndStatusRef() = dnd;
    if (!announce) return;
    if (afk && !wasAfk) {
        const std::string& text = owner_.afkMessageRef();
        owner_.systemGlobal("MARKED_AFK_MESSAGE",
                            {text.empty() ? owner_.globalString("DEFAULT_AFK_MESSAGE") : text});
    } else if (!afk && wasAfk) {
        owner_.systemGlobal("CLEARED_AFK");
    }
    if (dnd && !wasDnd) {
        const std::string& text = owner_.dndMessageRef();
        owner_.systemGlobal("MARKED_DND",
                            {text.empty() ? owner_.globalString("DEFAULT_DND_MESSAGE") : text});
    } else if (!dnd && wasDnd && !afk) {
        owner_.systemGlobal("CLEARED_DND");
    }
}

void ChatHandler::replyToLastWhisper(const std::string& message) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot send whisper: not in world or not connected");
        return;
    }

    if (owner_.lastWhisperSenderRef().empty()) return;

    if (message.empty()) {
        owner_.raiseUiError("You must specify a message to send.");
        return;
    }

    // Send whisper using the standard message chat function
    sendChatMessage(ChatType::WHISPER, message, owner_.lastWhisperSenderRef());
    LOG_INFO("Replied to ", owner_.lastWhisperSenderRef(), ": ", message);
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void ChatHandler::handleChannelList(network::Packet& packet) {
    // A channel type byte comes first - Channel::List writes `uint8(1)` before
    // the name - and this read the name straight from it. The stray byte does
    // not shift anything, because the name's terminator ends the string either
    // way, but it rides along on the front of every use of it: the roster was
    // filed under "\x01General" while getChannelRoster is asked for "General",
    // so the member count answered zero for every channel.
    //
    // Recognised rather than assumed, since not every realm's build sends it: a
    // channel name always begins with a printable character, so a control byte
    // here is the type and nothing else can be.
    if (!packet.hasRemaining(1)) return;
    if (!packet.getData().empty() &&
        packet.getReadPos() < packet.getData().size() &&
        packet.getData()[packet.getReadPos()] < 0x20) {
        packet.readUInt8();  // channel type
    }
    std::string chanName = packet.readString();
    if (!packet.hasRemaining(5)) return;
    /*uint8_t chanFlags =*/ packet.readUInt8();
    uint32_t memberCount = packet.readUInt32();
    memberCount = std::min(memberCount, 200u);

    // The server's own member flags. What was here read 0x01 as moderator and
    // 0x02 as muted, so the channel's owner was announced as its moderator and
    // its moderators as muted.
    constexpr uint8_t kOwner = 0x01, kModerator = 0x02, kMuted = 0x08;

    std::vector<ChannelMember> roster;
    roster.reserve(memberCount);
    for (uint32_t i = 0; i < memberCount; ++i) {
        if (!packet.hasRemaining(9)) break;
        ChannelMember m;
        m.guid = packet.readUInt64();
        const uint8_t memberFlags = packet.readUInt8();
        m.owner     = (memberFlags & kOwner) != 0;
        m.moderator = (memberFlags & kModerator) != 0;
        m.muted     = (memberFlags & kMuted) != 0;

        auto entity = owner_.getEntityManager().getEntity(m.guid);
        if (entity) {
            auto player = std::dynamic_pointer_cast<Player>(entity);
            if (player && !player->getName().empty()) m.name = player->getName();
        }
        if (m.name.empty()) m.name = owner_.lookupName(m.guid);
        // Asked for rather than shown as "(unknown)": 2.4.3 lists every
        // member of LookingForGroup by name (pair 11). The answer fills the
        // row in (onPlayerName).
        if (m.name.empty()) owner_.queryPlayerName(m.guid);

        roster.push_back(std::move(m));
    }

    // /chatlist: "[6. chantestx] *Edrikf" (pair 6), CHAT_CHANNEL_LIST_GET
    // from ChatFrame with the members as the message, the owner starred.
    // The moderator mark "@" is not observed (one-member channel).
    if (printChannelLists_.erase(chanName) && owner_.addonEventCallbackRef()) {
        std::string members;
        for (const auto& m : roster) {
            if (!members.empty()) members += ", ";
            members += (m.owner ? "*" : m.moderator ? "@" : "") + m.name;
        }
        const int number = getChannelIndex(chanName);
        const std::string shown = number > 0 ? std::to_string(number) + ". " + chanName : chanName;
        owner_.addonEventCallbackRef()("CHAT_MSG_CHANNEL_LIST", {
            members, "", "", shown, "", "", "0", std::to_string(number), chanName, "0", "0", "0x0000000000000000"
        });
    }

    // By name: 2.4.3's roster lists Edrick before Edrikk whatever order the
    // realm sent (pair 11).
    std::stable_sort(roster.begin(), roster.end(),
                     [](const ChannelMember& a, const ChannelMember& b) { return a.name < b.name; });
    channelMemberCounts_[chanName] = static_cast<uint32_t>(roster.size());
    channelRosters_[chanName] = std::move(roster);
    // Both, because the panel redraws its member list on the first and its
    // member count on the second, and it registers for each separately.
    //
    // By display index, not by name. ChannelRoster_Update and
    // ChannelList_CountUpdate both take a position in the channel list -
    // _G["ChannelButton"..id] is how the second finds its row, and the first is
    // called elsewhere with GetSelectedDisplayChannel(), which is an index. A
    // name built "ChannelButtonGeneral", which is nothing, so neither the
    // roster nor the count ever redrew.
    // The panel row, which is not the chat number: General is row 3 under
    // the Group and World headers (pair 11: CHANNEL_ROSTER_UPDATE 3, 2).
    const int displayIndex = channelDisplayIndex(chanName);
    if (displayIndex > 0) {
        const std::string idx = std::to_string(displayIndex);
        const std::string count = std::to_string(channelRosters_[chanName].size());
        owner_.fireAddonEvent("CHANNEL_COUNT_UPDATE", {idx, count});
        owner_.fireAddonEvent("CHANNEL_ROSTER_UPDATE", {idx, count});
    }
}


/// SMSG_USERLIST_ADD / _REMOVE / _UPDATE - one member of a channel changing.
///
/// The full roster comes on SMSG_CHANNEL_LIST and is asked for; these arrive
/// unasked as people join, leave and are promoted, and without them the roster
/// is only ever as fresh as the last time somebody opened the list.
///
/// Two readers rather than one with a branch, because the three layouts are
/// not quite the same - remove carries no player flags - and a handler that
/// reads its fields under an `if` cannot be checked against the server by
/// tools/packet_layout_check.py, which reads the run of widths a body asks
/// for. It flagged the first version of this, correctly by its own lights.
///
///   ADD, UPDATE  guid(8) playerFlags(1) channelFlags(1) count(4) name
///   REMOVE       guid(8)                channelFlags(1) count(4) name
void ChatHandler::applyUserlistChange(const std::string& chanName, uint64_t guid,
                                      uint8_t memberFlags, bool removing) {
    if (chanName.empty()) return;
    // The same bits the full list reads, and the same trap: 0x01 is the owner
    // and 0x02 the moderator, not the other way round.
    constexpr uint8_t kOwner = 0x01, kModerator = 0x02, kMuted = 0x08;
    auto& roster = channelRosters_[chanName];
    auto it = std::find_if(roster.begin(), roster.end(),
                           [&](const ChannelMember& m) { return m.guid == guid; });

    if (removing) {
        if (it != roster.end()) roster.erase(it);
    } else {
        if (it == roster.end()) {
            ChannelMember m;
            m.guid = guid;
            m.name = owner_.lookupName(guid);
            if (m.name.empty()) owner_.queryPlayerName(guid);
            roster.push_back(std::move(m));
            it = roster.end() - 1;
        }
        it->owner     = (memberFlags & kOwner) != 0;
        it->moderator = (memberFlags & kModerator) != 0;
        it->muted     = (memberFlags & kMuted) != 0;
    }

    // By display index, for the reason spelled out in handleChannelList: both
    // of these take a position in the channel list, not a name.
    std::stable_sort(roster.begin(), roster.end(),
                     [](const ChannelMember& a, const ChannelMember& b) { return a.name < b.name; });
    channelMemberCounts_[chanName] = static_cast<uint32_t>(roster.size());
    const int displayIndex = channelDisplayIndex(chanName);
    if (displayIndex > 0) {
        const std::string idx = std::to_string(displayIndex);
        const std::string count = std::to_string(roster.size());
        owner_.fireAddonEvent("CHANNEL_COUNT_UPDATE", {idx, count});
        owner_.fireAddonEvent("CHANNEL_ROSTER_UPDATE", {idx, count});
    }
}

void ChatHandler::handleUserlistAdd(network::Packet& packet) {
    if (!packet.hasRemaining(14)) return;
    const uint64_t guid = packet.readUInt64();
    const uint8_t memberFlags = packet.readUInt8();
    packet.readUInt8();    // the channel's own flags
    packet.readUInt32();   // how many are in it now
    applyUserlistChange(packet.readString(), guid, memberFlags, /*removing=*/false);
}

void ChatHandler::handleUserlistRemove(network::Packet& packet) {
    if (!packet.hasRemaining(13)) return;
    const uint64_t guid = packet.readUInt64();
    packet.readUInt8();    // the channel's own flags
    packet.readUInt32();   // how many are in it now
    applyUserlistChange(packet.readString(), guid, 0, /*removing=*/true);
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void ChatHandler::updateGmTicket(const std::string& text) {
    if (!owner_.isInWorld() || !owner_.getSocket()) return;
    // CMSG_GMTICKET_UPDATETEXT (WotLK 3.3.5a): the new text and nothing else.
    // Creating carries the player's position because a new ticket records where
    // it was raised; editing one does not move it.
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_UPDATETEXT));
    pkt.writeString(text);
    owner_.getSocket()->send(pkt);
    LOG_INFO("Updated GM ticket text");
}

void ChatHandler::submitGmTicket(const std::string& text) {
    if (!owner_.isInWorld()) return;

    // CMSG_GMTICKET_CREATE:
    //   uint32  mapId
    //   float   x, y, z          (server coords, no facing)
    //   string  message
    //   uint32  needResponse
    //   uint8   needMoreHelp
    //   uint32  count            - how many chat-log timestamps follow
    //   uint32  decompressedSize - the zlib'd chat log after it, if any
    //
    // The comment above these writes described a layout in the other order,
    // opening with the text and carrying a facing the server does not read,
    // and the writes matched the comment rather than the wire. So the map id
    // was taken out of the middle of the ticket text, the position out of
    // whatever followed it, and the message read from four bytes of a float -
    // and the trailing fields were missing entirely, which runs the read off
    // the end of the buffer. AzerothCore catches that, logs, and drops the
    // packet: the ticket was never created and nothing said so.
    //
    // One shape on every expansion, and it is the long one.
    //
    // The prefix through the message is what all three read: a 1.12 and a
    // 2.4.3 server take mapId, x, y, z and the message and stop there. The
    // four fields after it are 3.x, and the asymmetry is what decides this -
    // a WotLK server that does not get them reads off the end of the buffer
    // and drops the whole request, while a vanilla or TBC one simply leaves
    // them unread. Trailing bytes nobody reads cost nothing; missing ones
    // cost the ticket. So there is nothing to gate on the expansion here, and
    // a gate would be a second thing to keep right.
    //
    // The chat log is a client-side transcript this client does not keep, so
    // both of its lengths go across as zero, which is how the real client
    // sends a ticket raised outside a conversation.
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_CREATE));
    pkt.writeUInt32(owner_.currentMapIdRef());
    pkt.writeFloat(owner_.movementInfoRef().x);
    pkt.writeFloat(owner_.movementInfoRef().y);
    pkt.writeFloat(owner_.movementInfoRef().z);
    pkt.writeString(text);
    pkt.writeUInt32(1);  // needResponse = yes
    pkt.writeUInt8(0);   // needMoreHelp = no
    pkt.writeUInt32(0);  // no chat-log timestamps
    pkt.writeUInt32(0);  // and so no compressed chat log
    owner_.getSocket()->send(pkt);
    LOG_INFO("Submitted GM ticket: '", text, "'");
}

void ChatHandler::handleMotd(network::Packet& packet) {
    LOG_INFO("Handling SMSG_MOTD");

    MotdData data;
    if (!MotdParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_MOTD");
        return;
    }

    if (!data.isEmpty()) {
        LOG_INFO("========================================");
        LOG_INFO("   MESSAGE OF THE DAY");
        LOG_INFO("========================================");
        for (const auto& line : data.lines) {
            LOG_INFO(line);
            addSystemChatMessage(line);
        }
        LOG_INFO("========================================");
    }
}

} // namespace game
} // namespace wowee
