# Legacy animation time

Vanilla and TBC M2 tracks store keys on a shared absolute timeline. Each
animation sequence selects a window on that timeline. Track ranges may include
keys before the window so interpolation remains continuous across sparse keys.

`M2AnimationTrack::SequenceKeys::timeOffset` stores the sequence window start.
The shared track samplers add this offset to local animation time. Global
sequences keep their own clock. Unpartitioned tracks apply to every sequence.

HumanMale's Death sequence spans 30000-32000 ms. Its pelvis rotation range
starts at 3333 ms, while its translation range starts at 30000 ms. Both must
sample at 32000 ms at death completion. Rebasing each track to its first key
breaks their synchronization.

Validation: `test_m2_color_alpha` passes 27 assertions across nine cases,
including sparse float, vector, and quaternion tracks. A production-loader
CPU skinning probe of the original HumanMale asset confirms a horizontal
death pose. Live comparison artifacts belong in the workspace experiment
folder `20261116T135823Z.corpse_torch`.
