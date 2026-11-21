#include <catch_amalgamated.hpp>

#include "game/spell_classification.hpp"
#include "game/spell_defines.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace wowee::game::spellclass;

namespace {

// Real values from Spell.dbc / SpellRange.dbc (WotLK 3.3.5a).
constexpr float kSelfOnly     = 0.0f;   // "Self Only"    - Battle Shout, Hearthstone
constexpr float kCombatRange  = 5.0f;   // "Combat Range" - every melee ability
constexpr float kDeadlyThrow  = 30.0f;  // physical, but thrown
constexpr float kSteadyShot   = 35.0f;  // physical, but shot
constexpr float kFireball     = 35.0f;

// Stand-in for the spell name cache.
class FakeSpellDbc {
public:
    void add(uint32_t id, std::string name, std::string rank) {
        entries_[id] = SpellRankInfo{std::move(name), std::move(rank)};
    }
    std::function<const SpellRankInfo*(uint32_t)> lookup() const {
        return [this](uint32_t id) -> const SpellRankInfo* {
            auto it = entries_.find(id);
            return it != entries_.end() ? &it->second : nullptr;
        };
    }
private:
    std::unordered_map<uint32_t, SpellRankInfo> entries_;
};

} // namespace

// ---------------------------------------------------------------------------
// Range classification
// ---------------------------------------------------------------------------

TEST_CASE("Self-only spells are cast on the caster", "[spell][range]") {
    REQUIRE(isSelfCastRange(kSelfOnly));

    // Anything that can reach another unit is not a self-cast.
    REQUIRE_FALSE(isSelfCastRange(kCombatRange));
    REQUIRE_FALSE(isSelfCastRange(kSteadyShot));

    // Unknown range: SpellRange.dbc was missing, so assume nothing.
    REQUIRE_FALSE(isSelfCastRange(kUnknownRange));
}

TEST_CASE("Melee is decided by range, not by school", "[spell][range]") {
    // Melee abilities all sit at Combat Range.
    REQUIRE(isMeleeRange(kCombatRange));
    REQUIRE(isMeleeRange(4.0f));

    // Regression: Steady Shot, Aimed Shot, Multi-Shot, Taunt and Deadly Throw are all
    // physical school. Classifying by school made the client range-check them at 8
    // yards and swallow the cast, so Hunters could not shoot past 8 yards.
    REQUIRE_FALSE(isMeleeRange(kSteadyShot));
    REQUIRE_FALSE(isMeleeRange(kDeadlyThrow));
    REQUIRE_FALSE(isMeleeRange(kFireball));

    // Regression: Battle Shout is a physical-school self-buff. Read as a melee ability,
    // it was blocked by the range check whenever a distant target was selected.
    REQUIRE_FALSE(isMeleeRange(kSelfOnly));

    // Unknown range must not block a cast: let the server arbitrate.
    REQUIRE_FALSE(isMeleeRange(kUnknownRange));
}

TEST_CASE("A spell is never both self-cast and melee", "[spell][range]") {
    for (float r : {kUnknownRange, kSelfOnly, 1.0f, kCombatRange, 8.0f, kSteadyShot}) {
        const bool both = isSelfCastRange(r) && isMeleeRange(r);
        REQUIRE_FALSE(both);
    }
}

TEST_CASE("Ranged weapon auto-attacks ignore placeholder DBC resource costs",
          "[spell][ranged-auto]") {
    REQUIRE(isRangedWeaponAutoAttack(75));    // Auto Shot
    REQUIRE(isRangedWeaponAutoAttack(5019));  // Shoot
    REQUIRE(isRangedWeaponAutoAttack(2764));  // Throw

    REQUIRE_FALSE(isRangedWeaponAutoAttack(0));
    REQUIRE_FALSE(isRangedWeaponAutoAttack(19434)); // Aimed Shot still uses real costs
}

TEST_CASE("Fishing ranks use targetless server-placed casts", "[spell][fishing]") {
    for (uint32_t spellId : {7620u, 7731u, 7732u, 18248u, 33095u, 51294u}) {
        REQUIRE(isFishingCast(spellId));
    }

    REQUIRE_FALSE(isFishingCast(0));
    REQUIRE_FALSE(isFishingCast(2366));  // Herbalism
    REQUIRE_FALSE(isFishingCast(2575));  // Mining
}

