# Legacy indoor water

Legacy indoor WMO water uses the original `XTextures/river/lake_a.1.blp` through
`lake_a.30.blp` sequence, repeated every four liquid grid tiles. The frame changes
every 60 milliseconds. This path adds the texture RGB to the WMO material's
`color2` tint and blends with alpha 0.9. It bypasses procedural refraction, foam,
reflection and wave shading. Other liquid paths retain their existing renderer.

The implementation follows the legacy texture/material path documented in
[WoW Map Viewer's liquid renderer](https://github.com/cleverca22/wowmapviewer/blob/master/src/liquid.cpp)
and [water shader](https://github.com/cleverca22/wowmapviewer/blob/master/bin/shaders/wateroutdoor.fs).
These sources are independent implementations; their timing and opacity are
not proof of exact original-client parity. A saved native Mystic Ward screenshot
confirms the fine ripple appearance; exact timing and opacity remain unverified.

MLIQ's material index addresses MOMT. It is not a LiquidType ID. Resolve the
liquid type using the WMO root flags, group type and tile flags. Preserve authored
vertex heights. Group flag 0x2000 marks an interior; it does not hide water.
Loading a WMO over multiple frame budgets must resume its liquid groups rather
than discard it as an already-placed duplicate.

Animation descriptors are immutable and shared across water surfaces. Choosing
another frame binds another descriptor, avoiding writes to descriptors still in
use by the GPU. Texture mips and repeat filtering come from the BLP upload and
texture sampler. Missing assets or failed uploads abort initialization explicitly.
