// The caster guid of SMSG_SPELL_FAILURE, SMSG_SPELL_FAILED_OTHER and
// SMSG_SPELL_DELAYED through GameHandler's own dispatch: packed on TBC and
// WotLK, as cmangos-tbc and OregonCore write it, and whole on classic. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

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
