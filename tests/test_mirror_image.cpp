// Mirror image wire protocol (CMSG_GET_MIRRORIMAGE_DATA / SMSG_MIRRORIMAGE_DATA),
// UNIT_FLAG2_MIRROR_IMAGE state, and bare racial Character\* model-path fallback tests.
#include <catch_amalgamated.hpp>
#include "game/world_packets.hpp"
#include "game/opcodes.hpp"
#include "game/character.hpp"
#include "game/entity.hpp"
#include "network/packet.hpp"

using namespace wowee::game;
using namespace wowee::network;

namespace {

Packet buildMirrorImagePacket(const MirrorImageData& src, bool isWotlkFormat) {
    Packet pkt(wireOpcode(Opcode::SMSG_MIRRORIMAGE_DATA));
    pkt.writeUInt64(src.guid);
    pkt.writeUInt32(src.displayId);
    pkt.writeUInt8(src.raceId);
    pkt.writeUInt8(src.gender);
    if (isWotlkFormat) {
        pkt.writeUInt8(src.classId);
    }
    pkt.writeUInt8(src.skin);
    pkt.writeUInt8(src.face);
    pkt.writeUInt8(src.hairStyle);
    pkt.writeUInt8(src.hairColor);
    pkt.writeUInt8(src.facialHair);
    pkt.writeUInt32(src.guildId);
    for (uint32_t did : src.equipDisplayIds) {
        pkt.writeUInt32(did);
    }
    pkt.setReadPos(0);
    return pkt;
}

} // namespace

TEST_CASE("CMSG_GET_MIRRORIMAGE_DATA builds 8-byte guid payload", "[mirror_image]") {
    constexpr uint64_t kGuid = 0xF13000646C596F14ULL;
    Packet pkt = GetMirrorImageDataPacket::build(kGuid);
    REQUIRE(pkt.getSize() == 8);
    pkt.setReadPos(0);
    REQUIRE(pkt.readUInt64() == kGuid);
}

TEST_CASE("SMSG_MIRRORIMAGE_DATA TBC 2.4.3 wire format (67 bytes, no class byte)", "[mirror_image]") {
    MirrorImageData expected;
    expected.guid = 0xF13000646C596F14ULL;
    expected.displayId = 59; // Tauren Male
    expected.raceId = static_cast<uint8_t>(Race::TAUREN);
    expected.gender = 0;
    expected.classId = 0; // Not present in TBC 2.4.3
    expected.skin = 4;
    expected.face = 2;
    expected.hairStyle = 5;
    expected.hairColor = 3;
    expected.facialHair = 1;
    expected.guildId = 42;
    expected.equipDisplayIds = {
        31001, // 0: HEAD
        31002, // 1: SHOULDERS
        31003, // 2: BODY (shirt)
        31004, // 3: CHEST
        31005, // 4: WAIST
        31006, // 5: LEGS
        31007, // 6: FEET
        31008, // 7: WRISTS
        31009, // 8: HANDS
        31010, // 9: BACK (cloak)
        31011, // 10: TABARD
    };

    Packet pkt = buildMirrorImagePacket(expected, /*isWotlkFormat=*/false);
    REQUIRE(pkt.getSize() == 67);

    MirrorImageData parsed;
    REQUIRE(MirrorImageDataParser::parse(pkt, parsed, /*isWotlkFormat=*/false));
    REQUIRE(parsed.guid == expected.guid);
    REQUIRE(parsed.displayId == expected.displayId);
    REQUIRE(parsed.raceId == expected.raceId);
    REQUIRE(parsed.gender == expected.gender);
    REQUIRE(parsed.classId == 0);
    REQUIRE(parsed.skin == expected.skin);
    REQUIRE(parsed.face == expected.face);
    REQUIRE(parsed.hairStyle == expected.hairStyle);
    REQUIRE(parsed.hairColor == expected.hairColor);
    REQUIRE(parsed.facialHair == expected.facialHair);
    REQUIRE(parsed.guildId == expected.guildId);
    REQUIRE(parsed.equipDisplayIds == expected.equipDisplayIds);
    REQUIRE(parsed.packedAppearanceBytes() ==
            (uint32_t(4) | (uint32_t(2) << 8) | (uint32_t(5) << 16) | (uint32_t(3) << 24)));
    REQUIRE(parsed.hasPlayerAppearance());
}

