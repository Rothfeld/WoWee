// TBC 2.4.3 facing & movement-packet parity across GameObject and Unit interactions.
// Verified live on pair 20 against build 8606 (Gadgetzan Wanted Poster 150075,
// Mailbox 144112, The Twin Empires 175854, Explosive Charge 144065, Marvon's Chest
// 149036, Alien Egg 175567, Bench 133469, Tran'rek 7876, Marin Noggenfogger 7564):
// Native 2.4.3 never rotates the local character and never emits MSG_MOVE_SET_FACING
// (or a stationary MSG_MOVE_HEARTBEAT) when using any GameObject or talking to /
// looting a Unit.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "core/coordinates.hpp"
#include "game/quest_handler.hpp"
#include "rendering/renderer.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace {

using replay::SentPackets;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::UF;
using wowee::game::wireOpcode;
using wowee::network::Packet;

// Replay session with a real Renderer attached to GameServices so tests verify
// both wire packets / canonical movementInfo.orientation AND visual characterYaw.
class RenderedReplaySession {
public:
    static constexpr uint64_t kPlayerGuid = replay::Session::kPlayerGuid;

    explicit RenderedReplaySession(const char* expansion = "tbc")
        : services_{.renderer = &renderer_}, handler_(services_) {
        auto registry = std::make_unique<wowee::game::ExpansionRegistry>();
        registry->initialize(std::string(WOWEE_SOURCE_DIR) + "/Data");
        REQUIRE(registry->setActive(expansion));
        const std::string dir = registry->getActive()->dataPath;
        wowee::game::GameHandlerTestAccess::setExpansions(app_, std::move(registry));
        REQUIRE(handler_.getOpcodeTable().loadFromJson(dir + "/opcodes.json"));
        REQUIRE(handler_.getUpdateFieldTable().loadFromJson(dir + "/update_fields.json"));
        handler_.setPacketParsers(wowee::game::createPacketParsers(expansion));
        handler_.setPlayerGuid(kPlayerGuid);
        auto socket = std::make_unique<SentPackets>();
        sent_ = socket.get();
        wowee::game::GameHandlerTestAccess::setSocket(handler_, std::move(socket));
        wowee::game::GameHandlerTestAccess::enterWorld(handler_);
    }

    wowee::game::GameHandler& handler() { return handler_; }
    wowee::rendering::Renderer& renderer() { return renderer_; }
    const SentPackets& sent() const { return *sent_; }

    void receive(const Packet& packet) {
        wowee::game::GameHandlerTestAccess::receive(handler_, packet);
    }
    void receive(const UpdateObject& update) { receive(update.packet()); }

    void setPlayerPose(float x, float y, float z, float canonicalYaw) {
        handler_.setPosition(x, y, z);
        handler_.setOrientation(canonicalYaw);
        renderer_.setCharacterYaw(wowee::core::coords::canonicalToCharacterYawDeg(canonicalYaw));
    }

private:
    wowee::core::Application app_;
    wowee::rendering::Renderer renderer_;
    wowee::game::GameServices services_;
    wowee::game::GameHandler handler_;
    SentPackets* sent_ = nullptr;
};

Packet gameObjectQueryResponse(uint32_t entry, uint32_t type, const std::string& name,
                               uint32_t data7 = 0) {
    Packet p(wireOpcode(Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE));
    p.writeUInt32(entry);
    p.writeUInt32(type);
    p.writeUInt32(3053);  // displayId
    p.writeString(name);
    for (int i = 0; i < 6; ++i) p.writeString("");
    for (int i = 0; i < 24; ++i) {
        p.writeUInt32(i == 7 ? data7 : 0u);
    }
    p.writeFloat(1.0f);
    return p;
}

Packet tbcQuestListPacket(uint64_t giverGuid, uint32_t questId, const std::string& title) {
    Packet p(wireOpcode(Opcode::SMSG_QUESTGIVER_QUEST_LIST));
    p.writeUInt64(giverGuid);
    p.writeString("Wanted by the Cenarion Circle:");
    p.writeUInt32(0);   // EmoteDelay
    p.writeUInt32(0);   // EmoteId
    p.writeUInt8(1);    // questCount
    p.writeUInt32(questId);
    p.writeUInt32(6);   // TBC DIALOG_STATUS_AVAILABLE
    p.writeUInt32(59);  // questLevel
    p.writeString(title);
    return p;
}

