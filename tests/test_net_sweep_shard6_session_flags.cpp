// TBC 2.4.3 Network Wire-Payload Sweep - Shard 6 unit & packet tests:
// - Part A parsers & formatting: SMSG_DEFENSE_MESSAGE, SMSG_KICK_REASON,
//   SMSG_ACCOUNT_DATA_TIMES (128-byte MD5 vs WotLK timestamps),
//   SMSG_AUTH_RESPONSE (AUTH_OK / AUTH_WAIT_QUEUE / queue updates),
//   SMSG_SERVER_MESSAGE (empty-text SHUTDOWN_CANCELLED / RESTART_CANCELLED),
//   SMSG_TUTORIAL_FLAGS / CMSG_TUTORIAL_FLAG bitfield rules.
// - Part B UpdateFields.h indices in Data/expansions/tbc/update_fields.json
//   and bitflag domain rules across CreatureTypeFlags, UnitFlags, UnitFlags2,
//   UnitDynFlags, and NPCFlags (+ world_cursor.hpp).

#include <catch_amalgamated.hpp>

#include "game/session_flag_rules.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include "ui/world_cursor.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace wowee::game;
using wowee::network::Packet;

namespace {

#ifdef WOWEE_SOURCE_DIR
const std::string kSourceDir = std::string(WOWEE_SOURCE_DIR);
#else
const std::string kSourceDir = ".";
#endif

} // namespace

TEST_CASE("SMSG_DEFENSE_MESSAGE parser reads zoneId + messageLength + cstring", "[shard6][packets]") {
    const std::string msg = "The Crossroads is under attack!";
    Packet p(0x33A);
    p.writeUInt32(17u); // The Barrens
    p.writeUInt32(static_cast<uint32_t>(msg.size() + 1));
    p.writeString(msg);
    p.setReadPos(0);

    DefenseMessageData parsed;
    REQUIRE(DefenseMessageParser::parse(p, parsed));
    CHECK(parsed.zoneId == 17u);
    CHECK(parsed.messageLength == msg.size() + 1);
    CHECK(parsed.message == msg);
    CHECK(formatDefenseMessage("The Barrens", parsed.message) ==
          "[LocalDefense - The Barrens] The Crossroads is under attack!");
    CHECK(formatDefenseMessage("", parsed.message) ==
          "[Defense] The Crossroads is under attack!");
}

TEST_CASE("SMSG_KICK_REASON parses TBC uint8 reason + cstring and WotLK 12-byte fallback", "[shard6][packets]") {
    SECTION("TBC short reason (< 12 bytes) is parsed instead of dropped") {
        Packet p(0x3C4);
        p.writeUInt8(3u);
        p.writeString("GM kick");
        p.setReadPos(0);

        KickReasonData kick;
        REQUIRE(KickReasonParser::parse(p, kick, true));
        CHECK(kick.isTbcFormat);
        CHECK(kick.reason == 3u);
        CHECK(kick.text == "GM kick");
        CHECK(formatTbcKickReason(kick.reason, kick.text) == "Disconnected by server: GM kick");
    }

    SECTION("TBC long reason (>= 12 bytes) preserves full text without consuming 11 prefix bytes") {
        const std::string text = "Account suspended by administrator";
        Packet p(0x3C4);
        p.writeUInt8(1u);
        p.writeString(text);
        p.setReadPos(0);

        KickReasonData kick;
        REQUIRE(KickReasonParser::parse(p, kick, true));
        CHECK(kick.reason == 1u);
        CHECK(kick.text == text);
    }

    SECTION("TBC empty text formats numeric reason code") {
        Packet p(0x3C4);
        p.writeUInt8(7u);
        p.writeString("");
        p.setReadPos(0);

        KickReasonData kick;
        REQUIRE(KickReasonParser::parse(p, kick, true));
        CHECK(kick.reason == 7u);
        CHECK(kick.text.empty());
        CHECK(formatTbcKickReason(kick.reason, kick.text) == "Disconnected by server (reason 7).");
    }

    SECTION("WotLK 12-byte group-kick layout still parses when isPreWotlk is false") {
        Packet p(0x3C5);
        p.writeUInt64(0x1122334455667788ull);
        p.writeUInt32(2u);
        p.writeString("AFK in dungeon");
        p.setReadPos(0);

        KickReasonData kick;
        REQUIRE(KickReasonParser::parse(p, kick, false));
        CHECK_FALSE(kick.isTbcFormat);
        CHECK(kick.kickerGuid == 0x1122334455667788ull);
        CHECK(kick.reasonType == 2u);
        CHECK(kick.text == "AFK in dungeon");
    }
}