TEST_CASE("SMSG_MIRRORIMAGE_DATA WotLK 3.3.5a wire format (68 bytes, with class byte)", "[mirror_image]") {
    MirrorImageData expected;
    expected.guid = 0xF13000646C596F15ULL;
    expected.displayId = 49; // Human Male
    expected.raceId = static_cast<uint8_t>(Race::HUMAN);
    expected.gender = 0;
    expected.classId = static_cast<uint8_t>(Class::MAGE);
    expected.skin = 1;
    expected.face = 3;
    expected.hairStyle = 7;
    expected.hairColor = 2;
    expected.facialHair = 4;
    expected.guildId = 99;
    expected.equipDisplayIds = {101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111};

    Packet pkt = buildMirrorImagePacket(expected, /*isWotlkFormat=*/true);
    REQUIRE(pkt.getSize() == 68);

    MirrorImageData parsed;
    REQUIRE(MirrorImageDataParser::parse(pkt, parsed, /*isWotlkFormat=*/true));
    REQUIRE(parsed.guid == expected.guid);
    REQUIRE(parsed.displayId == expected.displayId);
    REQUIRE(parsed.raceId == expected.raceId);
    REQUIRE(parsed.gender == expected.gender);
    REQUIRE(parsed.classId == expected.classId);
    REQUIRE(parsed.skin == expected.skin);
    REQUIRE(parsed.face == expected.face);
    REQUIRE(parsed.hairStyle == expected.hairStyle);
    REQUIRE(parsed.hairColor == expected.hairColor);
    REQUIRE(parsed.facialHair == expected.facialHair);
    REQUIRE(parsed.guildId == expected.guildId);
    REQUIRE(parsed.equipDisplayIds == expected.equipDisplayIds);
}

TEST_CASE("SMSG_MIRRORIMAGE_DATA rejects truncated packets", "[mirror_image]") {
    MirrorImageData parsed;
    Packet shortTbc(0);
    for (int i = 0; i < 66; ++i) shortTbc.writeUInt8(0);
    shortTbc.setReadPos(0);
    REQUIRE_FALSE(MirrorImageDataParser::parse(shortTbc, parsed, /*isWotlkFormat=*/false));

    Packet shortWotlk(0);
    for (int i = 0; i < 67; ++i) shortWotlk.writeUInt8(0);
    shortWotlk.setReadPos(0);
    REQUIRE_FALSE(MirrorImageDataParser::parse(shortWotlk, parsed, /*isWotlkFormat=*/true));
}

TEST_CASE("mirrorImageEquipmentToPlayerSlots maps BACK(9)->slot 14 and TABARD(10)->slot 18", "[mirror_image]") {
    const std::array<uint32_t, 11> mirrorEquip = {
        1001, // HEAD
        1002, // SHOULDERS
        1003, // BODY
        1004, // CHEST
        1005, // WAIST
        1006, // LEGS
        1007, // FEET
        1008, // WRISTS
        1009, // HANDS
        1010, // BACK (cloak)
        1011, // TABARD
    };
    std::array<uint32_t, 19> disp{};
    std::array<uint8_t, 19> inv{};
    mirrorImageEquipmentToPlayerSlots(mirrorEquip, disp, inv);

    REQUIRE(disp[0] == 1001);  REQUIRE(inv[0] == 1);   // HEAD
    REQUIRE(disp[2] == 1002);  REQUIRE(inv[2] == 3);   // SHOULDERS
    REQUIRE(disp[3] == 1003);  REQUIRE(inv[3] == 4);   // BODY
    REQUIRE(disp[4] == 1004);  REQUIRE(inv[4] == 5);   // CHEST
    REQUIRE(disp[5] == 1005);  REQUIRE(inv[5] == 6);   // WAIST
    REQUIRE(disp[6] == 1006);  REQUIRE(inv[6] == 7);   // LEGS
    REQUIRE(disp[7] == 1007);  REQUIRE(inv[7] == 8);   // FEET
    REQUIRE(disp[8] == 1008);  REQUIRE(inv[8] == 9);   // WRISTS
    REQUIRE(disp[9] == 1009);  REQUIRE(inv[9] == 10);  // HANDS
    REQUIRE(disp[14] == 1010); REQUIRE(inv[14] == 16); // BACK / CLOAK
    REQUIRE(disp[18] == 1011); REQUIRE(inv[18] == 19); // TABARD
    // Weapons and jewelry slots stay untouched (0).
    REQUIRE(disp[15] == 0);
    REQUIRE(disp[16] == 0);
    REQUIRE(disp[17] == 0);
}

