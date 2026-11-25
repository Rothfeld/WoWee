// Dispel, spellsteal and instakill logs through GameHandler's own dispatch,
// built as the servers write them: AzerothCore for WotLK, cmangos for classic
// and TBC. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "audio/creature_sound_index.hpp"
#include "game/combat_handler.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
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
    // Missile spells play Dodge at arrival on Resist/Immune/Miss/Evade/Deflect/Reflect.
    CHECK(GameHandler::spellMissHitReaction(MI::RESIST, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::IMMUNE, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::IMMUNE2, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::EVADE, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::DEFLECT, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::REFLECT, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::MISS, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::DODGE, true) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::PARRY, true) == GameHandler::HitReaction::PARRY);
    CHECK(GameHandler::spellMissHitReaction(MI::BLOCK, true) == GameHandler::HitReaction::SHIELD_BLOCK);
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::ABSORB, true).has_value());
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::NONE, true).has_value());

    // Non-missile spells (instant magic, periodic aura pulses, melee abilities)
    // play a victim reaction only on explicit physical Dodge/Parry/Block.
    CHECK(GameHandler::spellMissHitReaction(MI::DODGE, false) == GameHandler::HitReaction::DODGE);
    CHECK(GameHandler::spellMissHitReaction(MI::PARRY, false) == GameHandler::HitReaction::PARRY);
    CHECK(GameHandler::spellMissHitReaction(MI::BLOCK, false) == GameHandler::HitReaction::SHIELD_BLOCK);
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::RESIST, false).has_value());
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::IMMUNE, false).has_value());
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::IMMUNE2, false).has_value());
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::EVADE, false).has_value());
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::DEFLECT, false).has_value());
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::REFLECT, false).has_value());
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::MISS, false).has_value());
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::ABSORB, false).has_value());
    CHECK_FALSE(GameHandler::spellMissHitReaction(MI::NONE, false).has_value());
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

    // Non-missile SMSG_SPELL_GO misses: physical Dodge/Parry/Block animate,
    // while Resist/Immune/Miss/Reflect on instant spells and periodic aura pulses
    // (e.g. Entropic Horror Fire Shield 11968, Fire Blast 2136, Arcane Explosion 1449) do not.
    struct Case {
        uint64_t caster;
        uint64_t victim;
        uint32_t spellId;
        uint8_t missType;
        std::optional<GameHandler::HitReaction> expected;
    };
    const Case cases[] = {
        {kMob,    kPlayer, 11968, MI::RESIST,  std::nullopt},
        {kMob,    kPlayer, 11968, MI::IMMUNE,  std::nullopt},
        {kMob,    kPlayer, 2136,  MI::RESIST,  std::nullopt},
        {kMob,    kPlayer, 2136,  MI::IMMUNE,  std::nullopt},
        {kMob,    kPlayer, 2136,  MI::REFLECT, std::nullopt},
        {kMob,    kPlayer, 1449,  MI::RESIST,  std::nullopt},
        {kMob,    kPlayer, 589,   MI::RESIST,  std::nullopt},
        {kMob,    kPlayer, 1752,  MI::DODGE,   GameHandler::HitReaction::DODGE},
        {kMob,    kPlayer, 1752,  MI::PARRY,   GameHandler::HitReaction::PARRY},
        {kMob,    kPlayer, 1752,  MI::BLOCK,   GameHandler::HitReaction::SHIELD_BLOCK},
        {kMob,    kPlayer, 1752,  MI::IMMUNE,  std::nullopt},
        {kMob,    kPlayer, 2764,  MI::MISS,    std::nullopt},
        {kPlayer, kMob,    2136,  MI::IMMUNE,  std::nullopt},
        {kPlayer, kMob,    1752,  MI::PARRY,   GameHandler::HitReaction::PARRY},
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
        CHECK(texts[0].type == CombatTextEntry::PERIODIC_ENERGIZE);
        CHECK(texts[0].powerType == 1);
        CHECK(texts[0].amount == 2);
    }
}

