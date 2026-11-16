# WMO lighting and face orientation

WMO group visibility and lighting are separate. MOGP flag `0x2000` marks an
interior group; `0x40` disables local lighting for that group. Stormwind street
and canal groups combine both flags. `pipeline::wmoUsesLocalLighting` applies
this override to WMO materials, doodad placement colors, and liquid lighting.
Interior portal visibility continues to use the visibility flags.

Examples from original TBC assets:

| Group | Flags | Lighting |
| --- | --- | --- |
| Stormwind thief01 | 0x2a41 | Exterior |
| Stormwind canal505 | 0xb841 | Exterior |
| Stormwind AuctionHouse | 0x2a05 | Local |
| Ironforge interior groups | 0x2805 | Local |

Flag definitions are also recorded in
[pywowlib's MOGP flags](https://github.com/wowdev/pywowlib/blob/master/file_formats/wmo_format_group.py).

The main WMO, M2 and character pipelines declare counter-clockwise front faces
under the camera's Vulkan projection. This matters even when culling is off:
fragment shaders use `gl_FrontFacing` to orient two-sided lighting normals.

TBC uses authored textures, vertex colors and environment lighting without
the extra point lights synthesized from fixture names, glow meshes or lava.
Those synthetic lights are not authored light records.

TBC window surfaces also retain their authored MOMT blend modes and normal
material shading. Texture names do not enable the synthetic glass pipeline,
its Fresnel reflections, or its view-dependent opacity. For example,
`MM_STRMWND_WINDOWS_01` is painted window artwork, not a request for transparency.
The ray-tracing geometry uses the same rule: opaque windows remain occluders.

TBC specular highlights follow the MOMT shader variant. Diffuse stone, paving,
wood, and cloth do not receive the shared pipeline's specular term. Batch merging
keeps diffuse and specular materials separate even when they share a texture.
The Specular variant (shader 1) uses texture alpha as its highlight mask.
For example, `MM_STREET_02.BLP` has mean alpha 19.75/255; treating its mask as
fully white produces broad orange highlights across the street.
Name-based lamp/clock emission boosts and flicker are disabled for TBC as well;
these surfaces use their authored material shading.

Comparison artifacts and asset inspection script:
`_experiments/20261116T094629Z.stormwind_shading` in the parent workspace.
Run `python _experiments/20261116T094629Z.stormwind_shading/inspect_assets.py`
there to inspect group flags, doodad references and authored triangle winding.
