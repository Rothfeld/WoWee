# Persistent spell models

Run `build/bin/test_m2_structs '[bone-scale],[state-effect],[billboard-parent],[material-opacity]'`
for authored scale, lifecycle, inherited billboard and batch opacity tests.

M2 bone tracks retain zero, tiny, and negative scale values. Zero collapses
geometry during an authored hidden phase; negative values mirror an axis.
Only absent scale tracks use identity. Shader normal normalization keeps
collapsed geometry finite without replacing its scale.

Billboard bones face their inherited card plane toward the camera around the
animated pivot. Parent rotation moves their orbit without tilting the final
card plane away from the viewer. This matters for `spells/sap_state_head.m2`,
whose star billboards sit under two animated parent bones.

Varying color-alpha and texture-weight tracks are sampled per batch and instance
using the current animation sequence and local/global clocks. Their product
multiplies instance alpha; static material alpha remains in the material UBO.
`spells/detectstealth_state_head.m2` starts opacity tracks at zero and later
raises them during its glow cycle. Selecting only an initial nonzero key cannot
represent that authored cycle.

Aura state models start at the beginning of Stand (0). When Hold (158) exists,
the regular one-shot transition enters that loop. These instances do not use
random doodad start times or idle variations. Removing the aura plays Decay
(159), when authored, then destroys the visual after its duration. Attachment
tracking continues during decay. Models without Hold keep their Stand loop;
models without Decay disappear on removal.

Original TBC `spells/detectstealth_state_head.m2` has a 700ms Stand intro,
2933ms Hold loop, and 1100ms Decay outro. Its intro scale is zero through 200ms,
then expands into visible keys. Effect lifecycle correctness does not establish
native attachment-height or material parity; those require paired visual checks.

SpellVisualKit HeadEffect binds to M2 AboveCharacter attachment18. This keeps
effects overhead when an animated head attachment11 moves down in a mounted
pose. Original TBC detection eyes (spell18950) and Dazed stars (spell1604)
independently establish this slot behavior. Hand, chest, base and breath slots
retain their own attachment semantics.

Set `WOWEE_TRACE_AURA_ATTACHMENT=1` before launch to log aura owner position,
chosen anchor, anatomical Head11, AboveCharacter18, clip coordinates and scale
once per second. Compare effect-to-head separation in matched cameras; raw
screen Y also includes rider pose and placement differences. This diagnostic
does not establish material, brightness or billboard orientation parity.