Packet tbcQuestDetailsPacket(uint64_t giverGuid, uint32_t questId, const std::string& title) {
    Packet p(wireOpcode(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS));
    p.writeUInt64(giverGuid);
    p.writeUInt32(questId);
    p.writeString(title);
    p.writeString("Bring Deathclasp's Pinchers to Cenarion Hold.");
    p.writeString("Slay Deathclasp.");
    p.writeUInt32(1);  // activateAccept
    p.writeUInt32(0);  // suggestedPlayers
    p.writeUInt32(0);  // choiceCount
    p.writeUInt32(0);  // rewardCount
    p.writeUInt32(0);  // rewardMoney
    return p;
}

Packet tbcCreateAt(uint64_t guid, ObjectType type, const UpdateObject::Fields& fields,
                   float x, float y, float z, bool self = false) {
    Packet body;
    body.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::CREATE_OBJECT2));
    body.writePackedGuid(guid);
    body.writeUInt8(static_cast<uint8_t>(type));
    body.writeUInt8(static_cast<uint8_t>(0x40u | (self ? 0x01u : 0u)));  // TBC stationary + self
    body.writeFloat(x);
    body.writeFloat(y);
    body.writeFloat(z);
    body.writeFloat(0.0f);

    std::map<uint16_t, uint32_t> byIndex;
    for (const auto& [field, value] : fields) {
        const uint16_t index = wowee::game::fieldIndex(field);
        REQUIRE(index != 0xFFFF);
        byIndex[index] = value;
    }
    const uint8_t maskBlocks =
        byIndex.empty() ? 0 : static_cast<uint8_t>(byIndex.rbegin()->first / 32 + 1);
    body.writeUInt8(maskBlocks);
    for (uint8_t b = 0; b < maskBlocks; ++b) {
        uint32_t mask = 0;
        for (const auto& entry : byIndex)
            if (entry.first / 32 == b) mask |= 1u << (entry.first % 32);
        body.writeUInt32(mask);
    }
    for (const auto& entry : byIndex) body.writeUInt32(entry.second);

    Packet pkt(wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
    pkt.writeUInt32(1);
    pkt.writeUInt8(0);  // TBC hasTransport=0
    pkt.writeBytes(body.getData().data(), body.getData().size());
    return pkt;
}

}  // namespace

