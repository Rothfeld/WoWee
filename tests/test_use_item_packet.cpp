// CMSG_USE_ITEM SpellCastTargets encoding across expansions.
//
// Items that enchant another item (sharpening stones, weightstones, weapon oils)
// must send TARGET_FLAG_ITEM plus the target item's packed GUID; the server drops
// the cast otherwise.
#include <catch_amalgamated.hpp>
#include "game/world_packets.hpp"
#include "game/packet_parsers.hpp"
#include "core/application.hpp"

// The packet builders live in translation units that inline isActiveExpansion(),
// which reaches through the Application singleton. The builders under test never
// call it, so a null instance is enough to satisfy the linker.
namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

// Packet::getData() is the body only - the opcode is held separately - so these
// offsets are into the CMSG_USE_ITEM payload.
using Bytes = std::vector<uint8_t>;

TEST_CASE("CMSG_USE_ITEM sends TARGET_FLAG_ITEM for item-targeted spells", "[use_item]") {
    constexpr uint64_t kItemGuid   = 0x0000000000001234ull;  // the stone
    constexpr uint64_t kTargetGuid = 0x0000000000005678ull;  // the weapon it sharpens
    constexpr uint32_t kSpellId    = 2828;                   // Sharpen Blade

    SECTION("WotLK: uint32 mask 0x10 + packed target GUID") {
        auto packet = UseItemPacket::build(0xFF, 23, kItemGuid, kSpellId, 0, kTargetGuid);
        const Bytes& bytes = packet.getData();

        // bag(1) slot(1) castCount(1) spellId(4) itemGuid(8) glyph(4) castFlags(1) = 20
        REQUIRE(bytes.size() > 20);
        const uint8_t* targets = bytes.data() + 20;
        uint32_t mask = targets[0] | (targets[1] << 8) | (targets[2] << 16) | (targets[3] << 24);
        REQUIRE(mask == 0x10);
        // Packed GUID: byte mask then the non-zero bytes, little end first.
        REQUIRE(targets[4] == 0x03);   // bytes 0 and 1 present (0x78, 0x56)
        REQUIRE(targets[5] == 0x78);
        REQUIRE(targets[6] == 0x56);
    }

    SECTION("Classic: uint16 mask 0x10 + packed target GUID") {
        ClassicPacketParsers parsers;
        auto packet = parsers.buildUseItem(0xFF, 23, kItemGuid, kSpellId, 0, kTargetGuid);
        const Bytes& bytes = packet.getData();

        // bag(1) slot(1) spellIndex(1) = 3
        REQUIRE(bytes.size() > 3);
        const uint8_t* targets = bytes.data() + 3;
        uint16_t mask = static_cast<uint16_t>(targets[0] | (targets[1] << 8));
        REQUIRE(mask == 0x10);
        REQUIRE(targets[2] == 0x03);
        REQUIRE(targets[3] == 0x78);
        REQUIRE(targets[4] == 0x56);
    }

    SECTION("TBC: uint32 mask 0x10 + packed target GUID") {
        TbcPacketParsers parsers;
        auto packet = parsers.buildUseItem(0xFF, 23, kItemGuid, kSpellId, 0, kTargetGuid);
        const Bytes& bytes = packet.getData();

        // bag(1) slot(1) spellIndex(1) castCount(1) itemGuid(8) = 12
        REQUIRE(bytes.size() > 12);
        const uint8_t* targets = bytes.data() + 12;
        uint32_t mask = targets[0] | (targets[1] << 8) | (targets[2] << 16) | (targets[3] << 24);
        REQUIRE(mask == 0x10);
        REQUIRE(targets[4] == 0x03);
        REQUIRE(targets[5] == 0x78);
        REQUIRE(targets[6] == 0x56);
    }
}

