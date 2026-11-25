// SMSG_SPELLLOGEXECUTE through GameHandler's own dispatch, built as
// AzerothCore's Spell::SendLogExecute (WotLK) and cmangos's SpellLog (classic
// and TBC) write it: the caster packed everywhere, a target packed on TBC and
// WotLK and whole on classic. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <algorithm>
#include <string>

using replay::Session;

namespace {

using wowee::game::CombatTextEntry;
using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

constexpr uint64_t kPlayer = Session::kPlayerGuid;
constexpr uint64_t kMob = 0xF130000000000123ull;

// The servers' SpellEffects numbers.
constexpr uint32_t kPowerDrain = 8;
constexpr uint32_t kPowerBurn = 62;
constexpr uint32_t kInterruptCast = 68;
constexpr uint32_t kFeedPet = 101;

Packet executeLog(uint64_t caster, uint32_t spellId, uint32_t effect) {
    Packet p(wireOpcode(Opcode::SMSG_SPELLLOGEXECUTE));
    p.writePackedGuid(caster);
    p.writeUInt32(spellId);
    p.writeUInt32(1);
    p.writeUInt32(effect);
    p.writeUInt32(1);
    return p;
}

void writeTarget(Packet& p, bool classic, uint64_t guid) {
    if (classic) p.writeUInt64(guid);
    else p.writePackedGuid(guid);
}

// Spell::ExecuteLogEffectTakeTargetPower.
Packet takePower(bool classic, uint32_t effect) {
    Packet p = executeLog(kPlayer, 5138, effect);
    writeTarget(p, classic, kMob);
    p.writeUInt32(250);
    p.writeUInt32(0);
    p.writeFloat(effect == kPowerDrain ? 1.0f : 0.0f);
    return p;
}

bool hasText(Session& s, CombatTextEntry::Type type, uint32_t spellId) {
    const auto& texts = s.handler().getCombatText();
    return std::any_of(texts.begin(), texts.end(), [&](const CombatTextEntry& e) {
        return e.type == type && e.spellId == spellId;
    });
}

bool chatHas(Session& s, const std::string& text) {
    const auto& history = s.handler().getChatHistory();
    return std::any_of(history.begin(), history.end(), [&](const auto& m) {
        return m.message.find(text) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("an interrupt in the execute log is effect 68", "[replay][execute]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        Session s(expansion);
        // On the target, where the panel shows spell mechanics by default.
        s.handler().setTargetGuidRaw(kMob);
        Packet p = executeLog(kPlayer, 2139, kInterruptCast);
        writeTarget(p, std::string(expansion) == "classic", kMob);
        p.writeUInt32(133);
        s.receive(p);
        CHECK(hasText(s, CombatTextEntry::INTERRUPT, 133));
    }
}

TEST_CASE("a drain is effect 8 and a burn is effect 62", "[replay][execute]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        for (uint32_t effect : {kPowerDrain, kPowerBurn}) {
            CAPTURE(expansion, effect);
            Session s(expansion);
            s.receive(takePower(std::string(expansion) == "classic", effect));
            const auto& texts = s.handler().getCombatText();
            const auto drain = std::find_if(texts.begin(), texts.end(), [](const auto& e) {
                return e.type == CombatTextEntry::POWER_DRAIN;
            });
            REQUIRE(drain != texts.end());
            CHECK(drain->spellId == 5138);
            CHECK(drain->amount == 250);
        }
    }
}

TEST_CASE("feeding a pet is effect 101, with a packed caster on TBC too", "[replay][execute]") {
    for (const char* expansion : {"wotlk", "tbc"}) {
        CAPTURE(expansion);
        Session s(expansion);
        Packet p = executeLog(kPlayer, 6991, kFeedPet);
        p.writeUInt32(4540);
        s.receive(p);
        auto queries = s.sent().of(Opcode::CMSG_ITEM_QUERY_SINGLE);
        REQUIRE(queries.size() == 1);
        CHECK(queries[0].readUInt32() == 4540u);
        CHECK_FALSE(chatHas(s, "You feed your pet"));
    }
}

TEST_CASE("power drain scales rage from tenths on wire", "[replay][execute]") {
    Session s("tbc");
    Packet p = executeLog(kPlayer, 5138, kPowerDrain);
    writeTarget(p, false, kMob);
    p.writeUInt32(100);  // 10 Rage in tenths
    p.writeUInt32(1);    // POWER_RAGE
    p.writeFloat(1.0f);
    s.receive(p);
    const auto& texts = s.handler().getCombatText();
    REQUIRE(texts.size() == 2);
    CHECK(texts[0].type == CombatTextEntry::POWER_DRAIN);
    CHECK(texts[0].powerType == 1);
    CHECK(texts[0].amount == 10);
    CHECK(texts[1].type == CombatTextEntry::ENERGIZE);
    CHECK(texts[1].powerType == 1);
    CHECK(texts[1].amount == 10);
}

TEST_CASE("TBC SMSG_SPELLLOGEXECUTE parses ADD_EXTRA_ATTACKS and all target-GUID effects without aborting trailing effects",
          "[replay][execute][tbc]") {
    namespace SE = wowee::game::SpellEffect;

    // 1. ADD_EXTRA_ATTACKS (19) records extra swings + fires SPELL_EXTRA_ATTACKS
    {
        Session s("tbc");
        std::vector<std::vector<std::string>> cleu;
        s.handler().setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>& args) {
            if (ev == "COMBAT_LOG_EVENT_UNFILTERED") cleu.push_back(args);
        });
        Packet p = executeLog(kPlayer, 25505, SE::ADD_EXTRA_ATTACKS);
        p.writePackedGuid(kPlayer);
        p.writeUInt32(2);
        s.receive(p);