TEST_CASE("Only restoration food and drink spells use the seated consumption loop",
          "[spell][consumable]") {
    REQUIRE(classifyRestChannel("Food") == RestChannelKind::FOOD);
    REQUIRE(classifyRestChannel("Drink") == RestChannelKind::DRINK);

    // Potions use the same drinking motion, but unrelated quest-item actions do not.
    REQUIRE(classifyRestChannel("Drink Potion") == RestChannelKind::POTION);
    REQUIRE(classifyRestChannel("Drink Minor Potion") == RestChannelKind::POTION);
    REQUIRE(classifyRestChannel("Drink Eye Potion") == RestChannelKind::POTION);
    REQUIRE(classifyRestChannel("Healing Potion") == RestChannelKind::POTION);
    REQUIRE(classifyRestChannel("Minor Mana Potion") == RestChannelKind::POTION);
    REQUIRE(classifyRestChannel("Swiftness Potion") == RestChannelKind::POTION);
    REQUIRE(classifyRestChannel("Create Fervor Potion") == RestChannelKind::NONE);
    REQUIRE(classifyRestChannel("Potion Toss") == RestChannelKind::NONE);
    REQUIRE(classifyRestChannel("Drink Disease Bottle") == RestChannelKind::NONE);
    REQUIRE(classifyRestChannel("Food (TEST)") == RestChannelKind::NONE);
    REQUIRE(classifyRestChannel("") == RestChannelKind::NONE);

    // The undead racial is the same kind of thing and neither of the two
    // shapes above: it stays standing, so the seated food path is wrong for
    // it, and it channels, so the single swig a potion gets is wrong too. It
    // has its own kind so the call site can tell all three apart.
    REQUIRE(classifyRestChannel("Cannibalize") == RestChannelKind::CANNIBALIZE);
    // And nothing else drifts into it on a prefix.
    REQUIRE(classifyRestChannel("Cannibalize Corpse") == RestChannelKind::NONE);
    REQUIRE(classifyRestChannel("Cannibal") == RestChannelKind::NONE);

    const uint32_t alcoholEffects[3] = {0, 100, 0};
    const uint32_t ordinaryEffects[3] = {6, 10, 0};
    REQUIRE(hasInebriateEffect(alcoholEffects, 3));
    REQUIRE_FALSE(hasInebriateEffect(ordinaryEffects, 3));
}

// ---------------------------------------------------------------------------
// Rank strings
// ---------------------------------------------------------------------------

TEST_CASE("Rank display strings parse to their number", "[spell][rank]") {
    REQUIRE(rankValue("Rank 1") == 1);
    REQUIRE(rankValue("Rank 9") == 9);
    REQUIRE(rankValue("Rank 14") == 14);

    // Rankless spells sort below every ranked one.
    REQUIRE(rankValue("") == 0);
    REQUIRE(rankValue("Racial") == 0);
}

// ---------------------------------------------------------------------------
// Superseded rank resolution
// ---------------------------------------------------------------------------

TEST_CASE("A superseded rank resolves to the highest known rank", "[spell][rank]") {
    FakeSpellDbc dbc;
    dbc.add(6673,  "Battle Shout", "Rank 1");
    dbc.add(5242,  "Battle Shout", "Rank 2");
    dbc.add(6192,  "Battle Shout", "Rank 3");
    dbc.add(11549, "Battle Shout", "Rank 4");

    // The player learned ranks 2 and 3; rank 1 is superseded and no longer active, so
    // the server silently discards casts of it. An action bar restored from the server
    // can still be holding rank 1.
    const std::unordered_set<uint32_t> known{5242, 6192};

    REQUIRE(resolveHighestKnownRank(6673, known, dbc.lookup()) == 6192);

    // Rank 4 is not known, so it must not be picked.
    REQUIRE(resolveHighestKnownRank(11549, known, dbc.lookup()) != 11549);
    REQUIRE(resolveHighestKnownRank(11549, known, dbc.lookup()) == 6192);
}

TEST_CASE("Rank resolution tolerates a lookup adapter with reused storage", "[spell][rank]") {
    const std::unordered_map<uint32_t, SpellRankInfo> entries{
        {6673, {"Battle Shout", "Rank 1"}},
        {5242, {"Battle Shout", "Rank 2"}},
        {6192, {"Battle Shout", "Rank 3"}},
        {133,  {"Fireball", "Rank 14"}},
    };
    SpellRankInfo scratch;
    auto reusedLookup = [&entries, &scratch](uint32_t id) -> const SpellRankInfo* {
        auto it = entries.find(id);
        if (it == entries.end()) return nullptr;
        scratch = it->second;
        return &scratch;
    };

    // The production Spell.dbc adapter reuses scratch storage. Looking up Fireball
    // must not overwrite the requested Battle Shout name and win merely by rank.
    const std::unordered_set<uint32_t> known{5242, 6192, 133};
    REQUIRE(resolveHighestKnownRank(6673, known, reusedLookup) == 6192);
}

TEST_CASE("A known spell is left alone", "[spell][rank]") {
    FakeSpellDbc dbc;
    dbc.add(772, "Rend", "Rank 1");
    dbc.add(6546, "Rend", "Rank 2");

    const std::unordered_set<uint32_t> known{772};

    // Rank 1 is still the active rank - casting it must not be rewritten.
    REQUIRE(resolveHighestKnownRank(772, known, dbc.lookup()) == 772);
}

TEST_CASE("Spells with no known sibling pass through unchanged", "[spell][rank]") {
    FakeSpellDbc dbc;
    dbc.add(8690, "Hearthstone", "");
    dbc.add(133,  "Fireball",    "Rank 1");
    dbc.add(6673, "Battle Shout", "Rank 1");
    dbc.add(5242, "Battle Shout", "Rank 2");

    const std::unordered_set<uint32_t> known{5242};

    // Item spells are absent from the known-spell list but have no known same-name
    // sibling, so they must be left alone rather than rewritten to something else.
    REQUIRE(resolveHighestKnownRank(8690, known, dbc.lookup()) == 8690);

    // A spell belonging to another class must not be rewritten either.
    REQUIRE(resolveHighestKnownRank(133, known, dbc.lookup()) == 133);
}

