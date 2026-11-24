#include <catch_amalgamated.hpp>
#include "rendering/preview_background.hpp"
#include "ui/unit_portrait.hpp"

using wowee::rendering::PreviewBackgroundMode;
using wowee::rendering::portraitBackgroundModeForFraming;
using wowee::rendering::previewClearColorRGBA;
using wowee::rendering::previewLoadsRacialBackdrop;
using wowee::rendering::shouldDesaturatePlayerPortrait;
using wowee::ui::creatureDisplayPortraitPath;

TEST_CASE("Unit portrait framing selects opaque black portrait disc vs transparent full-body", "[portrait]") {
    const auto faceMode = portraitBackgroundModeForFraming(/*isFaceFraming=*/true);
    CHECK(faceMode == PreviewBackgroundMode::OpaquePortrait);
    CHECK_FALSE(previewLoadsRacialBackdrop(faceMode));
    const auto faceClear = previewClearColorRGBA(faceMode);
    CHECK(faceClear[0] == 0.0f);
    CHECK(faceClear[1] == 0.0f);
    CHECK(faceClear[2] == 0.0f);
    CHECK(faceClear[3] == 1.0f);

    const auto fullBodyMode = portraitBackgroundModeForFraming(/*isFaceFraming=*/false);
    CHECK(fullBodyMode == PreviewBackgroundMode::Transparent);
    CHECK_FALSE(previewLoadsRacialBackdrop(fullBodyMode));
    const auto fullBodyClear = previewClearColorRGBA(fullBodyMode);
    CHECK(fullBodyClear[3] == 0.0f);

    const auto npcDialogFace = portraitBackgroundModeForFraming(
        /*isFaceFraming=*/true, /*hasPanelBackdrop=*/true);
    CHECK(npcDialogFace == PreviewBackgroundMode::Transparent);
    CHECK(previewClearColorRGBA(npcDialogFace)[3] == 0.0f);

    CHECK(previewLoadsRacialBackdrop(PreviewBackgroundMode::RacialScene));
    const auto sceneClear = previewClearColorRGBA(PreviewBackgroundMode::RacialScene);
    CHECK(sceneClear[3] == 1.0f);
}

TEST_CASE("Unit portrait 2D fallbacks distinguish players, creatures, pets, and 0-vertex stalkers", "[portrait]") {
    using wowee::ui::resolveUnitPortraitFallback;
    using wowee::ui::temporaryPortraitPath;

    CHECK(temporaryPortraitPath(1, 0, false, false) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Male-Human");
    CHECK(temporaryPortraitPath(4, 1, false, false) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Female-NightElf");
    CHECK(temporaryPortraitPath(0, 0xFF, true, false) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Pet");
    CHECK(temporaryPortraitPath(4, 0, false, true) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Monster");

    CHECK(resolveUnitPortraitFallback(0, "", 0xF1300047FF000001ULL, 4, 0, false, true, true) ==
          "Interface\\Buttons\\WHITE8X8");
    CHECK(resolveUnitPortraitFallback(0, "", 0xF1300047FF000001ULL, 4, 0, false, true, false) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Monster");
    CHECK(resolveUnitPortraitFallback(0, "", 0x1AULL, 1, 0, false, false, false) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Male-Human");
}

TEST_CASE("PlayerPortrait is never desaturated in ghost or Night Elf Wisp Spirit form", "[portrait]") {
    CHECK_FALSE(shouldDesaturatePlayerPortrait(/*isPlayerGhost=*/false));
    CHECK_FALSE(shouldDesaturatePlayerPortrait(/*isPlayerGhost=*/true));
}

TEST_CASE("CreatureDisplayInfo PortraitTextureName resolves to Interface\\Icons path", "[portrait][dbc]") {
    CHECK(creatureDisplayPortraitPath("").empty());
    CHECK(creatureDisplayPortraitPath("Spell_Shadow_GatherShadows") ==
          "Interface\\Icons\\Spell_Shadow_GatherShadows");
    CHECK(creatureDisplayPortraitPath("Spell_Fire_Fireball02") ==
          "Interface\\Icons\\Spell_Fire_Fireball02");
    CHECK(creatureDisplayPortraitPath("INV_Misc_Head_Undead_01") ==
          "Interface\\Icons\\INV_Misc_Head_Undead_01");
    CHECK(creatureDisplayPortraitPath("Interface\\Icons\\INV_Cask_04") ==
          "Interface\\Icons\\INV_Cask_04");
}
