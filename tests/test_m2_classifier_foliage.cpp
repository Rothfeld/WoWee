#include <catch_amalgamated.hpp>

#include "rendering/m2_model_classifier.hpp"

#include <string>

using wowee::rendering::classifyM2Model;

namespace {

// Bounds and counts stand in for a mid-size doodad. None of the cases below
// turn on geometry - they are all decided by the name - but the classifier
// needs something plausible to reason about.
wowee::rendering::M2ClassificationResult classify(const std::string& name,
                                                  float horiz = 3.0f,
                                                  float vert = 3.0f) {
    return classifyM2Model(name,
                           glm::vec3(-horiz * 0.5f, -horiz * 0.5f, 0.0f),
                           glm::vec3(horiz * 0.5f, horiz * 0.5f, vert),
                           500, 0);
}

} // namespace

TEST_CASE("summoning portal art stays nonblocking without discarding its mesh", "[m2][classifier]") {
    const auto portal = classifyM2Model(
        "World\\Generic\\ActiveDoodads\\InstancePortal\\Summon_Ritual.m2",
        glm::vec3(0, -2.11842f, 0), glm::vec3(0, 2.11842f, 5.72386f), 4, 4);
    CHECK(portal.isInstancePortal);
    CHECK(portal.isSpellEffect);
    CHECK_FALSE(portal.isInvisibleTrap);
}

// 2.4.3 plays a placed tree's own Stand and adds no motion to it: the canopy
// of StranglethornTree02 sways from its keyed bones, its roots and the
// StranglethornRoot01-03 doodads (one unkeyed bone) stand still.
TEST_CASE("plants and banners keep their authored animation", "[m2][classifier]") {
    for (const char* n : {"StranglethornTree02", "StranglethornRoot03", "BadlandsPalmTree02",
                          "ElwynnBush01", "dr_banner01", "StormwindLionBanner",
                          "InnBedCanopy", "StranglethornWaterfall01"}) {
        INFO(n);
        CHECK_FALSE(classify(n, 8.0f, 14.0f).disableAnimation);
    }
}

// isForge forces the batch additive, so a false positive renders a solid model
// as glowing translucent VFX - the same failure the Steam Tank had.
TEST_CASE("only an actual forge is treated as forge fire", "[m2][classifier][forge]") {
    SECTION("the city of Ironforge is not a forge") {
        for (const char* n : {"IronforgeBench_Average01", "IronforgeStatue_01",
                              "IronforgeCliff01", "IronforgeElevator",
                              "IronforgeHangingLantern01", "IronforgeBanner01",
                              "IronforgeSignpost", "ironforgepiston"}) {
            INFO(n);
            CHECK_FALSE(classify(n).isForge);
        }
    }

    SECTION("nor is a part of one, or a panel that controls one") {
        CHECK_FALSE(classify("Dalaran_ForgeArms").isForge);
        CHECK_FALSE(classify("Dalaran_ForgeSmelter").isForge);
        CHECK_FALSE(classify("BU_CrystalForgeController").isForge);
        CHECK_FALSE(classify("UL_Forge_Iron_Press").isForge);
    }

    SECTION("real forges still are, with or without a numeric suffix") {
        for (const char* n : {"DR_Forge_01", "OM_Forge_01", "BU_Forge_01",
                              "ID_Forge", "TS_Forge_01", "BE_Forge01",
                              "Dalaran_Forge", "SC_RuneForge_02",
                              "ET_CrystalForge", "BlacksmithForge",
                              "DarkIronForge", "Wolvar_Forge",
                              "WORLD\\GENERIC\\HUMAN\\FORGE\\ID_FORGE.M2"}) {
            INFO(n);
            CHECK(classify(n).isForge);
        }
    }

    SECTION("forge lava stays excluded as before") {
        CHECK_FALSE(classify("UL_ForgeLava").isForge);
    }
}