TEST_CASE("Rank resolution copes with missing data", "[spell][rank]") {
    FakeSpellDbc dbc;
    dbc.add(5242, "Battle Shout", "Rank 2");
    const std::unordered_set<uint32_t> known{5242};

    // Spell id 0 is "no spell".
    REQUIRE(resolveHighestKnownRank(0, known, dbc.lookup()) == 0);

    // A spell absent from Spell.dbc entirely cannot be resolved, so pass it through.
    REQUIRE(resolveHighestKnownRank(999999, known, dbc.lookup()) == 999999);

    // Nothing known at all: pass through rather than resolving to 0.
    REQUIRE(resolveHighestKnownRank(6673, {}, dbc.lookup()) == 6673);
}

// A heal and a nuke can share an effect id and a school, so neither tells them
// apart. EffectImplicitTargetA does: it is what the spell expects to be aimed
// at. These values are read from the shipped Spell.dbc.
TEST_CASE("friendly-target spells are the ones that may fall back to the caster",
          "[spell][classification]") {
    using namespace wowee::game::spellclass;

    SECTION("heals and buffs need a friendly unit") {
        CHECK(requiresFriendlyTarget(kImplicitTargetAlly));   // Flash Heal, Renew,
        CHECK(requiresFriendlyTarget(21));                    // Mark of the Wild...
    }

    SECTION("the friendly aims are four values, not one") {
        // Read out of the shipped Spell.dbc rather than assumed. Asking only
        // about 21 left the two commonest heals in the game refusing to
        // self-cast, since neither of them is 21.
        CHECK(requiresFriendlyTarget(kImplicitTargetChainHeal));  // Holy Light,
        CHECK(requiresFriendlyTarget(45));                        // Healing Wave
        CHECK(requiresFriendlyTarget(kImplicitTargetRaid));       // Beacon of Light,
        CHECK(requiresFriendlyTarget(57));                        // Intervene, Levitate
        CHECK(requiresFriendlyTarget(kImplicitTargetParty));      // Seal of Sacrifice
    }

    SECTION("a destination at the target is not a friendly unit") {
        // 63 carries Circle of Healing and Wild Growth, and also Fire Bomb and
        // Rain of Darkness. Taking it would turn a working hostile cast into
        // one aimed at the caster.
        CHECK_FALSE(requiresFriendlyTarget(63));
    }

    SECTION("damage does not") {
        CHECK_FALSE(requiresFriendlyTarget(kImplicitTargetEnemy));  // Smite, Fireball
        CHECK_FALSE(requiresFriendlyTarget(6));
    }

    SECTION("self-only spells are handled by range, not by this") {
        // Inner Fire and Blink read 1; they never carry a target either way, so
        // they must not be dragged through the friendly-target path.
        CHECK_FALSE(requiresFriendlyTarget(kImplicitTargetCaster));
    }

    SECTION("spells that take either target are left to the player") {
        // Dispel Magic reads 25. Retail does not self-cast it, because picking
        // between cleansing yourself and purging an enemy is guessing.
        CHECK_FALSE(requiresFriendlyTarget(kImplicitTargetAny));
    }

    SECTION("an unknown spell reads 0 and never self-casts") {
        CHECK_FALSE(requiresFriendlyTarget(0));
    }
}

// The other half of the same column, and the one with teeth: a spell that needs
// an enemy must not be sent with nothing selected. An empty SpellCastTargets is
// TARGET_FLAG_SELF on the wire and the server reads it as "the caster is the
// target", so an unaimed shot is a shot aimed at the hunter who fired it.
TEST_CASE("hostile-target spells are the ones that must not be sent unaimed",
          "[spell][classification]") {
    using namespace wowee::game::spellclass;

    SECTION("a nuke or a shot needs a hostile unit") {
        CHECK(requiresHostileTarget(kImplicitTargetEnemy));  // Arcane Shot, Fireball
        CHECK(requiresHostileTarget(6));                     // Steady Shot, Smite
    }

    SECTION("nothing friendly is caught by it") {
        // These fall back to the caster on purpose when auto self-cast is on,
        // which is the opposite decision - so the two tests must not overlap.
        CHECK_FALSE(requiresHostileTarget(kImplicitTargetAlly));
        CHECK_FALSE(requiresHostileTarget(kImplicitTargetParty));
        CHECK_FALSE(requiresHostileTarget(kImplicitTargetChainHeal));
        CHECK_FALSE(requiresHostileTarget(kImplicitTargetRaid));
    }

    SECTION("a self-cast is not refused for having no target") {
        // Aspect of the Hawk and Inner Fire read 1 and carry no target by
        // nature. Refusing those would take away every self-buff in the game.
        CHECK_FALSE(requiresHostileTarget(kImplicitTargetCaster));
    }

    SECTION("either-target and destination spells are left alone") {
        // 25 can go on the caster harmlessly and 63 is a point, not a unit.
        // Neither is the shape that kills the caster, so neither is blocked.
        CHECK_FALSE(requiresHostileTarget(kImplicitTargetAny));
        CHECK_FALSE(requiresHostileTarget(63));
    }

    SECTION("nothing a player casts on themselves reads as hostile") {
        // Measured over the shipped Spell.dbc, because the worry this answers
        // is that refusing an unaimed hostile spell also refuses a buff or a
        // self-heal. It cannot: none of them is 6.
        //
        //   Mark of the Wild 1126, Flash Heal 2061, Renew 139,
        //   Rejuvenation 774, Power Word: Fortitude 1243, Healing Touch 5185,
        //   Arcane Intellect 1459, Blessing of Might 19740, Holy Light 635,
        //   Lay on Hands 633, Power Word: Shield 17          all read 21
        //   Aspect of the Hawk 13165, Inner Fire 588,
        //   Trueshot Aura 19506, Feign Death 5384             all read 1
        //   Mend Pet 136                                          reads 5
        //   Battle Shout 2048                                    reads 56
        //
        // against the shots that started this:
        //   Arcane Shot 3044, Steady Shot 34120, Auto Shot 75,
        //   Serpent Sting 1978, Hunter's Mark 1130             all read 6
        for (uint32_t friendlyAim : {1u, 5u, 21u, 35u, 45u, 56u, 57u, 63u}) {
            CHECK_FALSE(requiresHostileTarget(friendlyAim));
        }
        CHECK(requiresHostileTarget(6));
    }

    SECTION("the two rules never both fire") {
        for (uint32_t t = 0; t < 128; ++t) {
            CHECK_FALSE((requiresFriendlyTarget(t) && requiresHostileTarget(t)));
        }
    }
}

