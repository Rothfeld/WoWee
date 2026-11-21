// The caster guid of SMSG_SPELL_FAILURE, SMSG_SPELL_FAILED_OTHER and
// SMSG_SPELL_DELAYED through GameHandler's own dispatch: packed on TBC and
// WotLK, as cmangos-tbc and OregonCore write it, and whole on classic. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/spell_cast_anim_policy.hpp"

#include <string>
#include <utility>
#include <vector>

using replay::Session;

namespace {

using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

constexpr uint64_t kMob = 0xF130000000000123ull;

// The names of the events the session fired whose first argument is "target".
struct TargetEvents {
    std::vector<std::string> names;

    explicit TargetEvents(Session& s) {
        s.handler().setTargetGuidRaw(kMob);
        s.handler().setAddonEventCallback([this](const std::string& name, const std::vector<std::string>& args) {
            if (!args.empty() && args.front() == "target") names.push_back(name);
        });
    }
};

void writeCaster(Packet& p, bool classic) {
    if (classic) p.writeUInt64(kMob);
    else p.writePackedGuid(kMob);
}

}  // namespace

TEST_CASE("an interrupted cast names its caster as the server writes it", "[replay][castguid]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        const std::string e = expansion;
        Session s(expansion);
        TargetEvents events(s);
        // Spell::SendInterrupted's SMSG_SPELL_FAILURE: WotLK adds the cast
        // count. Classic's whole guid is the layout this client has always
        // read; vmangos never sends the packet and cmangos-classic packs it.
        Packet p(wireOpcode(Opcode::SMSG_SPELL_FAILURE));
        writeCaster(p, e == "classic");
        if (e == "wotlk") p.writeUInt8(0);
        p.writeUInt32(133);
        p.writeUInt8(0);  // result
        s.receive(p);
        CHECK(events.names == std::vector<std::string>{"UNIT_SPELLCAST_INTERRUPTED", "UNIT_SPELLCAST_STOP"});
    }
}

TEST_CASE("the player's own interrupt is reported by the cast result, not the spell failure", "[replay][castguid]") {
    // CMaNGOS Spell::cancel sends SMSG_SPELL_FAILURE and then, for a cast
    // that is not a channel, SMSG_CAST_FAILED with the same reason. Original
    // 2.4.3 shows one "Interrupted" on the error line and nothing in chat; a
    // broken channel shows nothing. SPELL_FAILED_INTERRUPTED: 40 on WotLK,
    // 0x25 in cmangos-tbc's enum.
    for (const auto& [expansion, result] : {std::pair<const char*, uint8_t>{"wotlk", 40},
                                            std::pair<const char*, uint8_t>{"tbc", 0x25}}) {
        CAPTURE(expansion);
        const bool wotlk = std::string(expansion) == "wotlk";
        Session s(expansion);
        std::vector<std::string> errors;
        s.handler().setUIErrorCallback([&](const std::string& msg) { errors.push_back(msg); });
        s.handler().setGlobalStringLookup([](const char* name) {
            return std::string(name) == "SPELL_FAILED_INTERRUPTED" ? std::string("Interrupted") : std::string();
        });
        const size_t chatBefore = s.handler().getChatHistory().size();

        Packet failure(wireOpcode(Opcode::SMSG_SPELL_FAILURE));
        failure.writePackedGuid(Session::kPlayerGuid);
        if (wotlk) failure.writeUInt8(0);
        failure.writeUInt32(133);
        failure.writeUInt8(result);
        s.receive(failure);
        CHECK(errors.empty());
        CHECK(s.handler().getChatHistory().size() == chatBefore);

        Packet castFailed(wireOpcode(Opcode::SMSG_CAST_FAILED));
        if (wotlk) castFailed.writeUInt8(0);
        castFailed.writeUInt32(133);
        castFailed.writeUInt8(result);
        if (!wotlk) castFailed.writeUInt8(0);  // TBC: cast count after the result
        s.receive(castFailed);
        CHECK(errors == std::vector<std::string>{"Interrupted"});
        CHECK(s.handler().getChatHistory().size() == chatBefore);
    }
}

