# Swimming jumps

Space applies one upward impulse per press while swimming. Its launch speed is
9.1 yards/second. With gravity 19.29 yards/second squared, the rise is about 2.14
yards. Below the surface, ascent ends at the apex and swimming resumes. Crossing
the surface continues the ballistic arc until the character returns to swimming
height or lands. Holding Space does not generate repeated impulses.

Landing resolves after integration within each physics substep, before the new
position reaches rendering or camera tracking. Resolving only on the following
substep exposes a transient dip below swimming height.

TBC swimming depth is derived from the player model's scaled collision height
at the 75% liquid threshold, instead of a fixed feet-to-waterline distance.
CreatureModelData field 15 supplies collision height. The dwarf male model is
1.607 yards tall, giving a 1.20525-yard swimming depth. The server's deep-water
test uses this collision-height fraction; paired native landing samples support
using the same threshold for surface re-entry. Native discrete landing frames
can finish slightly below the threshold.

The airborne state persists across physics substeps even while feet remain below
the water plane. Reset and teleport clear it. Water exit setup runs once when
leaving the current swimming state, rather than once per substep using the
previous render frame's state.

Downward swimming uses the server swim speed. Movement jump packets carry the
water launch speed and swim horizontal speed, and clear the swimming flag.

Native position samples, bounded paired input, screenshots, and WoWee movement
traces are in workspace experiment `20261116T143220Z.swim_surface`. Native tests
measure a 2.144-yard rise both underwater and at the surface.