TEST_CASE("CMSG_USE_ITEM keeps unit/self targeting when no item target", "[use_item]") {
    constexpr uint64_t kItemGuid   = 0x0000000000001234ull;
    constexpr uint64_t kPlayerGuid = 0x0000000000000042ull;

    SECTION("unit target still uses TARGET_FLAG_UNIT") {
        auto packet = UseItemPacket::build(0xFF, 23, kItemGuid, 1234, kPlayerGuid, 0);
        const Bytes& bytes = packet.getData();
        REQUIRE(bytes.size() > 20);
        const uint8_t* targets = bytes.data() + 20;
        uint32_t mask = targets[0] | (targets[1] << 8) | (targets[2] << 16) | (targets[3] << 24);
        REQUIRE(mask == 0x02);
    }

    SECTION("no target still uses TARGET_FLAG_SELF") {
        auto packet = UseItemPacket::build(0xFF, 23, kItemGuid, 1234, 0, 0);
        const Bytes& bytes = packet.getData();
        REQUIRE(bytes.size() > 20);
        const uint8_t* targets = bytes.data() + 20;
        uint32_t mask = targets[0] | (targets[1] << 8) | (targets[2] << 16) | (targets[3] << 24);
        REQUIRE(mask == 0x00);
    }
}

TEST_CASE("CMSG_ALTER_APPEARANCE uses the WotLK four-field payload", "[barber]") {
    auto packet = AlterAppearancePacket::build(101, 7, 205, 309);
    REQUIRE(packet.getData().size() == 16);
    REQUIRE(packet.readUInt32() == 101);
    REQUIRE(packet.readUInt32() == 7);
    REQUIRE(packet.readUInt32() == 205);
    REQUIRE(packet.readUInt32() == 309);
    REQUIRE_FALSE(packet.hasData());
}

TEST_CASE("SMSG_MESSAGECHAT whispers use the exact layout for every expansion", "[chat]") {
    constexpr uint64_t kSenderGuid = 0x42ull;
    constexpr uint64_t kReceiverGuid = 0x99ull;
    const std::string message = "meet me at the tram";

    auto buildTbcOrWotlk = [&] {
        wowee::network::Packet packet(0);
        packet.writeUInt8(static_cast<uint8_t>(ChatType::WHISPER));
        packet.writeUInt32(static_cast<uint32_t>(ChatLanguage::UNIVERSAL));
        packet.writeUInt64(kSenderGuid);
        packet.writeUInt32(0);
        packet.writeUInt64(kReceiverGuid);
        packet.writeUInt32(static_cast<uint32_t>(message.size() + 1));
        packet.writeString(message);
        packet.writeUInt8(0x01);
        return packet;
    };

    auto verifySharedResult = [&](const MessageChatData& parsed) {
        REQUIRE(parsed.type == ChatType::WHISPER);
        REQUIRE(parsed.senderGuid == kSenderGuid);
        REQUIRE(parsed.receiverGuid == kReceiverGuid);
        REQUIRE(parsed.message == message);
        REQUIRE(parsed.senderName.empty());
        REQUIRE(parsed.receiverName.empty());
        REQUIRE(parsed.chatTag == 0x01);
    };

    SECTION("WotLK") {
        auto packet = buildTbcOrWotlk();
        MessageChatData parsed;
        REQUIRE(MessageChatParser::parse(packet, parsed));
        verifySharedResult(parsed);
        REQUIRE_FALSE(packet.hasData());
    }

    SECTION("TBC") {
        auto packet = buildTbcOrWotlk();
        MessageChatData parsed;
        TbcPacketParsers parsers;
        REQUIRE(parsers.parseMessageChat(packet, parsed));
        verifySharedResult(parsed);
        REQUIRE_FALSE(packet.hasData());
    }

    SECTION("Classic") {
        wowee::network::Packet packet(0);
        packet.writeUInt8(static_cast<uint8_t>(ChatType::WHISPER));
        packet.writeUInt32(static_cast<uint32_t>(ChatLanguage::UNIVERSAL));
        packet.writeUInt64(kSenderGuid);
        packet.writeUInt32(static_cast<uint32_t>(message.size() + 1));
        packet.writeString(message);
        packet.writeUInt8(0x01);

        MessageChatData parsed;
        ClassicPacketParsers parsers;
        REQUIRE(parsers.parseMessageChat(packet, parsed));
        REQUIRE(parsed.type == ChatType::WHISPER);
        REQUIRE(parsed.senderGuid == kSenderGuid);
        REQUIRE(parsed.message == message);
        REQUIRE(parsed.chatTag == 0x01);
        REQUIRE_FALSE(packet.hasData());
    }
}