TEST_CASE("another unit's failed cast names its caster as the server writes it", "[replay][castguid]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        Session s(expansion);
        TargetEvents events(s);
        Packet p(wireOpcode(Opcode::SMSG_SPELL_FAILED_OTHER));
        writeCaster(p, std::string(expansion) == "classic");
        p.writeUInt32(133);
        s.receive(p);
        CHECK(events.names == std::vector<std::string>{"UNIT_SPELLCAST_FAILED", "UNIT_SPELLCAST_STOP"});
    }
}

TEST_CASE("a pushed-back cast names its caster as the server writes it", "[replay][castguid]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        Session s(expansion);
        TargetEvents events(s);
        // Spell::Delayed's SMSG_SPELL_DELAYED.
        Packet p(wireOpcode(Opcode::SMSG_SPELL_DELAYED));
        writeCaster(p, std::string(expansion) == "classic");
        p.writeUInt32(500);
        s.receive(p);
        CHECK(events.names == std::vector<std::string>{"UNIT_SPELLCAST_DELAYED"});
    }
}

namespace {

Packet tbcSpellStart(uint64_t caster, uint32_t spellId, uint32_t castTimeMs, uint64_t target) {
    Packet p(wireOpcode(Opcode::SMSG_SPELL_START));
    p.writePackedGuid(caster);
    p.writePackedGuid(caster);
    p.writeUInt32(spellId);
    p.writeUInt8(1);           // castCount
    p.writeUInt16(0);          // castFlags
    p.writeUInt32(castTimeMs); // castTime
    p.writeUInt32(0x02);       // TARGET_FLAG_UNIT
    p.writePackedGuid(target);
    return p;
}

Packet tbcSpellGo(uint64_t caster, uint32_t spellId, uint64_t target) {
    Packet p(wireOpcode(Opcode::SMSG_SPELL_GO));
    p.writePackedGuid(caster);
    p.writePackedGuid(caster);
    p.writeUInt32(spellId);
    p.writeUInt16(0);          // castFlags
    p.writeUInt32(1000);       // timestamp
    p.writeUInt8(1);           // hitCount
    p.writeUInt64(target);     // full uint64 in TBC hit list
    p.writeUInt8(0);           // missCount
    p.writeUInt32(0x02);       // TARGET_FLAG_UNIT
    p.writePackedGuid(target);
    return p;
}

Packet tbcSpellDelayed(uint64_t caster, uint32_t delayMs) {
    Packet p(wireOpcode(Opcode::SMSG_SPELL_DELAYED));
    p.writePackedGuid(caster);
    p.writeUInt32(delayMs);
    return p;
}

// CMaNGOS TBC SMSG_ATTACKERSTATEUPDATE (normal melee hit -> CombatWound + BloodSpurt).
Packet tbcMeleeHit(uint64_t attacker, uint64_t victim, uint32_t damage) {
    Packet p(wireOpcode(Opcode::SMSG_ATTACKERSTATEUPDATE));
    p.writeUInt32(0x00000002); // HITINFO_NORMALSWING2
    p.writePackedGuid(attacker);
    p.writePackedGuid(victim);
    p.writeUInt32(damage);
    p.writeUInt8(1);           // subDamageCount
    p.writeUInt32(1);          // schoolMask (physical)
    p.writeFloat(static_cast<float>(damage));
    p.writeUInt32(damage);
    p.writeUInt32(0);          // absorbed
    p.writeUInt32(0);          // resisted
    p.writeUInt32(1);          // victimState = VICTIMSTATE_NORMAL
    p.writeUInt32(0);          // attackerState
    p.writeUInt32(0);          // meleeSpellId
    p.writeUInt32(0);          // blocked
    return p;
}

}  // namespace

