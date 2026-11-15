# TBC interface compatibility

The 2.4.3 interface uses the original extracted FrameXML and DBC data.
Keep the expansion's dbc_layouts.json and update_fields.json synchronized with
Data/expansions/tbc when changing data readers; extraction copies these files.

## Fonts and tooltips

Original build 8606 renders requested font sizes above 32 at size 32, while
GetFont returns the requested size. Text measurement and rendering share the
limit. Tooltip header and body regions retain their separate font heights.

Spell tooltip values come from Spell.dbc, SpellRange.dbc and current unit fields.
Ranged attack time is integer milliseconds on the wire. CMaNGOS stores it as a
float internally but converts it in Object.cpp before sending update fields.
Tooltip FontString text and colors refer to the same lines that the renderer draws.
SetPlayerBuff uses Spell.dbc's Tooltip field, which describes the active aura;
Description describes the castable spell and can contain different text.

## Aura packets

TBC has 40 positive and 16 negative visible aura slots. Its aura flags carry
effect bits; bit 0x02 does not classify a debuff. Classification uses the slot.
GetPlayerBuff and its related TBC UI methods expose one-based slots.

SMSG_INIT_EXTRA_AURA_INFO contains a packed GUID followed by zero or more
13-byte records. SMSG_SET_EXTRA_AURA_INFO and its NEED_UPDATE variant contain
one record. Each record is uint8 slot, uint32 spell ID, int32 maximum duration,
uint32 remaining duration. There is no count, effect index or flag byte.

Extra-aura metadata augments known object-field auras. It does not erase auras
omitted from the metadata packet. Duration can arrive before the spell ID;
empty-slot timing is preserved until the corresponding object fields arrive.
Unrelated field updates preserve a continuing aura's timer.
Server-only helper spells absent from Spell.dbc stay out of UI aura lists.
For example, CMaNGOS applies 20612 with Stoneform 20594; only 20594 has a
client display entry.

Food and drink restoration are aura-driven animation and sound states. They
do not emit spell-channel events or appear in UnitCastingInfo/UnitChannelInfo.
Moving still cancels restoration; actual timed casts and channels retain their
separate state. Animation classification reads the active restoration spell
identity independently of the current cast ID, which remains zero while eating.
EmoteEat runs on SpineLow and its descendants with an independent timeline;
Sitting supplies the pelvis and legs. Food ending removes the upper-body layer.

## FrameXML behavior

Missing optional API globals remain nil. WOWEE_LUA_API_FALLBACK=1 explicitly
enables diagnostic placeholders; it is not the normal execution mode.
Saved variables registered through RegisterForSave and RegisterForSavePerCharacter
are restored before login events and persisted at shutdown.

The world map uses ToggleWorldMap and the binding manager's actual command
lookup. FrameXML supplies map controls and labels. Zone bounds drive player
marker hit testing. Overlay hit rectangles are top/left/bottom/right coordinates
in the visible 1002x668 map, rather than the padded 1024x768 texture.
Unexplored overlay labels remain hidden.

Spellbook button names do not encode their slot IDs: SpellButton1 through
SpellButton4 have XML IDs 1, 7, 2, 8. Tab refresh checks must use GetID().
The General tab uses Ability_Kick. Internal/item-only/profession recipe entries
are filtered from the player spellbook while remaining available to gameplay.

## Settings and verification

Status Text is off by default in TBC; values appear on hover. Buff duration text
below icons is controlled by buffDurations and defaults off in stock FrameXML.
The tooltip countdown does not depend on that setting. Saved preferences win.
Automatic idle camera orbit is opt-in.

For UI comparisons, retain original-client screenshots as a baseline and capture
changed WoWee behavior. Match resolution, UI scale and settings, and account for
character inventory, progression and addons. Use separate live checks for hover,
casting and timer expiration; successful Lua loading alone does not verify pixels.

Local cast checks cover passive spells, death, combat restrictions, stealth,
combo points and available power. The server remains authoritative for complete
spell eligibility. Other expansion interfaces require their own comparison runs.

## Trade and depleted action items

TBC trade update headers contain client/server offer revisions. Item records run
to packet end; revisions are not counts. CMSG_ACCEPT_TRADE echoes the peer's
server revision. GetTradeTargetItemInfo returns quality before isUsable;
GetTradePlayerItemInfo does not. During trade, the NPC unit token resolves to
the trading player even when a vendor remains selected.

Item actions retain their identity at zero inventory count. Metadata is requested
independently of bag contents and matching actions refresh when it arrives.
Original FrameXML supplies the dim icon and zero count for depleted consumables.

