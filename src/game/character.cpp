#include "game/character.hpp"

namespace wowee {
namespace game {

bool isValidRaceClassCombo(Race race, Class cls) {
    // WoW 3.3.5a valid race/class combinations
    switch (race) {
        case Race::HUMAN:
            return cls == Class::WARRIOR || cls == Class::PALADIN || cls == Class::ROGUE ||
                   cls == Class::PRIEST || cls == Class::MAGE || cls == Class::WARLOCK ||
                   cls == Class::DEATH_KNIGHT;
        case Race::ORC:
            return cls == Class::WARRIOR || cls == Class::HUNTER || cls == Class::ROGUE ||
                   cls == Class::SHAMAN || cls == Class::WARLOCK || cls == Class::DEATH_KNIGHT;
        case Race::DWARF:
            return cls == Class::WARRIOR || cls == Class::PALADIN || cls == Class::HUNTER ||
                   cls == Class::ROGUE || cls == Class::PRIEST || cls == Class::DEATH_KNIGHT;
        case Race::NIGHT_ELF:
            return cls == Class::WARRIOR || cls == Class::HUNTER || cls == Class::ROGUE ||
                   cls == Class::PRIEST || cls == Class::DRUID || cls == Class::DEATH_KNIGHT;
        case Race::UNDEAD:
            return cls == Class::WARRIOR || cls == Class::ROGUE || cls == Class::PRIEST ||
                   cls == Class::MAGE || cls == Class::WARLOCK || cls == Class::DEATH_KNIGHT;
        case Race::TAUREN:
            return cls == Class::WARRIOR || cls == Class::HUNTER || cls == Class::DRUID ||
                   cls == Class::SHAMAN || cls == Class::DEATH_KNIGHT;
        case Race::GNOME:
            return cls == Class::WARRIOR || cls == Class::ROGUE || cls == Class::MAGE ||
                   cls == Class::WARLOCK || cls == Class::DEATH_KNIGHT;
        case Race::TROLL:
            return cls == Class::WARRIOR || cls == Class::HUNTER || cls == Class::ROGUE ||
                   cls == Class::PRIEST || cls == Class::SHAMAN || cls == Class::MAGE ||
                   cls == Class::DEATH_KNIGHT;
        case Race::BLOOD_ELF:
            return cls == Class::PALADIN || cls == Class::HUNTER || cls == Class::ROGUE ||
                   cls == Class::PRIEST || cls == Class::MAGE || cls == Class::WARLOCK ||
                   cls == Class::DEATH_KNIGHT;
        case Race::DRAENEI:
            return cls == Class::WARRIOR || cls == Class::PALADIN || cls == Class::HUNTER ||
                   cls == Class::PRIEST || cls == Class::SHAMAN || cls == Class::MAGE ||
                   cls == Class::DEATH_KNIGHT;
        default:
            return false;
    }
}

uint8_t getMaxSkin(Race /*race*/, Gender /*gender*/) { return 9; }
uint8_t getMaxFace(Race /*race*/, Gender /*gender*/) { return 9; }
uint8_t getMaxHairStyle(Race /*race*/, Gender /*gender*/) { return 11; }
uint8_t getMaxHairColor(Race /*race*/, Gender /*gender*/) { return 9; }
uint8_t getMaxFacialFeature(Race /*race*/, Gender /*gender*/) { return 8; }

const char* getRaceName(Race race) {
    switch (race) {
        case Race::HUMAN:        return "Human";
        case Race::ORC:          return "Orc";
        case Race::DWARF:        return "Dwarf";
        case Race::NIGHT_ELF:    return "Night Elf";
        case Race::UNDEAD:       return "Undead";
        case Race::TAUREN:       return "Tauren";
        case Race::GNOME:        return "Gnome";
        case Race::TROLL:        return "Troll";
        case Race::GOBLIN:       return "Goblin";
        case Race::BLOOD_ELF:    return "Blood Elf";
        case Race::DRAENEI:      return "Draenei";
        case Race::FEL_ORC:      return "Fel Orc";
        case Race::NAGA:         return "Naga";
        case Race::BROKEN:       return "Broken";
        case Race::SKELETON:     return "Skeleton";
        case Race::FOREST_TROLL: return "Forest Troll";
        default:                 return "Unknown";
    }
}

const char* getClassName(Class characterClass) {
    switch (characterClass) {
        case Class::WARRIOR:        return "Warrior";
        case Class::PALADIN:        return "Paladin";
        case Class::HUNTER:         return "Hunter";
        case Class::ROGUE:          return "Rogue";
        case Class::PRIEST:         return "Priest";
        case Class::DEATH_KNIGHT:   return "Death Knight";
        case Class::SHAMAN:         return "Shaman";
        case Class::MAGE:           return "Mage";
        case Class::WARLOCK:        return "Warlock";
        case Class::DRUID:          return "Druid";
        default:                    return "Unknown";
    }
}

const char* getGenderName(Gender gender) {
    switch (gender) {
        case Gender::MALE:      return "Male";
        case Gender::FEMALE:    return "Female";
        case Gender::NONBINARY: return "Nonbinary";
        default:                return "Unknown";
    }
}

std::string getPlayerModelPath(Race race, Gender gender, bool useFemaleModel) {
    // Female always uses female model
    // Nonbinary uses chosen model (useFemaleModel parameter)
    // Male always uses male model
    bool useFemale = (gender == Gender::FEMALE) ||
                     (gender == Gender::NONBINARY && useFemaleModel);

    switch (race) {
        case Race::HUMAN:
            return useFemale
                ? "Character\\Human\\Female\\HumanFemale.m2"
                : "Character\\Human\\Male\\HumanMale.m2";
        case Race::ORC:
            return useFemale
                ? "Character\\Orc\\Female\\OrcFemale.m2"
                : "Character\\Orc\\Male\\OrcMale.m2";
        case Race::DWARF:
            return useFemale
                ? "Character\\Dwarf\\Female\\DwarfFemale.m2"
                : "Character\\Dwarf\\Male\\DwarfMale.m2";
        case Race::NIGHT_ELF:
            return useFemale
                ? "Character\\NightElf\\Female\\NightElfFemale.m2"
                : "Character\\NightElf\\Male\\NightElfMale.m2";
        case Race::UNDEAD:
            return useFemale
                ? "Character\\Scourge\\Female\\ScourgeFemale.m2"
                : "Character\\Scourge\\Male\\ScourgeMale.m2";
        case Race::TAUREN:
            return useFemale
                ? "Character\\Tauren\\Female\\TaurenFemale.m2"
                : "Character\\Tauren\\Male\\TaurenMale.m2";
        case Race::GNOME:
            return useFemale
                ? "Character\\Gnome\\Female\\GnomeFemale.m2"
                : "Character\\Gnome\\Male\\GnomeMale.m2";
        case Race::TROLL:
            return useFemale
                ? "Character\\Troll\\Female\\TrollFemale.m2"
                : "Character\\Troll\\Male\\TrollMale.m2";
        case Race::GOBLIN:
            return useFemale
                ? "Character\\Goblin\\Female\\GoblinFemale.m2"
                : "Character\\Goblin\\Male\\GoblinMale.m2";
        case Race::BLOOD_ELF:
            return useFemale
                ? "Character\\BloodElf\\Female\\BloodElfFemale.m2"
                : "Character\\BloodElf\\Male\\BloodElfMale.m2";
        case Race::DRAENEI:
            return useFemale
                ? "Character\\Draenei\\Female\\DraeneiFemale.m2"
                : "Character\\Draenei\\Male\\DraeneiMale.m2";
        case Race::FEL_ORC:
            return useFemale
                ? "Character\\FelOrc\\Female\\FelOrcFemale.m2"
                : "Character\\FelOrc\\Male\\FelOrcMale.m2";
        case Race::NAGA:
            return useFemale
                ? "Character\\Naga_\\Female\\Naga_Female.m2"
                : "Character\\Naga_\\Male\\Naga_Male.m2";
        case Race::BROKEN:
            return useFemale
                ? "Character\\Broken\\Female\\BrokenFemale.m2"
                : "Character\\Broken\\Male\\BrokenMale.m2";
        case Race::SKELETON:
            return useFemale
                ? "Character\\Skeleton\\Female\\SkeletonFemale.m2"
                : "Character\\Skeleton\\Male\\SkeletonMale.m2";
        case Race::FOREST_TROLL:
            // TBC ChrRaces.dbc race 18 FemaleDisplayID (21964) shares TrollFemale.m2.
            return useFemale
                ? "Character\\Troll\\Female\\TrollFemale.m2"
                : "Character\\ForestTroll\\Male\\ForestTrollMale.m2";
        default:
            return "Character\\Human\\Male\\HumanMale.m2";
    }
}

bool hasPlayerModel(Race race) {
    switch (race) {
        case Race::HUMAN: case Race::ORC: case Race::DWARF:
        case Race::NIGHT_ELF: case Race::UNDEAD: case Race::TAUREN:
        case Race::GNOME: case Race::TROLL: case Race::GOBLIN:
        case Race::BLOOD_ELF: case Race::DRAENEI:
        case Race::FEL_ORC: case Race::NAGA: case Race::BROKEN:
        case Race::SKELETON: case Race::FOREST_TROLL:
            return true;
        default:
            return false;
    }
}

bool inferRaceSexFromCharacterModelPath(std::string_view m2Path,
                                        uint8_t& outRaceId,
                                        uint8_t& outSexId) {
    // Normalize slashes and lowercase for matching "character\\<race>\\<male|female>\\..."
    std::string lower;
    lower.reserve(m2Path.size());
    for (char c : m2Path) {
        if (c == '/') lower.push_back('\\');
        else if (c >= 'A' && c <= 'Z') lower.push_back(static_cast<char>(c - 'A' + 'a'));
        else lower.push_back(c);
    }
    constexpr std::string_view kPrefix = "character\\";
    if (lower.rfind(kPrefix, 0) != 0) return false;

    const size_t raceStart = kPrefix.size();
    const size_t raceEnd = lower.find('\\', raceStart);
    if (raceEnd == std::string::npos) return false;
    const std::string_view raceToken(lower.data() + raceStart, raceEnd - raceStart);

    const size_t sexStart = raceEnd + 1;
    const size_t sexEnd = lower.find('\\', sexStart);
    if (sexEnd == std::string::npos) return false;
    const std::string_view sexToken(lower.data() + sexStart, sexEnd - sexStart);

    uint8_t sexId = 0;
    if (sexToken == "male") sexId = 0;
    else if (sexToken == "female") sexId = 1;
    else return false;

    Race race = Race::HUMAN;
    if (raceToken == "human") race = Race::HUMAN;
    else if (raceToken == "orc") race = Race::ORC;
    else if (raceToken == "dwarf") race = Race::DWARF;
    else if (raceToken == "nightelf") race = Race::NIGHT_ELF;
    else if (raceToken == "scourge") race = Race::UNDEAD;
    else if (raceToken == "tauren") race = Race::TAUREN;
    else if (raceToken == "gnome") race = Race::GNOME;
    else if (raceToken == "troll") race = Race::TROLL;
    else if (raceToken == "goblin") race = Race::GOBLIN;
    else if (raceToken == "bloodelf") race = Race::BLOOD_ELF;
    else if (raceToken == "draenei") race = Race::DRAENEI;
    else if (raceToken == "felorc") race = Race::FEL_ORC;
    else if (raceToken == "naga_") race = Race::NAGA;
    else if (raceToken == "broken") race = Race::BROKEN;
    else if (raceToken == "skeleton") race = Race::SKELETON;
    else if (raceToken == "foresttroll") race = Race::FOREST_TROLL;
    else return false;

    outRaceId = static_cast<uint8_t>(race);
    outSexId = sexId;
    return true;
}

std::string getPlayerModelPath(const Character& character) {
    return getPlayerModelPath(character.race, character.gender, character.useFemaleModel);
}

} // namespace game
} // namespace wowee
