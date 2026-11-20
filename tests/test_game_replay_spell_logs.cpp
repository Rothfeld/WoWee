// Dispel, spellsteal and instakill logs through GameHandler's own dispatch,
// built as the servers write them: AzerothCore for WotLK, cmangos for classic
// and TBC. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <algorithm>
#include <string>

using replay::Session;

namespace {

using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

constexpr uint64_t kPlayer = Session::kPlayerGuid;
constexpr uint64_t kMob = 0xF130000000000123ull;
constexpr uint32_t kDispelMagic = 527;
constexpr uint32_t kPowerWordFortitude = 1243;

bool chatHas(Session& s, const std::string& text) {
    const auto& history = s.handler().getChatHistory();
    return std::any_of(history.begin(), history.end(), [&](const auto& m) {
        return m.message.find(text) != std::string::npos;
    });
}

// Spell::EffectDispel: victim, then caster. Classic has neither the dispel
// spell nor the byte after each dispelled one.
Packet dispelLog(bool classic, uint64_t victim, uint64_t caster) {
    Packet p(wireOpcode(Opcode::SMSG_SPELLDISPELLOG));
    p.writePackedGuid(victim);
    p.writePackedGuid(caster);
    if (!classic) {
        p.writeUInt32(kDispelMagic);
        p.writeUInt8(0);
    }
    p.writeUInt32(1);
    p.writeUInt32(kPowerWordFortitude);
    if (!classic) p.writeUInt8(0);
    return p;
}

// Spell::EffectInstaKill. Classic sends the victim alone.
Packet instaKillLog(bool classic, uint64_t caster, uint64_t victim) {
    Packet p(wireOpcode(Opcode::SMSG_SPELLINSTAKILLLOG));
    if (!classic) p.writeUInt64(caster);
    p.writeUInt64(victim);
    p.writeUInt32(5);
    return p;
}

Packet dispelFailed(bool classic, uint64_t caster, uint64_t victim) {
    Packet p(wireOpcode(Opcode::SMSG_DISPEL_FAILED));
    p.writeUInt64(caster);
    p.writeUInt64(victim);
    if (!classic) p.writeUInt32(kDispelMagic);
    p.writeUInt32(kPowerWordFortitude);
    return p;
}

}  // namespace

TEST_CASE("a dispel log names its victim before its caster", "[replay][dispel]") {
    using wowee::game::CombatTextEntry;
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        const bool classic = std::string(expansion) == "classic";

        {
            Session s(expansion);
            s.handler().setTargetGuidRaw(kPlayer);
            s.receive(dispelLog(classic, kPlayer, kMob));
            const auto& texts = s.handler().getCombatText();
            REQUIRE(texts.size() == 1);
            CHECK(texts[0].type == CombatTextEntry::DISPEL);
            CHECK(texts[0].spellId == kPowerWordFortitude);
            CHECK_FALSE(texts[0].isPlayerSource);
            CHECK(texts[0].srcGuid == kMob);
            CHECK(texts[0].dstGuid == kPlayer);
            CHECK_FALSE(chatHas(s, "dispel"));
        }
        {
            Session s(expansion);
            s.handler().setTargetGuidRaw(kMob);
            s.receive(dispelLog(classic, kMob, kPlayer));
            const auto& texts = s.handler().getCombatText();
            REQUIRE(texts.size() == 1);
            CHECK(texts[0].type == CombatTextEntry::DISPEL);
            CHECK(texts[0].spellId == kPowerWordFortitude);
            CHECK(texts[0].isPlayerSource);
            CHECK(texts[0].srcGuid == kPlayer);
            CHECK(texts[0].dstGuid == kMob);
            CHECK_FALSE(chatHas(s, "dispel"));
        }
    }
}

TEST_CASE("a spellsteal log has packed guids on TBC", "[replay][dispel]") {
    using wowee::game::CombatTextEntry;
    Session s("tbc");
    s.handler().setTargetGuidRaw(kMob);
    Packet p(wireOpcode(Opcode::SMSG_SPELLSTEALLOG));
    p.writePackedGuid(kMob);
    p.writePackedGuid(kPlayer);
    p.writeUInt32(30449);
    p.writeUInt8(0);
    p.writeUInt32(1);
    p.writeUInt32(kPowerWordFortitude);
    p.writeUInt8(0);
    s.receive(p);
    const auto& texts = s.handler().getCombatText();
    REQUIRE(texts.size() == 1);
    CHECK(texts[0].type == CombatTextEntry::STEAL);
    CHECK(texts[0].spellId == kPowerWordFortitude);
    CHECK(texts[0].isPlayerSource);
    CHECK(texts[0].srcGuid == kPlayer);
    CHECK(texts[0].dstGuid == kMob);
    CHECK_FALSE(chatHas(s, "stole"));
}

