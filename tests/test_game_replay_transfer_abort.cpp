// SMSG_TRANSFER_ABORTED and SMSG_AREA_TRIGGER_MESSAGE wire decoding,
// expansion-specific reason tables, Map.dbc Heroic difficulty text lookup,
// and CHAT_MSG_SYSTEM vs UI_INFO_MESSAGE routing.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

using replay::Session;
using wowee::game::GameHandlerTestAccess;
using wowee::game::Opcode;

namespace {

using Packet = wowee::network::Packet;

Packet transferAbortedPacket(uint32_t mapId, uint8_t reason, bool hasArg = false, uint8_t arg = 0) {
    Packet p(wowee::game::wireOpcode(Opcode::SMSG_TRANSFER_ABORTED));
    p.writeUInt32(mapId);
    p.writeUInt8(reason);
    if (hasArg) p.writeUInt8(arg);
    return p;
}

Packet areaTriggerMessagePacket(const std::string& text) {
    Packet p(wowee::game::wireOpcode(Opcode::SMSG_AREA_TRIGGER_MESSAGE));
    p.writeUInt32(static_cast<uint32_t>(text.size() + 1));
    p.writeString(text);
    return p;
}

struct CapturedNotices {
    std::vector<std::string> systemChat;
    std::vector<std::string> uiErrors;
    std::vector<std::string> uiInfo;

    void attach(Session& s) {
        s.handler().setAddonEventCallback(
            [this](const std::string& event, const std::vector<std::string>& args) {
                const std::string text = args.empty() ? std::string{} : args[0];
                if (event == "CHAT_MSG_SYSTEM") systemChat.push_back(text);
                else if (event == "UI_ERROR_MESSAGE") uiErrors.push_back(text);
                else if (event == "UI_INFO_MESSAGE") uiInfo.push_back(text);
            });
    }

    void clear() {
        systemChat.clear();
        uiErrors.clear();
        uiInfo.clear();
    }
};

}  // namespace

