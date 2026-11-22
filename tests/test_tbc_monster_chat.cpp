// TBC 2.4.3 SMSG_MESSAGECHAT wire parsing for monster, raid-boss, and foreign whisper types.
// Matches CMaNGOS-TBC ChatHandler::BuildChatPacket (Chat.cpp:3682-3748).
#include <catch_amalgamated.hpp>

#include "game/packet_parsers.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

using namespace wowee::game;

namespace {

wowee::network::Packet buildTbcMonsterChat(ChatType type,
                                           uint64_t senderGuid,
                                           const std::string& senderName,
                                           uint64_t targetGuid,
                                           const std::string& targetName,
                                           const std::string& message,
                                           uint8_t chatTag = 0) {
    wowee::network::Packet p(0x096);
    p.writeUInt8(static_cast<uint8_t>(type));
    p.writeUInt32(0); // LANG_UNIVERSAL
    p.writeUInt64(senderGuid);
    p.writeUInt32(0); // 2.1.0 unknown

    p.writeUInt32(static_cast<uint32_t>(senderName.size() + 1));
    p.writeString(senderName);

    p.writeUInt64(targetGuid);
    const bool isPlayer = (targetGuid != 0) && ((targetGuid >> 48) == 0x0000);
    if (targetGuid != 0 && !isPlayer && type != ChatType::WHISPER_FOREIGN) {
        p.writeUInt32(static_cast<uint32_t>(targetName.size() + 1));
        p.writeString(targetName);
    }

    p.writeUInt32(static_cast<uint32_t>(message.size() + 1));
    p.writeString(message);
    p.writeUInt8(chatTag);
    return p;
}

} // namespace

TEST_CASE("TBC SMSG_MESSAGECHAT parses all monster and raid-boss types with player target", "[chat][tbc][packet]") {
    constexpr uint64_t kBossGuid = 0xF13000624E596E69ULL;
    constexpr uint64_t kPlayerGuid = 0x0000000000000027ULL;

    TbcPacketParsers parsers;
    const ChatType types[] = {
        ChatType::MONSTER_SAY,
        ChatType::MONSTER_PARTY,
        ChatType::MONSTER_YELL,
        ChatType::MONSTER_WHISPER,
        ChatType::MONSTER_EMOTE,
        ChatType::RAID_BOSS_EMOTE,
        ChatType::RAID_BOSS_WHISPER,
    };

    for (ChatType type : types) {
        auto pkt = buildTbcMonsterChat(
            type, kBossGuid, "Grand Warlock Alythess", kPlayerGuid, "",
            "Alythess directs Conflagration at $n.");
        MessageChatData data;
        REQUIRE(parsers.parseMessageChat(pkt, data));
        CHECK(data.type == type);
        CHECK(data.senderGuid == kBossGuid);
        CHECK(data.senderName == "Grand Warlock Alythess");
        CHECK(data.receiverGuid == kPlayerGuid);
        CHECK(data.receiverName.empty());
        CHECK(data.message == "Alythess directs Conflagration at $n.");
        CHECK(data.chatTag == 0);
        CHECK_FALSE(pkt.hasData());
    }
}

TEST_CASE("TBC SMSG_MESSAGECHAT parses creature, pet, and zero targets without desync", "[chat][tbc][packet]") {
    constexpr uint64_t kBossGuid = 0xF13000624E596E69ULL;
    constexpr uint64_t kNpcTargetGuid = 0xF130000064000001ULL;
    constexpr uint64_t kPetTargetGuid = 0xF140000064000001ULL;

    TbcPacketParsers parsers;

    SECTION("Creature target carries targetNameLen + targetName") {
        auto pkt = buildTbcMonsterChat(
            ChatType::RAID_BOSS_EMOTE, kBossGuid, "Grand Warlock Alythess",
            kNpcTargetGuid, "TargetNPC", "Boss targets $n.");
        MessageChatData data;
        REQUIRE(parsers.parseMessageChat(pkt, data));
        CHECK(data.receiverGuid == kNpcTargetGuid);
        CHECK(data.receiverName == "TargetNPC");
        CHECK(data.message == "Boss targets $n.");
        CHECK_FALSE(pkt.hasData());
    }

    SECTION("Pet target carries empty targetName (len=1, NUL) in CMaNGOS-TBC") {
        auto pkt = buildTbcMonsterChat(
            ChatType::RAID_BOSS_EMOTE, kBossGuid, "Grand Warlock Alythess",
            kPetTargetGuid, "", "Pet target $n!");
        MessageChatData data;
        REQUIRE(parsers.parseMessageChat(pkt, data));
        CHECK(data.receiverGuid == kPetTargetGuid);
        CHECK(data.receiverName.empty());
        CHECK(data.message == "Pet target $n!");
        CHECK_FALSE(pkt.hasData());
    }

    SECTION("Zero target omits targetName") {
        auto pkt = buildTbcMonsterChat(
            ChatType::RAID_BOSS_EMOTE, kBossGuid, "Grand Warlock Alythess",
            0, "", "Zero target $n!");
        MessageChatData data;
        REQUIRE(parsers.parseMessageChat(pkt, data));
        CHECK(data.receiverGuid == 0);
        CHECK(data.receiverName.empty());
        CHECK(data.message == "Zero target $n!");
        CHECK_FALSE(pkt.hasData());
    }

    SECTION("WHISPER_FOREIGN carries senderName + targetGuid without targetName") {
        auto pkt = buildTbcMonsterChat(
            ChatType::WHISPER_FOREIGN, kBossGuid, "ForeignGM",
            kNpcTargetGuid, "ignored", "Cross-faction whisper");
        MessageChatData data;
        REQUIRE(parsers.parseMessageChat(pkt, data));
        CHECK(data.type == ChatType::WHISPER_FOREIGN);
        CHECK(data.senderName == "ForeignGM");
        CHECK(data.receiverGuid == kNpcTargetGuid);
        CHECK(data.receiverName.empty());
        CHECK(data.message == "Cross-faction whisper");
        CHECK_FALSE(pkt.hasData());
    }
}