TEST_CASE("SMSG_ACCOUNT_DATA_TIMES parses TBC 128-byte MD5 array and WotLK timestamps", "[shard6][packets]") {
    SECTION("TBC 8x16 MD5 digest payload") {
        Packet p(0x209);
        for (int slot = 0; slot < 8; ++slot) {
            for (int b = 0; b < 16; ++b) {
                uint8_t val = 0;
                if (slot == 0) val = static_cast<uint8_t>(0xA0 + b);
                if (slot == 4) val = static_cast<uint8_t>(0x40 + b);
                p.writeUInt8(val);
            }
        }
        REQUIRE(p.getSize() == 128);
        p.setReadPos(0);

        AccountDataTimesData data;
        REQUIRE(AccountDataTimesParser::parse(p, data, true));
        CHECK(data.isTbcMd5Layout);
        CHECK(data.mask == ((1u << 0) | (1u << 4)));
        CHECK(data.accountDataMd5[0][0] == 0xA0);
        CHECK(data.accountDataMd5[0][15] == 0xAF);
        CHECK(data.accountDataMd5[4][0] == 0x40);
        CHECK(data.accountDataMd5[1][0] == 0x00);
        CHECK(p.getRemainingSize() == 0);
    }

    SECTION("WotLK timestamp payload") {
        Packet p(0x209);
        p.writeUInt32(1700000000u);
        p.writeUInt8(1u);
        p.writeUInt32(0x15u);
        for (uint32_t i = 0; i < 8; ++i) {
            p.writeUInt32(1000u + i);
        }
        p.setReadPos(0);

        AccountDataTimesData data;
        REQUIRE(AccountDataTimesParser::parse(p, data, false));
        CHECK_FALSE(data.isTbcMd5Layout);
        CHECK(data.serverTime == 1700000000u);
        CHECK(data.unknown == 1u);
        CHECK(data.mask == 0x15u);
        CHECK(data.accountDataTimes[0] == 1000u);
        CHECK(data.accountDataTimes[7] == 1007u);
    }
}

