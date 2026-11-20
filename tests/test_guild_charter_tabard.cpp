#include <catch_amalgamated.hpp>
#include "core/application.hpp"
#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "game/update_field_table.hpp"
#include "pipeline/guild_tabard.hpp"
#include <cstring>
#include <string>
#include <vector>

namespace wowee::core {
Application* Application::instance = nullptr;
}

using namespace wowee::game;
using namespace wowee::network;
using namespace wowee::pipeline;

namespace {

struct TbcOpcodeFixture {
    OpcodeTable opcodes;
    UpdateFieldTable fields;
    TbcOpcodeFixture() {
        REQUIRE(opcodes.loadFromJson("Data/expansions/tbc/opcodes.json"));
        setActiveOpcodeTable(&opcodes);
        REQUIRE(fields.loadFromJson("Data/expansions/tbc/update_fields.json"));
        setActiveUpdateFieldTable(&fields);
    }
    ~TbcOpcodeFixture() {
        setActiveOpcodeTable(nullptr);
        setActiveUpdateFieldTable(nullptr);
    }
};

}  // namespace

TEST_CASE("TBC CMSG_PETITION_BUY matches mangos-tbc HandlePetitionBuyOpcode layout", "[guild][petition][tbc]") {
    TbcOpcodeFixture fx;
    const uint64_t npcGuid = 0xF13000136E013741ULL;
    const std::string name = "Charter XV";
    const uint32_t clientIndex = 3;  // 5v5 arena charter or 1 for guild

    Packet pkt = PetitionBuyPacket::build(npcGuid, name, clientIndex, /*isWotlk=*/false);
    REQUIRE(pkt.getOpcode() == 0x1BD);

    // Reproduce mangos-tbc PetitionsHandler.cpp::HandlePetitionBuyOpcode reads exactly:
    REQUIRE(pkt.readUInt64() == npcGuid);
    REQUIRE(pkt.readUInt32() == 0);
    REQUIRE(pkt.readUInt64() == 0);
    REQUIRE(pkt.readString() == name);
    for (int i = 0; i < 10; ++i) REQUIRE(pkt.readUInt32() == 0);
    REQUIRE(pkt.readUInt16() == 0);
    REQUIRE(pkt.readUInt8() == 0);
    REQUIRE(pkt.readUInt32() == clientIndex);
    REQUIRE(pkt.readUInt32() == 0);
    REQUIRE(pkt.getRemainingSize() == 0);
}

TEST_CASE("WotLK CMSG_PETITION_BUY preserves 10 empty signature strings", "[guild][petition][wotlk]") {
    TbcOpcodeFixture fx;
    const uint64_t npcGuid = 0xF13000136E013741ULL;
    const std::string name = "WotLK Guild";
    const uint32_t clientIndex = 2;

    Packet pkt = PetitionBuyPacket::build(npcGuid, name, clientIndex, /*isWotlk=*/true);
    REQUIRE(pkt.readUInt64() == npcGuid);
    REQUIRE(pkt.readUInt32() == 0);
    REQUIRE(pkt.readUInt64() == 0);
    REQUIRE(pkt.readString() == name);
    REQUIRE(pkt.readString().empty());
    for (int i = 0; i < 7; ++i) REQUIRE(pkt.readUInt32() == 0);
    REQUIRE(pkt.readUInt16() == 0);
    for (int i = 0; i < 3; ++i) REQUIRE(pkt.readUInt32() == 0);
    for (int i = 0; i < 10; ++i) REQUIRE(pkt.readString().empty());
    REQUIRE(pkt.readUInt32() == clientIndex);
    REQUIRE(pkt.readUInt32() == 0);
    REQUIRE(pkt.getRemainingSize() == 0);
}

TEST_CASE("TBC SMSG_PETITION_SHOWLIST parses Guild and Arena charters", "[guild][petition][tbc]") {
    TbcOpcodeFixture fx;
    const uint64_t npcGuid = 0xF13000136E013741ULL;

    SECTION("Guild Master single charter") {
        Packet pkt(0x1BC);
        pkt.writeUInt64(npcGuid);
        pkt.writeUInt8(1);
        pkt.writeUInt32(1);      // index
        pkt.writeUInt32(5863);   // Guild Charter
        pkt.writeUInt32(16161);  // displayId
        pkt.writeUInt32(1000);   // 10s
        pkt.writeUInt32(0);      // unknown
        pkt.writeUInt32(9);      // required signatures

        PetitionShowlistData data;
        REQUIRE(PetitionShowlistParser::parse(pkt, data));
        REQUIRE(data.npcGuid == npcGuid);
        REQUIRE(data.isGuildCharter());
        REQUIRE(data.charters.size() == 1);
        REQUIRE(data.cost == 1000);
        REQUIRE(data.requiredSigs == 9);
    }

    SECTION("Arena Organizer three charters") {
        Packet pkt(0x1BC);
        pkt.writeUInt64(npcGuid);
        pkt.writeUInt8(3);
        const uint32_t items[3] = {23560, 23561, 23562};
        const uint32_t costs[3] = {800000, 1200000, 2000000};
        const uint32_t sizes[3] = {2, 3, 5};
        for (int i = 0; i < 3; ++i) {
            pkt.writeUInt32(static_cast<uint32_t>(i + 1));
            pkt.writeUInt32(items[i]);
            pkt.writeUInt32(16161);
            pkt.writeUInt32(costs[i]);
            pkt.writeUInt32(sizes[i]);
            pkt.writeUInt32(sizes[i]);
        }

        PetitionShowlistData data;
        REQUIRE(PetitionShowlistParser::parse(pkt, data));
        REQUIRE_FALSE(data.isGuildCharter());
        REQUIRE(data.charters.size() == 3);
        REQUIRE(data.charters[0].itemId == 23560);
        REQUIRE(data.charters[1].cost == 1200000);
        REQUIRE(data.charters[2].index == 3);
    }
}