TEST_CASE("autoAcquiresHostileTarget matches native TBC 2.4.3 Spell.dbc flags",
          "[spell][classification][autotarget]") {
    using namespace wowee::game::spellclass;

    // Attack (6603)
    CHECK(autoAcquiresHostileTarget(6603, 0x00000010, 0x00000000, 0x00000000));
    // AttributesEx & 0x200 (Sinister Strike, Backstab, Kick, Riposte, Cleave, Deadly Throw)
    CHECK(autoAcquiresHostileTarget(1752, 0x00050010, 0x08000200, 0x00000000));
    CHECK(autoAcquiresHostileTarget(53,   0x00050010, 0x08000200, 0x00100000));
    CHECK(autoAcquiresHostileTarget(1766, 0x00050010, 0x08000200, 0x00000000));
    CHECK(autoAcquiresHostileTarget(14251,0x00050010, 0x00020200, 0x00000000));
    // Attributes & 0x04 next-swing without 0x200 (Heroic Strike, Raptor Strike)
    CHECK(autoAcquiresHostileTarget(78,   0x00050014, 0x08000000, 0x00000000));
    CHECK(autoAcquiresHostileTarget(2973, 0x00050404, 0x00000000, 0x00000000));
    // AttributesEx2 & 0x20 autorepeat (Auto Shot 75, Shoot 5019)
    CHECK(autoAcquiresHostileTarget(75,   0x00050012, 0x00000000, 0x00000020));
    CHECK(autoAcquiresHostileTarget(5019, 0x00000012, 0x00000000, 0x00000020));

    // Hostile spells without those flags do NOT auto-acquire a target:
    // Gouge (1776), Sap (6770), Blind (2094), Throw (2764), Fireball (133), Smite (585), SW:P (589)
    CHECK_FALSE(autoAcquiresHostileTarget(1776, 0x00150010, 0x08040000, 0x00000000));
    CHECK_FALSE(autoAcquiresHostileTarget(6770, 0x00370010, 0x08040520, 0x00000000));
    CHECK_FALSE(autoAcquiresHostileTarget(2094, 0x40110010, 0x00000000, 0x00000000));
    CHECK_FALSE(autoAcquiresHostileTarget(2764, 0x00410012, 0x00000000, 0x00000000));
    CHECK_FALSE(autoAcquiresHostileTarget(133,  0x00010000, 0x00000000, 0x00000000));
    CHECK_FALSE(autoAcquiresHostileTarget(585,  0x00010000, 0x00000000, 0x00000000));
    CHECK_FALSE(autoAcquiresHostileTarget(589,  0x00050000, 0x00000000, 0x00080000));
}

// ---------------------------------------------------------------------------
// Range between two units takes both combat reaches. Native 2.4.3
// IsSpellInRange, measured from a human rogue (reach 1.5) at a stunned Mottled
// Boar whose UNIT_FIELD_COMBATREACH was set to Onyxia's 23.4 and to 5
// (_experiments/20261118T170000Z.melee_range_pair2 in the workspace). All
// distances are 3D, centre to centre.

namespace {
constexpr uint32_t kMelee = kSpellRangeFlagMelee;    // SpellRange 2, "Combat Range"
constexpr uint32_t kRanged = kSpellRangeFlagRanged;  // SpellRange 74, "Ranged Weapon"
constexpr float kHuman = 1.5f;
constexpr float kOnyxia = 23.4f;
} // namespace

TEST_CASE("melee range is both reaches and a third over a yard", "[spellclass][range]") {
    // Sinister Strike at Onyxia: in at 26.231 yards, out at 26.236.
    CHECK(withinSpellRange(26.231f, 0.0f, 5.0f, kMelee, kHuman, kOnyxia));
    CHECK_FALSE(withinSpellRange(26.236f, 0.0f, 5.0f, kMelee, kHuman, kOnyxia));
    CHECK(meleeReach(kHuman, kOnyxia) == Catch::Approx(26.2333f));
    // Never less than Combat Range, however small the two are.
    CHECK(meleeReach(0.0f, 0.0f) == kCombatRangeYards);
    CHECK(withinSpellRange(5.0f, 0.0f, 5.0f, kMelee, 1.5f, 0.627f));
    CHECK_FALSE(withinSpellRange(5.1f, 0.0f, 5.0f, kMelee, 1.5f, 0.627f));
}