TEST_CASE("SMSG_AUTH_RESPONSE parses SendAuthOk, SendAuthQueued, and SendAuthWaitQue", "[shard6][packets]") {
    SECTION("SendAuthOk (11 bytes) extracts billing fields and TBC expansion=1") {
        Packet p(0x1EE);
        p.writeUInt8(static_cast<uint8_t>(AuthResult::OK));
        p.writeUInt32(3600u); // BillingTimeRemaining
        p.writeUInt8(2u);     // BillingPlanFlags
        p.writeUInt32(1800u); // BillingTimeRested
        p.writeUInt8(1u);     // Expansion = TBC
        p.setReadPos(0);

        AuthResponseData resp;
        REQUIRE(AuthResponseParser::parse(p, resp));
        CHECK(resp.isSuccess());
        CHECK_FALSE(resp.isQueued());
        CHECK(resp.hasBillingInfo);
        CHECK(resp.billingTimeRemaining == 3600u);
        CHECK(resp.billingPlanFlags == 2u);
        CHECK(resp.billingTimeRested == 1800u);
        CHECK(resp.expansion == 1u);
    }

    SECTION("SendAuthQueued (15 bytes) extracts billing, expansion, and queuePosition") {
        Packet p(0x1EE);
        p.writeUInt8(static_cast<uint8_t>(AuthResult::WAIT_QUEUE));
        p.writeUInt32(0u);
        p.writeUInt8(0u);
        p.writeUInt32(0u);
        p.writeUInt8(1u);
        p.writeUInt32(42u);
        p.setReadPos(0);

        AuthResponseData resp;
        REQUIRE(AuthResponseParser::parse(p, resp));
        CHECK(resp.isQueued());
        CHECK_FALSE(resp.isSuccess());
        CHECK(resp.hasBillingInfo);
        CHECK(resp.expansion == 1u);
        CHECK(resp.queuePosition == 42u);
    }

    SECTION("SendAuthWaitQue update (5 bytes) and release (1 byte)") {
        Packet pUpdate(0x1EE);
        pUpdate.writeUInt8(static_cast<uint8_t>(AuthResult::WAIT_QUEUE));
        pUpdate.writeUInt32(9u);
        pUpdate.setReadPos(0);

        AuthResponseData upd;
        REQUIRE(AuthResponseParser::parse(pUpdate, upd));
        CHECK(upd.isQueued());
        CHECK_FALSE(upd.hasBillingInfo);
        CHECK(upd.queuePosition == 9u);

        Packet pRelease(0x1EE);
        pRelease.writeUInt8(static_cast<uint8_t>(AuthResult::OK));
        pRelease.setReadPos(0);

        AuthResponseData rel;
        REQUIRE(AuthResponseParser::parse(pRelease, rel));
        CHECK(rel.isSuccess());
        CHECK_FALSE(rel.hasBillingInfo);
    }
}

TEST_CASE("SMSG_SERVER_MESSAGE formats empty-text shutdown and restart cancellations", "[shard6][chat]") {
    const auto shutdownCancel = formatServerMessage(4u, "");
    CHECK(shutdownCancel.chatLine == "[Shutdown cancelled]");
    CHECK(shutdownCancel.uiError == "Server shutdown cancelled.");

    const auto restartCancel = formatServerMessage(5u, "");
    CHECK(restartCancel.chatLine == "[Restart cancelled]");
    CHECK(restartCancel.uiError == "Server restart cancelled.");

    const auto shutdownTimer = formatServerMessage(1u, "5:00");
    CHECK(shutdownTimer.chatLine == "[Shutdown] 5:00");
    CHECK(shutdownTimer.uiError == "Server shutdown: 5:00");

    const auto emptyCustom = formatServerMessage(3u, "");
    CHECK(emptyCustom.chatLine.empty());
    CHECK(emptyCustom.uiError.empty());
}

TEST_CASE("Tutorial bitfield helpers match CMaNGOS TBC 8x32 layout", "[shard6][tutorials]") {
    std::array<uint32_t, 8> words{};
    CHECK_FALSE(isTutorialBitSet(words, 0));
    CHECK_FALSE(isTutorialBitSet(words, 1));
    CHECK_FALSE(isTutorialBitSet(words, 257));

    // Tutorial 1 -> word 0 bit 0 (wire index 0)
    const auto first = setTutorialBit(words, 1);
    REQUIRE(first.has_value());
    CHECK(*first == 0u);
    CHECK(words[0] == 0x1u);
    CHECK(isTutorialBitSet(words, 1));
    // Duplicate set returns nullopt (no redundant CMSG_TUTORIAL_FLAG)
    CHECK_FALSE(setTutorialBit(words, 1).has_value());

    // Tutorial 33 -> word 1 bit 0 (wire index 32)
    const auto thirtyThird = setTutorialBit(words, 33);
    REQUIRE(thirtyThird.has_value());
    CHECK(*thirtyThird == 32u);
    CHECK(words[1] == 0x1u);

    // Tutorial 256 -> word 7 bit 31 (wire index 255)
    const auto last = setTutorialBit(words, 256);
    REQUIRE(last.has_value());
    CHECK(*last == 255u);
    CHECK(words[7] == 0x80000000u);

    const auto all = flaggedTutorialIds(words);
    REQUIRE(all == std::vector<int>{1, 33, 256});
}