        const auto& texts = s.handler().getCombatText();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0].type == CombatTextEntry::EXTRA_ATTACKS);
        CHECK(texts[0].spellId == 25505u);
        CHECK(texts[0].amount == 2);

        REQUIRE(cleu.size() == 1);
        REQUIRE(cleu[0].size() >= 12);
        CHECK(cleu[0][1] == "SPELL_EXTRA_ATTACKS");
        CHECK(cleu[0][8] == "25505");
        CHECK(cleu[0][11] == "2");
    }

    // 2. Multi-effect packet containing every TBC target-GUID effect + DURABILITY_DAMAGE
    //    before INTERRUPT_CAST and POWER_DRAIN: trailing effects must NOT be aborted.
    {
        Session s("tbc");
        s.handler().setTargetGuidRaw(kMob);
        std::vector<std::vector<std::string>> cleu;
        s.handler().setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>& args) {
            if (ev == "COMBAT_LOG_EVENT_UNFILTERED") cleu.push_back(args);
        });

        const uint32_t targetGuidEffects[] = {
            SE::RESURRECT, SE::SUMMON, SE::OPEN_LOCK, SE::TRANS_DOOR,
            SE::SUMMON_PET, SE::OPEN_LOCK_ITEM, SE::SUMMON_OBJECT_WILD,
            SE::DUEL, SE::DISMISS_PET,
            SE::SUMMON_OBJECT_SLOT1, SE::SUMMON_OBJECT_SLOT2,
            SE::SUMMON_OBJECT_SLOT3, SE::SUMMON_OBJECT_SLOT4,
            SE::RESURRECT_NEW,
        };
        const uint32_t totalEffects = static_cast<uint32_t>(std::size(targetGuidEffects)) + 3u;

        Packet p(wireOpcode(Opcode::SMSG_SPELLLOGEXECUTE));
        p.writePackedGuid(kPlayer);
        p.writeUInt32(2139); // Counterspell
        p.writeUInt32(totalEffects);

        for (uint32_t eff : targetGuidEffects) {
            p.writeUInt32(eff);
            p.writeUInt32(1);
            p.writePackedGuid(kMob);
        }

        p.writeUInt32(SE::DURABILITY_DAMAGE);
        p.writeUInt32(1);
        p.writePackedGuid(kMob);
        p.writeUInt32(19019);
        p.writeUInt32(1);

        p.writeUInt32(SE::INTERRUPT_CAST);
        p.writeUInt32(1);
        p.writePackedGuid(kMob);
        p.writeUInt32(133); // Fireball

        p.writeUInt32(SE::POWER_DRAIN);
        p.writeUInt32(1);
        p.writePackedGuid(kMob);
        p.writeUInt32(150);
        p.writeUInt32(0);
        p.writeFloat(0.0f);

        s.receive(p);

        CHECK(hasText(s, CombatTextEntry::INTERRUPT, 133));
        CHECK(hasText(s, CombatTextEntry::POWER_DRAIN, 2139));

        const auto it = std::find_if(cleu.begin(), cleu.end(), [](const auto& a) {
            return a.size() >= 14 && a[1] == "SPELL_INTERRUPT";
        });
        REQUIRE(it != cleu.end());
        CHECK((*it)[8] == "2139");  // Interrupting spell (Counterspell)
        CHECK((*it)[11] == "133");  // Interrupted spell (Fireball)
    }
}
