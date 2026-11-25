// End-to-end GameHandler replay tests for TBC 2.4.3 Network Sweep Shard 6:
// - SMSG_AUTH_RESPONSE queue wait -> queue update -> AUTH_OK release
// - SMSG_ACCOUNT_DATA_TIMES TBC 128-byte MD5 array persistence
// - SMSG_TUTORIAL_FLAGS + CMSG_TUTORIAL_FLAG / CLEAR / RESET round-trip
// - SMSG_DEFENSE_MESSAGE, SMSG_SERVER_MESSAGE (empty cancel), SMSG_KICK_REASON (TBC)
// - SMSG_WEATHER isAbrupt forwarding
// - UNIT_FIELD_FLAGS (PACIFIED / CONFUSED / FLEEING / CLIENT_CONTROL_LOST),
//   UNIT_FIELD_FLAGS_2 (HIDE_BODY / DO_NOT_FADE_IN), and NPCFlags direct interactions

#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <cstdint>
#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::AuthResult;
using wowee::game::GameHandlerTestAccess;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::UF;
using wowee::game::WorldState;
using wowee::network::Packet;

namespace {

std::vector<std::string> systemChatLines(Session& s) {
    std::vector<std::string> out;
    for (const auto& m : s.handler().getChatHistory()) {
        out.push_back(m.message);
    }
    return out;
}

} // namespace

TEST_CASE("TBC SMSG_AUTH_RESPONSE stays connected across WAIT_QUEUE until AUTH_OK", "[replay][shard6]") {
    Session s("tbc");
    GameHandlerTestAccess::setWorldState(s.handler(), WorldState::AUTH_SENT);

    // 1. SendAuthQueued (15 bytes): position 12, expansion 1
    Packet queued(wowee::game::wireOpcode(Opcode::SMSG_AUTH_RESPONSE));
    queued.writeUInt8(static_cast<uint8_t>(AuthResult::WAIT_QUEUE));
    queued.writeUInt32(7200u);
    queued.writeUInt8(1u);
    queued.writeUInt32(3600u);
    queued.writeUInt8(1u); // TBC
    queued.writeUInt32(12u);
    s.receive(queued);

    CHECK(s.handler().getState() == WorldState::AUTH_SENT);
    CHECK(s.handler().isInAuthQueue());
    CHECK(s.handler().getAuthQueuePosition() == 12u);
    CHECK(s.handler().getAccountExpansion() == 1u);
    CHECK(s.handler().getBillingTimeRemaining() == 7200u);

    // 2. SendAuthWaitQue(4) (5 bytes): position drops to 4
    Packet update(wowee::game::wireOpcode(Opcode::SMSG_AUTH_RESPONSE));
    update.writeUInt8(static_cast<uint8_t>(AuthResult::WAIT_QUEUE));
    update.writeUInt32(4u);
    s.receive(update);

    CHECK(s.handler().getState() == WorldState::AUTH_SENT);
    CHECK(s.handler().isInAuthQueue());
    CHECK(s.handler().getAuthQueuePosition() == 4u);
    CHECK(s.handler().getAccountExpansion() == 1u); // Preserved from initial queued packet

    // 3. SendAuthWaitQue(0) (1 byte): AUTH_OK releases from queue into READY
    Packet ok(wowee::game::wireOpcode(Opcode::SMSG_AUTH_RESPONSE));
    ok.writeUInt8(static_cast<uint8_t>(AuthResult::OK));
    s.receive(ok);

    CHECK_FALSE(s.handler().isInAuthQueue());
    CHECK(s.handler().getAuthQueuePosition() == 0u);
    CHECK(s.handler().getState() == WorldState::READY);
}

TEST_CASE("TBC SMSG_ACCOUNT_DATA_TIMES stores 128-byte MD5 digests on GameHandler", "[replay][shard6]") {
    Session s("tbc");
    Packet p(wowee::game::wireOpcode(Opcode::SMSG_ACCOUNT_DATA_TIMES));
    for (int slot = 0; slot < 8; ++slot) {
        for (int b = 0; b < 16; ++b) {
            p.writeUInt8(slot == 2 ? static_cast<uint8_t>(0x10 + b) : 0u);
        }
    }
    s.receive(p);

    CHECK(s.handler().getAccountDataTimes().isTbcMd5Layout);
    CHECK(s.handler().getAccountDataMask() == (1u << 2));
    CHECK(s.handler().getAccountDataTimes().accountDataMd5[2][0] == 0x10u);
    CHECK(s.handler().getAccountDataTimes().accountDataMd5[2][15] == 0x1Fu);
}

