// The $-tokens of server text, as build 8606 fills them in.
//
// Every expectation here is a string native's GetGossipText returned for the
// same npc_text (_experiments/20261118T203000Z.text_tokens/results-gossip.jsonl):
// a human male rogue "Edrikp" and the same character as a blood elf female
// paladin, both with world state 9001 = 7 where it is used.
#include <catch_amalgamated.hpp>

#include "game/text_tokens.hpp"

using namespace wowee;

namespace {
game::TextSubject humanRogue() {
    game::TextSubject s;
    s.name = "Edrikp";
    s.className = "Rogue";
    s.raceName = "Human";
    s.female = false;
    s.pvpRankName = "Private";
    s.worldState = [](uint32_t id) { return id == 9001 ? 7u : 0u; };
    return s;
}

game::TextSubject bloodElfPaladin() {
    game::TextSubject s = humanRogue();
    s.className = "Paladin";
    s.raceName = "Blood Elf";
    s.female = true;
    s.pvpRankName = "Scout";
    return s;
}

std::string as(const game::TextSubject& s, const std::string& text) {
    return game::resolveTextTokens(text, s);
}
}  // namespace

TEST_CASE("each letter, lower case", "[text][tokens]") {
    CHECK(as(humanRogue(),
             "M a[$a]b[$b]c[$c]d[$d]e[$e]f[$f]h[$h]i[$i]j[$j]k[$k]l[$l]m[$m]n[$n]o[$o]p[$p]q[$q]"
             "r[$r]s[$s]u[$u]v[$v]w[$w]x[$x]y[$y]z[$z]") ==
          "M a[$a]b[\n]c[rogue]d[0 |4minute:minutes;]e[0]f[$f]h[$h]i[$i]j[$j]k[$k]l[$l]m[$m]"
          "n[Edrikp]o[$o]p[$p]q[$q]r[human]s[$s]u[$u]v[$v]w[0]x[$x]y[$y]z[$z]");
    CHECK(as(bloodElfPaladin(), "c[$c]r[$r]") == "c[paladin]r[blood elf]");
}

TEST_CASE("each letter, upper case", "[text][tokens]") {
    CHECK(as(humanRogue(),
             "M A[$A]B[$B]C[$C]D[$D]E[$E]F[$F]H[$H]I[$I]J[$J]K[$K]L[$L]M[$M]N[$N]O[$O]P[$P]Q[$Q]"
             "R[$R]S[$S]U[$U]V[$V]W[$W]X[$X]Y[$Y]Z[$Z]") ==
          "M A[$A]B[\n]C[Rogue]D[0 |4minute:minutes;]E[0]F[$F]H[$H]I[$I]J[$J]K[$K]L[$L]M[$M]"
          "N[Edrikp]O[$O]P[$P]Q[$Q]R[Human]S[$S]U[$U]V[$V]W[0]X[$X]Y[$Y]Z[$Z]");
    CHECK(as(bloodElfPaladin(), "C[$C]R[$R]") == "C[Paladin]R[Blood Elf]");
}

TEST_CASE("$g switches", "[text][tokens]") {
    const std::string g =
        "1[$gma:fe;]2[$Gma:fe;]3[$g ma : fe ;]4[$gma:fe:nb;]5[$gonly;]6[$gm:f]7[x;]8[$g:;]"
        "9[$g$n:$c;]0[$gA;B:C;]e";
    CHECK(as(humanRogue(), g) == "1[ma]2[ma]3[ma]4[ma]5[only;]6[$gm]8[]9[$n]0[A;B]e");
    CHECK(as(bloodElfPaladin(), g) == "1[fe]2[fe]3[fe]4[fe]5[f]7[x]8[]9[$c]0[C]e");

    const std::string g2 = "1[$gma:fe:nb;]2[$g ma : fe ;]3[$Gm: f ;]4[$gm:;]5[$g:f;]6[$c$gx:y;$r]7[$gm:f]e";
    CHECK(as(humanRogue(), g2) == "1[ma]2[ma]3[m]4[m]5[]6[roguexhuman]7[:f]e");
    CHECK(as(bloodElfPaladin(), g2) == "1[fe]2[fe]3[f]4[]5[f]6[paladinyblood elf]7[:f]e");
}

TEST_CASE("$t writes the honour rank, whatever its switch says", "[text][tokens]") {
    CHECK(as(humanRogue(), "1[$tma:fe;]2[$Tma:fe;]3[$T scrub : girlie;]4[x]5[$t]6[$T]e") ==
          "1[private]2[Private]3[Private]4[x]5[]6[]e");
    CHECK(as(bloodElfPaladin(), "1[$t;]2[$tx;]3[$t:;]4[$ta:b:c;]5[$tma:fe;]6[$Tq;]e") ==
          "1[scout]4[scout]5[scout]6[q;]e");
}

TEST_CASE("numbers, world states and stray dollars", "[text][tokens]") {
    CHECK(as(humanRogue(),
             "M 1[$1]2[$962w]3[$4262d]4[$2285W]5[$$]6[$ ]7[$9999w]8[$1x]9[$12345678901w]0[$962W]"
             "a[$0w]b[$-1w]end$") ==
          "M 1[$]2[0]3[0 |4minute:minutes;]4[0]5[$$]6[$ ]7[0]8[$x]9[0]0[0]a[0]b[$-1w]end$");
    CHECK(as(humanRogue(), "w[$9001w]W[$9001W]e[$9001e]") == "w[7]W[7]e[0]");
    CHECK(as(humanRogue(), "M 1[$N$n]2[$$n]3[$Nfoo]4[$C$c$R$r]5[$B$b]6[$|n]7[%s]8[|n]9[$n$]e") ==
          "M 1[EdrikpEdrikp]2[$Edrikp]3[Edrikpfoo]4[RoguerogueHumanhuman]5[\n\n]6[$|n]7[%s]8[|n]9[Edrikp$]e");
}

TEST_CASE("text with no dollar comes back as it was", "[text][tokens]") {
    CHECK(as(humanRogue(), "Hail, traveller.|n") == "Hail, traveller.|n");
}
