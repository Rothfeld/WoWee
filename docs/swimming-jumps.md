# Swimming jumps

Space applies one upward impulse per press while swimming. Its launch speed is
9.1 yards/second. With gravity 19.29 yards/second squared, the rise is about 2.14
yards. Below the surface, ascent ends at the apex and swimming resumes. Crossing
the surface continues the ballistic arc until the character returns to swimming
height or lands. Holding Space does not generate repeated impulses.

The airborne state persists across physics substeps even while feet remain below
the water plane. Reset and teleport clear it. Water exit setup runs once when
leaving the current swimming state, rather than once per substep using the
previous render frame's state.

Downward swimming uses the server swim speed. Movement jump packets carry the
water launch speed and swim horizontal speed, and clear the swimming flag.

Native position samples, bounded paired input, screenshots, and WoWee movement
traces are in workspace experiment `20261116T143220Z.swim_surface`. Native tests
measure a 2.144-yard rise both underwater and at the surface.