TEST_CASE("TBC SMSG_TUTORIAL_FLAGS persists bitfields and emits CMSG_TUTORIAL_FLAG/CLEAR/RESET", "[replay][shard6]") {
    Session s("tbc");

    Packet tut(wowee::game::wireOpcode(Opcode::SMSG_TUTORIAL_FLAGS));
    tut.writeUInt32(0x00000005u); // Tutorials 1 and 3
    for (int i = 1; i < 8; ++i) tut.writeUInt32(0u);
    s.receive(tut);

    REQUIRE(s.handler().hasTutorialFlags());
    CHECK(s.handler().isTutorialFlagged(1));
    CHECK_FALSE(s.handler().isTutorialFlagged(2));
    CHECK(s.handler().isTutorialFlagged(3));

    // Flagging already-set tutorial 1 sends nothing
    s.handler().flagTutorial(1);
    CHECK(s.sent().of(Opcode::CMSG_TUTORIAL_FLAG).empty());

    // Flagging tutorial 2 sets bit 1 and sends CMSG_TUTORIAL_FLAG(1)
    s.handler().flagTutorial(2);
    CHECK(s.handler().isTutorialFlagged(2));
    auto flagged = s.sent().of(Opcode::CMSG_TUTORIAL_FLAG);
    REQUIRE(flagged.size() == 1);
    flagged[0].setReadPos(0);
    CHECK(flagged[0].readUInt32() == 1u);

    // ClearTutorials sets all bits and sends CMSG_TUTORIAL_CLEAR
    s.handler().clearTutorials();
    CHECK(s.handler().isTutorialFlagged(200));
    CHECK(s.sent().of(Opcode::CMSG_TUTORIAL_CLEAR).size() == 1);

    // ResetTutorials clears all bits and sends CMSG_TUTORIAL_RESET
    s.handler().resetTutorials();
    CHECK_FALSE(s.handler().isTutorialFlagged(1));
    CHECK_FALSE(s.handler().isTutorialFlagged(200));
    CHECK(s.sent().of(Opcode::CMSG_TUTORIAL_RESET).size() == 1);
}

TEST_CASE("TBC SMSG_DEFENSE_MESSAGE, SMSG_SERVER_MESSAGE, SMSG_KICK_REASON, and SMSG_WEATHER", "[replay][shard6]") {
    Session s("tbc");
    GameHandlerTestAccess::setAreaName(s.handler(), 44u, "Redridge Mountains");

    std::vector<std::pair<std::string, std::vector<std::string>>> events;
    s.handler().setAddonEventCallback(
        [&events](const std::string& ev, const std::vector<std::string>& args) {
            events.emplace_back(ev, args);
        });

    // 1. SMSG_DEFENSE_MESSAGE with uint32 zoneId + uint32 messageLength + cstring
    const std::string defText = "Lakeshire is under attack!";
    Packet defPkt(wowee::game::wireOpcode(Opcode::SMSG_DEFENSE_MESSAGE));
    defPkt.writeUInt32(44u);
    defPkt.writeUInt32(static_cast<uint32_t>(defText.size() + 1));
    defPkt.writeString(defText);
    s.receive(defPkt);

    // 2. SMSG_SERVER_MESSAGE type 4 (SHUTDOWN_CANCELLED) with empty text
    Packet cancelPkt(wowee::game::wireOpcode(Opcode::SMSG_SERVER_MESSAGE));
    cancelPkt.writeUInt32(4u);
    cancelPkt.writeString("");
    s.receive(cancelPkt);

    // 3. SMSG_KICK_REASON TBC 2-field payload (< 12 bytes)
    Packet kickPkt(wowee::game::wireOpcode(Opcode::SMSG_KICK_REASON));
    kickPkt.writeUInt8(2u);
    kickPkt.writeString("AFK");
    s.receive(kickPkt);

    const auto lines = systemChatLines(s);
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0] == "[LocalDefense - Redridge Mountains] Lakeshire is under attack!");
    CHECK(lines[1] == "[Shutdown cancelled]");
    CHECK(lines[2] == "Disconnected by server: AFK");

    // 4. SMSG_WEATHER isAbrupt = 1 vs 0
    Packet wAbrupt(wowee::game::wireOpcode(Opcode::SMSG_WEATHER));
    wAbrupt.writeUInt32(3u);
    wAbrupt.writeFloat(0.8f);
    wAbrupt.writeUInt8(1u);
    s.receive(wAbrupt);
    CHECK(s.handler().isWeatherAbrupt());

    Packet wSmooth(wowee::game::wireOpcode(Opcode::SMSG_WEATHER));
    wSmooth.writeUInt32(3u);
    wSmooth.writeFloat(0.4f);
    wSmooth.writeUInt8(0u);
    s.receive(wSmooth);
    CHECK_FALSE(s.handler().isWeatherAbrupt());
}