TEST_CASE("TBC 2.4.3 SMSG_TRANSFER_ABORTED reason table and Map.dbc heroic text",
          "[game_replay][transfer_abort][tbc]") {
    Session s("tbc");
    CapturedNotices notices;
    notices.attach(s);

    GameHandlerTestAccess::setMapInfo(
        s.handler(), 585, "Magister's Terrace",
        {"", "Heroic Difficulty requires completion of the \"Hard to Kill\" quest.", ""});
    GameHandlerTestAccess::setMapInfo(
        s.handler(), 543, "Hellfire Citadel: Ramparts",
        {"", "Heroic Difficulty requires the Flamewrought Key.", ""});
    GameHandlerTestAccess::setMapInfo(s.handler(), 36, "Deadmines");

    // 0x00 TRANSFER_ABORT_NONE -> silent
    s.receive(transferAbortedPacket(585, 0x00));
    CHECK(notices.systemChat.empty());
    CHECK(notices.uiErrors.empty());
    CHECK(notices.uiInfo.empty());

    // 0x01 TRANSFER_ABORT_MAX_PLAYERS
    s.receive(transferAbortedPacket(543, 0x01));
    REQUIRE(notices.systemChat.size() == 1);
    CHECK(notices.systemChat.back() == "Transfer Aborted: instance is full");
    CHECK(notices.uiErrors.empty());
    CHECK(notices.uiInfo.empty());

    // 0x02 TRANSFER_ABORT_NOT_FOUND
    notices.clear();
    s.receive(transferAbortedPacket(543, 0x02));
    REQUIRE(notices.systemChat.size() == 1);
    CHECK(notices.systemChat.back() == "Transfer Aborted: instance not found");
    CHECK(notices.uiErrors.empty());

    // 0x03 TRANSFER_ABORT_TOO_MANY_INSTANCES
    notices.clear();
    s.receive(transferAbortedPacket(543, 0x03));
    REQUIRE(notices.systemChat.size() == 1);
    CHECK(notices.systemChat.back() == "You have entered too many instances recently.");
    CHECK(notices.uiErrors.empty());

    // 0x05 TRANSFER_ABORT_ZONE_IN_COMBAT
    notices.clear();
    s.receive(transferAbortedPacket(543, 0x05));
    REQUIRE(notices.systemChat.size() == 1);
    CHECK(notices.systemChat.back() == "Unable to zone in while an encounter is in progress.");
    CHECK(notices.uiErrors.empty());

    // 0x06 TRANSFER_ABORT_INSUF_EXPAN_LVL (arg=1 -> TBC)
    notices.clear();
    s.receive(transferAbortedPacket(543, 0x06, true, 1));
    REQUIRE(notices.systemChat.size() == 1);
    CHECK(notices.systemChat.back() ==
          "You must have The Burning Crusade expansion installed to access this area.");
    CHECK(notices.uiErrors.empty());

    // 0x07 TRANSFER_ABORT_DIFFICULTY (arg=1 Heroic) with Map.dbc custom text (Magisters' Terrace)
    notices.clear();
    s.receive(transferAbortedPacket(585, 0x07, true, 1));
    REQUIRE(notices.systemChat.size() == 1);
    CHECK(notices.systemChat.back() ==
          "Heroic Difficulty requires completion of the \"Hard to Kill\" quest.");
    CHECK(notices.uiErrors.empty());
    CHECK(notices.uiInfo.empty());

    // 0x07 TRANSFER_ABORT_DIFFICULTY (arg=1 Heroic) with Map.dbc custom text (Hellfire Ramparts)
    notices.clear();
    s.receive(transferAbortedPacket(543, 0x07, true, 1));
    REQUIRE(notices.systemChat.size() == 1);
    CHECK(notices.systemChat.back() == "Heroic Difficulty requires the Flamewrought Key.");
    CHECK(notices.uiErrors.empty());

    // 0x07 TRANSFER_ABORT_DIFFICULTY (arg=1 Heroic) with NO custom text in Map.dbc -> fallback format
    notices.clear();
    s.receive(transferAbortedPacket(36, 0x07, true, 1));
    REQUIRE(notices.systemChat.size() == 1);
    CHECK(notices.systemChat.back() == "Heroic difficulty mode is not available for Deadmines.");
    CHECK(notices.uiErrors.empty());

    // 0x07 TRANSFER_ABORT_DIFFICULTY (arg=0 Normal / arg=2 Epic) fallback format
    notices.clear();
    s.receive(transferAbortedPacket(36, 0x07, true, 0));
    s.receive(transferAbortedPacket(36, 0x07, true, 2));
    REQUIRE(notices.systemChat.size() == 2);
    CHECK(notices.systemChat[0] == "Normal difficulty mode is not available for Deadmines.");
    CHECK(notices.systemChat[1] == "Epic difficulty mode is not available for Deadmines.");
}

TEST_CASE("TBC 2.4.3 SMSG_TRANSFER_ABORTED honors localized GlobalStrings",
          "[game_replay][transfer_abort][tbc]") {
    Session s("tbc");
    CapturedNotices notices;
    notices.attach(s);

    const std::map<std::string, std::string> globals = {
        {"TRANSFER_ABORT_MAX_PLAYERS", "Localized full"},
        {"TRANSFER_ABORT_DIFFICULTY2", "Heroic unavailable for %s."},
        {"TRANSFER_ABORT_INSUF_EXPAN_LVL1", "Localized TBC required"},
    };
    s.handler().setGlobalStringLookup([&](const char* key) -> std::string {
        auto it = globals.find(key);
        return (it != globals.end()) ? it->second : std::string{};
    });
    GameHandlerTestAccess::setMapInfo(s.handler(), 36, "Deadmines");

    s.receive(transferAbortedPacket(36, 0x01));
    s.receive(transferAbortedPacket(36, 0x06, true, 1));
    s.receive(transferAbortedPacket(36, 0x07, true, 1));

    REQUIRE(notices.systemChat.size() == 3);
    CHECK(notices.systemChat[0] == "Localized full");
    CHECK(notices.systemChat[1] == "Localized TBC required");
    CHECK(notices.systemChat[2] == "Heroic unavailable for Deadmines.");
}