TEST_CASE("TBC Wrath cast persists across melee flinch and SMSG_SPELL_DELAYED pushback past initial castTime",
          "[replay][castguid][pushback]") {
    constexpr uint32_t kWrathSpellId = 5176;
    Session s("tbc");
    s.handler().setTargetGuidRaw(kMob);

    std::vector<std::string> playerEvents;
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        if (!args.empty() && args.front() == "player") playerEvents.push_back(name);
    });

    int hitReactions = 0;
    s.handler().setHitReactionCallback([&](uint64_t /*attacker*/, uint64_t victim, wowee::game::GameHandler::HitReaction r) {
        if (victim == Session::kPlayerGuid && r == wowee::game::GameHandler::HitReaction::WOUND) {
            ++hitReactions;
        }
    });

    std::vector<wowee::game::SpellAnimPhase> animPhases;
    s.handler().setSpellCastAnimCallback([&](uint64_t guid, wowee::game::SpellAnimPhase phase, uint32_t) {
        if (guid == Session::kPlayerGuid) animPhases.push_back(phase);
    });

    // 1. Start 1500ms Wrath cast.
    s.receive(tbcSpellStart(Session::kPlayerGuid, kWrathSpellId, 1500, kMob));
    REQUIRE(s.handler().isCasting());
    CHECK(s.handler().getCurrentCastSpellId() == kWrathSpellId);
    CHECK(s.handler().getCastTimeTotal() == Catch::Approx(1.5f));
    CHECK(s.handler().getCastTimeRemaining() == Catch::Approx(1.5f));

    // 2. Advance 1.0s into the 1.5s cast (0.5s remaining), then take a melee hit
    //    and two SMSG_SPELL_DELAYED pushbacks (500ms + 500ms). Total cast duration
    //    stays 1.5s while remaining time pushes back toward 1.5s (clamped to total).
    s.handler().updateTimers(1.0f);
    CHECK(s.handler().getCastTimeRemaining() == Catch::Approx(0.5f));
    s.receive(tbcMeleeHit(kMob, Session::kPlayerGuid, 12));
    s.receive(tbcSpellDelayed(Session::kPlayerGuid, 500));
    CHECK(s.handler().getCastTimeTotal() == Catch::Approx(1.5f));
    CHECK(s.handler().getCastTimeRemaining() == Catch::Approx(1.0f));
    s.receive(tbcSpellDelayed(Session::kPlayerGuid, 800));
    CHECK(hitReactions == 1);
    CHECK(s.handler().isCasting());
    CHECK(s.handler().getCastTimeTotal() == Catch::Approx(1.5f));
    CHECK(s.handler().getCastTimeRemaining() == Catch::Approx(1.5f));

    // 3. SMSG_SPELL_GO completes the pushed-back cast cleanly.
    s.receive(tbcSpellGo(Session::kPlayerGuid, kWrathSpellId, kMob));
    CHECK_FALSE(s.handler().isCasting());
    CHECK(animPhases == std::vector<wowee::game::SpellAnimPhase>{
        wowee::game::SpellAnimPhase::PRECAST,
        wowee::game::SpellAnimPhase::CAST,
    });
    CHECK(std::find(playerEvents.begin(), playerEvents.end(), "UNIT_SPELLCAST_DELAYED") != playerEvents.end());

    // 4. Starting another Wrath, pushing it back, and interrupting via SMSG_SPELL_FAILURE clears cast cleanly.
    s.receive(tbcSpellStart(Session::kPlayerGuid, kWrathSpellId, 1500, kMob));
    s.handler().updateTimers(0.9f);
    s.receive(tbcSpellDelayed(Session::kPlayerGuid, 500));
    REQUIRE(s.handler().isCasting());
    CHECK(s.handler().getCastTimeTotal() == Catch::Approx(1.5f));
    CHECK(s.handler().getCastTimeRemaining() == Catch::Approx(1.1f));
    Packet failure(wireOpcode(Opcode::SMSG_SPELL_FAILURE));
    failure.writePackedGuid(Session::kPlayerGuid);
    failure.writeUInt32(kWrathSpellId);
    failure.writeUInt8(0x25); // SPELL_FAILED_INTERRUPTED
    s.receive(failure);
    CHECK_FALSE(s.handler().isCasting());
}

