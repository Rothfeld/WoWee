// Which name tooltip a hovered game object shows, from its server template
// (CMaNGOS GameObject.h GAMEOBJECT_TYPE_GENERIC: data0 floatingTooltip,
// data1 highlight). Values are tbcmangos gameobject_template rows.
#include <catch_amalgamated.hpp>

#include "game/game_object_interaction.hpp"

using wowee::game::ObjectNameTooltip;
using wowee::game::objectNameTooltip;

TEST_CASE("generic objects label themselves from floatingTooltip and highlight", "[gameobject][tooltip]") {
    // 183865 Guild Master & Tabards: highlight only - ledger #04, the
    // original shows its name in the bottom-right tooltip.
    CHECK(objectNameTooltip(5, 0, 1) == ObjectNameTooltip::Default);
    // 92524 Guild: floating label at the cursor.
    CHECK(objectNameTooltip(5, 1, 1) == ObjectNameTooltip::Floating);
    // Unlabelled decorative scenery (953 rows with neither).
    CHECK(objectNameTooltip(5, 0, 0) == ObjectNameTooltip::None);
    // Chairs keep their cursor only; other types always name themselves.
    CHECK(objectNameTooltip(7, 1, 1) == ObjectNameTooltip::None);
    CHECK(objectNameTooltip(3, 0, 0) == ObjectNameTooltip::Default);
}