TEST_CASE("GameObject questgiver notice board preserves player facing and sends no MSG_MOVE_SET_FACING",
          "[replay][gameobject][quest][facing]") {
    constexpr uint64_t kNoticeBoardGuid = 0xF11002C0E000C114ull;
    constexpr uint32_t kNoticeBoardEntry = 180448;
    constexpr uint32_t kQuestId = 8283;
    constexpr float kInitialCanonicalYaw = 0.37f;

    // Place entities at real Gadgetzan server coordinates (far from (0,0,0) so
    // MovementHandler's near-origin guard never masks outgoing MSG_MOVE_* packets).
    // Note: SMSG_UPDATE_OBJECT carries server (x, y, z); GameHandler::setPosition
    // takes canonical (x, y, z) = serverToCanonical(server) = (serverY, serverX, serverZ).
    constexpr float kServerX = -7118.87f;
    constexpr float kServerY = -3795.27f;
    constexpr float kServerZ = 8.38f;
    const glm::vec3 boardCanon = wowee::core::coords::renderToCanonical(glm::vec3(kServerX, kServerY, kServerZ));
    const glm::vec3 playerCanon = boardCanon + glm::vec3(2.5f, -1.8f, 0.0f);

    for (const bool autoFace : {false, true}) {
        DYNAMIC_SECTION("autoFaceTarget=" << autoFace) {
            const uint64_t noticeBoardGuid = kNoticeBoardGuid + (autoFace ? 1ull : 0ull);
            RenderedReplaySession s("tbc");
            auto& gh = s.handler();
            gh.setAutoFaceTarget(autoFace);

            s.receive(tbcCreateAt(
                RenderedReplaySession::kPlayerGuid, ObjectType::PLAYER,
                {{UF::UNIT_FIELD_HEALTH, 1000}, {UF::UNIT_FIELD_MAXHEALTH, 1000}, {UF::UNIT_FIELD_LEVEL, 60}},
                playerCanon.y, playerCanon.x, playerCanon.z, true));
            s.receive(tbcCreateAt(
                noticeBoardGuid, ObjectType::GAMEOBJECT,
                {{UF::OBJECT_FIELD_ENTRY, kNoticeBoardEntry},
                 {UF::GAMEOBJECT_FLAGS, 0},
                 {UF::GAMEOBJECT_DYNAMIC, 1}},
                kServerX, kServerY, kServerZ));
            s.receive(gameObjectQueryResponse(kNoticeBoardEntry, 2, "Wanted Poster: Deathclasp"));

            // Stand sideways/angled to the notice board; board yaw would be ~2.52 rad.
            s.setPlayerPose(playerCanon.x, playerCanon.y, playerCanon.z, kInitialCanonicalYaw);
            const float initialRenderYawDeg = s.renderer().getCharacterYaw();

            gh.interactWithGameObject(noticeBoardGuid);

            const auto uses = s.sent().of(Opcode::CMSG_GAMEOBJ_USE);
            REQUIRE(uses.size() == 1);
            Packet useBody = uses[0];
            CHECK(useBody.readUInt64() == noticeBoardGuid);

            CHECK(s.sent().of(Opcode::MSG_MOVE_SET_FACING).empty());
            CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());
            CHECK(gh.getMovementInfo().orientation == Catch::Approx(kInitialCanonicalYaw));
            CHECK(s.renderer().getCharacterYaw() == Catch::Approx(initialRenderYawDeg));

            // Server opens quest list on the notice board; player clicks the quest and accepts.
            s.receive(tbcQuestListPacket(noticeBoardGuid, kQuestId,
                                         "Wanted - Deathclasp, Terror of the Sands"));
            REQUIRE(gh.isGossipWindowOpen());
            gh.selectGossipQuest(kQuestId);
            REQUIRE(s.sent().of(Opcode::CMSG_QUESTGIVER_QUERY_QUEST).size() == 1);

            s.receive(tbcQuestDetailsPacket(noticeBoardGuid, kQuestId,
                                            "Wanted - Deathclasp, Terror of the Sands"));
            REQUIRE(gh.isQuestDetailsOpen());
            gh.acceptQuest();
            REQUIRE(s.sent().of(Opcode::CMSG_QUESTGIVER_ACCEPT_QUEST).size() == 1);

            CHECK(s.sent().of(Opcode::MSG_MOVE_SET_FACING).empty());
            CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());
            CHECK(gh.getMovementInfo().orientation == Catch::Approx(kInitialCanonicalYaw));
            CHECK(s.renderer().getCharacterYaw() == Catch::Approx(initialRenderYawDeg));
        }
    }
}