TEST_CASE("an instakill log has whole guids", "[replay][instakill]") {
    using wowee::game::CombatTextEntry;
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        const bool classic = std::string(expansion) == "classic";
        Session s(expansion);
        s.receive(instaKillLog(classic, kMob, kPlayer));
        const auto& texts = s.handler().getCombatText();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0].type == CombatTextEntry::INSTAKILL);
        CHECK(texts[0].spellId == 5u);
        CHECK_FALSE(texts[0].isPlayerSource);
        CHECK(texts[0].srcGuid == (classic ? 0ull : kMob));
        CHECK(texts[0].dstGuid == kPlayer);
        CHECK_FALSE(chatHas(s, "instant-kill"));
    }
}

TEST_CASE("a failed dispel has whole guids, and classic has no dispel spell", "[replay][dispel]") {
    using wowee::game::CombatTextEntry;
    SECTION("wotlk") {
        Session s;
        s.handler().setTargetGuidRaw(kMob);
        s.receive(dispelFailed(false, kPlayer, kMob));
        const auto& texts = s.handler().getCombatText();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0].type == CombatTextEntry::RESIST);
        CHECK(texts[0].spellId == kDispelMagic);
        CHECK(texts[0].isPlayerSource);
        CHECK(texts[0].srcGuid == kPlayer);
        CHECK(texts[0].dstGuid == kMob);
        CHECK_FALSE(chatHas(s, "Dispel failed"));
    }
    SECTION("classic") {
        Session s("classic");
        s.handler().setTargetGuidRaw(kMob);
        s.receive(dispelFailed(true, kPlayer, kMob));
        const auto& texts = s.handler().getCombatText();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0].type == CombatTextEntry::RESIST);
        CHECK(texts[0].spellId == kPowerWordFortitude);
        CHECK(texts[0].isPlayerSource);
        CHECK(texts[0].srcGuid == kPlayer);
        CHECK(texts[0].dstGuid == kMob);
        CHECK_FALSE(chatHas(s, "Dispel failed"));
    }
}

namespace {

Packet tbcSpellGoMiss(uint64_t caster, uint32_t spellId, uint64_t victim,
                      uint8_t missType, uint8_t reflectResult = 0) {
    Packet p(wireOpcode(Opcode::SMSG_SPELL_GO));
    p.writePackedGuid(caster);
    p.writePackedGuid(caster);
    p.writeUInt32(spellId);
    p.writeUInt16(0);       // castFlags
    p.writeUInt32(1000);    // timestamp
    p.writeUInt8(0);        // hitCount
    p.writeUInt8(1);        // missCount
    p.writeUInt64(victim);
    p.writeUInt8(missType);
    if (missType == wowee::game::SpellMissInfo::REFLECT) {
        p.writeUInt8(reflectResult);
    }
    p.writeUInt32(0);       // SpellCastTargets flags = TARGET_FLAG_SELF/none
    return p;
}

}  // namespace

