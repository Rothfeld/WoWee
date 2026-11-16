# Ghost presentation

Ghost world lighting selects the fifth Light.dbc condition (field 11). This is
independent of `ffxDeath`: disabling the option disables screen compositing,
while the authored ghost lighting remains active.

The scene compositor converts luminance and adds a cyan midtone contribution.
UI renders afterward, retaining its colors. Antialiasing stays independently
controlled even when the effect requires the scene-compositing pass.

References:
- TBC Light table layout: https://github.com/wowdev/WoWDBDefs/blob/master/definitions/Light.dbd
- Independent renderer formula and condition slots:
  https://github.com/samwhosung/benilla/blob/main/crates/benilla-world/src/shaders/ffx_glow.wgsl
  https://github.com/samwhosung/benilla/blob/main/crates/benilla-formats/src/light.rs
- Native TBC on/off captures, data-table probe and measured pixel transfer:
  `../_experiments/20261116T212353.622566Z.death_effect_healthbars/`.

Native captures confirm luminance-dependent color rather than a flat overlay.
Exact scene matching still depends on base lighting and bloom; do not calibrate
the death effect by changing global brightness or live-world color settings.

Resurrection replaces the body with bones using the same GUID and owner.
Check corpse flag `0x1`; owner matching alone cannot identify a reclaimable body.
Bodies use the corpse's appearance/equipment snapshot. Bones use the shipped
race/sex DeathSkeleton model, without equipped meshes, and cannot restore the
player's reclaimable corpse GUID.

Whole-model opacity changes (ghosts and camera fading) need a nearest-surface
depth prepass for opaque and alpha-key meshes, followed by EQUAL-depth alpha
blending. Blending while writing depth exposes triangle submission order as
patchwork. Authored translucent and additive materials retain their own paths.