Inventory rebuilds notify item action slots after the new bag contents are
available. ActionButton does not listen for BAG_UPDATE; slot fields can arrive
before item objects, so an earlier player-field notification is insufficient.

## Combo points and online creature scale

TBC SMSG_UPDATE_COMBO_POINTS carries a packed target GUID followed by one byte
of points (CMaNGOS Player::SendComboPoints). GetComboPoints() reports the stored
points only when the selected target matches that GUID. Original ComboFrame
handles PLAYER_COMBO_POINTS and PLAYER_TARGET_CHANGED.

Online creature size combines display/model size with CreatureFamily's
level-dependent size. Wild creatures use the larger of these; pets use family
size. OBJECT_FIELD_SCALE_X multiplies the result. Creature query metadata may
arrive after spawning, so live instance scale is refreshed as metadata and
levels change.

Original build 8606 routine 0x60bc70, executed with installed DBC records, gives
Ragged Young Wolf (entry 705, display 855, family 1) model scale 0.7 at level 1
and approximately 0.705084741 at level 2. Display scale alone is 0.4; object
scale alone is 1.0. Neither alone gives the native size.

Reference: https://gist.github.com/robinsch/6ab8e96227b991db1a7c572aa77f5779
The published reconstruction identified the family rule; local executable
execution verifies it for build 8606. Model scaling for humanoid display extras
and other client builds needs separate validation.

## Creature walk/run state

Creature gait comes from movement-block WALKING and explicit
SMSG_SPLINE_MOVE_SET_WALK_MODE / SET_RUN_MODE messages. MonsterMove path flags
must not replace that state: CMaNGOS TBC packet_builder.cpp deliberately sets
Runmode (0x100) even on walking paths. The same rule applies to transport paths.
Source: mangos-tbc/src/game/Movement/packet_builder.cpp, WriteCommonMonsterMovePart;
mangos-tbc/src/game/Entities/Creature.cpp, Creature::SetWalk.

### Creature locomotion cadence and stopping

Creature Walk/Run clocks use authoritative path length divided by duration,
then divide by the M2 sequence's `movingSpeed` and rendered instance scale.
Ground projection does not feed this clock. Global sequences and stationary
animations keep real-time clocks. For `creature/troglodyte/troglodyte.m2`, Walk
is 900 ms at 2.5 yards/s; display 606 has scale 0.55. At 2.5 yards/s this means
1.81818 playback rate and a 495 ms stride cycle.

Stand/Walk/Run transitions use the incoming sequence's authored `blendTime`
(150 ms for the trogg). Translation and scale blend linearly with smoothstep
weight; rotation uses shortest-arc slerp. The outgoing sequence continues at
its previous playback rate during the transition. One-shot chains and
independent arm/upper-body layers keep their existing ownership.

Format reference: https://skarndev.github.io/wowlib/python/m2/records/
Cross-fade mechanism reference (1.12.1, not a TBC binary proof):
https://github.com/samwhosung/wow-1121-client-internals/blob/main/docs/animation.md

Validation: production renderer probe loads original trogg/wolf M2 files and
checks clocks, global-time independence, pose continuity at stop, blend expiry,
and death transition exclusion. Workspace experiments named `trogg_stride_rate`,
`locomotion_stop_blend`, and `trogg_stride_stop` hold probe and paired footage.
Exact TBC transition easing remains a visual comparison, not binary-verified.

### Creature ground tilt

Grounded creature orientation honors M2 global flags `0x1` (slope along local X)
and `0x2` (slope along local Y). The original TBC wolf and boar models carry
`0x1`; trogg and dwarf models carry zero. These are M2 header flags, not
CreatureModelData flags. Flag reference:
https://skarndev.github.io/wowlib/python/m2/root/

The local +X forward axis pitches around -Y on an uphill slope. Floor probes
span the inner half of the scaled collision footprint and use the same floor
arbitration as creature placement, including WMO/M2 support. Unknown support
leaves that axis upright. Flying, swimming and transport-attached creatures
skip ground alignment. Probe spacing is an implementation choice; exact native
sampling distances and tilt smoothing have not been reverse-engineered.

The `creature_ground_tilt` experiment loads original models and checks rendered
forward vectors on uphill/downhill/flat planes at four headings, flags-disabled
models, and missing floor samples. Paired clips preserve visual comparisons.

### World map ownership and comparison

Blizzard FrameXML draws the world map tiles, explored overlays, landmarks,
party/corpse textures, controls and hover highlights. The embedded renderer
supplies its player arrow without compositing another map or custom zone labels
above the interface. The separate map window retains its own renderer.
Weather draws behind interface widgets.