TEST_CASE("All 39 newly mapped TBC UpdateFields match CMaNGOS TBC UpdateFields.h", "[shard6][updatefields]") {
    UpdateFieldTable table;
    REQUIRE(table.loadFromJson(kSourceDir + "/Data/expansions/tbc/update_fields.json"));

    // Corpse fields (OBJECT_END = 6)
    CHECK(table.index(UF::CORPSE_FIELD_OWNER) == 6);
    CHECK(table.index(UF::CORPSE_FIELD_PARTY) == 8);
    CHECK(table.index(UF::CORPSE_FIELD_FACING) == 10);
    CHECK(table.index(UF::CORPSE_FIELD_POS_X) == 11);
    CHECK(table.index(UF::CORPSE_FIELD_POS_Y) == 12);
    CHECK(table.index(UF::CORPSE_FIELD_POS_Z) == 13);
    CHECK(table.index(UF::CORPSE_FIELD_DISPLAY_ID) == 14);
    CHECK(table.index(UF::CORPSE_FIELD_ITEM) == 15);
    CHECK(table.index(UF::CORPSE_FIELD_BYTES_1) == 34);
    CHECK(table.index(UF::CORPSE_FIELD_BYTES_2) == 35);
    CHECK(table.index(UF::CORPSE_FIELD_GUILD) == 36);
    CHECK(table.index(UF::CORPSE_FIELD_FLAGS) == 37);
    CHECK(table.index(UF::CORPSE_FIELD_DYNAMIC_FLAGS) == 38);

    // Item fields (OBJECT_END = 6)
    CHECK(table.index(UF::ITEM_FIELD_OWNER) == 6);
    CHECK(table.index(UF::ITEM_FIELD_CONTAINED) == 8);
    CHECK(table.index(UF::ITEM_FIELD_CREATOR) == 10);
    CHECK(table.index(UF::ITEM_FIELD_GIFTCREATOR) == 12);
    CHECK(table.index(UF::ITEM_FIELD_DURATION) == 15);
    CHECK(table.index(UF::ITEM_FIELD_SPELL_CHARGES) == 16);
    CHECK(table.index(UF::ITEM_FIELD_FLAGS) == 21);
    CHECK(table.index(UF::ITEM_FIELD_ENCHANTMENT_1_1) == 22);
    CHECK(table.index(UF::ITEM_FIELD_PROPERTY_SEED) == 55);
    CHECK(table.index(UF::ITEM_FIELD_RANDOM_PROPERTIES_ID) == 56);
    CHECK(table.index(UF::ITEM_FIELD_ITEM_TEXT_ID) == 57);

    // Unit fields (OBJECT_END = 6)
    CHECK(table.index(UF::UNIT_FIELD_PERSUADED) == 18);
    CHECK(table.index(UF::UNIT_FIELD_AURALEVELS) == 118);
    CHECK(table.index(UF::UNIT_FIELD_MINOFFHANDDAMAGE) == 157);
    CHECK(table.index(UF::UNIT_FIELD_MAXOFFHANDDAMAGE) == 158);
    CHECK(table.index(UF::UNIT_FIELD_BASE_HEALTH) == 208);

    // Player fields (UNIT_END = 234)
    CHECK(table.index(UF::PLAYER_TRACK_CREATURES) == 1314);
    CHECK(table.index(UF::PLAYER_FIELD_KILLS) == 1514);
    CHECK(table.index(UF::PLAYER_FIELD_TODAY_CONTRIBUTION) == 1515);
    CHECK(table.index(UF::PLAYER_FIELD_YESTERDAY_CONTRIBUTION) == 1516);
    CHECK(table.index(UF::PLAYER_FIELD_LIFETIME_HONORABLE_KILLS) == 1517);
    CHECK(table.index(UF::PLAYER_FIELD_WATCHED_FACTION_INDEX) == 1519);
    CHECK(table.index(UF::PLAYER_FIELD_MAX_LEVEL) == 1566);
    CHECK(table.index(UF::PLAYER_FIELD_DAILY_QUESTS_1) == 1567);

    // GameObject fields (OBJECT_END = 6)
    CHECK(table.index(UF::GAMEOBJECT_TYPE_ID) == 21);
    CHECK(table.index(UF::GAMEOBJECT_ARTKIT) == 23);
}