TEST_CASE("a spell range reaches its maximum plus both reaches", "[spellclass][range]") {
    // Fireball (35) and Arcane Shot (35, ranged weapon): in at 58, out at 59.902.
    CHECK(withinSpellRange(58.0f, 0.0f, 35.0f, 0, kHuman, kOnyxia));
    CHECK_FALSE(withinSpellRange(59.902f, 0.0f, 35.0f, 0, kHuman, kOnyxia));
    CHECK_FALSE(withinSpellRange(59.902f, 0.0f, 35.0f, kRanged, kHuman, kOnyxia));
    // Blind (10): in at 34.849, out at 34.950.
    CHECK(withinSpellRange(34.849f, 0.0f, 10.0f, 0, kHuman, kOnyxia));
    CHECK_FALSE(withinSpellRange(34.950f, 0.0f, 10.0f, 0, kHuman, kOnyxia));
}

TEST_CASE("a minimum range takes both reaches too", "[spellclass][range]") {
    // Charge (8 to 25): out at 32.838, in from 32.939 to 49.854, out at 50.058.
    CHECK_FALSE(withinSpellRange(32.838f, 8.0f, 25.0f, 0, kHuman, kOnyxia));
    CHECK(withinSpellRange(32.939f, 8.0f, 25.0f, 0, kHuman, kOnyxia));
    CHECK(withinSpellRange(49.854f, 8.0f, 25.0f, 0, kHuman, kOnyxia));
    CHECK_FALSE(withinSpellRange(50.058f, 8.0f, 25.0f, 0, kHuman, kOnyxia));
}

TEST_CASE("a ranged weapon is out inside melee reach", "[spellclass][range]") {
    // Throw (0 to 30, flag 2) is 0 exactly where Sinister Strike is 1.
    CHECK_FALSE(withinSpellRange(26.231f, 0.0f, 30.0f, kRanged, kHuman, kOnyxia));
    CHECK(withinSpellRange(26.236f, 0.0f, 30.0f, kRanged, kHuman, kOnyxia));
    // Out again past 30 plus both reaches: in at 50.467, out at 54.964.
    CHECK(withinSpellRange(50.467f, 0.0f, 30.0f, kRanged, kHuman, kOnyxia));
    CHECK_FALSE(withinSpellRange(54.964f, 0.0f, 30.0f, kRanged, kHuman, kOnyxia));
}

TEST_CASE("a percent cost is a share of the pool, rounded down", "[spellclass][cost]") {
    using wowee::game::spellclass::percentPowerCost;
    // Arcane Shot is 5% of base mana; a level 6 hunter's base is around 160.
    CHECK(percentPowerCost(160, 5) == 8);
    CHECK(percentPowerCost(159, 5) == 7);
    CHECK(percentPowerCost(0, 9) == 0);
}

// IsHarmfulSpell / IsHelpfulSpell, against what the 2.4.3 client answered for
// these Spell.dbc rows (implicit targets A and B of effects 0-2).
TEST_CASE("Spell disposition follows the 2.4.3 client", "[spell_classification][disposition]") {
    struct Row { const char* name; uint32_t a[3]; uint32_t b[3]; SpellDisposition want; };
    using D = SpellDisposition;
    const Row rows[] = {
        {"Fireball",           {6, 6, 0},    {0, 0, 0},  D::Harmful},
        {"Frost Nova",         {22, 22, 0},  {15, 15, 0}, D::Harmful},
        {"Blizzard",           {28, 1, 0},   {0, 0, 0},  D::Harmful},
        {"Eviscerate",         {6, 1, 0},    {0, 0, 0},  D::Harmful},
        {"Hunter's Mark",      {25, 6, 6},   {0, 0, 0},  D::Harmful},
        {"Arcane Missiles 31742", {1, 6, 0}, {0, 0, 0},  D::Harmful},
        {"Force of Nature",    {8, 16, 0},   {0, 0, 0},  D::Harmful},
        {"Shadowfiend",        {53, 1, 0},   {0, 0, 0},  D::Harmful},
        {"Cone of Cold",       {24, 0, 0},   {0, 0, 0},  D::Harmful},
        {"Hellfire Effect",    {18, 0, 0},   {16, 0, 0}, D::Harmful},
        {"Arcane Intellect",   {21, 0, 0},   {0, 0, 0},  D::Helpful},
        {"Battle Shout",       {20, 0, 0},   {0, 0, 0},  D::Helpful},
        {"Holy Word: Fortitude", {22, 0, 0}, {30, 0, 0}, D::Helpful},
        {"Bloodlust",          {22, 0, 0},   {33, 0, 0}, D::Helpful},
        {"Tranquility 740",    {29, 1, 0},   {0, 0, 0},  D::Helpful},
        {"Tree of Life",       {0, 1, 1},    {0, 0, 0},  D::Helpful},
        {"Maypole",            {1, 25, 1},   {0, 0, 0},  D::Helpful},
        {"Blink",              {1, 1, 1},    {55, 0, 0}, D::Helpful},
        {"Mend Pet",           {5, 0, 0},    {0, 0, 0},  D::Helpful},
        {"Seed of Corruption 43991", {31, 0, 0}, {0, 0, 0}, D::Helpful},
        {"Dispel Magic",       {25, 0, 0},   {0, 0, 0},  D::Neither},
        {"Mind Vision",        {25, 1, 25},  {0, 0, 0},  D::Neither},
        {"Revive Pet",         {47, 1, 0},   {0, 0, 0},  D::Neither},
        {"Fishing",            {39, 0, 1},   {0, 0, 0},  D::Neither},
        {"Arcane Missile 7268", {77, 0, 0},  {0, 0, 0},  D::Neither},
        {"Summon Imp",         {32, 0, 0},   {0, 0, 0},  D::Neither},
        {"Searing Totem",      {44, 0, 0},   {0, 0, 0},  D::Neither},
        {"Dodge",              {0, 0, 0},    {0, 0, 0},  D::Neither},
    };
    for (const auto& r : rows) {
        INFO(r.name);
        CHECK(spellDisposition(r.a, r.b) == r.want);
    }
}