TEST_CASE("Classic 1.12 SMSG_TRANSFER_ABORTED uses pre-WotLK reason codes",
          "[game_replay][transfer_abort][classic]") {
    Session s("classic");
    CapturedNotices notices;
    notices.attach(s);

    s.receive(transferAbortedPacket(36, 0x01));
    s.receive(transferAbortedPacket(36, 0x02));
    s.receive(transferAbortedPacket(36, 0x03));
    s.receive(transferAbortedPacket(36, 0x05));

    REQUIRE(notices.systemChat.size() == 4);
    CHECK(notices.systemChat[0] == "Transfer Aborted: instance is full");
    CHECK(notices.systemChat[1] == "Transfer Aborted: instance not found");
    CHECK(notices.systemChat[2] == "You have entered too many instances recently.");
    CHECK(notices.systemChat[3] == "Unable to zone in while an encounter is in progress.");
    CHECK(notices.uiErrors.empty());
}

TEST_CASE("WotLK 3.3.5a SMSG_TRANSFER_ABORTED preserves shifted reason codes",
          "[game_replay][transfer_abort][wotlk]") {
    Session s("wotlk");
    CapturedNotices notices;
    notices.attach(s);
    GameHandlerTestAccess::setMapInfo(s.handler(), 574, "Utgarde Keep");

    s.receive(transferAbortedPacket(574, 0x00));  // NONE -> silent
    s.receive(transferAbortedPacket(574, 0x01));  // ERROR
    s.receive(transferAbortedPacket(574, 0x02));  // MAX_PLAYERS
    s.receive(transferAbortedPacket(574, 0x04));  // TOO_MANY_INSTANCES
    s.receive(transferAbortedPacket(574, 0x06));  // ZONE_IN_COMBAT
    s.receive(transferAbortedPacket(574, 0x07, true, 2));  // INSUF_EXPAN_LVL
    s.receive(transferAbortedPacket(574, 0x08, true, 1));  // DIFFICULTY
    s.receive(transferAbortedPacket(574, 0x0B));  // NEED_GROUP
    s.receive(transferAbortedPacket(574, 0x10));  // MAP_NOT_ALLOWED

    REQUIRE(notices.systemChat.size() == 8);
    CHECK(notices.systemChat[0] == "Transfer aborted: an error occurred.");
    CHECK(notices.systemChat[1] == "Transfer Aborted: instance is full");
    CHECK(notices.systemChat[2] == "You have entered too many instances recently.");
    CHECK(notices.systemChat[3] == "Unable to zone in while an encounter is in progress.");
    CHECK(notices.systemChat[4] == "You do not have the expansion required to enter this area.");
    CHECK(notices.systemChat[5] == "Heroic difficulty mode is not available for Utgarde Keep.");
    CHECK(notices.systemChat[6] == "Transfer Aborted: you must be in a group to enter.");
    CHECK(notices.systemChat[7] == "This map cannot be entered at this time.");
    CHECK(notices.uiErrors.empty());
}

TEST_CASE("SMSG_AREA_TRIGGER_MESSAGE routes to yellow UI_INFO_MESSAGE only",
          "[game_replay][transfer_abort][tbc]") {
    Session s("tbc");
    CapturedNotices notices;
    notices.attach(s);

    s.receive(areaTriggerMessagePacket("You must be at least level 55 to enter."));

    REQUIRE(notices.uiInfo.size() == 1);
    CHECK(notices.uiInfo[0] == "You must be at least level 55 to enter.");
    CHECK(notices.uiErrors.empty());
    CHECK(notices.systemChat.empty());
}
