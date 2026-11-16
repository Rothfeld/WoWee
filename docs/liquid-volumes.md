# Liquid volume queries

Water surface coverage alone does not establish that a point is underwater.
Exodar lies below ocean surfaces: its dry rooms must remain separate volumes.

`WaterRenderer::getNearestWaterHeightAt` rejects surfaces separated from the
query point by solid terrain or a WMO ceiling. `Renderer` installs the geometry
query when creating the water renderer. Both character swimming and camera
underwater effects use the filtered query. Surface-only queries remain useful
for drawing water and do not establish immersion.

Live verification location: map 530, (-4134.16, -11450.63, -130.65), Vault of
Lights. The character remains grounded with swimming false and no underwater
tint. Dedicated Edrik/Edric sessions provide this check without moving shared
comparison characters.