// The second invisible wall at that same portal. AuraPurple.m2 is the glow
// inside the arch; its VFX identity is only in the DIRECTORY, and tokens are
// matched on the basename, so "particleemitter" in kEffectTokens never fired.
// Its real unscaled bounds (1.96 x 1.96 x 3.08) land inside genericSolid's
// window, so it became a solid prop - and the doodad is placed at scale 10.69,
// making a ~21 x 21 x 33 unit block of solid nothing over the portal.
TEST_CASE("models under PARTICLEEMITTERS are VFX, not props",
          "[m2][classifier][collision]") {
    const auto aura = classify("WORLD\\GENERIC\\PASSIVEDOODADS\\PARTICLEEMITTERS"
                               "\\AURAPURPLE.M2", 1.96f, 3.08f);
    CHECK(aura.isSpellEffect);

    SECTION("the whole directory, whichever separator the path uses") {
        for (const char* p : {
                 "WORLD\\GENERIC\\PASSIVEDOODADS\\PARTICLEEMITTERS\\AURABLUETALL.M2",
                 "WORLD\\GENERIC\\PASSIVEDOODADS\\PARTICLEEMITTERS\\ASHENVALEWISPS.M2",
                 "world/generic/passivedoodads/particleemitters/auragreen.m2"}) {
            INFO(p);
            const auto cls = classify(p, 1.96f, 3.08f);
            CHECK(cls.isSpellEffect);
        }
    }

    SECTION("a real prop of the same size elsewhere still blocks") {
        const auto crate = classify("WORLD\\GENERIC\\HUMAN\\PASSIVE DOODADS"
                                    "\\CRATES\\CRATE01.M2", 1.96f, 3.08f);
        CHECK_FALSE(crate.isSpellEffect);
    }
}

TEST_CASE("a small tree with leaf emitters is a tree, not an effect",
          "[m2][classifier][foliage]") {
    const auto tree = classifyM2Model(
        "WORLD\\KALIMDOR\\AZSHARA\\PASSIVEDOODADS\\TREES\\AZR_TREE01.M2",
        glm::vec3(-10.0f), glm::vec3(10.0f), 197, 3);
    CHECK_FALSE(tree.isSpellEffect);

    // Falling leaves with no tree under them stay an effect.
    const auto leaves = classifyM2Model(
        "WORLD\\EXPANSION02\\DOODADS\\DRAGONBLIGHT\\DB_LEAVES_FALL.M2",
        glm::vec3(-10.0f), glm::vec3(10.0f), 0, 3);
    CHECK(leaves.isSpellEffect);
}

TEST_CASE("a candelabra with a flame per candle is a fixture, not an effect",
          "[m2][classifier]") {
    const auto candelabra = classifyM2Model(
        "WORLD\\GENERIC\\HUMAN\\PASSIVE DOODADS\\CANDELABRAS\\GENERALCANDELABRA01.M2",
        glm::vec3(-0.5f), glm::vec3(0.5f), 167, 3);
    CHECK_FALSE(candelabra.isSpellEffect);
    CHECK(candelabra.isLanternLike);
}

TEST_CASE("a waterfall is not foliage, whatever zone it is named for",
          "[m2][classifier][foliage]") {
    CHECK_FALSE(classify("StranglethornWaterfall01").disableAnimation);
    CHECK(classify("StranglethornWaterfall01").isWaterfall);
}