TEST_CASE("All usable GameObject types preserve player facing on interaction",
          "[replay][gameobject][facing]") {
    struct GoCase {
        uint32_t type;
        const char* name;
        uint64_t guid;
        uint32_t entry;
        uint32_t data7 = 0;
    };
    constexpr std::array<GoCase, 7> kCases{{
        {1,  "Alien Egg",           0xF11002ADDF02654Eull, 175567, 0},
        {3,  "Battered Chest",      0xF110000B1C001001ull, 2844,   0},
        {7,  "Bench",               0xF11002095D0043A9ull, 133469, 0},
        {9,  "The Twin Empires",    0xF11002AEFE00443Full, 175854, 1},
        {10, "Explosive Charge",    0xF1100232C1000014ull, 144065, 0},
        {19, "Mailbox",             0xF1100232F0004441ull, 144112, 0},
        {22, "Altar of Shadows",    0xF11002C800005001ull, 182256, 0},
    }};

    constexpr float kServerX = -7154.40f;
    constexpr float kServerY = -3829.52f;
    constexpr float kServerZ = 8.75f;
    const glm::vec3 goCanon = wowee::core::coords::renderToCanonical(glm::vec3(kServerX, kServerY, kServerZ));
    const glm::vec3 playerCanon = goCanon + glm::vec3(2.2f, 1.9f, 0.0f);

    RenderedReplaySession s("tbc");
    auto& gh = s.handler();
    s.receive(tbcCreateAt(
        RenderedReplaySession::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 1000}, {UF::UNIT_FIELD_MAXHEALTH, 1000}, {UF::UNIT_FIELD_LEVEL, 60}},
        playerCanon.y, playerCanon.x, playerCanon.z, true));

    constexpr float kInitialCanonicalYaw = -0.85f;
    s.setPlayerPose(playerCanon.x, playerCanon.y, playerCanon.z, kInitialCanonicalYaw);
    const float initialRenderYawDeg = s.renderer().getCharacterYaw();

    for (size_t i = 0; i < kCases.size(); ++i) {
        const auto& c = kCases[i];
        s.receive(tbcCreateAt(
            c.guid, ObjectType::GAMEOBJECT,
            {{UF::OBJECT_FIELD_ENTRY, c.entry},
             {UF::GAMEOBJECT_FLAGS, 0},
             {UF::GAMEOBJECT_DYNAMIC, 0}},
            kServerX, kServerY, kServerZ));
        s.receive(gameObjectQueryResponse(c.entry, c.type, c.name, c.data7));

        const size_t prevUses = s.sent().of(Opcode::CMSG_GAMEOBJ_USE).size();
        gh.interactWithGameObject(c.guid);
        CHECK(s.sent().of(Opcode::CMSG_GAMEOBJ_USE).size() == prevUses + 1);
        CHECK(s.sent().of(Opcode::MSG_MOVE_SET_FACING).empty());
        CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());
        CHECK(gh.getMovementInfo().orientation == Catch::Approx(kInitialCanonicalYaw));
        CHECK(s.renderer().getCharacterYaw() == Catch::Approx(initialRenderYawDeg));
    }

    // Moving player stops at current heading without snapping toward the GameObject.
    constexpr uint64_t kMovingPosterGuid = 0xF110024A3B004395ull;
    constexpr uint32_t kMovingPosterEntry = 150075;
    s.receive(tbcCreateAt(
        kMovingPosterGuid, ObjectType::GAMEOBJECT,
        {{UF::OBJECT_FIELD_ENTRY, kMovingPosterEntry},
         {UF::GAMEOBJECT_FLAGS, 0},
         {UF::GAMEOBJECT_DYNAMIC, 1}},
        kServerX, kServerY, kServerZ));
    s.receive(gameObjectQueryResponse(kMovingPosterEntry, 2, "Wanted Poster"));
    gh.movementInfoRef().flags |= static_cast<uint32_t>(wowee::game::MovementFlags::FORWARD);

    gh.interactWithGameObject(kMovingPosterGuid);
    CHECK(s.sent().of(Opcode::MSG_MOVE_STOP).size() == 1);
    CHECK(s.sent().of(Opcode::MSG_MOVE_SET_FACING).empty());
    CHECK(gh.getMovementInfo().orientation == Catch::Approx(kInitialCanonicalYaw));
    CHECK(s.renderer().getCharacterYaw() == Catch::Approx(initialRenderYawDeg));
}