TEST_CASE("spell miss resist effect and victim animation rules match TBC 2.4.3", "[replay][spellmiss]") {
    using wowee::game::GameHandler;
    using wowee::game::SpellHandler;
    namespace MI = wowee::game::SpellMissInfo;

    // Resist/Immune/Miss/Deflect/Evade on missiles show HARDCODED Resist Spell (3207).
    CHECK(SpellHandler::spellMissShowsResistEffect(MI::MISS));
    CHECK(SpellHandler::spellMissShowsResistEffect(MI::RESIST));
    CHECK(SpellHandler::spellMissShowsResistEffect(MI::EVADE));
    CHECK(SpellHandler::spellMissShowsResistEffect(MI::IMMUNE));
    CHECK(SpellHandler::spellMissShowsResistEffect(MI::IMMUNE2));
    CHECK(SpellHandler::spellMissShowsResistEffect(MI::DEFLECT));

    // Dodge/Parry/Block/Absorb/Reflect/None do not show the resist dome.
    CHECK_FALSE(SpellHandler::spellMissShowsResistEffect(MI::NONE));
    CHECK_FALSE(SpellHandler::spellMissShowsResistEffect(MI::DODGE));
    CHECK_FALSE(SpellHandler::spellMissShowsResistEffect(MI::PARRY));
    CHECK_FALSE(SpellHandler::spellMissShowsResistEffect(MI::BLOCK));
    CHECK_FALSE(SpellHandler::spellMissShowsResistEffect(MI::ABSORB));
    CHECK_FALSE(SpellHandler::spellMissShowsResistEffect(MI::REFLECT));

    // Victim reaction mapping across missile and non-missile spells.
    CHECK(GameHandler::spellMissHitReaction(MI::RESIST, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::RESIST, false) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::IMMUNE, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::IMMUNE, false) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::DODGE, false) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::PARRY, false) == GameHandler::HitReaction::PARRY);
    CHECK(GameHandler::spellMissHitReaction(MI::BLOCK, false) == GameHandler::HitReaction::SHIELD_BLOCK);
    CHECK(GameHandler::spellMissHitReaction(MI::REFLECT, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::MISS, true) == GameHandler::HitReaction::DODGE);
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::MISS, false).has_value());
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::ABSORB, true).has_value());
}

TEST_CASE("TBC SMSG_SPELL_GO miss targets fire victim hit reactions and parse reflectResult", "[replay][spellmiss]") {
    using wowee::game::GameHandler;
    namespace MI = wowee::game::SpellMissInfo;

    // Parser preserves reflectResult byte on SPELL_MISS_REFLECT.
    {
        Session s("tbc");
        Packet p = tbcSpellGoMiss(kMob, 7588, kPlayer, MI::REFLECT, MI::IMMUNE);
        wowee::game::SpellGoData data;
        REQUIRE(s.handler().getPacketParsers()->parseSpellGo(p, data));
        REQUIRE(data.missTargets.size() == 1);
        CHECK(data.missTargets[0].targetGuid == kPlayer);
        CHECK(data.missTargets[0].missType == MI::REFLECT);
        CHECK(data.missTargets[0].reflectResult == MI::IMMUNE);
    }

    // Non-missile SMSG_SPELL_GO misses trigger victim hit reactions on both player and creature.
    struct Case {
        uint64_t caster;
        uint64_t victim;
        uint32_t spellId;
        uint8_t missType;
        std::optional<GameHandler::HitReaction> expected;
    };
    const Case cases[] = {
        {kMob,    kPlayer, 2136, MI::RESIST,  GameHandler::HitReaction::DODGE},
        {kMob,    kPlayer, 2136, MI::IMMUNE,  GameHandler::HitReaction::DODGE},
        {kMob,    kPlayer, 1752, MI::DODGE,   GameHandler::HitReaction::DODGE},
        {kMob,    kPlayer, 1752, MI::PARRY,   GameHandler::HitReaction::PARRY},
        {kMob,    kPlayer, 1752, MI::BLOCK,   GameHandler::HitReaction::SHIELD_BLOCK},
        {kMob,    kPlayer, 2764, MI::MISS,    std::nullopt},
        {kPlayer, kMob,    2136, MI::IMMUNE,  GameHandler::HitReaction::DODGE},
        {kPlayer, kMob,    1752, MI::PARRY,   GameHandler::HitReaction::PARRY},
        {kMob,    kPlayer, 2136, MI::REFLECT, GameHandler::HitReaction::DODGE},
    };
    for (const auto& c : cases) {
        Session s("tbc");
        std::vector<std::tuple<uint64_t, uint64_t, GameHandler::HitReaction>> reactions;
        s.handler().setHitReactionCallback([&](uint64_t a, uint64_t v, GameHandler::HitReaction r) {
            reactions.emplace_back(a, v, r);
        });
        s.receive(tbcSpellGoMiss(c.caster, c.spellId, c.victim, c.missType));
        if (c.expected) {
            REQUIRE(reactions.size() == 1);
            CHECK(std::get<0>(reactions[0]) == c.caster);
            CHECK(std::get<1>(reactions[0]) == c.victim);
            CHECK(std::get<2>(reactions[0]) == *c.expected);
        } else {
            CHECK(reactions.empty());
        }
    }
}