TEST_CASE("direct spell damage triggers hit-flinch while absorbed hits and periodic DoT ticks do not", "[replay][spell][hitflinch]") {
    using wowee::game::GameHandler;
    using HR = GameHandler::HitReaction;

    // Pure classification rules
    CHECK(GameHandler::spellDamageHitReaction(1351, false, 64) == HR::SPELL_WOUND);      // Arcane Missiles
    CHECK(GameHandler::spellDamageHitReaction(2100, true, 64) == HR::SPELL_CRIT_WOUND);  // Arcane crit
    CHECK(GameHandler::spellDamageHitReaction(420, false, 32) == HR::SPELL_WOUND);       // Shadow Bolt
    CHECK(GameHandler::spellDamageHitReaction(500, false, 1) == HR::WOUND);              // Physical spell
    CHECK(GameHandler::spellDamageHitReaction(900, true, 1) == HR::CRIT_WOUND);          // Physical spell crit
    CHECK_FALSE(GameHandler::spellDamageHitReaction(0, false, 64).has_value());          // Fully absorbed/resisted
    // Spells without a per-target SpellVisual ImpactKit/TargetImpactKit (Void Zone Consumption 28865,
    // Four Horsemen Mark 28836, Meteor 28884, periodic ground pulses) do not flinch in TBC 2.4.3.
    CHECK_FALSE(GameHandler::spellDamageHitReaction(901, false, 32, false).has_value());
    CHECK_FALSE(GameHandler::spellDamageHitReaction(95040, true, 32, false).has_value());
    CHECK_FALSE(GameHandler::spellDamageHitReaction(500, false, 1, false).has_value());

    auto makeTbcSpellDamageLog = [](uint64_t target, uint64_t attacker, uint32_t spellId,
                                    uint32_t damage, uint8_t schoolMask, uint32_t absorbed,
                                    uint32_t flags) {
        Packet p(wireOpcode(Opcode::SMSG_SPELLNONMELEEDAMAGELOG));
        p.writePackedGuid(target);
        p.writePackedGuid(attacker);
        p.writeUInt32(spellId);
        p.writeUInt32(damage);
        p.writeUInt8(schoolMask);
        p.writeUInt32(absorbed);
        p.writeUInt32(0);      // resisted
        p.writeUInt8(0);       // physicalLog
        p.writeUInt8(0);       // unused
        p.writeUInt32(0);      // blocked
        p.writeUInt32(flags);  // HitInfo (0x02 = crit)
        p.writeUInt8(0);       // debug
        return p;
    };

    // 1. Arcane Missiles (29956) hitting local player triggers SPELL_WOUND
    {
        Session s("tbc");
        std::vector<std::tuple<uint64_t, uint64_t, HR>> reactions;
        s.handler().setHitReactionCallback([&](uint64_t a, uint64_t v, HR r) {
            reactions.emplace_back(a, v, r);
        });
        s.receive(makeTbcSpellDamageLog(kPlayer, kMob, 29956, 1351, 64, 0, 0));
        REQUIRE(reactions.size() == 1);
        CHECK(std::get<0>(reactions[0]) == kMob);
        CHECK(std::get<1>(reactions[0]) == kPlayer);
        CHECK(std::get<2>(reactions[0]) == HR::SPELL_WOUND);
    }

    // 2. Observed spell crit hitting another player also triggers SPELL_CRIT_WOUND
    {
        Session s("tbc");
        std::vector<std::tuple<uint64_t, uint64_t, HR>> reactions;
        s.handler().setHitReactionCallback([&](uint64_t a, uint64_t v, HR r) {
            reactions.emplace_back(a, v, r);
        });
        constexpr uint64_t kOtherPlayer = 0x0000000000000099ULL;
        s.receive(makeTbcSpellDamageLog(kOtherPlayer, kMob, 686, 1800, 32, 0, 0x02));
        REQUIRE(reactions.size() == 1);
        CHECK(std::get<1>(reactions[0]) == kOtherPlayer);
        CHECK(std::get<2>(reactions[0]) == HR::SPELL_CRIT_WOUND);
    }

    // 3. Fully absorbed spell (damage == 0, absorbed > 0) does NOT trigger hit-flinch
    {
        Session s("tbc");
        std::vector<std::tuple<uint64_t, uint64_t, HR>> reactions;
        s.handler().setHitReactionCallback([&](uint64_t a, uint64_t v, HR r) {
            reactions.emplace_back(a, v, r);
        });
        s.receive(makeTbcSpellDamageLog(kPlayer, kMob, 8921, 0, 64, 300, 0));
        CHECK(reactions.empty());
    }

    // 4. Periodic DoT tick (SMSG_PERIODICAURALOG auraType=3) does NOT trigger hit-flinch
    {
        Session s("tbc");
        std::vector<std::tuple<uint64_t, uint64_t, HR>> reactions;
        s.handler().setHitReactionCallback([&](uint64_t a, uint64_t v, HR r) {
            reactions.emplace_back(a, v, r);
        });
        Packet p(wireOpcode(Opcode::SMSG_PERIODICAURALOG));
        p.writePackedGuid(kPlayer);
        p.writePackedGuid(kMob);
        p.writeUInt32(172);  // Corruption
        p.writeUInt32(1);    // count
        p.writeUInt32(3);    // SPELL_AURA_PERIODIC_DAMAGE
        p.writeUInt32(45);   // damage
        p.writeUInt32(32);   // schoolMask (Shadow)
        p.writeUInt32(0);    // absorbed
        p.writeUInt32(0);    // resisted
        s.receive(p);
        CHECK(reactions.empty());
    }

    // 5. Real TBC Spell.dbc + SpellVisual.dbc per-target impact-kit classification:
    //    periodic/ground/mark spells (Consumption 28865, Mark 28836, Meteor 28884,
    //    Hellfire 1949, Flamestrike 2120, Rain of Fire 5740) author neither ImpactKit
    //    nor TargetImpactKit, whereas direct-impact spells do.
    {
        const std::filesystem::path dbcDir = "/workspace/azerothcore/client/wowee-data/expansions/tbc/dbfilesclient";
        if (std::filesystem::exists(dbcDir / "spell.dbc") &&
            std::filesystem::exists(dbcDir / "spellvisual.dbc")) {
            auto loadDbc = [&](const char* name) {
                wowee::pipeline::DBCFile f;
                std::ifstream in(dbcDir / name, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());
                REQUIRE(f.load(bytes));
                return f;
            };
            const auto spells = loadDbc("spell.dbc");
            const auto visuals = loadDbc("spellvisual.dbc");
            auto spellHasImpactKit = [&](uint32_t spellId) -> bool {
                const int32_t sRow = spells.findRecordById(spellId);
                REQUIRE(sRow >= 0);
                const uint32_t visId = spells.getUInt32(static_cast<uint32_t>(sRow), 122);
                if (visId == 0) return false;
                const int32_t vRow = visuals.findRecordById(visId);
                if (vRow < 0) return false;
                const uint32_t impKit = visuals.getUInt32(static_cast<uint32_t>(vRow), 3);
                const uint32_t targetImpKit = visuals.getUInt32(static_cast<uint32_t>(vRow), 14);
                return impKit != 0 || targetImpKit != 0;
            };
            // No per-target impact kit -> no hit-flinch on SMSG_SPELLNONMELEEDAMAGELOG
            for (uint32_t id : {28865u, 28836u, 28884u, 1949u, 2120u, 5740u, 27220u, 122u}) {
                CAPTURE(id);
                CHECK_FALSE(spellHasImpactKit(id));
            }
            // Per-target ImpactKit or TargetImpactKit authored -> flinch preserved
            for (uint32_t id : {133u, 8921u, 686u, 29956u, 7268u, 585u, 26573u, 28882u, 28883u, 30451u, 5308u}) {
                CAPTURE(id);
                CHECK(spellHasImpactKit(id));
            }
        }
    }
}

