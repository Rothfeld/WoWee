#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace wowee {
namespace game {

/**
 * Logical update field identifiers (expansion-agnostic).
 * Wire indices are loaded at runtime from JSON.
 */
enum class UF : uint16_t {
    // Object fields
    OBJECT_FIELD_ENTRY,
    OBJECT_FIELD_SCALE_X,

    // Unit fields
    UNIT_FIELD_TARGET_LO,
    UNIT_FIELD_TARGET_HI,
    /// What the unit channels at (a unit or game object) and the spell it
    /// channels. Held for the whole channel, so a channel that began before
    /// the unit came into view is still known: Netherstorm Trigger's Green
    /// Beam (33346) at Forge Camp: Mageddon runs for ten minutes.
    UNIT_FIELD_CHANNEL_OBJECT_LO,
    UNIT_FIELD_CHANNEL_OBJECT_HI,
    UNIT_CHANNEL_SPELL,
    UNIT_FIELD_BYTES_0,
    UNIT_FIELD_BYTES_1,  // byte2 = visibility flags; byte3 is expansion-dependent
    UNIT_FIELD_BYTES_2,  // WotLK: byte3 = shapeshift form (BYTES_1 byte3 is the anim tier there)
    UNIT_VIRTUAL_ITEM_SLOT_ID, // Three display IDs before Wrath; item entries in Wrath.
    UNIT_VIRTUAL_ITEM_INFO,    // Pre-Wrath: two packed metadata fields per slot.
    UNIT_FIELD_HEALTH,
    UNIT_FIELD_POWER1,
    UNIT_FIELD_MAXHEALTH,
    UNIT_FIELD_MAXPOWER1,
    UNIT_FIELD_LEVEL,
    UNIT_FIELD_FACTIONTEMPLATE,
    UNIT_FIELD_FLAGS,
    UNIT_FIELD_FLAGS_2,
    UNIT_FIELD_AURASTATE,      // Reactive combat opportunities (e.g. dodge/block/parry)
    /// How wide the unit is, and how far it can reach past that. Range is
    /// measured between the two units' edges rather than their centres, so a
    /// large creature is in reach from further away - which is the whole of
    /// why a charge at one read as out of range. Both are floats.
    UNIT_FIELD_BOUNDINGRADIUS,
    UNIT_FIELD_COMBATREACH,
    UNIT_FIELD_DISPLAYID,
    UNIT_FIELD_NATIVEDISPLAYID,
    UNIT_FIELD_MOUNTDISPLAYID,
    UNIT_FIELD_AURAS,           // Start of aura spell ID array (48 consecutive uint32 slots, pre-WotLK clients)
    UNIT_FIELD_AURAFLAGS,       // Aura flags packed 4-per-uint32 (12 uint32 slots); 0x01=cancelable,0x02=harmful,0x04=helpful
    UNIT_FIELD_AURAAPPLICATIONS, // Aura stack count minus one, packed 4-per-uint32 (pre-WotLK)
    UNIT_NPC_FLAGS,
    UNIT_NPC_EMOTESTATE,       // Persistent NPC emote animation ID (uint32)
    UNIT_DYNAMIC_FLAGS,
    UNIT_FIELD_PETNUMBER,      // Persistent pet identity used by model scaling
    UNIT_FIELD_PET_NAME_TIMESTAMP, // Set when a pet carries its own name (CMSG_PET_NAME_QUERY)
    UNIT_FIELD_PETEXPERIENCE,   // A hunter pet's experience toward its next level
    UNIT_FIELD_PETNEXTLEVELEXP, // ...and what that level costs
    UNIT_FIELD_RESISTANCES,   // Physical armor (index 0 of the resistance array)
    UNIT_FIELD_BASE_MANA,     // Mana before gear and stats, which percent costs are taken of
    UNIT_FIELD_STAT0,         // Strength (effective base, includes items)
    UNIT_FIELD_STAT1,         // Agility
    UNIT_FIELD_STAT2,         // Stamina
    UNIT_FIELD_STAT3,         // Intellect
    UNIT_FIELD_STAT4,         // Spirit
    UNIT_END,

    // Unit combat fields (WotLK: PRIVATE+OWNER - only visible for the player character)
    UNIT_FIELD_MINDAMAGE,            // Weapon damage low end (float bits)
    UNIT_FIELD_MAXDAMAGE,            // ...and high end
    UNIT_FIELD_ATTACK_POWER,         // Melee attack power (int32)
    UNIT_FIELD_RANGED_ATTACK_POWER,  // Ranged attack power (int32)
    UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER,             // Power regen while not casting (float[7], mana=0); PRIVATE/OWNER.
                                                      // TBC: PLAYER_FIELD_MOD_MANA_REGEN, mana only.
    UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER, // ...and the reduced rate during the five-second rule
                                                      // (TBC: PLAYER_FIELD_MOD_MANA_REGEN_INTERRUPT).

