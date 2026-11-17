# Persistent spell models

Run `build/bin/test_m2_structs '[bone-scale],[state-effect]'` for authored scale
and state-effect startup tests.

M2 bone tracks retain zero, tiny, and negative scale values. Zero collapses
geometry during an authored hidden phase; negative values mirror an axis.
Only absent scale tracks use identity. Shader normal normalization keeps
collapsed geometry finite without replacing its scale.

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