// Which units a single-unit spell takes decides whether the current target is
// used, autoSelfCast applies, or the targeting cursor arms (native 2.4.3, pair 13).
TEST_CASE("unitAim reads EffectImplicitTargetA and SpellCastTargetFlags", "[spell][classification]") {
    using namespace wowee::game::spellclass;
    CHECK(unitAim(6) == UnitAim::Enemy);             // Smite, Shadow Word: Pain
    CHECK(unitAim(53) == UnitAim::Enemy);            // Shadowfiend
    CHECK(unitAim(77) == UnitAim::Enemy);            // Arcane Missile
    CHECK(unitAim(21) == UnitAim::Friend);           // Arcane Intellect, Flash Heal
    CHECK(unitAim(37, 0x100) == UnitAim::Friend);    // Prayer of Fortitude, Arcane Brilliance
    CHECK(unitAim(45) == UnitAim::Friend);           // Healing Wave, Chain Heal
    CHECK(unitAim(35) == UnitAim::GroupMember);      // Fire Shield
    CHECK(unitAim(57) == UnitAim::GroupMember);      // Power Word: Shield, Prayer of Mending
    CHECK(unitAim(61) == UnitAim::GroupMember);      // Greater Blessing of Might
    CHECK(unitAim(25) == UnitAim::Any);              // Dispel Magic, Holy Shock
    CHECK(unitAim(0, 0x8000) == UnitAim::DeadFriend);// Resurrection, Redemption
    CHECK(unitAim(22, 0x8000) == UnitAim::None);     // Mass Resurrection (caster AoE)
    CHECK(unitAim(1) == UnitAim::None);              // Inner Fire
    CHECK(unitAim(5) == UnitAim::None);              // Mend Pet
    CHECK(unitAim(22) == UnitAim::None);             // Holy Nova
    CHECK(unitAim(23, 0x4000) == UnitAim::None);     // Herb Gathering / Mining
    CHECK(unitAim(26) == UnitAim::None);             // Pick Lock
    CHECK(unitAim(28, 0x40) == UnitAim::None);       // Blizzard
    CHECK(unitAim(63) == UnitAim::None);             // destination coordinates
}

TEST_CASE("isAuraVisible and isAuraCancellable match native TBC 2.4.3 Spell.dbc flags",
          "[spell][classification][aura]") {
    using namespace wowee::game::spellclass;
    constexpr uint32_t kApplyAura[3]   = {6, 0, 0};
    constexpr uint32_t kAreaParty[3]   = {35, 0, 0};
    constexpr uint32_t kShapeshift[3]  = {6, 6, 0};

    // Pure passive spells without area aura: hidden & uncancellable.
    CHECK_FALSE(isAuraVisible(0x00000040u, 0x00000000u, kApplyAura)); // Magic Resistance 822
    CHECK_FALSE(isAuraVisible(0x00000050u, 0x00000000u, kApplyAura)); // Gemcutting 28875, Arcane Affinity 28877, Shadow Resistance 20579
    CHECK_FALSE(isAuraCancellable(0x00000040u, 0x00000000u, false));
    CHECK_FALSE(isAuraCancellable(0x00000050u, 0x00000000u, false));

    // SPELL_ATTR_DO_NOT_DISPLAY (0x80): hidden even if area aura.
    CHECK_FALSE(isAuraVisible(0x000001d0u, 0x00000000u, kApplyAura)); // Predatory Strikes 16972, Heart of the Wild 17003
    CHECK_FALSE(isAuraVisible(0x000000c0u, 0x00000000u, kAreaParty));

    // SPELL_ATTR_EX_NO_AURA_ICON (0x10000000): hidden & uncancellable.
    CHECK_FALSE(isAuraVisible(0x29050010u, 0x90000400u, kApplyAura)); // Battle Stance 2457
    CHECK_FALSE(isAuraVisible(0x29050010u, 0x10000400u, kApplyAura)); // Defensive Stance 71
    CHECK_FALSE(isAuraVisible(0x09040010u, 0x10020020u, kApplyAura)); // Track Humanoids 19883
    CHECK_FALSE(isAuraCancellable(0x29050010u, 0x90000400u, false));

    // Passive area-aura buffs (Heroic Presence 6562, Inspiring Presence 28878,
    // Totem of Wrath 30708, Strength of Earth 8076): visible, uncancellable.
    CHECK(isAuraVisible(0x09050040u, 0x00000000u, kAreaParty)); // Heroic Presence 6562
    CHECK(isAuraVisible(0x00000140u, 0x00000000u, kAreaParty)); // Totem of Wrath 30708
    CHECK_FALSE(isAuraCancellable(0x09050040u, 0x00000000u, false));

    // Active area-aura buffs (Moonkin Aura 24907, Leader of the Pack 24932, Tree of Life party 34123): visible.
    CHECK(isAuraVisible(0x00000010u, 0x00000400u, kAreaParty));
    CHECK(isAuraVisible(0x00000000u, 0x00000400u, kAreaParty));
    CHECK(isAuraVisible(0x08000000u, 0x00000400u, kAreaParty));

    // Shapeshift forms (Dire Bear 9634, Cat 768, Moonkin 24858, Tree of Life 33891, Ghost Wolf 2645) and normal buffs (Thorns 26992).
    CHECK(isAuraVisible(0x00050010u, 0x00018000u, kShapeshift));
    CHECK(isAuraCancellable(0x00050010u, 0x00018000u, false));
    CHECK(isAuraVisible(0x00010000u, 0x00000000u, kApplyAura));
    CHECK(isAuraCancellable(0x00010000u, 0x00000000u, false));

    // SPELL_ATTR_NO_AURA_CANCEL (0x80000000) and debuffs: visible, not cancellable.
    CHECK(isAuraVisible(0x88000100u, 0x00000000u, kApplyAura));
    CHECK_FALSE(isAuraCancellable(0x88000100u, 0x00000000u, false));
    CHECK_FALSE(isAuraCancellable(0x00010000u, 0x00000000u, true));
}

