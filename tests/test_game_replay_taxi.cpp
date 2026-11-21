// TBC 2.4.3 taxi chat & UIErrorsFrame parity:
// - TakeTaxiNode / activateTaxi + SMSG_ACTIVATETAXIREPLY(ERR_TAXIOK) emits zero chat/error/info messages.
// - SMSG_ACTIVATETAXIREPLY(1..12) routes to UI_ERROR_MESSAGE with exact TBC GlobalStrings ERR_TAXI* text.
// - Unreachable destination routes to UI_ERROR_MESSAGE ERR_TAXINOSUCHPATH (zero chat).
// - SMSG_NEW_TAXI_PATH routes ERR_NEWTAXIPATH to UI_INFO_MESSAGE only (zero chat, zero UI_ERROR_MESSAGE).
// - SMSG_SHOWTAXINODES discovering new mask bits emits zero chat messages.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using replay::Session;
using wowee::game::GameHandlerTestAccess;
using wowee::game::MovementHandler;
using wowee::game::Opcode;

namespace {

using Packet = wowee::network::Packet;

std::string taxiGlobalString(const char* name) {
    static const std::unordered_map<std::string, std::string> kStrings = {
        {"ERR_NEWTAXIPATH", "New flight path discovered!"},
        {"ERR_TAXIUNSPECIFIEDSERVERERROR", "UNSPECIFIED TAXI SERVER ERROR"},
        {"ERR_TAXINOSUCHPATH", "There is no direct path to that destination!"},
        {"ERR_TAXINOTENOUGHMONEY", "You don't have enough money!"},
        {"ERR_TAXITOOFARAWAY", "You are too far away from the taxi stand!"},
        {"ERR_TAXINOVENDORNEARBY", "There is no taxi vendor nearby!"},
        {"ERR_TAXINOTVISITED", "You haven't reached that taxi node on foot yet!"},
        {"ERR_TAXIPLAYERBUSY", "You are busy and can't use the taxi service now."},
        {"ERR_TAXIPLAYERALREADYMOUNTED", "You are already mounted! Dismount first."},
        {"ERR_TAXIPLAYERSHAPESHIFTED", "You can't take a taxi while shapeshifted!"},
        {"ERR_TAXIPLAYERMOVING", "You are moving."},
        {"ERR_TAXISAMENODE", "You are already there!"},
        {"ERR_TAXINOTSTANDING", "You need to be standing to go anywhere."},
    };
    auto it = kStrings.find(name);
    return it == kStrings.end() ? std::string{} : it->second;
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

Packet showTaxiNodesPacket(uint64_t npcGuid, uint32_t nearestNode, uint32_t maskWord0) {
    Packet p(wowee::game::wireOpcode(Opcode::SMSG_SHOWTAXINODES));
    p.writeUInt32(1);  // windowFlag
    p.writeUInt64(npcGuid);
    p.writeUInt32(nearestNode);
    p.writeUInt32(maskWord0);
    for (int i = 1; i < 8; ++i) p.writeUInt32(0);
    return p;
}

Packet activateTaxiReplyPacket(uint32_t result) {
    Packet p(wowee::game::wireOpcode(Opcode::SMSG_ACTIVATETAXIREPLY));
    p.writeUInt32(result);
    return p;
}

void seedTwoNodeGraph(Session& s) {
    std::unordered_map<uint32_t, MovementHandler::TaxiNode> nodes;
    nodes[6] = MovementHandler::TaxiNode{
        .id = 6, .mapId = 0, .x = -4821.13f, .y = -1152.40f, .z = 502.30f,
        .name = "Ironforge, Dun Morogh", .mountDisplayIdAlliance = 541};
    nodes[8] = MovementHandler::TaxiNode{
        .id = 8, .mapId = 0, .x = -5424.85f, .y = -2929.87f, .z = 347.65f,
        .name = "Thelsamar, Loch Modan", .mountDisplayIdAlliance = 541};
    nodes[9] = MovementHandler::TaxiNode{
        .id = 9, .mapId = 0, .x = -3793.20f, .y = -782.05f, .z = 9.10f,
        .name = "Menethil Harbor, Wetlands", .mountDisplayIdAlliance = 541};
    std::vector<MovementHandler::TaxiPathEdge> edges = {
        {1, 6, 8, 50},
        {2, 8, 6, 50},
    };
    GameHandlerTestAccess::setTaxiGraph(s.handler(), std::move(nodes), std::move(edges));
}

}  // namespace

TEST_CASE("TBC 2.4.3 SMSG_NEW_TAXI_PATH and SMSG_SHOWTAXINODES discovery parity",
          "[game_replay][taxi][tbc]") {
    Session s("tbc");
    s.handler().setGlobalStringLookup(taxiGlobalString);
    GameHandlerTestAccess::setNoticesInterfaceReady(s.handler(), true);
    CapturedNotices notices;
    notices.attach(s);
    seedTwoNodeGraph(s);

    // SMSG_NEW_TAXI_PATH -> UI_INFO_MESSAGE only (yellow in UIErrorsFrame), zero chat.
    Packet newPath(wowee::game::wireOpcode(Opcode::SMSG_NEW_TAXI_PATH));
    s.receive(newPath);
    REQUIRE(notices.uiInfo.size() == 1);
    CHECK(notices.uiInfo.back() == "New flight path discovered!");
    CHECK(notices.systemChat.empty());
    CHECK(notices.uiErrors.empty());

    // First SMSG_SHOWTAXINODES initializes known mask (node 6 known).
    notices.clear();
    const uint32_t maskNode6 = 1u << (6 - 1);
    const uint32_t maskNode6And8 = maskNode6 | (1u << (8 - 1));
    s.receive(showTaxiNodesPacket(0xF130000625000001ull, 6, maskNode6));
    CHECK(notices.systemChat.empty());
    CHECK(notices.uiErrors.empty());
    CHECK(notices.uiInfo.empty());

    // Second SMSG_SHOWTAXINODES with newly discovered node 8 bit -> zero chat spam.
    s.receive(showTaxiNodesPacket(0xF130000624000002ull, 8, maskNode6And8));
    CHECK(notices.systemChat.empty());
    CHECK(notices.uiErrors.empty());
    CHECK(notices.uiInfo.empty());
}

TEST_CASE("TBC 2.4.3 TakeTaxiNode and SMSG_ACTIVATETAXIREPLY OK emits zero chat or error messages",
          "[game_replay][taxi][tbc]") {
    Session s("tbc");
    s.handler().setGlobalStringLookup(taxiGlobalString);
    GameHandlerTestAccess::setNoticesInterfaceReady(s.handler(), true);
    CapturedNotices notices;
    notices.attach(s);
    seedTwoNodeGraph(s);

    const uint32_t maskNode6And8 = (1u << (6 - 1)) | (1u << (8 - 1));
    s.receive(showTaxiNodesPacket(0xF130000624000002ull, 8, maskNode6And8));
    notices.clear();

    // Activating flight must NOT emit "Requesting flight to ..." in chat.
    s.handler().activateTaxi(6);
    CHECK(notices.systemChat.empty());
    CHECK(notices.uiErrors.empty());
    CHECK(notices.uiInfo.empty());
    REQUIRE(s.sent().of(Opcode::CMSG_ACTIVATETAXI).size() == 1);

    // Server replies ERR_TAXIOK (0) -> flight begins silently with zero chat/error/info messages.
    s.receive(activateTaxiReplyPacket(0));
    CHECK(notices.systemChat.empty());
    CHECK(notices.uiErrors.empty());
    CHECK(notices.uiInfo.empty());
    CHECK(s.handler().isOnTaxiFlight());
}

TEST_CASE("TBC 2.4.3 SMSG_ACTIVATETAXIREPLY error codes route to UI_ERROR_MESSAGE with exact ERR_TAXI* text",
          "[game_replay][taxi][tbc]") {
    Session s("tbc");
    s.handler().setGlobalStringLookup(taxiGlobalString);
    GameHandlerTestAccess::setNoticesInterfaceReady(s.handler(), true);
    CapturedNotices notices;
    notices.attach(s);
    seedTwoNodeGraph(s);

    const uint32_t maskNode6And8 = (1u << (6 - 1)) | (1u << (8 - 1));
    s.receive(showTaxiNodesPacket(0xF130000624000002ull, 8, maskNode6And8));

    const std::vector<std::pair<uint32_t, std::string>> kExpected = {
        {1, "UNSPECIFIED TAXI SERVER ERROR"},
        {2, "There is no direct path to that destination!"},
        {3, "You don't have enough money!"},
        {4, "You are too far away from the taxi stand!"},
        {5, "There is no taxi vendor nearby!"},
        {6, "You haven't reached that taxi node on foot yet!"},
        {7, "You are busy and can't use the taxi service now."},
        {8, "You are already mounted! Dismount first."},
        {9, "You can't take a taxi while shapeshifted!"},
        {10, "You are moving."},
        {11, "You are already there!"},
        {12, "You need to be standing to go anywhere."},
    };

    for (const auto& [code, expectedText] : kExpected) {
        INFO("ActivateTaxiReply code=" << code);
        notices.clear();
        s.handler().activateTaxi(6);
        CHECK(notices.systemChat.empty());
        s.receive(activateTaxiReplyPacket(code));
        REQUIRE(notices.uiErrors.size() == 1);
        CHECK(notices.uiErrors.back() == expectedText);
        CHECK(notices.systemChat.empty());
        CHECK(notices.uiInfo.empty());
        CHECK_FALSE(s.handler().isOnTaxiFlight());
    }

    // Unreachable destination (node 9 has no edge from node 8) -> UI_ERROR_MESSAGE ERR_TAXINOSUCHPATH.
    notices.clear();
    s.handler().activateTaxi(9);
    REQUIRE(notices.uiErrors.size() == 1);
    CHECK(notices.uiErrors.back() == "There is no direct path to that destination!");
    CHECK(notices.systemChat.empty());
    CHECK(notices.uiInfo.empty());
}