    // Player fields
    PLAYER_DUEL_ARBITER,
    PLAYER_DUEL_TEAM,
    PLAYER_FLAGS,
    PLAYER_TRACK_RESOURCES,
    /// Unspent talent points (TBC: UNIT_END + 0x436). The server owns the
    /// count (Player::SetFreeTalentPoints); UnitCharacterPoints reads it.
    PLAYER_CHARACTER_POINTS1,
    PLAYER_BYTES,
    PLAYER_BYTES_2,
    PLAYER_XP,
    PLAYER_NEXT_LEVEL_XP,
    PLAYER_REST_STATE_EXPERIENCE,
    PLAYER_FIELD_COINAGE,
    PLAYER_QUEST_LOG_START,
    PLAYER_FIELD_INV_SLOT_HEAD,
    PLAYER_FIELD_PACK_SLOT_1,
    PLAYER_FIELD_KEYRING_SLOT_1,
    PLAYER_FIELD_BANK_SLOT_1,
    PLAYER_FIELD_BANKBAG_SLOT_1,
    PLAYER_SKILL_INFO_START,
    PLAYER_EXPLORED_ZONES_START,
    PLAYER_CHOSEN_TITLE,         // Active title index (-1 = no title)
    // Private player byte flags. Byte 0 holds the release-spirit state the
    // server sets on death (RELEASE_TIMER 0x08, NO_RELEASE_WINDOW 0x10).
    // TBC only: CMaNGOS UpdateFields.h, UNIT_END + 0x04E4.
    PLAYER_FIELD_BYTES,

    // Player spell power / healing bonus (WotLK: PRIVATE - int32 per school)
    PLAYER_FIELD_MOD_DAMAGE_DONE_POS,  // Spell damage bonus (first of 7 schools)
    PLAYER_FIELD_MOD_HEALING_DONE_POS, // Healing bonus

    // Player combat stats (WotLK: PRIVATE - float values)
    PLAYER_BLOCK_PERCENTAGE,         // Block chance %
    PLAYER_DODGE_PERCENTAGE,         // Dodge chance %
    PLAYER_PARRY_PERCENTAGE,         // Parry chance %
    PLAYER_CRIT_PERCENTAGE,          // Melee crit chance %
    PLAYER_RANGED_CRIT_PERCENTAGE,   // Ranged crit chance %
    PLAYER_SPELL_CRIT_PERCENTAGE1,   // Spell crit chance % (first school; 7 consecutive float fields)
    PLAYER_FIELD_COMBAT_RATING_1,    // First of 25 int32 combat rating slots (CR_* indices)
    // Expertise, in expertise *points* rather than percent - the character
    // sheet prints the points and works the percent out at a quarter of one
    // each. Ints, not floats, unlike the percentages above. WotLK only here:
    // the indices were read off AzerothCore's UpdateFields.h and checked by
    // reproducing PLAYER_BLOCK_PERCENTAGE's known 1024 from the same UNIT_END.
    PLAYER_EXPERTISE,
    PLAYER_OFFHAND_EXPERTISE,

    // Player PvP currency (TBC/WotLK only - Classic uses the old weekly honor system)
    PLAYER_FIELD_HONOR_CURRENCY,     // Accumulated honor points (uint32)
    PLAYER_FIELD_ARENA_CURRENCY,     // Accumulated arena points (uint32)
    // The first of the player's arena team slots: per team (2v2, 3v3, 5v5) its
    // id, member flag, week and season games, season wins and personal rating.
    // TBC only here (CMaNGOS UpdateFields.h UNIT_END + 0x051E, size 18).
    PLAYER_FIELD_ARENA_TEAM_INFO_1_1,