TEST_CASE("lethal melee and spell hits suppress victim hit-flinch while non-lethal hits still flinch",
          "[replay][combat][hitflinch][death]") {
    using wowee::game::GameHandler;
    using HR = GameHandler::HitReaction;

    // Pure classification rules
    CHECK(GameHandler::isLethalDamage(20958, 20958, 300000, -1));     // TBC one-hit overkill
    CHECK(GameHandler::isLethalDamage(20958, 20958, 20958, -1));      // exact lethal blow
    CHECK(GameHandler::isLethalDamage(500, 20958, 500, -1));          // multi-hit final lethal blow
    CHECK(GameHandler::isLethalDamage(0, 20958, 100, -1));            // already dead
    CHECK(GameHandler::isLethalDamage(0, 0, 500, 50));                // explicit WotLK/Classic overkill
    CHECK_FALSE(GameHandler::isLethalDamage(20958, 20958, 100, -1));  // non-lethal hit
    CHECK_FALSE(GameHandler::isLethalDamage(20958, 20958, 0, -1));    // zero damage (miss/dodge/absorb)
    CHECK_FALSE(GameHandler::isLethalDamage(0, 0, 100, -1));          // unknown health (maxHealth == 0)

    auto makeTbcMeleeHit = [](uint64_t attacker, uint64_t target, int32_t damage, uint32_t hitInfo = 0x02) {
        Packet p(wireOpcode(Opcode::SMSG_ATTACKERSTATEUPDATE));
        p.writeUInt32(hitInfo);
        p.writePackedGuid(attacker);
        p.writePackedGuid(target);
        p.writeUInt32(static_cast<uint32_t>(damage));
        p.writeUInt8(1);       // subDamageCount
        p.writeUInt32(1);      // schoolMask (Physical)
        p.writeFloat(static_cast<float>(damage));
        p.writeUInt32(static_cast<uint32_t>(damage));
        p.writeUInt32(0);      // absorbed
        p.writeUInt32(0);      // resisted
        p.writeUInt32(1);      // VICTIMSTATE_NORMAL
        p.writeUInt32(0);      // unknown
        p.writeUInt32(0);      // spellId
        p.writeUInt32(0);      // blocked
        return p;
    };

    auto makeTbcSpellDamageLog = [](uint64_t target, uint64_t attacker, uint32_t spellId,
                                    uint32_t damage, uint8_t schoolMask) {
        Packet p(wireOpcode(Opcode::SMSG_SPELLNONMELEEDAMAGELOG));
        p.writePackedGuid(target);
        p.writePackedGuid(attacker);
        p.writeUInt32(spellId);
        p.writeUInt32(damage);
        p.writeUInt8(schoolMask);
        p.writeUInt32(0);      // absorbed
        p.writeUInt32(0);      // resisted
        p.writeUInt8(0);       // physicalLog
        p.writeUInt8(0);       // unused
        p.writeUInt32(0);      // blocked
        p.writeUInt32(0);      // flags
        p.writeUInt8(0);       // debug
        return p;
    };

    auto spawnMobWithHp = [](Session& s, uint64_t guid, uint32_t hp, uint32_t maxHp) {
        auto unit = std::make_shared<wowee::game::Unit>(guid);
        unit->setHealth(hp);
        unit->setMaxHealth(maxHp);
        s.handler().getEntityManager().addEntity(guid, unit);
    };

    // 1. One-hit melee kill (300000 dmg on 20958 HP mob) suppresses HitReaction, keeps combat text
    {
        Session s("tbc");
        spawnMobWithHp(s, kMob, 20958, 20958);
        std::vector<HR> reactions;
        s.handler().setHitReactionCallback([&](uint64_t, uint64_t victim, HR r) {
            if (victim == kMob) reactions.push_back(r);
        });
        s.receive(makeTbcMeleeHit(kPlayer, kMob, 300000));
        CHECK(reactions.empty());
        const auto& texts = s.handler().getCombatText();
        REQUIRE(!texts.empty());
        CHECK(texts.back().amount == 300000);
    }

    // 2. Non-lethal melee hit (100 dmg) still plays WOUND; subsequent lethal hit (20858 dmg) suppresses it
    {
        Session s("tbc");
        spawnMobWithHp(s, kMob, 20958, 20958);
        std::vector<HR> reactions;
        s.handler().setHitReactionCallback([&](uint64_t, uint64_t victim, HR r) {
            if (victim == kMob) reactions.push_back(r);
        });
        s.receive(makeTbcMeleeHit(kPlayer, kMob, 100));
        REQUIRE(reactions.size() == 1);
        CHECK(reactions[0] == HR::WOUND);

        auto unit = std::static_pointer_cast<wowee::game::Unit>(
            s.handler().getEntityManager().getEntity(kMob));
        unit->setHealth(20858);
        s.receive(makeTbcMeleeHit(kPlayer, kMob, 20858));
        CHECK(reactions.size() == 1);  // no second wound on lethal blow
    }

    // 3. One-hit spell kill (300000 dmg Fireball) suppresses SPELL_WOUND; non-lethal spell hit still flinches
    {
        Session s("tbc");
        spawnMobWithHp(s, kMob, 20958, 20958);
        std::vector<HR> reactions;
        s.handler().setHitReactionCallback([&](uint64_t, uint64_t victim, HR r) {
            if (victim == kMob) reactions.push_back(r);
        });
        s.receive(makeTbcSpellDamageLog(kMob, kPlayer, 133, 100, 4));
        REQUIRE(reactions.size() == 1);
        CHECK(reactions[0] == HR::SPELL_WOUND);

        s.receive(makeTbcSpellDamageLog(kMob, kPlayer, 133, 300000, 4));
        CHECK(reactions.size() == 1);  // lethal spell hit suppressed
    }
}

