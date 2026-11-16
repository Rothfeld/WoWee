# Authored water lighting

`LightingManager` samples river and ocean shallow/deep colors from
LightIntBand channels 16/17 and 14/15 respectively. Both time-of-day sampling
and spatial light-volume blending apply to all four colors. LightParams supplies
separate shallow/deep alpha pairs; they blend with the same light-volume weights
and temporal smoothing as the colors.

Original TBC LightParams.dbc contains nine 32-bit fields. Fields 4 through 8
are Glow, WaterShallowAlpha, WaterDeepAlpha, OceanShallowAlpha, OceanDeepAlpha.
Field 3 is CloudTypeID. The original table has 582 records. Darkshore volume
197 uses clear profile 204: glow 1, river alpha (0.5, 1), ocean alpha (0.75, 1).

The [MaNGOS TBC schema](https://www.getmangos.eu/wiki/referenceinfo/dbcfiles/mangosonedbc/LightParams-r7666/)
matches these offsets. WoWDBDefs' legacy LightParams definition currently omits
CloudTypeID and appends Flags. Applying that definition shifts the floats and
interprets the final 1.0 alpha as integer flags 1065353216. Check original table
data before changing these offsets.

LightIntBand stores packed color values and half-minute time keys. Darkshore
profile 204 has different noon river/ocean shallow values (82 versus 86), while
both deep colors are 60. This distinction must survive sampling and blending.