TEST_CASE("Part B bitflag policies match TBC 2.4.3 semantics across all 5 families", "[shard6][flags]") {
    SECTION("CreatureTypeFlags: ghost visibility, dead interaction, tooltip, nameplate, shadow, world name") {
        CHECK(creatureVisibleToPlayer(false, true, 0u, 0u));
        CHECK_FALSE(creatureVisibleToPlayer(true, true, 0u, 0u));
        CHECK(creatureVisibleToPlayer(true, false, 0u, 0u)); // uncached template stays visible
        CHECK(creatureVisibleToPlayer(true, true, NPC_FLAG_SPIRIT_HEALER, 0u));
        CHECK(creatureVisibleToPlayer(true, true, 0u, CREATURE_TYPEFLAG_VISIBLE_TO_GHOSTS));

        CHECK_FALSE(creatureInteractableWhileDead(0u));
        CHECK(creatureInteractableWhileDead(CREATURE_TYPEFLAG_INTERACT_WHILE_DEAD));

        CHECK(creatureShowsFactionTooltip(0u));
        CHECK_FALSE(creatureShowsFactionTooltip(CREATURE_TYPEFLAG_NO_FACTION_TOOLTIP));

        CHECK(creatureAllowsNameplate(0u));
        CHECK_FALSE(creatureAllowsNameplate(CREATURE_TYPEFLAG_NO_NAME_PLATE));

        CHECK(creatureCastsShadowBlob(0u));
        CHECK_FALSE(creatureCastsShadowBlob(CREATURE_TYPEFLAG_HAS_NO_SHADOW_BLOB));

        CHECK(creatureRendersObjectName(0u));
        CHECK_FALSE(creatureRendersObjectName(CREATURE_TYPEFLAG_DO_NOT_RENDER_OBJECT_NAME));
    }

    SECTION("UnitFlags: control loss, silence, pacify, disarm, prevent anim, sheathe override") {
        CHECK(playerHasFullControl(0u, 0u));
        CHECK_FALSE(playerHasFullControl(UNIT_FLAG_CLIENT_CONTROL_LOST, 0u));
        CHECK_FALSE(playerHasFullControl(UNIT_FLAG_STUNNED, 0u));
        CHECK_FALSE(playerHasFullControl(UNIT_FLAG_CONFUSED, 0u));
        CHECK_FALSE(playerHasFullControl(UNIT_FLAG_FLEEING, 0u));
        CHECK_FALSE(playerHasFullControl(UNIT_FLAG_TAXI_FLIGHT, 0u));
        CHECK_FALSE(playerHasFullControl(0u, 0x1234ull));

        CHECK(unitIsSilenced(UNIT_FLAG_SILENCED));
        CHECK(unitIsPacified(UNIT_FLAG_PACIFIED));
        CHECK(unitIsDisarmed(UNIT_FLAG_DISARMED));
        CHECK(unitPreventsAnim(UNIT_FLAG_PREVENT_ANIM));

        CHECK(effectiveUnitSheathState(1u, 0u) == 1u);
        CHECK(effectiveUnitSheathState(2u, 0u) == 2u);
        CHECK(effectiveUnitSheathState(1u, UNIT_FLAG_SHEATHE) == 0u);
        CHECK(effectiveUnitSheathState(2u, UNIT_FLAG_SHEATHE) == 0u);
    }

    SECTION("UnitFlags2: HIDE_BODY and DO_NOT_FADE_IN + Unit::setUnitFlags2") {
        Unit u(0x100ull);
        u.setUnitFlags2(UNIT_FLAG2_HIDE_BODY | UNIT_FLAG2_DO_NOT_FADE_IN | UNIT_FLAG2_CLONED);
        CHECK(u.getUnitFlags2() == (UNIT_FLAG2_HIDE_BODY | UNIT_FLAG2_DO_NOT_FADE_IN | UNIT_FLAG2_CLONED));
        CHECK(u.isMirrorImage());
        CHECK(unitHidesBody(u.getUnitFlags2()));
        CHECK(unitSkipsSpawnFadeIn(u.getUnitFlags2()));
    }

    SECTION("UnitDynFlags: TRACK_UNIT and SPECIALINFO") {
        CHECK_FALSE(unitTrackedOnMinimap(0u));
        CHECK(unitTrackedOnMinimap(UNIT_DYNFLAG_TRACK_UNIT));
        CHECK_FALSE(unitExposesSpecialInfo(0u));
        CHECK(unitExposesSpecialInfo(UNIT_DYNFLAG_SPECIALINFO));
    }

    SECTION("NPCFlags: direct service dispatch and cursor selection") {
        CHECK(resolveDirectNpcServiceAction(NPC_FLAG_GUILD_BANKER, true) ==
              DirectNpcServiceAction::GUILD_BANKER_ACTIVATE);
        CHECK(resolveDirectNpcServiceAction(NPC_FLAG_TABARDDESIGNER, true) ==
              DirectNpcServiceAction::TABARDVENDOR_ACTIVATE);
        CHECK(resolveDirectNpcServiceAction(NPC_FLAG_PETITIONER, true) ==
              DirectNpcServiceAction::PETITION_SHOWLIST);
        CHECK(resolveDirectNpcServiceAction(NPC_FLAG_STABLEMASTER, true) ==
              DirectNpcServiceAction::LIST_STABLED_PETS);
        // Gossip takes precedence when combined so multi-option gossip menus still open
        CHECK(resolveDirectNpcServiceAction(NPC_FLAG_STABLEMASTER | NPC_FLAG_GOSSIP, true) ==
              DirectNpcServiceAction::GOSSIP_HELLO);
        // Spellclick is WotLK-only (STATUS_NEVER in CMaNGOS TBC)
        CHECK(resolveDirectNpcServiceAction(NPC_FLAG_SPELLCLICK, true) ==
              DirectNpcServiceAction::GOSSIP_HELLO);
        CHECK(resolveDirectNpcServiceAction(NPC_FLAG_SPELLCLICK, false) ==
              DirectNpcServiceAction::SPELLCLICK);

        // Cursor checks
        CHECK(wowee::ui::unitCursorPath(true, 0u, 5, NPC_FLAG_GUILD_BANKER, QuestGiverStatus::NONE) ==
              "Interface\\Cursor\\Buy.blp");
        CHECK(wowee::ui::unitCursorPath(true, 0u, 5, NPC_FLAG_PETITIONER, QuestGiverStatus::NONE) ==
              "Interface\\Cursor\\Speak.blp");
        CHECK(wowee::ui::unitCursorPath(true, 0u, 5, NPC_FLAG_TABARDDESIGNER, QuestGiverStatus::NONE) ==
              "Interface\\Cursor\\Speak.blp");
        CHECK(wowee::ui::unitCursorPath(true, 0u, 5, NPC_FLAG_TRAINER_CLASS, QuestGiverStatus::NONE) ==
              "Interface\\Cursor\\Trainer.blp");
        // Dead NPC with INTERACT_WHILE_DEAD shows Speak cursor instead of Point
        CHECK(wowee::ui::unitCursorPath(false, 0u, 5, NPC_FLAG_GOSSIP, QuestGiverStatus::NONE,
                                        {}, false, 0u, false, false, true, true, true) ==
              "Interface\\Cursor\\Speak.blp");
    }
}
