#pragma once
#include <cstdint>

namespace wowee::rendering {
inline constexpr bool gestureSheathesWeapons(uint32_t emoteFlags, uint32_t animationWeaponFlags) {
    // Only pure-NOSHEATHE emotes (flags == 0x4: Wave 70, Cheer 71, Laugh 153)
    // suppress AnimationData.WeaponFlags sheathing on a shared animation.
    // Talk/Exclamation/Question (0x8c/0xa4/0x94) and Loot (0x4404) still
    // follow AnimationData.WeaponFlags (0x04/0x10) and sheathe weapons.
    return emoteFlags != 0x4u && (animationWeaponFlags & 0x14u) != 0;
}
inline constexpr bool gestureWeaponPoseActive(uint32_t gestureAnimation,
                                              uint32_t baseAnimation,
                                              uint32_t upperAnimation) {
    return gestureAnimation != 0 &&
        (gestureAnimation == baseAnimation || gestureAnimation == upperAnimation);
}
/// Sheath state shown once the unit has played an animation with these
/// AnimationData.WeaponFlags: 0x20 (Attack1H/2H/Off, Ready1H/2H, Whirlwind,
/// ...) draws the melee weapons (sheath state 1) whatever the unit's own
/// sheath state, and they stay drawn afterwards. Build 8606: Leotheras the
/// Blind (21215) has sheath state 0 (creature_template_addon) all fight and
/// holds his glaives through Ready1H, Attack1H, AttackOff and Whirlwind; a
/// Defias Thug set to sheath state 0 shows its sword at the hip until its
/// first fight, then holds it through Stand and Walk for at least 50 s after
/// combat and through a server sheath state change 1 -> 0
/// (_experiments/20261118T1330Z.leotheras_glaives). It replaces only the
/// unarmed state 0: a drawn ranged weapon (state 2, e.g. a creature's bow
/// shot, EntitySpawner::setCreatureRangedDrawn) stays the one in hand, as
/// native shows an Illidari Heartseeker with only its bow during shots.
inline constexpr uint8_t animationSheathState(uint32_t animationWeaponFlags, uint8_t sheathState) {
    return (animationWeaponFlags & 0x20u) && sheathState == 0 ? 1 : sheathState;
}
/// Update whether an animation with AnimationData.WeaponFlags 0x20 is currently
/// holding a unit's melee weapons drawn at sheath state 0.
/// - NPCs (`latchPersistently = true`) stay drawn after their first 0x20 swing
///   (Leotheras the Blind 21215, Defias Thug in build 8606).
/// - Players (`latchPersistently = false`) control sheath state explicitly via
///   CMSG_SETSHEATHED / UNIT_FIELD_BYTES_2 byte 0, so a one-shot 0x20 swing at
///   sheath state 0 (e.g. `.damage` or a melee packet arriving before
///   UNIT_FIELD_BYTES_2) draws the weapon only while the 0x20 animation plays
///   and returns it cleanly to its sheath point when Stand resumes.
inline constexpr bool updateWeaponsDrawnByAnimation(bool currentlyDrawn,
                                                    bool latchPersistently,
                                                    bool poseSheathes,
                                                    bool activeAnimDrawsMelee) {
    if (latchPersistently) {
        return currentlyDrawn || (!poseSheathes && activeAnimDrawsMelee);
    }
    return !poseSheathes && activeAnimDrawsMelee;
}
/// Whether a pose animation flagged in AnimationData.WeaponFlags (0x04 or 0x10)
/// sheathes a weapon whose held sheath state is `weaponHeldSheathState` while
/// the unit is in `unitSheathState`.
///
/// Only bow/gun/crossbow shot animations (AttackBow 46, AttackRifle 49, both
/// WeaponFlags 0x10) keep a drawn ranged weapon (sheath state 2) in hand.
/// Every other 0x10/0x04 pose — including ReadyThrown (108), AttackThrown
/// (107), AttackUnarmed (16), emotes, SitGround, Kneel, and spell casts —
/// sheathes/hides both melee and ranged weapons.
inline constexpr bool poseAnimationSheathesWeapon(uint32_t poseAnimation,
                                                  uint32_t poseWeaponFlags,
                                                  int8_t weaponHeldSheathState,
                                                  uint8_t unitSheathState) {
    if (poseAnimation == 0) return false;
    constexpr int8_t kSheathStateRanged = 2;
    constexpr uint32_t kAttackBow = 46;
    constexpr uint32_t kAttackRifle = 49;
    if (weaponHeldSheathState == kSheathStateRanged &&
        unitSheathState == kSheathStateRanged &&
        (poseWeaponFlags & 0x04u) == 0 &&
        (poseAnimation == kAttackBow || poseAnimation == kAttackRifle)) {
        return false;
    }
    return true;
}

/// Whether `poseAnimation` (with `AnimationData.WeaponFlags` `poseWeaponFlags`)
/// sheathes the unit's currently active weapon set in `unitSheathState`.
/// Used to decide whether a local character's pose animation should transition
/// sheath state to unarmed (CMSG_SETSHEATHED 0): AttackBow (46) and AttackRifle
/// (49) carry WeaponFlags 0x10 but keep a drawn ranged weapon (state 2) in hand.
inline constexpr bool poseAnimationSheathesUnit(uint32_t poseAnimation,
                                                uint32_t poseWeaponFlags,
                                                uint8_t unitSheathState) {
    if (poseAnimation == 0) return false;
    return poseAnimationSheathesWeapon(
        poseAnimation, poseWeaponFlags,
        static_cast<int8_t>(unitSheathState), unitSheathState);
}
} // namespace wowee::rendering