TEST_CASE("Unit mirror image flag constant matches UNIT_FLAG2_CLONED (0x10)", "[mirror_image]") {
    REQUIRE(Unit::kUnitFlag2MirrorImage == 0x00000010u);
    Unit unit(0xF13000646C596F14ULL);
    REQUIRE_FALSE(unit.isMirrorImage());
    unit.setMirrorImage(true);
    REQUIRE(unit.isMirrorImage());
    unit.setMirrorImage(false);
    REQUIRE_FALSE(unit.isMirrorImage());
}

TEST_CASE("inferRaceSexFromCharacterModelPath resolves all playable Character\\* bare racial models", "[mirror_image]") {
    struct Case {
        const char* path;
        Race race;
        uint8_t sex;
    };
    const Case cases[] = {
        {"Character\\Human\\Male\\HumanMale.m2", Race::HUMAN, 0},
        {"Character\\Human\\Female\\HumanFemale.m2", Race::HUMAN, 1},
        {"Character\\Orc\\Male\\OrcMale.m2", Race::ORC, 0},
        {"Character\\Orc\\Female\\OrcFemale.m2", Race::ORC, 1},
        {"Character\\Dwarf\\Male\\DwarfMale.m2", Race::DWARF, 0},
        {"Character\\Dwarf\\Female\\DwarfFemale.m2", Race::DWARF, 1},
        {"Character\\NightElf\\Male\\NightElfMale.m2", Race::NIGHT_ELF, 0},
        {"Character\\NightElf\\Female\\NightElfFemale.m2", Race::NIGHT_ELF, 1},
        {"Character\\Scourge\\Male\\ScourgeMale.m2", Race::UNDEAD, 0},
        {"Character\\Scourge\\Female\\ScourgeFemale.m2", Race::UNDEAD, 1},
        {"Character\\Tauren\\Male\\TaurenMale.m2", Race::TAUREN, 0},
        {"Character\\Tauren\\Female\\TaurenFemale.m2", Race::TAUREN, 1},
        {"Character\\Gnome\\Male\\GnomeMale.m2", Race::GNOME, 0},
        {"Character\\Gnome\\Female\\GnomeFemale.m2", Race::GNOME, 1},
        {"Character\\Troll\\Male\\TrollMale.m2", Race::TROLL, 0},
        {"Character\\Troll\\Female\\TrollFemale.m2", Race::TROLL, 1},
        {"Character\\BloodElf\\Male\\BloodElfMale.m2", Race::BLOOD_ELF, 0},
        {"Character\\BloodElf\\Female\\BloodElfFemale.m2", Race::BLOOD_ELF, 1},
        {"Character\\Draenei\\Male\\DraeneiMale.m2", Race::DRAENEI, 0},
        {"Character\\Draenei\\Female\\DraeneiFemale.m2", Race::DRAENEI, 1},
        {"character/tauren/male/taurenmale.m2", Race::TAUREN, 0},
    };
    for (const auto& c : cases) {
        uint8_t race = 0, sex = 255;
        REQUIRE(inferRaceSexFromCharacterModelPath(c.path, race, sex));
        REQUIRE(race == static_cast<uint8_t>(c.race));
        REQUIRE(sex == c.sex);
    }

    uint8_t race = 0, sex = 0;
    REQUIRE_FALSE(inferRaceSexFromCharacterModelPath("Creature\\Wolf\\Wolf.m2", race, sex));
    REQUIRE_FALSE(inferRaceSexFromCharacterModelPath("Character\\Broken\\Male\\BrokenMale.m2", race, sex));
    REQUIRE_FALSE(inferRaceSexFromCharacterModelPath("Character\\Naga_\\Male\\Naga_Male.m2", race, sex));
    REQUIRE_FALSE(inferRaceSexFromCharacterModelPath("", race, sex));
}