TEST_CASE("WotLK guild achievement chat retains its trailing achievement ID",
          "[chat][achievement]") {
    constexpr uint64_t kSenderGuid = 0x42ull;
    constexpr uint64_t kReceiverGuid = 0x99ull;
    constexpr uint32_t kAchievementId = 2136u;
    const std::string message = "%s has earned the achievement $a";

    wowee::network::Packet packet(0);
    packet.writeUInt8(static_cast<uint8_t>(ChatType::GUILD_ACHIEVEMENT));
    packet.writeUInt32(static_cast<uint32_t>(ChatLanguage::UNIVERSAL));
    packet.writeUInt64(kSenderGuid);
    packet.writeUInt32(0);
    packet.writeUInt64(kReceiverGuid);
    packet.writeUInt32(static_cast<uint32_t>(message.size() + 1));
    packet.writeString(message);
    packet.writeUInt8(0);
    packet.writeUInt32(kAchievementId);

    MessageChatData parsed;
    REQUIRE(MessageChatParser::parse(packet, parsed));
    REQUIRE(parsed.type == ChatType::GUILD_ACHIEVEMENT);
    REQUIRE(parsed.senderGuid == kSenderGuid);
    REQUIRE(parsed.message == message);
    REQUIRE(parsed.achievementId == kAchievementId);
    REQUIRE_FALSE(packet.hasData());
}

TEST_CASE("Addon chat is identified before it reaches visible whisper history", "[chat][addon]") {
    MessageChatData addon;
    addon.type = ChatType::WHISPER;
    addon.language = ChatLanguage::ADDON;
    addon.message = "TRP3\tHI_NI\t1";

    std::string prefix;
    std::string payload;
    REQUIRE(decodeAddonChatPayload(addon, prefix, payload));
    REQUIRE(prefix == "TRP3");
    REQUIRE(payload == "HI_NI\t1");

    SECTION("legacy spoken-language addon envelopes remain hidden") {
        addon.language = ChatLanguage::COMMON;
        REQUIRE(decodeAddonChatPayload(addon, prefix, payload));
    }

    SECTION("ordinary whispers remain player chat") {
        addon.language = ChatLanguage::COMMON;
        addon.message = "meet me at the tram";
        REQUIRE_FALSE(decodeAddonChatPayload(addon, prefix, payload));
    }

    SECTION("tabs in ordinary say messages remain player chat") {
        addon.type = ChatType::SAY;
        addon.language = ChatLanguage::COMMON;
        addon.message = "hello\tthere";
        REQUIRE_FALSE(decodeAddonChatPayload(addon, prefix, payload));
    }

    SECTION("outgoing addon packets use LANG_ADDON") {
        auto packet = MessageChatPacket::build(
            ChatType::WHISPER, ChatLanguage::ADDON, "TRP3\tHI_NI\t1", "Frezha");
        REQUIRE(packet.readUInt32() == static_cast<uint32_t>(ChatType::WHISPER));
        // The literal, not the enum. Reading back the value the packet was
        // built from is true whatever that value is, and the language matters
        // here: the server treats a client message in LANG_UNIVERSAL as a
        // hacking attempt and discards it, so an addon message sent in
        // anything but 0xFFFFFFFF never arrives.
        REQUIRE(packet.readUInt32() == 0xFFFFFFFFu);
        REQUIRE(packet.readString() == "Frezha");
        REQUIRE(packet.readString() == "TRP3\tHI_NI\t1");
        REQUIRE_FALSE(packet.hasData());
    }
}