TEST_CASE("isAuraCombatTextLogged matches native TBC 2.4.3 Spell.dbc Attributes",
          "[spell][classification][aura][fct]") {
    using namespace wowee::game::spellclass;

    // Suppressed by SPELL_ATTR_DO_NOT_DISPLAY (0x80) and/or SPELL_ATTR_DO_NOT_LOG (0x100):
    CHECK_FALSE(isAuraCombatTextLogged(0x00000180u)); // Rampage Enabler (DND) 33380 (0x80 | 0x100)
    CHECK_FALSE(isAuraCombatTextLogged(0x08000080u)); // Echeyakee's Grace 5320 (0x80 only)
    CHECK_FALSE(isAuraCombatTextLogged(0x18000100u)); // Food 433 (0x100 only, visible buff)
    CHECK_FALSE(isAuraCombatTextLogged(0x00000100u)); // Unscathed (DND) 29479 (0x100 only)
    CHECK_FALSE(isAuraCombatTextLogged(0x00000140u)); // Totem of Wrath 30708 (0x40 | 0x100)

    // Logged in COMBAT_TEXT_UPDATE (no 0x80 or 0x100):
    CHECK(isAuraCombatTextLogged(0x00010000u));       // Prayer of Fortitude 21562
    CHECK(isAuraCombatTextLogged(0x24000000u));       // Weakened Soul 6788
    CHECK(isAuraCombatTextLogged(0x29050010u));       // Defensive Stance 71 (NO_AURA_ICON in attrEx still logs)
    CHECK(isAuraCombatTextLogged(0x00040014u));       // Sundering Cleave 25174 (AQ20 Scarab Terrace)
    CHECK(isAuraCombatTextLogged(0x04810000u));       // Hive'Zara Catalyst 25187 (AQ20 Scarab Terrace)
    CHECK(isAuraCombatTextLogged(0x00040000u));       // Flurry 12970
    CHECK(isAuraCombatTextLogged(0x00000010u));       // Dazed 1604
}

// ---------------------------------------------------------------------------
// Action bar rank auto-upgrade (Native 2.4.3 SMSG_LEARNED_SPELL parity)
// ---------------------------------------------------------------------------