TEST_CASE("energize and periodic power logs scale rage and runic power from tenths", "[replay][energize]") {
    using wowee::game::CombatTextEntry;
    using wowee::game::displayPowerAmount;

    CHECK(displayPowerAmount(0, 100) == 100);  // Mana
    CHECK(displayPowerAmount(1, 100) == 10);   // Rage (Furor / Bloodrage: 100 raw = 10 Rage)
    CHECK(displayPowerAmount(1, 200) == 20);   // Rage (Enrage: 200 raw = 20 Rage)
    CHECK(displayPowerAmount(1, 5) == 0);      // Sub-point rage rounds down
    CHECK(displayPowerAmount(2, 50) == 50);    // Focus
    CHECK(displayPowerAmount(3, 40) == 40);    // Energy (Cat Form Furor: 40 Energy)
    CHECK(displayPowerAmount(6, 150) == 15);   // Runic Power

    // TBC Furor (17057) on Dire Bear Form: SMSG_SPELLENERGIZELOG powerType=1, raw=100 -> +10 Rage
    {
        Session s("tbc");
        std::vector<std::pair<std::string, std::vector<std::string>>> events;
        s.handler().setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>& args) {
            events.emplace_back(ev, args);
        });
        Packet p(wireOpcode(Opcode::SMSG_SPELLENERGIZELOG));
        p.writePackedGuid(kPlayer);
        p.writePackedGuid(kPlayer);
        p.writeUInt32(17057); // Furor
        p.writeUInt32(1);     // POWER_RAGE
        p.writeUInt32(100);   // 10 Rage in tenths
        s.receive(p);

        const auto& texts = s.handler().getCombatText();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0].type == CombatTextEntry::ENERGIZE);
        CHECK(texts[0].powerType == 1);
        CHECK(texts[0].amount == 10);

        bool sawCtu = false;
        bool sawCleu = false;
        for (const auto& [ev, args] : events) {
            if (ev == "COMBAT_TEXT_UPDATE") {
                REQUIRE(args.size() == 2);
                CHECK(args[0] == "RAGE");
                CHECK(args[1] == "10");
                sawCtu = true;
            } else if (ev == "COMBAT_LOG_EVENT_UNFILTERED") {
                REQUIRE(args.size() >= 13);
                CHECK(args[1] == "SPELL_ENERGIZE");
                CHECK(args[8] == "17057");
                CHECK(args[11] == "10");
                CHECK(args[12] == "1");
                sawCleu = true;
            }
        }
        CHECK(sawCtu);
        CHECK(sawCleu);
    }

    // TBC Cat Form Furor (17099): powerType=3 (Energy), raw=40 -> +40 Energy unscaled
    {
        Session s("tbc");
        Packet p(wireOpcode(Opcode::SMSG_SPELLENERGIZELOG));
        p.writePackedGuid(kPlayer);
        p.writePackedGuid(kPlayer);
        p.writeUInt32(17099);
        p.writeUInt32(3);     // POWER_ENERGY
        p.writeUInt32(40);
        s.receive(p);

        const auto& texts = s.handler().getCombatText();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0].powerType == 3);
        CHECK(texts[0].amount == 40);
    }

    // TBC Enrage (5229) periodic tick: SMSG_PERIODICAURALOG auraType=24, powerType=1, raw=20 -> +2 Rage
    {
        Session s("tbc");
        Packet p(wireOpcode(Opcode::SMSG_PERIODICAURALOG));
        p.writePackedGuid(kPlayer);
        p.writePackedGuid(kPlayer);
        p.writeUInt32(5229);  // Enrage
        p.writeUInt32(1);     // count
        p.writeUInt32(24);    // SPELL_AURA_PERIODIC_ENERGIZE
        p.writeUInt32(1);     // POWER_RAGE
        p.writeUInt32(20);    // 2 Rage in tenths
        s.receive(p);

        const auto& texts = s.handler().getCombatText();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0].type == CombatTextEntry::ENERGIZE);
        CHECK(texts[0].powerType == 1);
        CHECK(texts[0].amount == 2);
    }
}