// A monster line addressed to something that is neither a player nor a pet
// carries the target's name, which we skip past. The length that says how far
// to skip comes from the server, and it was taken on trust.
TEST_CASE("A monster chat line cannot skip past the end of its own packet", "[chat]") {
    constexpr uint64_t kSenderGuid = 0x0042ull;
    // A creature GUID: not a player (high 0x0000), not a pet (0xF040/0xF014),
    // which is what sends the parser looking for a receiver name at all.
    constexpr uint64_t kCreatureGuid = 0xF130000000001234ull;
    const std::string sender = "Gazlowe";
    const std::string message = "Get that shipment moving!";

    auto build = [&](uint32_t recvNameLen, const std::string& recvName) {
        wowee::network::Packet packet(0);
        packet.writeUInt8(static_cast<uint8_t>(ChatType::MONSTER_SAY));
        packet.writeUInt32(static_cast<uint32_t>(ChatLanguage::UNIVERSAL));
        packet.writeUInt64(kSenderGuid);
        packet.writeUInt32(0);
        packet.writeUInt32(static_cast<uint32_t>(sender.size() + 1));
        packet.writeString(sender);
        packet.writeUInt64(kCreatureGuid);
        packet.writeUInt32(recvNameLen);
        if (!recvName.empty()) packet.writeString(recvName);
        packet.writeUInt32(static_cast<uint32_t>(message.size() + 1));
        packet.writeString(message);
        packet.writeUInt8(0);
        return packet;
    };

    SECTION("an honest one parses, with the name skipped") {
        const std::string target = "Sputtervalve";
        auto packet = build(static_cast<uint32_t>(target.size() + 1), target);
        MessageChatData parsed;
        REQUIRE(MessageChatParser::parse(packet, parsed));
        REQUIRE(parsed.senderName == sender);
        REQUIRE(parsed.receiverGuid == kCreatureGuid);
        REQUIRE(parsed.message == message);
    }

    SECTION("an absurd length is refused") {
        // Not merely unskipped: a length of 256 or more was skipped by nothing
        // at all, so the message length below read the name's own bytes and
        // the line came out of the wrong part of the packet. Refused now.
        auto packet = build(0xFFFFu, "");
        MessageChatData parsed;
        REQUIRE_FALSE(MessageChatParser::parse(packet, parsed));
    }

    SECTION("and so is one that merely overruns the remaining bytes") {
        // This is the one that got through. A length under 256 was skipped on
        // trust, setReadPos walked off the end, the message length then read a
        // clamped zero, and that zero passed its own bounds test - so the
        // interface was handed an empty monster line no server had sent.
        const std::string target = "Sputtervalve";
        auto packet = build(static_cast<uint32_t>(target.size() + 40), target);
        MessageChatData parsed;
        REQUIRE_FALSE(MessageChatParser::parse(packet, parsed));
    }

    SECTION("a name that eats the packet exactly is refused too") {
        // The edge the remaining-size check alone leaves open: a length equal
        // to what is left lands the read position on the end rather than past
        // it, so the skip is legal by that test. The message length then reads
        // the same clamped zero and delivers the same empty line - which is
        // why the length is asked for before it is read.
        //
        // The tail after the name field is the message length (4), the message
        // and its terminator, and the chat tag.
        auto packet = build(static_cast<uint32_t>(message.size() + 6), "");
        MessageChatData parsed;
        REQUIRE_FALSE(MessageChatParser::parse(packet, parsed));
    }
}

#include "game/game_object_lock.hpp"

// A key opens its lock with its own opening spell. Tainted Core 31088
// (item_template): slot 0 Throw Key 38134 (CREATE_ITEM), slot 1 Opening 3366
// (OPEN_LOCK_ITEM), slot 2 Paralyze 38132 (not on use). Original 2.4.3 uses
// the core at Shield Generator 185051 and the core is gone (pair 17).
TEST_CASE("a key is used through the item spell that opens locks", "[use_item][go-lock]") {
    std::array<ItemQueryResponseData::ItemSpell, 5> core{};
    core[0].spellId = 38134; core[0].spellTrigger = 0;
    core[1].spellId = 3366;  core[1].spellTrigger = 0;
    core[2].spellId = 38132; core[2].spellTrigger = 1;
    const auto effects = [](uint32_t spellId) {
        switch (spellId) {
            case 38134: return std::array<uint32_t, 3>{24, 0, 0};
            case 3366:  return std::array<uint32_t, 3>{59, 0, 0};
            case 38132: return std::array<uint32_t, 3>{6, 0, 0};
            default:    return std::array<uint32_t, 3>{};
        }
    };
    REQUIRE(keyOpeningSpellIndex(core, effects) == std::optional<uint8_t>{1});

    std::array<ItemQueryResponseData::ItemSpell, 5> crescentKey{};
    crescentKey[0].spellId = 3366;
    CHECK(keyOpeningSpellIndex(crescentKey, effects) == std::optional<uint8_t>{0});

    std::array<ItemQueryResponseData::ItemSpell, 5> throwOnly{};
    throwOnly[0].spellId = 38134;
    CHECK_FALSE(keyOpeningSpellIndex(throwOnly, effects).has_value());

    SECTION("TBC CMSG_USE_ITEM names the slot the server casts") {
        TbcPacketParsers parsers;
        constexpr uint64_t kCore = 0x4000000000001234ull, kGenerator = 0xF11002D2DB539E5Aull;
        auto packet = parsers.buildUseItem(0xFF, 23, kCore, 3366, 0, 0, kGenerator, 1);
        const Bytes& bytes = packet.getData();
        // bag(1) slot(1) spellIndex(1) castCount(1) itemGuid(8) targetMask(4)
        REQUIRE(bytes.size() > 16);
        CHECK(bytes[2] == 1);
        const uint32_t mask = bytes[12] | (bytes[13] << 8) | (bytes[14] << 16) | (bytes[15] << 24);
        CHECK(mask == 0x0800);
    }
}

