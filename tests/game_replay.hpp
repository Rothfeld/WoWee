#pragma once

// A GameHandler fed server packets the way the socket feeds it, with no server,
// window or renderer behind it.
//
// Most of what goes wrong between this client and a realm is a byte read from
// the wrong place: a flag tested against the wrong bit, a field index from the
// wrong expansion, a guid read packed where the server wrote it whole. None of
// that shows in a unit test of a helper, because the helper is right and the
// call site is not. Here the packet goes in through the same dispatch the
// realm's would, with one expansion active and its own opcode and update field
// tables and packet parsers, and the test reads back what the client concluded.
//
// The packets are built by hand, field by field as the server writes them;
// each test says which server function it copies.
//
// The socket is never connected; it only keeps what the client sends, for a
// test to read back.

#include <catch_amalgamated.hpp>

#include "core/application.hpp"
#include "game/expansion_profile.hpp"
#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/inventory_handler.hpp"
#include "game/opcode_table.hpp"
#include "game/packet_parsers.hpp"
#include "game/social_handler.hpp"
#include "game/update_field_table.hpp"
#include "network/packet.hpp"
#include "network/world_socket.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wowee {
namespace game {

struct GameHandlerTestAccess {
    static void setAreaMap(GameHandler& gh, uint32_t area, uint32_t map) {
        gh.areaMapCache_[area] = map;
    }
    // AreaTable without the DBC: the name an area id resolves to.
    static void setAreaName(GameHandler& gh, uint32_t area, const std::string& name) {
        gh.areaNameCache_[area] = name;
        gh.areaNameCacheLoaded_ = true;
    }
    // Map.dbc without the DBC: name and optional TBC difficulty requirement texts.
    static void setMapInfo(GameHandler& gh, uint32_t mapId, const std::string& name,
                           std::array<std::string, 3> difficultyText = {}) {
        gh.mapNameCacheLoaded_ = true;
        gh.mapNameCache_[mapId] = name;
        if (!difficultyText[0].empty() || !difficultyText[1].empty() || !difficultyText[2].empty()) {
            gh.mapDifficultyTextCache_[mapId] = std::move(difficultyText);
        } else {
            gh.mapDifficultyTextCache_.erase(mapId);
        }
    }
    // Faction.dbc without the DBC: name, repListId, optional parentFactionId.
    static void setFaction(GameHandler& gh, uint32_t factionId, uint32_t repListId,
                           const std::string& name, uint32_t parentId = 0) {
        gh.factionNameCacheLoaded_ = true;
        gh.factionNameCache_[factionId] = name;
        if (repListId != 0xFFFFFFFFu) {
            gh.factionRepListToId_[repListId] = factionId;
            gh.factionIdToRepList_[factionId] = repListId;
        }
        if (parentId != 0) gh.factionParent_[factionId] = parentId;
    }
    // Queued and drained the way update() drains the socket's packets, frame
    // after frame, since update object blocks are applied after their packet
    // rather than inside it. Bounded, so work that never drains fails here
    // instead of hanging the run or leaving the test half-applied state.
    static void receive(GameHandler& gh, const network::Packet& packet) {
        gh.enqueueIncomingPacket(packet);
        auto pending = [&] {
            return !gh.pendingIncomingPackets_.empty() ||
                   gh.entityController_->hasPendingUpdateObjectWork();
        };
        for (int frame = 0; frame < 1000 && pending(); ++frame) gh.processQueuedIncomingPackets();
        REQUIRE_FALSE(pending());
    }
    static void enterWorld(GameHandler& gh) { gh.setState(WorldState::IN_WORLD); }
    // The thirty seconds of a running ready check, over.
    static void expireReadyCheck(GameHandler& gh) {
        gh.socialHandler_->readyCheckDeadline_ = std::chrono::steady_clock::now();
    }
    static void setSocket(GameHandler& gh, std::unique_ptr<network::WorldSocket> socket) {
        gh.socket = std::move(socket);
    }
    static void setExpansions(core::Application& app, std::unique_ptr<ExpansionRegistry> registry) {
        app.expansionRegistry_ = std::move(registry);
    }
    static void fireBagUpdates(GameHandler& gh) {
        if (gh.inventoryHandler_) gh.inventoryHandler_->fireBagUpdates();
    }
};

}  // namespace game
}  // namespace wowee

namespace replay {

using wowee::game::Opcode;
using wowee::game::UF;

// SMSG_UPDATE_OBJECT, one block per call, in WotLK's layout. Fields are keyed by name and
// resolved through the loaded update field table, so a test states which field
// it sets rather than which index that is this expansion.
class UpdateObject {
public:
    using Fields = std::map<UF, uint32_t>;

    // 2.4.3's layout instead (CMaNGOS TBC UpdateData::BuildPacket and
    // BuildMovementUpdate): a has-transport byte after the block count and
    // one byte of update flags.
    UpdateObject& tbc() {
        REQUIRE(blocks_ == 0);
        tbc_ = true;
        return *this;
    }