    // The player's active non-combat companion, as a 64-bit guid (low word at
    // this index, high word at the next). WotLK only - no earlier expansion
    // published it, and none has CMSG_DISMISS_CRITTER to act on it either.
    UNIT_FIELD_CRITTER,
    // Who this unit is controlling, and who is controlling it. Both are guid
    // pairs. WotLK only for now: the indices below were read off AzerothCore's
    // UpdateFields.h and checked against UNIT_FIELD_CRITTER, which that header
    // puts at OBJECT_END + 4 and this client's table already has at 10 - so
    // OBJECT_END is 6 and the table holds absolute indices. No such check was
    // possible for the other three expansions, and an update field index
    // guessed is an arbitrary field read.
    UNIT_FIELD_CHARM,
    /// The unit or object the player's camera looks from (Camera::SetView).
    PLAYER_FARSIGHT,
    UNIT_FIELD_CHARMEDBY,
    /// Who summoned this unit, if anyone. Two fields, low half first, like
    /// every other guid here. A pet, a guardian and a totem all carry it; what
    /// separates them is UNIT_FLAG_PLAYER_CONTROLLED and the creature type.
    UNIT_FIELD_SUMMONEDBY_LO,
    UNIT_FIELD_SUMMONEDBY_HI,
    /// Creature owner; non-combat pets publish CREATEDBY without SUMMONEDBY.
    UNIT_FIELD_CREATEDBY_LO,
    UNIT_FIELD_CREATEDBY_HI,

    // GameObject fields
    /// Low word of the creating unit's guid; the high word follows it.
    GAMEOBJECT_CREATED_BY,
    GAMEOBJECT_DISPLAYID,
    GAMEOBJECT_FLAGS,
    GAMEOBJECT_BYTES_1,
    // MO_TRANSPORT route clock. LEVEL carries the route's period in ms; the high
    // int16 of DYNAMIC carries how far through that period the transport is, as a
    // fraction of 65535. Transport phase is WotLK-only; the low bits carry
    // per-player quest-object activation in earlier expansions too.
    GAMEOBJECT_DYNAMIC,
    /// FactionTemplate id; build 8606 refuses use of objects hostile to the player.
    GAMEOBJECT_FACTION,
    GAMEOBJECT_LEVEL,

    // Item fields
    ITEM_FIELD_STACK_COUNT,
    ITEM_FIELD_DURABILITY,
    ITEM_FIELD_MAXDURABILITY,

    // Container fields
    CONTAINER_FIELD_NUM_SLOTS,
    CONTAINER_FIELD_SLOT_1,

    UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE,
    UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE,
    /// Buffs and debuffs on each primary stat (5 fields each); UNIT_FIELD_STAT0
    /// already includes them.
    UNIT_FIELD_POSSTAT0,
    UNIT_FIELD_NEGSTAT0,
    /// Ranged weapon damage the server works out, float bits.
    UNIT_FIELD_MINRANGEDDAMAGE,
    UNIT_FIELD_MAXRANGEDDAMAGE,
    /// What a block stops (int).
    PLAYER_SHIELD_BLOCK,
    /// The item entry the ammo slot names; the ammo itself stays in the bags.
    PLAYER_AMMO_ID,
    /// Main-hand and off-hand attack time, ms (2 fields).
    UNIT_FIELD_BASEATTACKTIME,
    UNIT_FIELD_RANGEDATTACKTIME,

    // TBC carries GO state in a standalone word, unlike Wrath's BYTES_1.
    GAMEOBJECT_STATE,

    // DynamicObject fields: the spell whose persistent area this is
    // (Flamestrike, Blizzard, Consecration) and where it lies.
    DYNAMICOBJECT_SPELLID,
    DYNAMICOBJECT_POS_X,
    DYNAMICOBJECT_POS_Y,
    DYNAMICOBJECT_POS_Z,

    COUNT
};

/**
 * Maps logical update field names to expansion-specific wire indices.
 * Loaded from JSON (e.g. Data/expansions/wotlk/update_fields.json).
 */
class UpdateFieldTable {
public:
    /** Load from JSON file. Returns true if successful. */
    bool loadFromJson(const std::string& path);

    /** Get the wire index for a logical field. Returns 0xFFFF if unknown. */
    [[nodiscard]] uint16_t index(UF field) const;

    /** Override a wire index at runtime (used for auto-detecting custom field layouts). */
    void setIndex(UF field, uint16_t idx) { fieldMap_[static_cast<uint16_t>(field)] = idx; }

    /** Check if a field is mapped. */
    [[nodiscard]] bool hasField(UF field) const;

    /** Number of mapped fields. */
    [[nodiscard]] size_t size() const { return fieldMap_.size(); }

private:
    std::unordered_map<uint16_t, uint16_t> fieldMap_;  // UF enum → wire index
};

/**
 * Global active update field table (set by Application at startup).
 */
void setActiveUpdateFieldTable(const UpdateFieldTable* table);
const UpdateFieldTable* getActiveUpdateFieldTable();

/** Convenience: get wire index for a logical field. */
inline uint16_t fieldIndex(UF field) {
    const auto* t = getActiveUpdateFieldTable();
    return t ? t->index(field) : 0xFFFF;
}

} // namespace game
} // namespace wowee