TEST_CASE("Shard 2 TBC network wire-payload sweep fixes round-trip cleanly",
          "[replay][tbc][net_sweep_shard2]") {
    using wowee::game::CombatTextEntry;

    // 1. P0 SMSG_CLEAR_EXTRA_AURA_INFO (0x3A6): PackedGuid(target) + uint32(spellId)
    //    6-byte player packet and 11+-byte creature packet both match by spellId.
    {
        Session s("tbc");
        s.handler().setTargetGuidRaw(kMob);

        auto setExtraAura = [](uint64_t guid, uint8_t slot, uint32_t spellId, int32_t maxMs, uint32_t remMs) {
            Packet p(wireOpcode(Opcode::SMSG_SET_EXTRA_AURA_INFO_OBSOLETE));
            p.writePackedGuid(guid);
            p.writeUInt8(slot);
            p.writeUInt32(spellId);
            p.writeUInt32(static_cast<uint32_t>(maxMs));
            p.writeUInt32(remMs);
            return p;
        };
        auto clearExtraAura = [](uint64_t guid, uint32_t spellId) {
            Packet p(wireOpcode(Opcode::SMSG_CLEAR_EXTRA_AURA_INFO));
            p.writePackedGuid(guid);
            p.writeUInt32(spellId);
            return p;
        };

        s.receive(setExtraAura(kPlayer, 0, 21562, 1800000, 1800000));
        s.receive(setExtraAura(kPlayer, 1, 1459, 1800000, 1800000));
        s.receive(setExtraAura(kMob, 40, 172, 18000, 18000));
        REQUIRE(s.handler().getPlayerAuras().size() >= 2);
        CHECK(s.handler().getPlayerAuras()[0].spellId == 21562u);
        CHECK(s.handler().getPlayerAuras()[1].spellId == 1459u);
        REQUIRE(s.handler().getTargetAuras().size() >= 41);
        CHECK(s.handler().getTargetAuras()[40].spellId == 172u);

        // 6-byte player packet (2-byte packed GUID + 4-byte spellId)
        Packet playerClear = clearExtraAura(kPlayer, 21562);
        CHECK(playerClear.getSize() < 9u);
        s.receive(playerClear);
        CHECK(s.handler().getPlayerAuras()[0].spellId == 0u);
        CHECK(s.handler().getPlayerAuras()[1].spellId == 1459u);

        // Creature target packet
        s.receive(clearExtraAura(kMob, 172));
        CHECK(s.handler().getTargetAuras()[40].spellId == 0u);
    }

    // 2. P0 CMSG_CANCEL_CAST (4-byte TBC vs 5-byte WotLK) + P1 CMSG_CANCEL_CHANNELLING (0x13B)
    {
        Session sTbc("tbc");
        Packet startCast(wireOpcode(Opcode::SMSG_SPELL_START));
        startCast.writePackedGuid(kPlayer);
        startCast.writePackedGuid(kPlayer);
        startCast.writeUInt32(133); // Fireball
        startCast.writeUInt8(3);
        startCast.writeUInt16(0);
        startCast.writeUInt32(2500);
        startCast.writeUInt32(0x02);
        startCast.writePackedGuid(kMob);
        sTbc.receive(startCast);
        REQUIRE(sTbc.handler().isCasting());

        sTbc.handler().cancelCast();
        auto cancelledTbc = sTbc.sent().of(Opcode::CMSG_CANCEL_CAST);
        REQUIRE(cancelledTbc.size() == 1);
        REQUIRE(cancelledTbc[0].getSize() == 4u);
        CHECK(cancelledTbc[0].readUInt32() == 133u);
        CHECK_FALSE(cancelledTbc[0].hasData());

        // Channel cancel sends CMSG_CANCEL_CHANNELLING (0x13B), 4-byte spellId
        Packet chanStart(wireOpcode(Opcode::MSG_CHANNEL_START));
        chanStart.writePackedGuid(kPlayer);
        chanStart.writeUInt32(12051); // Evocation
        chanStart.writeUInt32(8000);
        sTbc.receive(chanStart);
        REQUIRE(sTbc.handler().isCasting());
        REQUIRE(sTbc.handler().isChanneling());

        sTbc.handler().cancelCast();
        auto cancelledChan = sTbc.sent().of(Opcode::CMSG_CANCEL_CHANNELLING);
        REQUIRE(cancelledChan.size() == 1);
        REQUIRE(cancelledChan[0].getSize() == 4u);
        CHECK(cancelledChan[0].readUInt32() == 12051u);
        CHECK(sTbc.sent().of(Opcode::CMSG_CANCEL_CAST).size() == 1); // no extra CMSG_CANCEL_CAST

        // WotLK CancelCastPacket keeps 5-byte [uint8 castCount + uint32 spellId]
        Session sWotlk("wotlk");
        Packet wotlkCancel = wowee::game::CancelCastPacket::build(133u, 7u);
        REQUIRE(wotlkCancel.getSize() == 5u);
        CHECK(wotlkCancel.readUInt8() == 7u);
        CHECK(wotlkCancel.readUInt32() == 133u);
    }

    // 3. P1 SMSG_SPELL_FAILED_OTHER (0x2A6) reads wire spellId even when caster was untracked
    {
        Session s("tbc");
        s.handler().setTargetGuidRaw(kMob);
        s.handler().spellNameCacheRef()[116].name = "Frostbolt";
        std::vector<std::pair<std::string, std::vector<std::string>>> events;
        s.handler().setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>& args) {
            if (ev == "UNIT_SPELLCAST_FAILED" || ev == "UNIT_SPELLCAST_STOP") {
                events.emplace_back(ev, args);
            }
        });
        uint32_t animStoppedSpell = 0;
        s.handler().setSpellCastAnimCallback([&](uint64_t guid, wowee::game::SpellAnimPhase phase, uint32_t sp) {
            if (guid == kMob && phase == wowee::game::SpellAnimPhase::STOP) animStoppedSpell = sp;
        });

        Packet failOther(wireOpcode(Opcode::SMSG_SPELL_FAILED_OTHER));
        failOther.writePackedGuid(kMob);
        failOther.writeUInt32(116); // Frostbolt (never preceded by SMSG_SPELL_START)
        s.receive(failOther);

        CHECK(animStoppedSpell == 116u);
        REQUIRE(events.size() == 2);
        REQUIRE(events[0].second.size() == 3);
        CHECK(events[0].second[0] == "target");
        CHECK(events[0].second[1] == "Frostbolt");
        CHECK(events[1].second[0] == "target");
        CHECK(events[1].second[1] == "Frostbolt");
    }

    // 4. P1 SMSG_SPELL_START (0x131) reads CAST_FLAG_AMMO (0x20) trailer + source/dest targets
    {
        Session s("tbc");
        Packet p(wireOpcode(Opcode::SMSG_SPELL_START));
        p.writePackedGuid(kMob);
        p.writePackedGuid(kMob);
        p.writeUInt32(75);        // Auto Shot
        p.writeUInt8(1);          // castCount
        p.writeUInt16(0x20);      // CAST_FLAG_AMMO
        p.writeUInt32(500);       // castTime
        p.writeUInt32(0x02 | 0x20 | 0x40); // UNIT | SOURCE_LOCATION | DEST_LOCATION
        p.writePackedGuid(kPlayer);
        p.writeFloat(10.0f); p.writeFloat(20.0f); p.writeFloat(30.0f);
        p.writeFloat(40.0f); p.writeFloat(50.0f); p.writeFloat(60.0f);
        p.writeUInt32(5996);      // ammoDisplayId (arrow)
        p.writeUInt32(24);        // INVTYPE_AMMO

        wowee::game::SpellStartData parsed;
        REQUIRE(s.handler().getPacketParsers()->parseSpellStart(p, parsed));
        CHECK_FALSE(p.hasData()); // 0 unread bytes!
        CHECK(parsed.targetGuid == kPlayer);
        CHECK(parsed.targetFlags == (0x02u | 0x20u | 0x40u));
        REQUIRE(parsed.source.has_value());
        CHECK(parsed.source->x == Catch::Approx(10.0f));
        REQUIRE(parsed.destination.has_value());
        CHECK(parsed.destination->z == Catch::Approx(60.0f));
        CHECK(parsed.ammoDisplayId == 5996u);
        CHECK(parsed.ammoInventoryType == 24u);
    }

    // 5. P1 SMSG_DISPEL_FAILED + SMSG_SPELLDISPELLOG + SMSG_SPELLSTEALLOG preserve both spell IDs
    {
        Session s("tbc");
        s.handler().setTargetGuidRaw(kMob);
        std::vector<std::vector<std::string>> cleu;
        s.handler().setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>& args) {
            if (ev == "COMBAT_LOG_EVENT_UNFILTERED") cleu.push_back(args);
        });

        // Multi-aura SMSG_DISPEL_FAILED
        Packet df(wireOpcode(Opcode::SMSG_DISPEL_FAILED));
        df.writeUInt64(kPlayer);
        df.writeUInt64(kMob);
        df.writeUInt32(kDispelMagic); // 527
        df.writeUInt32(1243);         // Fortitude
        df.writeUInt32(1459);         // Arcane Intellect
        s.receive(df);

        const auto& texts = s.handler().getCombatText();
        REQUIRE(texts.size() == 2);
        CHECK(texts[0].spellId == kDispelMagic);
        CHECK(texts[0].extraSpellId == 1243u);
        CHECK(texts[1].spellId == kDispelMagic);
        CHECK(texts[1].extraSpellId == 1459u);
        REQUIRE(cleu.size() == 2);
        CHECK(cleu[0][1] == "SPELL_DISPEL_FAILED");
        CHECK(cleu[0][8] == "527");
        CHECK(cleu[0][11] == "1243");
        CHECK(cleu[1][11] == "1459");

        // SMSG_SPELLDISPELLOG + SMSG_SPELLSTEALLOG carry casting spell in extraSpellId
        cleu.clear();
        s.receive(dispelLog(false, kMob, kPlayer));
        CHECK(s.handler().getCombatText().back().spellId == kPowerWordFortitude);
        CHECK(s.handler().getCombatText().back().extraSpellId == kDispelMagic);
        REQUIRE(cleu.size() == 1);
        CHECK(cleu[0][1] == "SPELL_DISPEL");
        CHECK(cleu[0][8] == "527");
        CHECK(cleu[0][11] == "1243");
        CHECK(cleu[0][14] == "BUFF");
    }

    // 6. P1 readCastResultArgs: SPELL_FAILED_NOT_READY (0x3F) optional u32 + SPELL_FAILED_MIN_SKILL (0x95)
    {
        Session s("tbc");
        Packet pNotReadyPerm(wireOpcode(Opcode::SMSG_CAST_FAILED));
        pNotReadyPerm.writeUInt32(20608); // Reincarnation
        pNotReadyPerm.writeUInt8(0x3F);   // TBC SPELL_FAILED_NOT_READY -> 67
        pNotReadyPerm.writeUInt8(1);      // castCount
        pNotReadyPerm.writeUInt32(1);     // IsSpellOnPermanentCooldown = 1
        wowee::game::CastFailedData cfPerm;
        REQUIRE(s.handler().getPacketParsers()->parseCastFailed(pNotReadyPerm, cfPerm));
        CHECK_FALSE(pNotReadyPerm.hasData());
        CHECK(cfPerm.result == 67u);
        CHECK(cfPerm.miscArg == 1u);

        Packet pMinSkill(wireOpcode(Opcode::SMSG_CAST_FAILED));
        pMinSkill.writeUInt32(2575);
        pMinSkill.writeUInt8(0x95);       // TBC SPELL_FAILED_MIN_SKILL -> 150
        pMinSkill.writeUInt8(2);
        pMinSkill.writeUInt32(186);       // Mining SkillLine
        wowee::game::CastFailedData cfSkill;
        REQUIRE(s.handler().getPacketParsers()->parseCastFailed(pMinSkill, cfSkill));
        CHECK_FALSE(pMinSkill.hasData());
        CHECK(cfSkill.result == 150u);
        CHECK(cfSkill.miscArg == 186u);
    }

    // 7. P1 SMSG_SPELLBREAKLOG (0x14F): SPELL_AURA_BROKEN and SPELL_AURA_BROKEN_SPELL
    {
        Session s("tbc");
        CHECK(wireOpcode(Opcode::SMSG_SPELLBREAKLOG) == 0x014Fu);
        s.handler().setTargetGuidRaw(kMob);
        std::vector<std::vector<std::string>> cleu;
        s.handler().setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>& args) {
            if (ev == "COMBAT_LOG_EVENT_UNFILTERED") cleu.push_back(args);
        });

        // Fireball (133) breaks Polymorph (118, debuff=1) on kMob
        Packet brkSpell(wireOpcode(Opcode::SMSG_SPELLBREAKLOG));
        brkSpell.writePackedGuid(kMob);
        brkSpell.writePackedGuid(kPlayer);
        brkSpell.writeUInt32(133); // breakingSpellId = Fireball
        brkSpell.writeUInt8(0);
        brkSpell.writeUInt32(1);
        brkSpell.writeUInt32(118); // broken aura = Polymorph
        brkSpell.writeUInt8(1);    // !IsPositive() => DEBUFF
        s.receive(brkSpell);

        REQUIRE(cleu.size() == 1);
        REQUIRE(cleu[0].size() >= 15);
        CHECK(cleu[0][1] == "SPELL_AURA_BROKEN_SPELL");
        CHECK(cleu[0][8] == "118");
        CHECK(cleu[0][11] == "133");
        CHECK(cleu[0][14] == "DEBUFF");

        // Melee swing (breakingSpellId = 0) breaks Ice Armor (168, debuff=0 => BUFF)
        cleu.clear();
        Packet brkMelee(wireOpcode(Opcode::SMSG_SPELLBREAKLOG));
        brkMelee.writePackedGuid(kPlayer);
        brkMelee.writePackedGuid(kMob);
        brkMelee.writeUInt32(0);
        brkMelee.writeUInt8(0);
        brkMelee.writeUInt32(1);
        brkMelee.writeUInt32(168);
        brkMelee.writeUInt8(0);    // BUFF
        s.receive(brkMelee);

        REQUIRE(cleu.size() == 1);
        REQUIRE(cleu[0].size() >= 12);
        CHECK(cleu[0][1] == "SPELL_AURA_BROKEN");
        CHECK(cleu[0][8] == "168");
        CHECK(cleu[0][11] == "BUFF");
    }

    // 8. P1 SMSG_PET_ACTION_SOUND (0x324) & SMSG_PET_DISMISS_SOUND (0x325) + TBC CreatureSoundData
    {
        Session s("tbc");
        auto* ch = s.handler().getCombatHandler();
        REQUIRE(ch != nullptr);

        uint64_t seenPetGuid = 0;
        uint32_t seenPetTalk = 99;
        uint32_t seenDismissRow = 0;
        glm::vec3 seenDismissPos{0.0f};
        ch->setPetActionSoundCallback([&](uint64_t g, uint32_t talk) {
            seenPetGuid = g;
            seenPetTalk = talk;
        });
        ch->setPetDismissSoundCallback([&](uint32_t rowId, const glm::vec3& pos) {
            seenDismissRow = rowId;
            seenDismissPos = pos;
        });

        constexpr uint64_t kPetGuid = 0xF14000102A000001ull;
        Packet pAct(wireOpcode(Opcode::SMSG_PET_ACTION_SOUND));
        pAct.writeUInt64(kPetGuid);
        pAct.writeUInt32(1); // PET_TALK_ATTACK
        s.receive(pAct);
        CHECK(seenPetGuid == kPetGuid);
        CHECK(seenPetTalk == 1u);

        Packet pDis(wireOpcode(Opcode::SMSG_PET_DISMISS_SOUND));
        pDis.writeUInt32(68); // Doomguard CreatureSoundData row
        pDis.writeFloat(100.0f);
        pDis.writeFloat(-200.0f);
        pDis.writeFloat(15.5f);
        s.receive(pDis);
        CHECK(seenDismissRow == 68u);
        CHECK(seenDismissPos.x == Catch::Approx(100.0f));
        CHECK(seenDismissPos.y == Catch::Approx(-200.0f));
        CHECK(seenDismissPos.z == Catch::Approx(15.5f));

        // Verify CreatureSoundData columns 28/29/30 against real TBC DBC if present
        const std::filesystem::path dbcDir = "/workspace/azerothcore/wowee/Data/expansions/tbc/dbfilesclient";
        if (std::filesystem::exists(dbcDir / "creaturesounddata.dbc")) {
            auto loadDbc = [&](const char* name) {
                wowee::pipeline::DBCFile f;
                std::ifstream in(dbcDir / name, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());
                REQUIRE(f.load(bytes));
                return f;
            };
            const auto disp = loadDbc("creaturedisplayinfo.dbc");
            const auto mod  = loadDbc("creaturemodeldata.dbc");
            const auto snd  = loadDbc("creaturesounddata.dbc");
            const auto ftl  = loadDbc("footstepterrainlookup.dbc");
            const auto tt   = loadDbc("terraintype.dbc");
            wowee::audio::CreatureSoundIndex idx;
            idx.load(disp, mod, snd, ftl, tt);
            // Doomguard (row 68): Attack=9104, Order=9105, Dismiss=9103 (NOT SoundEntries[68]!)
            CHECK(idx.sound(68, wowee::audio::CreatureSoundColumn::PetAttack) == 9104u);
            CHECK(idx.sound(68, wowee::audio::CreatureSoundColumn::PetOrder) == 9105u);
            CHECK(idx.sound(68, wowee::audio::CreatureSoundColumn::PetDismiss) == 9103u);
        }
    }
}