TEST_CASE("TBC UnitFlags, UnitFlags2, and dedicated NPCFlags wire actions", "[replay][shard6]") {
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 500},
         {UF::UNIT_FIELD_MAXHEALTH, 500},
         {UF::UNIT_FIELD_FLAGS, wowee::game::UNIT_FLAG_PACIFIED | wowee::game::UNIT_FLAG_DISARMED |
                                wowee::game::UNIT_FLAG_SILENCED | wowee::game::UNIT_FLAG_CONFUSED}},
        true));

    CHECK(s.handler().isPlayerPacified());
    CHECK(s.handler().isPlayerDisarmed());
    CHECK(s.handler().isPlayerSilenced());
    CHECK_FALSE(s.handler().hasPlayerFullControl());

    constexpr uint64_t kEnemyGuid = 0xF130000000000901ull;
    constexpr uint64_t kTabardNpc = 0xF130000000000902ull;
    constexpr uint64_t kPetitionNpc = 0xF130000000000903ull;
    constexpr uint64_t kStableNpc = 0xF130000000000904ull;

    s.receive(UpdateObject().tbc()
        .create(kEnemyGuid, ObjectType::UNIT,
                {{UF::UNIT_FIELD_HEALTH, 200},
                 {UF::UNIT_FIELD_MAXHEALTH, 200},
                 {UF::UNIT_FIELD_FLAGS_2, wowee::game::UNIT_FLAG2_HIDE_BODY | wowee::game::UNIT_FLAG2_DO_NOT_FADE_IN}})
        .create(kTabardNpc, ObjectType::UNIT,
                {{UF::UNIT_FIELD_HEALTH, 100},
                 {UF::UNIT_FIELD_MAXHEALTH, 100},
                 {UF::UNIT_NPC_FLAGS, wowee::game::NPC_FLAG_TABARDDESIGNER}})
        .create(kPetitionNpc, ObjectType::UNIT,
                {{UF::UNIT_FIELD_HEALTH, 100},
                 {UF::UNIT_FIELD_MAXHEALTH, 100},
                 {UF::UNIT_NPC_FLAGS, wowee::game::NPC_FLAG_PETITIONER}})
        .create(kStableNpc, ObjectType::UNIT,
                {{UF::UNIT_FIELD_HEALTH, 100},
                 {UF::UNIT_FIELD_MAXHEALTH, 100},
                 {UF::UNIT_NPC_FLAGS, wowee::game::NPC_FLAG_STABLEMASTER}}));

    // UNIT_FLAG_PACIFIED blocks starting melee auto-attack
    s.handler().startAutoAttack(kEnemyGuid);
    CHECK(s.sent().of(Opcode::CMSG_ATTACKSWING).empty());

    // UNIT_FIELD_FLAGS_2 stored on Unit
    auto enemy = std::dynamic_pointer_cast<wowee::game::Unit>(
        s.handler().getEntityManager().getEntity(kEnemyGuid));
    REQUIRE(enemy != nullptr);
    CHECK(wowee::game::unitHidesBody(enemy->getUnitFlags2()));
    CHECK(wowee::game::unitSkipsSpawnFadeIn(enemy->getUnitFlags2()));

    // Dedicated single-service NPCFlags send their TBC wire opcodes
    s.handler().interactWithNpc(kTabardNpc);
    CHECK(s.sent().of(Opcode::MSG_TABARDVENDOR_ACTIVATE).size() == 1);

    s.handler().interactWithNpc(kPetitionNpc);
    CHECK(s.sent().of(Opcode::CMSG_PETITION_SHOWLIST).size() == 1);

    s.handler().interactWithNpc(kStableNpc);
    CHECK(s.sent().of(Opcode::MSG_LIST_STABLED_PETS).size() == 1);
}