TEST_CASE("Learning a higher spell rank upgrades only the previous highest known rank",
          "[spell][rank][rankupgrade]") {
    FakeSpellDbc dbc;
    dbc.add(686,  "Shadow Bolt", "Rank 1");
    dbc.add(695,  "Shadow Bolt", "Rank 2");
    dbc.add(705,  "Shadow Bolt", "Rank 3");
    dbc.add(1088, "Shadow Bolt", "Rank 4");
    dbc.add(172,  "Corruption",  "Rank 1");
    dbc.add(8690, "Hearthstone", "");

    SECTION("Rank 1 -> Rank 2 upgrades Rank 1") {
        const std::unordered_set<uint32_t> known{686, 172};
        CHECK(findActionBarRankUpgradeSource(695, known, dbc.lookup()) == 686);
    }

    SECTION("Rank 1 + Rank 2 -> Rank 3 upgrades Rank 2 only, preserving intentional Rank 1 downrank") {
        const std::unordered_set<uint32_t> known{686, 695};
        CHECK(findActionBarRankUpgradeSource(705, known, dbc.lookup()) == 695);
    }

    SECTION("Skipping a rank (know only Rank 1 -> learn Rank 3) upgrades Rank 1 -> Rank 3") {
        const std::unordered_set<uint32_t> known{686};
        CHECK(findActionBarRankUpgradeSource(705, known, dbc.lookup()) == 686);
    }

    SECTION("Learning a lower rank while a higher rank is known never downgrades or replaces") {
        const std::unordered_set<uint32_t> known{686, 705};
        CHECK(findActionBarRankUpgradeSource(695, known, dbc.lookup()) == 0);
    }

    SECTION("Already known, first rank, rankless, and unknown spells return 0") {
        const std::unordered_set<uint32_t> known{686, 695};
        CHECK(findActionBarRankUpgradeSource(695, known, dbc.lookup()) == 0);
        CHECK(findActionBarRankUpgradeSource(172, known, dbc.lookup()) == 0);
        CHECK(findActionBarRankUpgradeSource(8690, known, dbc.lookup()) == 0);
        CHECK(findActionBarRankUpgradeSource(0, known, dbc.lookup()) == 0);
        CHECK(findActionBarRankUpgradeSource(999999, known, dbc.lookup()) == 0);
    }

    SECTION("Tolerates lookup adapter with reused scratch storage") {
        const std::unordered_map<uint32_t, SpellRankInfo> entries{
            {686, {"Shadow Bolt", "Rank 1"}},
            {695, {"Shadow Bolt", "Rank 2"}},
            {172, {"Corruption",  "Rank 1"}},
        };
        SpellRankInfo scratch;
        auto reusedLookup = [&entries, &scratch](uint32_t id) -> const SpellRankInfo* {
            auto it = entries.find(id);
            if (it == entries.end()) return nullptr;
            scratch = it->second;
            return &scratch;
        };
        const std::unordered_set<uint32_t> known{686, 172};
        CHECK(findActionBarRankUpgradeSource(695, known, reusedLookup) == 686);
    }
}

TEST_CASE("isProfessionSkillLine excludes racial, riding, and survival lines in category 9",
          "[spell][classification][profession][warstomp]") {
    using namespace wowee::game::spellclass;

    // Category 11 (Professions): every primary profession qualifies.
    for (uint32_t sl : {164u, 165u, 171u, 182u, 186u, 197u, 202u, 333u, 393u, 755u}) {
        CHECK(isProfessionSkillLine(sl, 11u));
    }

    // Category 9 (Secondary Skills): only First Aid (129), Cooking (185), Fishing (356).
    CHECK(isProfessionSkillLine(129u, 9u));
    CHECK(isProfessionSkillLine(185u, 9u));
    CHECK(isProfessionSkillLine(356u, 9u));

    // Regression: SkillLine.dbc category 9 also holds all ten racial skill lines
    // (124 Tauren Racial -> War Stomp 20549, etc.), Riding, Survival, and Swimming.
    // Treating all of category 9 as profession spells suppressed War Stomp's and
    // every other racial ability's SpellVisual, sound, and miss reactions.
    for (uint32_t racialSl : {101u, 124u, 125u, 126u, 220u, 733u, 753u, 754u, 756u, 760u}) {
        CHECK_FALSE(isProfessionSkillLine(racialSl, 9u));
    }
    for (uint32_t nonProfSl : {142u, 148u, 149u, 150u, 152u, 155u, 533u, 553u, 554u, 713u, 762u}) {
        CHECK_FALSE(isProfessionSkillLine(nonProfSl, 9u));
    }

    // Class skills (cat 7) and weapon skills (cat 6) are never professions.
    CHECK_FALSE(isProfessionSkillLine(26u, 7u));
    CHECK_FALSE(isProfessionSkillLine(43u, 6u));
}

TEST_CASE("Overpower reactive combo-point flag is distinguished from Rogue/Druid finishers",
          "[spell][classification][overpower][tbc]") {
    using namespace wowee::game::spellclass;

    // Overpower (7384/7887/11584/11585): FINISHING_MOVE_DAMAGE (0x100000) | COMBO_ON_BLOCK (0x40000000).
    // Uses SMSG_UPDATE_COMBO_POINTS for bar usability, NOT a combo-point finisher prerequisite.
    constexpr uint32_t kOverpowerAttrEx = 0x48100200u;
    CHECK(isReactiveComboSpell(kOverpowerAttrEx));
    CHECK(usesComboPointsForUsability(kOverpowerAttrEx));
    CHECK_FALSE(requiresComboPointsPrerequisite(kOverpowerAttrEx));

    // Genuine Rogue & Druid combo-point finishers.
    for (uint32_t finisherAttrEx : {
             0x00100200u, // Eviscerate, Rupture, Expose Armor, Envenom, Deadly Throw, Rip, Ferocious Bite
             0x00400400u, // Slice and Dice
             0x00440200u, // Kidney Shot
             0x00540000u, // Maim
         }) {
        CHECK_FALSE(isReactiveComboSpell(finisherAttrEx));
        CHECK(usesComboPointsForUsability(finisherAttrEx));
        CHECK(requiresComboPointsPrerequisite(finisherAttrEx));
    }

    // Aura-state reactive abilities (Revenge, Execute, Victory Rush, Riposte, Mongoose Bite).
    for (uint32_t reactiveAttrEx : {0x08000200u, 0x00020200u, 0x00000200u}) {
        CHECK_FALSE(isReactiveComboSpell(reactiveAttrEx));
        CHECK_FALSE(usesComboPointsForUsability(reactiveAttrEx));
        CHECK_FALSE(requiresComboPointsPrerequisite(reactiveAttrEx));
    }
}