TEST_CASE("MSG_PETITION_DECLINE and MSG_SAVE_GUILD_EMBLEM wire layouts", "[guild][tabard][tbc]") {
    TbcOpcodeFixture fx;

    const uint64_t petGuid = 0x4000000000001234ULL;
    Packet dec = PetitionDeclinePacket::build(petGuid);
    REQUIRE(dec.getOpcode() == 0x1C2);
    REQUIRE(dec.readUInt64() == petGuid);
    REQUIRE(dec.getRemainingSize() == 0);

    const uint64_t vendorGuid = 0xF130001449013745ULL;
    Packet save = SaveGuildEmblemPacket::build(vendorGuid, 42, 7, 3, 12, 19);
    REQUIRE(save.getOpcode() == 0x1F1);
    REQUIRE(save.readUInt64() == vendorGuid);
    REQUIRE(save.readUInt32() == 42);
    REQUIRE(save.readUInt32() == 7);
    REQUIRE(save.readUInt32() == 3);
    REQUIRE(save.readUInt32() == 12);
    REQUIRE(save.readUInt32() == 19);
    REQUIRE(save.getRemainingSize() == 0);
}

TEST_CASE("GuildTabardEmblem validation, cycling, and region layer generation", "[guild][tabard]") {
    GuildTabardEmblem none;
    REQUIRE_FALSE(none.isValid());

    GuildTabardEmblem emb{0, 0, 0, 0, 0};
    REQUIRE(emb.isValid());
    REQUIRE(isGuildTabardTextureName("Tabard_A_05Default_Chest_TU"));
    REQUIRE(isGuildTabardTextureName("tabard_a_05default_chest_tl"));
    REQUIRE_FALSE(isGuildTabardTextureName("Tabard_A_13Aldor_Chest_TU"));

    // Wrap backwards from 0 -> max-1
    REQUIRE(cycleGuildTabardVariation(emb, 1, -1));
    REQUIRE(emb.emblemStyle == 169);
    REQUIRE(cycleGuildTabardVariation(emb, 2, -1));
    REQUIRE(emb.emblemColor == 16);
    REQUIRE(cycleGuildTabardVariation(emb, 3, -1));
    REQUIRE(emb.borderStyle == 5);
    REQUIRE(cycleGuildTabardVariation(emb, 4, -1));
    REQUIRE(emb.borderColor == 16);
    REQUIRE(cycleGuildTabardVariation(emb, 5, -1));
    REQUIRE(emb.backgroundColor == 50);

    // Wrap forwards from max-1 -> 0
    REQUIRE(cycleGuildTabardVariation(emb, 1, 1));
    REQUIRE(emb.emblemStyle == 0);
    REQUIRE(cycleGuildTabardVariation(emb, 3, 1));
    REQUIRE(emb.borderStyle == 0);

    std::vector<std::pair<int, std::string>> layers;
    GuildTabardEmblem sample{12, 5, 2, 9, 31};
    REQUIRE(appendGuildTabardRegionLayers(layers, sample));
    REQUIRE(layers.size() == 6);
    REQUIRE(layers[0] == std::make_pair(3, std::string("Textures\\GuildEmblems\\Background_31_TU_U.blp")));
    REQUIRE(layers[1] == std::make_pair(4, std::string("Textures\\GuildEmblems\\Background_31_TL_U.blp")));
    REQUIRE(layers[2] == std::make_pair(3, std::string("Textures\\GuildEmblems\\Border_02_09_TU_U.blp")));
    REQUIRE(layers[3] == std::make_pair(4, std::string("Textures\\GuildEmblems\\Border_02_09_TL_U.blp")));
    REQUIRE(layers[4] == std::make_pair(3, std::string("Textures\\GuildEmblems\\Emblem_12_05_TU_U.blp")));
    REQUIRE(layers[5] == std::make_pair(4, std::string("Textures\\GuildEmblems\\Emblem_12_05_TL_U.blp")));
}

TEST_CASE("TBC update_fields.json defines PLAYER_GUILDID, PLAYER_GUILDRANK, PLAYER_GUILD_TIMESTAMP", "[guild][tbc]") {
    TbcOpcodeFixture fx;
    REQUIRE(fieldIndex(UF::UNIT_END) == 234);
    REQUIRE(fieldIndex(UF::PLAYER_GUILDID) == 237);
    REQUIRE(fieldIndex(UF::PLAYER_GUILDRANK) == 238);
    REQUIRE(fieldIndex(UF::PLAYER_GUILD_TIMESTAMP) == 243);
}