TEST_CASE("a lock's slots apply by the object's state and GO_FLAG_LOCKED", "[go-lock]") {
    // Shut objects use Action 1 when flagged, 0 otherwise; open ones Action 2.
    CHECK(lockActionNow(1, true) == 1);
    CHECK(lockActionNow(1, false) == 0);
    CHECK(lockActionNow(kGoStateActive, true) == 2);
    CHECK(lockActionNow(kGoStateActive, false) == 2);

    // Lock 1718 (Shield Generator): slot 0 key Tainted Core, Action 0.
    LockRecord generator;
    generator.type[0] = kLockKeyItem; generator.index[0] = 31088;
    CHECK(lockKeyItems(generator, 0) == std::vector<uint32_t>{31088});
    CHECK(lockKeyItems(generator, 1).empty());

    // Lock 879 (Gauntlet Gate) and 1562 (Dire Maul's Door 177198): key with
    // Action 1, Lockpicking 300, Quick Open, Quick Close, Blasting 300.
    LockRecord gate;
    gate.type = {kLockKeyItem, kLockKeySkill, kLockKeySkill, kLockKeySkill, kLockKeySkill, 0, 0, 0};
    gate.index = {12382, 1, 10, 11, 16, 0, 0, 0};
    gate.skill = {0, 300, 0, 0, 300, 0, 0, 0};
    gate.action = {1, 1, 0, 2, 1, 0, 0, 0};
    // Shut and flagged (175357): "Requires Key to the City"; Lockpicking and
    // Blasting are the skill slots.
    CHECK(lockKeyItems(gate, 1) == std::vector<uint32_t>{12382});
    CHECK(lockSkillSlots(gate, 1) == std::vector<uint32_t>{1, 4});
    // Shut and unflagged (175356): Quick Open only, no key line.
    CHECK(lockKeyItems(gate, 0).empty());
    CHECK(lockSkillSlots(gate, 0) == std::vector<uint32_t>{2});
    // Open (King's Square Gate spawned): Quick Close only.
    CHECK(lockKeyItems(gate, 2).empty());
    CHECK(lockSkillSlots(gate, 2) == std::vector<uint32_t>{3});
}

TEST_CASE("lock line colours follow the rank over the lock's Skill", "[go-lock]") {
    // Lucius's Lockbox, Lockpicking 25 (pair 22).
    CHECK(lockDifficulty(20, 25) == LockDifficulty::Impossible);
    CHECK(lockDifficultyColour(LockDifficulty::Impossible).g == 0.125f);
    CHECK(lockDifficulty(30, 25) == LockDifficulty::Orange);
    CHECK(lockDifficulty(120, 25) == LockDifficulty::Green);
    CHECK(lockDifficulty(200, 25) == LockDifficulty::Trivial);
    CHECK(lockDifficultyColour(LockDifficulty::Trivial).r == 0.5f);
    // Quick Close at an open gate: Closing's 100 points against Skill 0.
    CHECK(lockDifficulty(100, 0) == LockDifficulty::Trivial);
}