    // An object appearing, laid out as Object::BuildCreateUpdateBlockForPlayer
    // writes it, but with the movement part cut down to a stationary position
    // (UPDATEFLAG_STATIONARY_POSITION): where it stands does not matter to
    // anything these tests read. `self` is UPDATEFLAG_SELF, the mark on the
    // receiving player's own block, which adds nothing to the payload.
    UpdateObject& create(uint64_t guid, wowee::game::ObjectType type, const Fields& fields,
                         bool self = false) {
        ++blocks_;
        body_.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::CREATE_OBJECT2));
        body_.writePackedGuid(guid);
        body_.writeUInt8(static_cast<uint8_t>(type));
        const auto flags = static_cast<uint16_t>(kStationaryPosition | (self ? kSelf : 0));
        if (tbc_) body_.writeUInt8(static_cast<uint8_t>(flags));
        else body_.writeUInt16(flags);
        for (int i = 0; i < 4; ++i) body_.writeFloat(0.0f);
        writeFields(fields);
        return *this;
    }

    // Fields changing on something already there.
    UpdateObject& values(uint64_t guid, const Fields& fields) {
        ++blocks_;
        body_.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::VALUES));
        body_.writePackedGuid(guid);
        writeFields(fields);
        return *this;
    }

    // The same, by wire index, for fields the table has no name for (TBC's
    // vendor buyback and vanity pet slots).
    UpdateObject& valuesAt(uint64_t guid, const std::map<uint16_t, uint32_t>& byIndex) {
        ++blocks_;
        body_.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::VALUES));
        body_.writePackedGuid(guid);
        writeIndexedFields(byIndex);
        return *this;
    }

    [[nodiscard]] wowee::network::Packet packet() const {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
        p.writeUInt32(blocks_);
        if (tbc_) p.writeUInt8(0);
        const auto& body = body_.getData();
        p.writeBytes(body.data(), body.size());
        return p;
    }

private:
    static constexpr uint16_t kSelf = 0x0001;
    static constexpr uint16_t kStationaryPosition = 0x0040;

    void writeFields(const Fields& fields) {
        std::map<uint16_t, uint32_t> byIndex;
        for (const auto& [field, value] : fields) {
            const uint16_t index = wowee::game::fieldIndex(field);
            REQUIRE(index != 0xFFFF);
            byIndex[index] = value;
        }
        writeIndexedFields(byIndex);
    }

    void writeIndexedFields(const std::map<uint16_t, uint32_t>& byIndex) {
        const uint8_t maskBlocks =
            byIndex.empty() ? 0 : static_cast<uint8_t>(byIndex.rbegin()->first / 32 + 1);
        body_.writeUInt8(maskBlocks);
        for (uint8_t b = 0; b < maskBlocks; ++b) {
            uint32_t mask = 0;
            for (const auto& entry : byIndex)
                if (entry.first / 32 == b) mask |= 1u << (entry.first % 32);
            body_.writeUInt32(mask);
        }
        for (const auto& entry : byIndex) body_.writeUInt32(entry.second);
    }

    uint32_t blocks_ = 0;
    bool tbc_ = false;
    wowee::network::Packet body_;
};

// Keeps what the client sends instead of writing it anywhere.
class SentPackets : public wowee::network::WorldSocket {
public:
    void send(const wowee::network::Packet& packet) override { sent_.push_back(packet); }

    // Everything sent with this opcode, oldest first.
    [[nodiscard]] std::vector<wowee::network::Packet> of(Opcode opcode) const {
        std::vector<wowee::network::Packet> found;
        for (const auto& p : sent_)
            if (p.getOpcode() == wowee::game::wireOpcode(opcode)) found.push_back(p);
        return found;
    }

    // Everything sent, oldest first.
    [[nodiscard]] const std::vector<wowee::network::Packet>& all() const { return sent_; }

private:
    std::vector<wowee::network::Packet> sent_;
};

// A client in the world as one character, ready for packets. WotLK unless
// another expansion is named.
class Session {
public:
    static constexpr uint64_t kPlayerGuid = 0x0000000000000042ull;

    // The expansion is chosen and its tables loaded the way the client does it
    // at startup, from the profiles under Data/expansions in this repository.
    explicit Session(const char* expansion = "wotlk") : handler_(services_) {
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
    const SentPackets& sent() const { return *sent_; }

    void receive(const wowee::network::Packet& packet) {
        wowee::game::GameHandlerTestAccess::receive(handler_, packet);
    }
    void receive(const UpdateObject& update) { receive(update.packet()); }

    // The player's own create block, as the server sends it on login.
    void spawnPlayer(const UpdateObject::Fields& fields) {
        receive(UpdateObject().create(kPlayerGuid, wowee::game::ObjectType::PLAYER, fields,
                                      true));
    }

private:
    // Never initialized: it is here because the game code asks the
    // application's expansion registry which protocol it is speaking.
    wowee::core::Application app_;
    wowee::game::GameServices services_;
    wowee::game::GameHandler handler_;
    SentPackets* sent_ = nullptr;
};

// CMaNGOS-TBC CharacterNameQueryResponse: guid, name, realm byte, race,
// gender, class as uint32s, no declined names.
inline wowee::network::Packet nameResponse(uint64_t guid, const std::string& name) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_NAME_QUERY_RESPONSE));
    p.writeUInt64(guid);
    p.writeString(name);
    p.writeUInt8(0);
    p.writeUInt32(1);
    p.writeUInt32(0);
    p.writeUInt32(4);
    p.writeUInt8(0);
    return p;
}

}  // namespace replay