TEST_CASE("TBC remote cast pushback extends state and SMSG_SPELL_FAILED_OTHER stops cast animation",
          "[replay][castguid][pushback]") {
    constexpr uint32_t kWrathSpellId = 5176;
    Session s("tbc");
    s.handler().setTargetGuidRaw(kMob);

    std::vector<wowee::game::SpellAnimPhase> remotePhases;
    s.handler().setSpellCastAnimCallback([&](uint64_t guid, wowee::game::SpellAnimPhase phase, uint32_t) {
        if (guid == kMob) remotePhases.push_back(phase);
    });

    s.receive(tbcSpellStart(kMob, kWrathSpellId, 1500, Session::kPlayerGuid));
    REQUIRE(s.handler().getUnitCastState(kMob) != nullptr);
    CHECK(s.handler().getUnitCastState(kMob)->timeTotal == Catch::Approx(1.5f));

    s.handler().updateTimers(0.9f);
    s.receive(tbcSpellDelayed(kMob, 500));
    REQUIRE(s.handler().getUnitCastState(kMob) != nullptr);
    CHECK(s.handler().getUnitCastState(kMob)->timeTotal == Catch::Approx(1.5f));
    CHECK(s.handler().getUnitCastState(kMob)->timeRemaining == Catch::Approx(1.1f));

    Packet failOther(wireOpcode(Opcode::SMSG_SPELL_FAILED_OTHER));
    failOther.writePackedGuid(kMob);
    failOther.writeUInt32(kWrathSpellId);
    s.receive(failOther);
    CHECK(s.handler().getUnitCastState(kMob) == nullptr);
    CHECK(remotePhases == std::vector<wowee::game::SpellAnimPhase>{
        wowee::game::SpellAnimPhase::PRECAST,
        wowee::game::SpellAnimPhase::STOP,
    });
}

TEST_CASE("TBC back-to-back remote SMSG_SPELL_START / SMSG_SPELL_GO preserves CAST completion",
          "[replay][castguid][spell_cast_anim]") {
    using namespace wowee::rendering;
    using wowee::game::SpellAnimPhase;

    constexpr uint32_t kEarthgrabTotem = 8376;   // Precast 52, Cast 54
    constexpr uint32_t kLightningBolt = 15234;   // Precast 51, Cast 53
    constexpr uint32_t kNoAnim = ~0u;

    Session s("tbc");
    SpellCastAnimState animState{};
    bool playingOneShot = false;
    uint32_t playingOneShotAnim = 0;
    std::vector<SpellCastAnimAction> actions;

    s.handler().setSpellCastAnimCallback([&](uint64_t guid, SpellAnimPhase phase, uint32_t spellId) {
        if (guid != kMob) return;
        uint32_t kitAnim = kNoAnim;
        if (spellId == kEarthgrabTotem) {
            kitAnim = (phase == SpellAnimPhase::PRECAST) ? anim::READY_SPELL_OMNI : anim::SPELL_CAST_OMNI;
        } else if (spellId == kLightningBolt) {
            kitAnim = (phase == SpellAnimPhase::PRECAST) ? anim::READY_SPELL_DIRECTED : anim::SPELL_CAST_DIRECTED;
        }
        const auto action = stepRemoteSpellCastAnim(
            animState, phase, spellId, kitAnim, kNoAnim, anim::STAND,
            playingOneShot, playingOneShotAnim);
        actions.push_back(action);
        if (action.kind == SpellCastAnimAction::Kind::PLAY_ONESHOT) {
            playingOneShot = true;
            playingOneShotAnim = action.animation;
        } else if (action.kind == SpellCastAnimAction::Kind::PLAY_LOOP) {
            playingOneShot = false;
            playingOneShotAnim = 0;
        }
    });

    // Earthgrab Totem (500ms cast) -> SMSG_SPELL_GO -> 0ms later Lightning Bolt (3000ms cast)
    s.receive(tbcSpellStart(kMob, kEarthgrabTotem, 500, Session::kPlayerGuid));
    s.receive(tbcSpellGo(kMob, kEarthgrabTotem, Session::kPlayerGuid));
    s.receive(tbcSpellStart(kMob, kLightningBolt, 3000, Session::kPlayerGuid));

    REQUIRE(actions.size() == 3);
    CHECK(actions[0].kind == SpellCastAnimAction::Kind::PLAY_LOOP);
    CHECK(actions[0].animation == anim::READY_SPELL_OMNI);
    CHECK(actions[1].kind == SpellCastAnimAction::Kind::PLAY_ONESHOT);
    CHECK(actions[1].animation == anim::SPELL_CAST_OMNI);
    CHECK(actions[1].returnAnimation == anim::STAND);
    // 0ms back-to-back Lightning Bolt PRECAST retargets SpellCastOmni's return to ReadySpellDirected
    // instead of cutting SpellCastOmni short:
    CHECK(actions[2].kind == SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN);
    CHECK(actions[2].returnAnimation == anim::READY_SPELL_DIRECTED);
}
