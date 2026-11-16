# Aquatic creature placement

Run `build/bin/test_animation_ids '[creature-locomotion]'` from WoWee.

Creature spline animation uses flying, swimming, then ground walking/running
priority. A walking-speed flag does not replace a swimmer's authored swim pose.
The same selector drives the movement callback and per-frame state transitions.

Swimming creatures use the existing player surface-depth rule:
`0.75 * CreatureModelData.CollisionHeight * instance scale`. Their displayed Z
is capped at liquid surface minus this depth. Deeper authoritative positions
remain unchanged. Flying units, transport passengers, and coordinates without a
loaded liquid surface receive no waterline adjustment. Network position is never
changed by this presentation rule.

Elder Darkshore Thresher display994 has collision height2.083 and scale1.15.
Its server path uses liquid-surface Z0.183376. Paired native captures confirm its
swim pose and torso waterline, plus School of Fish display4878 surface placement.
Evidence: workspace experiment `20261116T230740Z.thresher_waterline`.