Top-level XML frames without a declared or inherited parent remain parentless.
This matters for DropDownList1: fullscreen ShowUIPanel hides UIParent, so
implicitly parenting dropdowns there makes an open dropdown invisible.

GetMapOverlayInfo exposes only overlays revealed by server exploration bits.
Continent hover resolves ZMP subareas through AreaTable's parent chain, including
coastal water without an exploration overlay. UpdateMapHighlight supplies the
zone's original highlight texture, cropped to its aspect ratio, and the
WorldMapArea rectangle projected into continent coordinates. Native 2.4.3 probe
at (0.52, 0.43) on Eastern Kingdoms returns Hinterlands with texture UV
(1, 0.6640625), dimensions (0.09449898, 0.09453761), and offset
(0.48469314, 0.35763493). Coastal (0.4057508, 0.6964856) resolves to Westfall.

TBC's 32-pixel font rasterization cap applies after effective UI scale.
Measurement and rendering use the same physical cap; ordinary small labels
still scale normally. The world-map player marker uses the 16-unit FrameXML
marker footprint and scales with the detail frame. It remains a textured quad;
native uses MinimapArrow.m2 plus an effect model, so full 3D/glow parity is not
established by the size correction.

Stock TBC has no automatic quest-giver/objective world-map pins. QuestHelper
adds them. The workspace original client has QuestHelper disabled and moved
outside Interface/AddOns; comparisons use that addon-free reference. NPC and
minimap quest markers remain separate from world-map quest pins.

Evidence: workspace experiment `20261115T094832.882389Z.world_map_completion`,
including paired zone/continent hover captures and native API probes.

Minimap terrain renders opaque. The custom ImGui panel opacity setting does not
fade its terrain; otherwise a saved `ui_opacity=65` leaks the world scene through
an otherwise opaque FrameXML minimap border.

The in-game default cursor uses `Interface/Cursor/Point.blp` over empty world
space and FrameXML. Contextual world cursors take precedence. Software cursor
rendering stops while SDL relative mouse mode is active or mouse focus is absent.

Attack mode stays armed outside melee range. Local attack intent drives the
action-button checked state, combat ready pose, and selected attack target's
ground-circle pulse. Server swing-error packets report range; explicit stop,
target death, or server attack stop ends attack mode. No local distance timer
cancels or periodically resends attack commands.

The ground-circle pulse approximates the roughly one-second pink/yellow cycle
observed in build 8606 footage; exact native color curve is not established.
AppearanceComposer owns weapon attachment changes and requests draw animations
through ActivityFSM. Combat stance transitions do not draw or sheath again.
Explicit draw animation completes over combat idle; attack animations retain
priority. Regression test: `test_character_animator`.

Clearing selection (including Escape) sends attack stop and zero selection to
the server. Attack-button toggling and stop-on-target-change use local attack
intent, including the interval before server swing confirmation. Cancelling
attack does not require a currently selected target.

CMaNGOS emits melee range errors only when its swing error changes. The client
repeats the active range warning at the UI warning interval until a successful
swing, a different swing error, or attack cancellation clears it. This does
not resend attacks or cancel attack intent. TBC melee uses ERR_BADATTACKPOS
("You are too far away!"). MessageFrame text and shadows honor justifyH,
including UIErrorsFrame's CENTER and chat frames' LEFT alignment.

Standing, locomotion, and weapon-ready poses cross-fade using the destination
M2 sequence's blendTime. DwarfMale TBC Stand and Ready1H both specify 150 ms.
This skeletal transition does not request a draw/sheath animation. One-shot
attack and activity chains retain their own completion handling.

Local-player ground locomotion supplies the movement controller's selected
speed to the renderer. Walk, Run, and WalkBackwards playback uses movement
speed divided by abs(M2 movingSpeed) and model scale. Negative movingSpeed
encodes a backwards stride, not a disabled speed track. DwarfMale's backwards
sequence has duration 800 ms and movingSpeed -2.5; at 4.5 yards/s its playback
rate is 1.8, giving a roughly 444 ms cycle. Diagonal input uses normalized
movement speed.

Selection ground artwork uses source-alpha additive blending. This preserves
the ground brightness beneath its glow; ordinary alpha blending turns the
unflagged player ring dark violet despite its pale blue tint. Depth testing
and terrain fitting remain enabled. Visual evidence: workspace experiment
20261115T132046.645965Z.selection_ground_blend.