// "bonfire" and "campfire" are matched as plain substrings. A delimited-word
// match wants a non-letter in front of the token, and WoW's model names do not
// oblige: 19 of the 37 fires in the WotLK data failed it, the Grom'gol bonfire
// among them, and got none of the flame treatment or the ambient fire loop.
TEST_CASE("a fire whose name runs its words together is still a fire",
          "[m2][classifier][fire]") {
    SECTION("no delimiter before the token") {
        CHECK(classify("World\\Azeroth\\BurningSteppes\\PassiveDoodads\\Bonfire\\OrcBonFire.m2")
                  .isBrazierOrFire);
        CHECK(classify("orcpvpbonfirelarge.m2").isBrazierOrFire);
        CHECK(classify("blackrockorccampfire.m2").isBrazierOrFire);
        CHECK(classify("karazahnbonfire01.m2").isBrazierOrFire);
        CHECK(classify("northrendundeadcampfire.m2").isBrazierOrFire);
        CHECK(classify("orgrimmarbonfire01.m2").isBrazierOrFire);
    }

    SECTION("delimited names still match") {
        CHECK(classify("summerfest_bonfire_large01.m2").isBrazierOrFire);
        CHECK(classify("elwynncampfire.m2").isBrazierOrFire);
    }

    SECTION("firepit too, which has the same shape") {
        CHECK(classify("largefirepit01.m2").isBrazierOrFire);
        CHECK(classify("smallfirepit01.m2").isBrazierOrFire);
    }

    // A bare "fire" has to stand alone at both ends. Guarding only the front
    // let it match the head of these, so a stack of logs was a burning one -
    // with a flame's colour floor and an ambient fire loop over it.
    SECTION("words that merely begin with fire are not fires") {
        CHECK_FALSE(classify("firewoodpile03.m2").isBrazierOrFire);
        CHECK_FALSE(classify("fireflies01.m2").isBrazierOrFire);
        CHECK_FALSE(classify("gunshopfireworks01.m2").isBrazierOrFire);
        CHECK_FALSE(classify("g_firework01blue.m2").isBrazierOrFire);
        CHECK_FALSE(classify("firecrackerstring_red01.m2").isBrazierOrFire);
    }

    // And a fire named at the end of a compound still counts.
    SECTION("fire at the end of a name") {
        CHECK(classify("valgarde_fire.m2").isBrazierOrFire);
        CHECK(classify("orctablecooker01fire.m2").isBrazierOrFire);
    }

    // The unlit variant of a fire is not a burning one: it would pick up a
    // flame's colour floor and an ambient fire loop while standing cold.
    SECTION("an explicitly unlit variant is not lit") {
        CHECK_FALSE(classify("orcbonfireoff.m2").isBrazierOrFire);
        CHECK_FALSE(classify("torch_out.m2").isTorch);
        CHECK(classify("torch.m2").isTorch);
    }
}

// The ambient sound system asks the same question with its own entry point.
// It had its own copy of the test and the two had already drifted: a model
// could be drawn as flame and play no fire, or the reverse.
TEST_CASE("the fire sound and the fire rendering agree on what a fire is",
          "[m2][classifier][fire]") {
    using wowee::rendering::AmbientEmitterType;
    const auto emitter = [](const char* name) {
        return wowee::rendering::classifyAmbientEmitter(name);
    };
    for (const char* name : {"orcbonfire", "orcpvpbonfirelarge", "blackrockorccampfire",
                             "largefirepit01", "valgarde_fire"}) {
        CAPTURE(name);
        CHECK(classify(std::string(name) + ".m2").isBrazierOrFire);
        const auto type = emitter(name);
        CHECK((type == AmbientEmitterType::FireplaceSmall ||
               type == AmbientEmitterType::FireplaceLarge));
    }
    for (const char* name : {"firewoodpile03", "gunshopfireworks01", "orcbonfireoff"}) {
        CAPTURE(name);
        CHECK_FALSE(classify(std::string(name) + ".m2").isBrazierOrFire);
        CHECK(emitter(name) == AmbientEmitterType::None);
    }
}

TEST_CASE("Small solid props retain geometry when they contain particles", "[m2][classifier][bowl]") {
    for (const char* name : {"BubblingBowl01", "BubblingBowl02", "SmallPropWithParticles"}) {
        INFO(name);
        const auto prop = classifyM2Model(name, glm::vec3(-0.5f), glm::vec3(0.5f), 42, 3);
        CHECK_FALSE(prop.isSpellEffect);
    }
}

// Ground clutter is decided by who placed the model, not by its folder. The
// Stranglethorn fern beside the big trees is an ADT doodad in a Detail folder;
// 2.4.3 draws it with its authored alpha key, hard and opaque.
TEST_CASE("Ground detail comes from the ground-effect placement", "[m2][classifier][detail]") {
    for (const char* name : {
             "World\\Azeroth\\Stranglethorn\\PassiveDoodads\\Detail\\StranglePlant10.m2",
             "world\\nodxt\\detail\\wetbus01.m2"}) {
        INFO(name);
        const glm::vec3 lo(-1.0f), hi(1.0f);
        CHECK_FALSE(classifyM2Model(name, lo, hi, 500, 0).isGroundDetail);
        const auto clutter = classifyM2Model(name, lo, hi, 500, 0, /*groundEffect=*/true);
        CHECK(clutter.isGroundDetail);
    }
}