TEST_CASE("Unit questgiver, gossip, vendor, and corpse loot preserve facing while autoFaceTarget only turns for combat",
          "[replay][unit][facing]") {
    constexpr uint64_t kQuestNpcGuid = 0xF130001EC4005B7Aull;
    constexpr uint64_t kDeadMobGuid  = 0xF130001530007001ull;
    constexpr uint64_t kHostileGuid  = 0xF130001530007002ull;
    constexpr float kInitialCanonicalYaw = 1.05f;
    constexpr float kServerNpcX = -7105.84f;
    constexpr float kServerNpcY = -3776.83f;
    constexpr float kServerNpcZ = 8.79f;
    const glm::vec3 npcCanon = wowee::core::coords::renderToCanonical(
        glm::vec3(kServerNpcX, kServerNpcY, kServerNpcZ));
    const glm::vec3 playerCanon = npcCanon + glm::vec3(2.4f, -1.6f, 0.0f);

    RenderedReplaySession s("tbc");
    auto& gh = s.handler();
    s.receive(tbcCreateAt(
        RenderedReplaySession::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 1000}, {UF::UNIT_FIELD_MAXHEALTH, 1000},
         {UF::UNIT_FIELD_LEVEL, 60}, {UF::UNIT_FIELD_FACTIONTEMPLATE, 1}},
        playerCanon.y, playerCanon.x, playerCanon.z, true));
    s.receive(tbcCreateAt(
        kQuestNpcGuid, ObjectType::UNIT,
        {{UF::UNIT_FIELD_HEALTH, 500}, {UF::UNIT_FIELD_MAXHEALTH, 500},
         {UF::UNIT_NPC_FLAGS, 131},  // GOSSIP | QUESTGIVER | VENDOR
         {UF::UNIT_FIELD_FACTIONTEMPLATE, 1}},
        kServerNpcX, kServerNpcY, kServerNpcZ));
    s.receive(tbcCreateAt(
        kDeadMobGuid, ObjectType::UNIT,
        {{UF::UNIT_FIELD_HEALTH, 0}, {UF::UNIT_FIELD_MAXHEALTH, 500},
         {UF::UNIT_DYNAMIC_FLAGS, 1}},
        kServerNpcX, kServerNpcY, kServerNpcZ));
    s.receive(tbcCreateAt(
        kHostileGuid, ObjectType::UNIT,
        {{UF::UNIT_FIELD_HEALTH, 500}, {UF::UNIT_FIELD_MAXHEALTH, 500},
         {UF::UNIT_FIELD_FACTIONTEMPLATE, 14}},
        kServerNpcX, kServerNpcY, kServerNpcZ));

    s.setPlayerPose(playerCanon.x, playerCanon.y, playerCanon.z, kInitialCanonicalYaw);
    const float initialRenderYawDeg = s.renderer().getCharacterYaw();

    // Service NPC interaction (questgiver / gossip / vendor): never turns player.
    gh.interactWithNpc(kQuestNpcGuid);
    CHECK(s.sent().of(Opcode::CMSG_GOSSIP_HELLO).size() == 1);
    gh.openVendor(kQuestNpcGuid);
    CHECK(s.sent().of(Opcode::CMSG_LIST_INVENTORY).size() == 1);

    // Dead unit loot: never turns player.
    gh.lootTarget(kDeadMobGuid);
    CHECK(s.sent().of(Opcode::CMSG_LOOT).size() == 1);

    // Hostile attack with TBC default autoFaceTarget=false: never turns player.
    gh.startAutoAttack(kHostileGuid);
    CHECK(s.sent().of(Opcode::CMSG_ATTACKSWING).size() == 1);
    CHECK(s.sent().of(Opcode::MSG_MOVE_SET_FACING).empty());
    CHECK(gh.getMovementInfo().orientation == Catch::Approx(kInitialCanonicalYaw));
    CHECK(s.renderer().getCharacterYaw() == Catch::Approx(initialRenderYawDeg));

    // Debug autoFaceTarget=true turns toward hostile combat target on new attack intent.
    gh.stopAutoAttack();
    gh.setAutoFaceTarget(true);
    gh.startAutoAttack(kHostileGuid);
    CHECK(s.sent().of(Opcode::MSG_MOVE_SET_FACING).size() == 1);
    const float expectedYawToHostile =
        std::atan2(-(npcCanon.y - playerCanon.y), npcCanon.x - playerCanon.x);
    CHECK(gh.getMovementInfo().orientation == Catch::Approx(expectedYawToHostile));
}
