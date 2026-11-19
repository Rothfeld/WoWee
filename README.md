# WoWee

<p align="center">
  <img src="assets/Wowee.png" alt="WoWee logo" width="240" />
</p>

<p align="center">
  A native C++ World of Warcraft client with a custom Vulkan renderer.
</p>

<p align="center">
  <a href="https://github.com/sponsors/Kelsidavis"><img src="https://img.shields.io/github/sponsors/Kelsidavis?label=Sponsor&logo=GitHub" alt="Sponsor" /></a>
  <a href="https://discord.gg/PSdMPS8uje"><img src="https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&logoColor=white" alt="Join the WoWee Discord" /></a>
</p>

WoWee is a complete replacement for the World of Warcraft client: a native C++
program with its own Vulkan renderer, networking and audio, that logs into
**Vanilla 1.12**, **TBC 2.4.3**, **WotLK 3.3.5a** and **Turtle WoW 1.18** servers
and plays the game from your own game data. It runs Blizzard's own interface and
your addons, and it answers the servers' anti-cheat. It is played on
AzerothCore/ChromieCraft, TrinityCore, CMaNGOS/vmangos and Turtle WoW, on Linux,
macOS, Windows and Android.

[Watch the latest demonstration](https://youtu.be/1Ax1jeNV_GU) ·
[Download the latest release](https://github.com/Kelsidavis/WoWee/releases/latest) ·
[Read the project status](docs/status.md) ·
[Hear the soundtrack](https://kelsidavis.bandcamp.com/album/wowee-soundtrack)

TBC rendering notes: [meeting stone effects and validation](docs/meeting-stone-effects.md).
Interface textures honor `SetDesaturated` using a separate grayscale texture
variant; atlas UVs and alpha stay unchanged. `GameTooltip:SetText("")` hides
the tooltip, while `AddLine("")` remains a valid spacer.
MCLQ water height checks use vertices belonging to visible cells. Dry vertices
can contain `FLT_MAX`; they must not flatten a wet chunk or overwrite a wet
neighbor's shared edge during merging.
Spline particle trails retain their animated emitter's birth pose in model
space. Reapplying the current bone to every live particle collapses Draenei
holographic rune trails into bright bulbs.
Sandstorms are SMSG_WEATHER states 22, 41 and 42 (Weather.dbc EffectType 3,
no EffectTexture). `rendering::SandStorm` draws them as 2.4.3 does: point
grains moved by the game's own `Shaders\Vertex\sand.bls` program (linear
path, 0.2 s fade in and out, distance-sized points) and
`Textures\Weather\WeatherMistGrainy01.blp` puffs, both in the fog colour and
streaming at the camera from where it looks. Ruins of Ahn'Qiraj gets one when
Ossirian sets heavy storm weather; Silithus, Tanaris and the AQ zones roll it
from `game_weather`. `.wchange 3 1` forces it. Gotchas: the ruins are outdoor
WMO groups, so only indoor groups switch it off; 239 LightIntBand and 41
LightFloatBand rows store a key out of time order (storm LightParams 8 among
them) and are sorted at load. Measurements and which constants are fitted:
`_experiments/20261118T171000Z.aq20_sandstorm/` in the workspace.
A map with no Light.dbc light at the player and no default row of its own is
lit by Light 1, map 0's default row, on the world clock. Hyjal (534) has no
Light row at all; native shows it at night at 21:47, with Light 1's sky top
and green river water (params 12), while Kalimdor's default (params 192, via
Map.dbc's continent column) has navy river water. Other maps without rows are
WMO interiors (Gruul, Magtheridon, Coilfang, MC, BRD, ...) and the Outland
arenas. Evidence: `_experiments/20261118T214906Z.hyjal_sky/` in the workspace.
Matrix-created WMO doodad particles use authored placement scale for sprite
width. See [authored particle motion](docs/authored-particles.md).
An M2 with emitters and no vertices is a whole model (`M2Model::hasRenderableContent`);
`isValid()` (triangles) is only for collision and picking. 463 TBC models are
emitters only, 62 of them placed by ADTs, WMOs or game objects: Blaze 181832
(`StratholmeFireSmokeEmberM.m2`, Magtheridon's floor fires), Roaring Flame
182260 (`LowPolyFire.m2`, 110 burning houses at Hyjal's Alliance Base),
`Hellfire_FireWall_01.m2`, `blacksmith_smoke.m2`. Game objects, ADT and WMO
doodads and transport doodads all take them; they were dropped as invalid.
Evidence: `_experiments/20261118T213500Z.mag_fires/` in the workspace.
Ground clutter uses expansion-specific DBC layouts. Vanilla/TBC
`GroundEffectTexture.dbc` has seven columns: ID, four doodad IDs, density,
terrain type. Its doodad slots have equal weights; `0xffffffff` means empty.
Wrath has eleven columns with four explicit weights. Vanilla/TBC
`GroundEffectDoodad.dbc` stores the model path in column 2 (zero-based),
while Wrath stores it in column 1. Reading the Wrath offsets on TBC yields
zero density and malformed model paths. Validate both tables together when
changing terrain detail rendering. TBC model references use `DoodadIdTag`
(column 1, zero is valid), rather than the DBC row ID. Placement uses authored
models only; it does not substitute Elwynn grass for missing data or empty tiles.
Detail paths pass through `modelPathToM2`: TBC uses both `.mdl` and `.mdx`
names for shipped `.m2` files. Placements use MCNK's 8x8 layer and suppression
maps, including terrain holes. Cached placements become live instances only
near the camera, within the ground-detail distance plus patch bounds. Models
stay pinned while their terrain tiles reference them, and unpin on unload.
`DoodadIdTag` is confirmed by data and by native: Nagrand's grass effect 23807
names tags 464-483, which are not row IDs, and 2.4.3 grows grass there. Over
all ADTs, tag lookup resolves every used effect; row-ID lookup misses 21% of
growing cells. The 2-bit MCNK layer map is read low bits first, y-major: it
agrees with the dominant alpha layer in 96% of cells (MSB-first: 66%).
2.4.3 lights clutter as the ground it stands on and blends it by texture
alpha. Detail quads have horizontal normals; lighting those left every tuft
facing away from the sun ambient-only blue-grey (Beezil's Wreck), and the
224 alpha key thinned mipmapped fronds to specks (Hyjal Summit's HyjGra
ferns). Native blade edges are soft and see-through near the camera while its
alpha-key tree branches stay hard, so clutter alone skips the key.
Clutter is what the scatterer places (`classifyM2Model(..., groundEffect)`,
its own model ids), not what sits in a `Detail` folder: 61 models there are
ADT/WMO doodads (1184 MMDX references; `StranglePlant08/10`,
`StranglethornFern05`, `ElwynnRock1/2`) and ADTs place 36
`World\NoDXT\Detail` models directly. Native draws them with their authored
materials; StranglePlant10's alpha-key fronds by Stranglethorn's big trees
stay hard and opaque. Taken as clutter they were blended with depth written
in the solid pass, before what stands behind them, so the clear/sky colour
showed as a cyan rim round every frond. Evidence:
`_experiments/20261119T083411Z.foliage_alpha/` in the workspace.
Evidence: `_experiments/20261118T210330Z.ground_effects/` in the workspace.
Blended clutter writes depth, so overlapping tufts depend on draw order: a
faint blade fringe drawn first hides the blades behind it. M2Renderer draws
clutter after every other opaque model, far to near; grouped only by model,
the order changed with each change of the visible set and grass blinked while
the camera turned (Stranglethorn). M2 visibility is culled on the CPU against
the current frame's camera. The former GPU cull compute was read back two
frames late (frames in flight) and dropped small models entering view during
turns; its HiZ occlusion pyramid was never built. Evidence:
`_experiments/20261119T083457Z.clutter_flicker/` in the workspace.
World M2 and character opaque/alpha-key batches complete before blending.
Their blended meshes share one camera-depth-ordered stream, far to near:
crystal surface layers and moth wings must sort across renderer boundaries.
One worker records both streams; inline recording uses the same merge.
Particles follow the mesh passes. Particle headers use model version, not
expansion: `BubblingBowl01.m2`
(260) has 16-bit blend/emitter fields, while `DR_Cookpot_01.m2` (263) packs
them into bytes. Both ship in TBC. Pre-264 head/tail (0 head, 1 tail, 2 both)
is byte `0x2c`; `0x2d` is zero in every TBC emitter. Wrath uses `0x2d`.
Billboard bones require per-frame updates
even without animation tracks, and cannot reuse another instance's camera-facing
pose. Silvermyst crystal glow cards are an example. Character models
and the items attached to them (helms, shoulders, weapons) turn their
billboard bones toward the camera too, in the frame the wearer places them
in, and a billboard bone's children ride the turned frame
(`rendering::billboardBoneMatrix`, shared with M2 doodads). The raid warlock
helm on High Astromancer Solarian (18805,
`Helm_Robe_RaidWarlock_D_01_BEF.m2`) hangs its black smoke 0.43 yd along -X
of billboard bone 1: native keeps it behind her torso seen from front and side; left
in the helm's authored frame it trailed behind her back, out beside her when
seen from the side or while casting
(`_experiments/20261118T124000Z.astromancer_glow/` in the workspace). Visible
character and M2 bones update every frame; distance/frustum culling still
limits work. Units are drawn and animated out to the view distance.
Names over units (build 8606) are world text: their size follows perspective
(projected 0.2 yd line, larger only over name attachments above 4 yd), with no
distance fade and no distance cut of their own; a human's name is a few
pixels at 120 yd and gone at 190 yd. A corpse keeps its name, grey, at the height it had
standing, so corpses at the camera's feet print large grey names across the
upper screen in both clients (Sun's Reach Harbor). Which units are named:
`ui::tbcUnitNameCVar`. 2.4.3 has eleven name CVars and none of Wrath's totem,
guardian or critter ones: a player's totems and guardians follow
UnitName*CreationName, critters and every NPC's summons UnitNameNPC, whose
default is 0. Evidence: `_experiments/20261119T093137Z.far_unit_names/` in the
workspace.
Remote NPCs and players consume `UNIT_FIELD_BYTES_1` stand states. Persistent
aura visuals use `SpellVisual.StateKit` and its authored attachment slots;
their lifetime follows aura fields and owner existence, not cast duration.
For example, Draenei Survivor (16483) sleeps (state 3) under spell 35046,
visual 8270, kit 6608, `Spells/RedGhost_state.m2`.
State kits also change the unit's own model through `CharProc`: type 1
multiplies the shaded model by `CharParamZero` (an `0xRRGGBB` integer stored
as a float), type 14 sets its opacity to `CharParamZero`. The latest applied
aura's kit wins for each; removing it restores the previous one. A ghost is
Ghost (8326), visual 886, kit 989: colour `0x8CB9FD`, opacity 0.5. Vertex Color
Black (39662) draws a black silhouette, Transparency (75%) (37802) opacity 0.25.
The colour changes in one frame and stays for the aura's lifetime;
`CharParamOne`-`Three` do not modulate it (original 2.4.3: Brood Affliction
Blue/Red, Ignite Flesh, Enrage, also during a Chromaggus fight). Opacity moves
to the new value over 1 s as the cube of elapsed time, both ways, whatever the
kit's parameters (Transparency 25%/75%, Steal Magic, Banish).
Placed doodads, banners, boats and ground clutter move only by their own keyed
bones: no shader wind, cloth sway, boat bob or player push-aside, whatever the name. `StranglethornTree02` and `BadlandsPalmTree02` sway their canopies from keyed
bones; `StranglethornRoot01`-`03`, Elwynn bushes and trees, the palm trunks and the
unkeyed banners (`dr_banner01` at the Temple of Telhamat) stand still in 2.4.3 and
here. Native grass tufts stand upright through a walking character's legs.
Nothing about collision, fade, cutouts or shadows is read from a model's name.
Collision is the authored collision triangles alone (`ElwynnBush01` has none and is
walked through; `ElwynnTreeMid01` has 12 round its trunk). A batch is cut out by its
material: alpha-key batches with texture alpha, and opaque batches whose texture
alpha is a silhouette (`alphaIsSilhouette`: DXT1 punch-through, e.g. the
`MullgoreThorn01`-`07` arches, which native cuts along the ragged edge). The shadow
pass casts each batch with the same coverage. Distance fade starts at one fraction
of the draw distance for every model except `disableAnimation` (chest) models.
Gotcha: the M2 draw distances themselves (`M2_MAX_RENDER_DISTANCE_*`, radius/12
factor) are WoWee constants, not measured against native.
Evidence: `_experiments/20261119T083245Z.stv_root_anim/` and
`_experiments/20261119T085643Z.doodad_motion/` in the workspace.
M2 texture caching: GPU entries include WrapS/WrapT flags as well as normalized
asset path, because each `VkTexture` owns its sampler. `barrel01.m2` clamps
`barrel04dark2.blp`, while `barrellowpoly.m2` repeats that same texture with U
coordinates reaching 2.915. Sharing their sampler stretches edge pixels across
the repeated barrel faces. File decoding remains keyed by asset path.
TBC group finder: [LFG/LFM protocol and validation](docs/tbc-lfg.md).
Party frames and map transfers: [protocol and input lifecycle](docs/party-stats-and-map-input.md).
Interior minimaps: [authored room tiles and validation](docs/interior-minimap.md).
Minimap blips, rim arrows and tooltips: [original rules](docs/minimap-markers.md).
PvP menu: [preference, active flag, and validation](docs/pvp-menu.md).

<p align="center">
  <img src="assets/hellfire-peninsula.png" alt="WoWee rendering Honor Hold in Hellfire Peninsula" width="100%" />
</p>

> [!NOTE]
> macOS release DMGs are Developer ID signed, notarized by Apple, and stapled
> before publication. Gatekeeper should identify them as notarized Developer ID
> software without requiring an **Open Anyway** exception.

> [!IMPORTANT]
> WoWee is an educational and research project. It contains no Blizzard
> Entertainment assets, data, or proprietary code. You must supply your own
> legally obtained game data and comply with the laws in your jurisdiction.
> WoWee is not affiliated with or endorsed by Blizzard Entertainment.

## What works

**The game.** Character creation and selection, movement and swimming, flying
and mounts, combat, spells and auras, talents, pets, quests and their map
objectives, loot and roll windows, inventory, bags, the bank, vendors, trainers,
the auction house, mail, trade, gossip, chat and channels, parties and raids,
guilds, the Dungeon Finder, reputation, equipment sets, achievements, the world
and flight maps, taxis, boats, zeppelins and lifts, and instances. Each
expansion is spoken in its own wire format, from expansion profiles and packet
parsers.

**The interface.** Blizzard's FrameXML, loaded from your game data and drawn by
the client: the same frames, Lua and events the original client runs, with a
Lua API wide enough for the stock interface of every expansion. Addons in the
game's `Interface\AddOns` folder load as they would there. Key bindings are the
game's own panel, with two keys per command, and the client's settings sit in
the game's Video, Sound and Interface panels.

**The world.** A Vulkan 1.3 renderer for terrain, buildings, models and
characters, water, lava and slime, particles and spell effects, shadows,
weather, the day and night sky, volumetric fog and sun shafts, GPU grass, and
asynchronous world streaming. FSR 1 and FSR 3 upscaling are built in.

**The sound.** Zone and city music, tavern and weather ambience, footsteps,
mounts, combat, spell, interface and NPC voice audio, from your game data, and
[a soundtrack of its own](#soundtrack).

**Logging in.** SRP6 authentication, the realm list and header encryption for
every expansion, the login hashes servers check, and Warden: the module the
server sends is verified and run under x86 emulation, and its checks are
answered as a genuine client answers them. The answers are graded against
AzerothCore's own check list in the test suite.

**Your game data.** An asset builder, in the client and as the standalone
`wowee_assets` window, extracts your own installation - several games into one
folder if you like - and can borrow upgraded art from a later client you own.

**Input.** Keyboard and mouse, mouse buttons 4 and 5, or a game controller on
every platform, with buttons named as the pad in hand names them (Xbox,
PlayStation, Nintendo, Steam Deck) and its back paddles on action slots.

**Platforms.** Linux, macOS and Windows on x86-64 and ARM64, and Android on
arm64 with on-screen controls.

### Experimental components

The world editor, ray traced lighting and AMD FSR3 frame generation are early
developer features; their interfaces, formats and behavior may change. Frame
generation is only built when `WOWEE_ENABLE_AMD_FSR3_FRAMEGEN` is turned on
with AMD's SDK under `extern/`. Cataclysm 4.3.4 support is in progress: its
movement packets are done, and the client does not reach its world yet.

## Quick start

### 1. Install dependencies

The desktop client uses SDL3 and needs a Vulkan 1.3 driver (MoltenVK on
macOS). Unicorn enables Warden execution. StormLib is needed by
`asset_extract`, the `wowee_assets` window and the asset builder inside the
client; without it the client still builds, with those left out.

**Linux** needs no libraries and no compiler on your machine. The build runs in
a container and ends as an AppImage that carries its own libraries, so
[Docker](https://docs.docker.com/get-docker/) or Podman and a Vulkan 1.3 graphics
driver are all there is to install. The package lists for compiling natively on
Ubuntu, Debian, Fedora and Arch, which only development needs, are in the
[complete build guide](BUILD_INSTRUCTIONS.md).

<details>
<summary>macOS</summary>

Vulkan runs through MoltenVK.

```bash
brew install cmake pkg-config sdl3 glm openssl@3 zlib ffmpeg \
  vulkan-loader vulkan-headers molten-vk shaderc

# Optional: Warden execution and MPQ extraction
brew install unicorn stormlib
```

</details>

For Windows/MSYS2, Visual Studio, and platform-specific notes, see the
[complete build guide](BUILD_INSTRUCTIONS.md).

### 2. Clone and build

```bash
git clone --recurse-submodules https://github.com/Kelsidavis/WoWee.git
cd WoWee
```

**Linux.** The [releases](https://github.com/Kelsidavis/WoWee/releases/latest)
are AppImages, and the container builds the same:

```bash
./container/run-linux.sh        # on a Windows host: .\container\run-linux.ps1
```

It leaves `build/linux/wowee-<version>-<arch>.AppImage` and nothing else. The
first run builds the container image and compiles everything, which takes a
while; later runs compile only what changed.

**macOS and Windows:**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

With the default Makefile generator, `--parallel` without a number starts
every compile job at once, and the larger files need a lot of memory; that has run a
32 GB machine out of memory. Give it a limit, such as `cmake --build build -j2`,
or `-j4` with 16 GB or more.

The `build.sh` and `build.ps1` helpers run the same build and also clone AMD's
FidelityFX SDKs into `extern/`. Those are only used when `WOWEE_ENABLE_AMD_FSR2`
or `WOWEE_ENABLE_AMD_FSR3_FRAMEGEN` is turned on, and both are off by default;
the client's own FSR 1 and FSR 3 upscaling need neither.

### 3. Extract game data

WoWee does not read MPQs at runtime. It needs a loose-file tree with a
generated `manifest.json`, extracted from a legally obtained client.

The asset builder does this. When the client starts and finds nothing
extracted, it opens the builder instead of the login screen; later it is under
**more options** → **add or rebuild assets...** on the login screen, and it is
also the standalone `wowee_assets` window. Point it at your World of Warcraft
folder and it extracts into the per-user data directory the client reads.
Reopen the client when a build finishes. See [the asset manager](docs/asset-manager.md)
for upgrades, packs, and several games in one folder.

From a terminal, the scripts extract into `Data/` in the checkout:

```bash
# macOS
./extract_assets.sh /path/to/WoW/Data wotlk

# Windows PowerShell
.\extract_assets.ps1 "C:\Games\WoW-3.3.5a\Data" wotlk
```

The Linux AppImage carries the extractor and the asset manager. The manager
writes to the per-user data directory the client reads; point the extractor
there:

```bash
./wowee-*.AppImage --asset-manager
./wowee-*.AppImage --asset-extract --mpq-dir /path/to/WoW/Data \
  --output ~/.local/share/wowee/Data --expansion-subdir --expansion wotlk
```

Valid targets are `classic`, `turtle`, `tbc`, and `wotlk`. Each target is kept
separately, so multiple expansions can coexist:

```text
Data/
└── expansions/
    ├── classic/manifest.json
    ├── turtle/manifest.json
    ├── tbc/manifest.json
    └── wotlk/manifest.json
```

The login-screen **Assets** selector appears once more than one set is
installed. It normally follows the selected server's expansion and can be
overridden per saved server profile.

The client looks for data in this order:

1. `WOW_DATA_PATH`, if set
2. The per-user data directory, if it holds an extraction:
   `~/Library/Application Support/Wowee/Data` on macOS,
   `%LOCALAPPDATA%\Wowee\Data` on Windows, and `$XDG_DATA_HOME/wowee/Data`
   (default `~/.local/share/wowee/Data`) on Linux
3. `Data/` in the working directory, which on Linux and macOS is the
   executable's own directory. CMake links `build/bin/Data` to the checkout's
   `Data/` on macOS and in a native Linux build; on Windows `build.ps1` makes a
   junction. In the AppImage that directory is inside the image and holds only
   the expansion profiles, so extract into the per-user directory, as above

To store data elsewhere:

```bash
export WOW_DATA_PATH=/path/to/extracted/Data
```

### 4. Run

On Linux, run the AppImage you built or downloaded:

```bash
chmod +x wowee-*.AppImage
./wowee-*.AppImage
```

With no arguments it starts the client, and any arguments are passed on to the
client. `--asset-manager` and `--asset-extract` start the asset tools instead
(see step 3); they work only as the first argument.

An AppImage mounts itself through FUSE. Where there is none, such as inside a
container, run it with `--appimage-extract-and-run`. On macOS and Windows:

```bash
./build/bin/wowee
```

Choose a server from the login screen's **Server** list, which carries
ChromieCraft and every server you have logged into. **Somewhere else...** opens
the address and port fields under **more options**, where the expansion is
chosen too.

Once at startup the client asks GitHub whether a newer release exists and, if
so, shows its tag beside the version on the login screen. Nothing is
downloaded; **Check for new versions** under Interface → WoWee → Interface
turns it off.

If a MaNGOS realm advertises a world address that is unreachable from your LAN,
override only the host; the advertised port is preserved:

```bash
export WOWEE_REALM_HOST_OVERRIDE=192.168.1.50
```

Turtle WoW defaults to authentication build 7272. Older compatible servers can
use:

```bash
export WOWEE_TURTLE_AUTH_BUILD=7234
```

## Android

The Android client is the same tree built for arm64. It reaches the login
screen, character selection and the world, and is tested on a Pixel 9a.

Install `wowee-<version>-android-arm64.apk` from
[the latest release](https://github.com/Kelsidavis/WoWee/releases/latest). It
needs **Android 13 or newer** on **arm64**: Android's `libvulkan.so` only
exports the Vulkan 1.2/1.3 entry points the renderer uses from API 33. The APK
is signed with the debug key, so Android asks you to allow installation from an
unknown source.

### Game data on a phone

Extract on a desktop exactly as above. There is no need to extract on the
device. A full extraction is around 18 GB, so cut it down to a profile that
fits, then copy the result across. Open the app once before pushing, so Android
creates its folder:

```bash
tools/android/make_minimal_data.py --source ~/Data --out ~/Data-phone \
    --profile world --maps all
adb push ~/Data-phone/. /sdcard/Android/data/com.wowee.client/files/Data/
adb shell chmod -R a+rwX /sdcard/Android/data/com.wowee.client/files/Data
```

The subset also carries the extracted `WoW.exe` and its DLLs from
`expansions/*/misc`. Without them a server that runs Warden sees every memory
check from the phone fail, which some servers treat as a modified client.

The trailing `/.` matters: push the **contents**, not the directory. Nesting it
one level deeper leaves the client unable to find its `manifest.json`, and it
starts with no game data at all.

The `chmod` matters too. Files `adb push` writes into the app's folder belong to
the shell user, not the app, and their directories are closed to it; without
the `chmod` the client stops at startup with `Permission denied` on
`manifest.json`. Run it again after any later push.

| Profile | Size | Reaches |
|---|---|---|
| `login` | 787 MB | Login, character selection and creation. No world |
| `world --maps azeroth` | 7.5 GB | One continent |
| `world --maps all` | 12 GB | Every map |
| `full` | 18 GB | Everything, sound included |

A subset carries its own rewritten `manifest.json`, because that file is how the
client locates its data root. See [tools/android/README.md](tools/android/README.md)
for the profiles and how to check one.

### Touch controls

| Input | Action |
|---|---|
| Left thumb, lower left | Move and strafe |
| Right thumb, drag | Turn the view; the character faces where it looks |
| Two fingers | Zoom the camera |
| Tap | Target, interact, and everything in the interface |

A keyboard and mouse should work when attached, since SDL delivers them the same
way, but that is untested.

### Building it yourself

```bash
tools/build-android-deps.sh arm64-v8a          # OpenSSL, once
cd android && ./gradlew :app:assembleDebug     # needs ANDROID_HOME and NDK 28
```

Debug builds carry this project's own music; release builds leave it out, the
same split the desktop archives use. `adb logcat -s wowee` shows the client's
log, and `adb shell setprop debug.wowee.loglevel info` opens it up beyond
warnings.

## Container builds

Docker or Podman can build all supported targets without installing a host
toolchain:

```bash
./container/run-linux.sh    # build/linux/wowee-<version>-<arch>.AppImage
./container/run-macos.sh    # build/macos/bin/wowee
./container/run-windows.sh  # build/windows/bin/wowee.exe
```

PowerShell equivalents are included. The Linux build leaves only the AppImage;
its build tree stays in the Docker volume `wowee-linux-build`. macOS defaults to
ARM64; set `MACOS_ARCH=x86_64` for Intel. See
[Container Builds](container/README.md) for all options.

## Experimental world editor

`wowee_editor` is an early-stage tool for experimenting with custom zones and
open, JSON-friendly formats. It is not yet a production-ready content pipeline.

```bash
cmake --build build --target wowee_editor
./build/bin/wowee_editor --data Data

# Batch conversion examples
./build/bin/wowee_editor --convert-m2 Creature/Bear/Bear.m2 --data Data
./build/bin/wowee_editor --convert-wmo World/WMO/Stormwind/Stormwind.wmo --data Data
```

Current work includes terrain editing, object placement, water, NPCs, quests,
lighting, and experimental AzerothCore export. Exported zones can load from
`custom_zones/` or `output/`, but formats and compatibility may change.

See the [editor format specification](tools/editor/FORMAT_SPEC.md) for the
custom binary and catalog formats.

## Controls

| Input | Action |
|---|---|
| `W` `S` | Move forward and back |
| `A` `D` | Turn; strafe while the right mouse button is held |
| `Q` `E` | Strafe |
| Mouse | Look or orbit camera |
| Left click | Target or interact |
| `Tab` | Cycle targets |
| `1`–`0`, `-`, `=` | Action-bar slots 1–12 |
| `B` / `C` / `P` / `N` | Bags / character / spellbook / talents |
| `L` / `M` / `O` / `H` | Quest log / map / social / player vs player |
| `Enter` or `/` | Open chat |
| `/unstuck` | Recover when terrain or WMO collision traps the character |
| `Escape` | Close windows, deselect, or open the game menu |
| `F1` | Performance HUD (debug builds only) |

Keys are listed and can be rebound in the game's **Key Bindings** panel, from
the menu `Escape` opens.

On Android the same actions are driven by
[touch controls](#touch-controls).

### Controller

A controller works on every platform the client runs on. The left stick moves,
the right stick looks and the triggers zoom. The bottom face button jumps and
the right one, like Start, acts as `Escape`. The other two face buttons and the
D-pad are action slots 1–6; hold the left bumper for 7–12. The right bumper
targets the nearest enemy, clicking the left stick toggles autorun, and clicking
the right stick sits (or dives while swimming). Back paddles - a Steam Deck's
L4, R4, L5 and R5, an Elite pad's P1 to P4, a DualSense Edge's - take slots
7–10 with nothing held.

**Back** switches the right stick to moving the pointer, for looting, gossip
and vendors: the bottom face button clicks and the left one right-clicks.

Buttons are named as the pad in hand names them - A, Cross or B for the same
button on Xbox, PlayStation and Nintendo pads - and the scheme is listed in the
**Key Bindings** panel, where it can be rebound. Look speed, inversion and
deadzone are under Interface → WoWee → Camera. A pad SDL does not recognise
can be described in a `gamecontrollerdb.txt` in the per-user data directory.

### Settings

Escape opens the game menu. This client's own settings sit under a **WoWee**
heading in the game's **Video**, **Sound** and **Interface** panels: Graphics,
Detail, Grass, Upscaling and Display under Video, with the quality preset on
Graphics. View distance, shadow distance and MSAA have the largest performance
cost; FSR 1 or FSR 3 upscaling can improve frame rate on weaker hardware.

## Soundtrack

The client carries music of its own: the login theme, the track taverns open
their rotation on, and zone rotations that play alongside whatever your game
data provides - Elwynn, the Barrens, Booty Bay, Lordaeron, Stormwind and
Ironforge among them. **WoWee soundtrack** under Sound → WoWee → Sound turns it
off and leaves the zone music to the game's own files.

<p align="center">
  <a href="https://kelsidavis.bandcamp.com/album/wowee-soundtrack">
    <img src="assets/wowee-soundtrack.png" alt="WoWee Soundtrack cover" width="320" />
  </a>
</p>

[**WoWee Soundtrack**](https://kelsidavis.bandcamp.com/album/wowee-soundtrack) -
19 tracks, 56 minutes, available in lossless FLAC. The tracks are original
compositions and contain no Blizzard audio.

The files live in `assets/Original Music/`. Build trees and the debug APK carry
them; release archives, AppImages and the release APK leave them out. The music
is not covered by the MIT license; see [LICENSE](LICENSE).

## Documentation

- [TBC interface behavior and data conventions](docs/tbc-interface.md)

### Getting started

- [Quick Start](docs/quickstart.md)
- [Complete Build Instructions](BUILD_INSTRUCTIONS.md)
- [Server Setup](docs/server-setup.md)
- [Project Status](docs/status.md)
- [Asset Manager](docs/asset-manager.md) and [Upgraded Assets](docs/upgraded-assets.md)

### Internals

- [Architecture](docs/architecture.md)
- [Authentication](docs/authentication.md) and [SRP Implementation](docs/srp-implementation.md)
- [Packet Framing](docs/packet-framing.md) and [Realm List](docs/realm-list.md)
- [Sky System](docs/SKY_SYSTEM.md)
- [Warden Quick Reference](docs/WARDEN_QUICK_REFERENCE.md) and [Implementation](docs/WARDEN_IMPLEMENTATION.md)

## Development and CI

WoWee uses C++20, CMake 3.15+, SDL3 and Vulkan 1.3. GitHub Actions builds
Linux x86-64/ARM64 AppImages, Windows x86-64/ARM64, macOS ARM64/x86-64 and Android
arm64 releases. Security checks include CodeQL, Semgrep, AddressSanitizer, and
UndefinedBehaviorSanitizer.

A native Linux build, with the packages listed in the
[build guide](BUILD_INSTRUCTIONS.md), is for development: it leaves
`build/bin/wowee`, and `container/appimage/build-appimage.sh build` turns that
build into an AppImage.

The codebase is split into focused rendering, networking, gameplay, asset, UI,
audio, and editor modules. Start with the [architecture guide](docs/architecture.md)
before making broad changes.

## Warden

Servers that run Warden read memory from the client's executable. The asset
builder caches each expansion's `WoW.exe` with its game data, in
`Data/expansions/<id>/misc`, and the client reads the one for the server being
played. Extract from the same client you would otherwise play with: an
executable of another build answers every memory check wrongly, which servers
treat as a modified client, and the client says so in its log.

A realm that signs its own Warden module needs its key set as
`wardenRsaModulus` in the expansion's `expansion.json`. To log in without
answering Warden at all, start the client with `WOWEE_WARDEN_ANSWER_HASH=0`:
the server then sends no checks, which most servers tolerate. See the
[Warden reference](docs/WARDEN_QUICK_REFERENCE.md).

## Known limitations

- The restored shadows toggle still needs live GPU validation. Turning shadows
  off disables sampling while retaining the shadow pass.
- Warden checks are graded against AzerothCore's list and build 5875 memory
  fixtures from CMaNGOS and vmangos. Set `WOWEE_TEST_CLASSIC_WOW_EXE` to a
  1.12.1 executable and run `test_warden_check_grader '[classic]'`. The supplied
  RetroWoW executable passes all 74 CMaNGOS checks and all 80 vmangos checks
  with the build 5875 runtime defaults initialized. These fixtures do not cover scripted scans
  or establish live realm compatibility.
  TBC build 8606 has 61 CMaNGOS memory fixtures: set `WOWEE_TEST_TBC_WOW_EXE`
  to a 2.4.3 executable and run `test_warden_check_grader '[tbc]'`. All 61 pass
  against the supplied enGB repack. Executables are not included in the repository.
  CMaNGOS TBC and WotLK scripted scans that locate system information and
  EndScene use addresses from CMaNGOS's source and have not been tested on a live realm.
- Where terrain meets a building, the character can occasionally take the wrong
  floor or stick. Enter `/unstuck` in chat to recover, press `F8` to write the
  floor data at your position to the log, and include the location and those
  log lines in a bug report.
- On Android the session drops a few seconds after the app goes to the
  background, because the system closes the socket of an app without a
  foreground service. The client returns to the login screen as it does on any
  disconnect.
- On a phone the interface starts at the largest UI Scale its frames still fit
  at, and is still small on a small screen. The client's own panels scale with
  display density, and **Window scale** under Interface → WoWee → Interface
  reaches 3x.

When reporting a bug, include the relevant client log lines, expansion, server
core, and reproduction steps.

## Automatic startup for development

Set `WOWEE_START_CHARACTER` to an exact character name to log in once using
the selected saved server profile and enter that character automatically.
The profile must already have remembered credentials. No password belongs in
the environment variable or command line. The normal realm-selection rules
still apply: a single unlocked realm is selected automatically; multiple
realms require selection.

The startup request runs once per process. Disconnecting or logging out does
not trigger another login. A missing saved login or a character name absent
from a nonempty character list fails with an explicit error. An empty list
remains on the character screen. Unset the variable for normal interactive
startup.

## License and references

WoWee source code is available under the [MIT License with an additional
restriction](LICENSE): it may not be used, in whole or in part, as the basis
for or a component of a commercial video game or other commercial game
product without written permission. Original music and audio assets are not
covered by the MIT terms at all and are reserved - see [LICENSE](LICENSE) and
[NOTICE](NOTICE). World of Warcraft and its assets are property of Blizzard
Entertainment, Inc.

- [WoWDev Wiki](https://wowdev.wiki/) - file-format documentation
- [TrinityCore](https://github.com/TrinityCore/TrinityCore) - server reference
- [MaNGOS](https://github.com/cmangos/mangos-wotlk) - server reference
- [StormLib](https://github.com/ladislav-zezula/StormLib) - MPQ library

Optional development tool: [external comparison camera](docs/view-sync.md).

TBC comparison behavior and debugging notes: [overhead labels, hover, NPC tooltips and equipment](docs/tbc-overhead-labels.md).

### Legacy particle rendering

TBC particle headers store the emitter type at byte 0x2a; Wrath stores it at
0x29. Plane emitters use their authored area, angular ranges and speed variation.
Particle textures supply their silhouette, and SRC_ALPHA blending consumes
unpremultiplied shader color. The upload buffer grows to fit visible particles;
a fixed global cap can hide newly created loot and spell effects behind scenery.
Set `WOWEE_PARTICLE_TRACE=LootFX` to log emitter colors, sizes and submitted bounds.
Emitter int16 `+0x2E` is a priority plane (TBC values -100..17). Negative
emitters draw between the opaque and transparent mesh passes, the rest after
them: `BFD_WispSmall.m2` #1 is a black alpha underlay (-100) that shows as a
dark cloud when drawn over the wisp glow. Pre-264 emitter `+0x194` is drag,
speed times `exp(-drag * t)`; `BFD_WallLight03.m2` #0 (drag 5.5, 4 s life)
otherwise rises as a column to the ceiling.
Particles of a doodad in an interior WMO group take its MODD placement colour,
as its mesh does, unless the emitter has flag 0x1: `BFD_WaterFalls01.m2` splash
(0x420, authored 190,218,220) is blue under placement (84,125,223) and
`BFD_WaterFalls10.m2` teal under (43,91,100), while `Torch.m2` (0x21) and
`Candle01.m2` (0x39) flames keep their authored yellow-white.
Flag 0x1000 (XYQuad) sprites lie in the XY plane of their emitter bone as
placed in the world (`particleQuadAxes`): `WaterElemental.m2` #16/#21 sit on
unrotated bones and are the ground rings under an Aqua Guardian; Time Rift
(display 18877, `time_rift1.m2`) hangs its rings (#0-#2) on bones turned 90
degrees about Y under a Z-cylindrical billboard (bone 3, 0x40), so they face
the camera in the original. In the model's XY plane they lay flat like a
halo. 145 of 363 TBC XYQuad emitters sit under a rotated or billboard bone
(Blink, Arcane/Fire/Frost Ward, Ashbringer, Ethereal storm lightning).
Evidence: `_experiments/20261119T082500Z.orient_shadow/` in the workspace
(scan_xyquad.txt, sweep_fix/cases/18877).
Flag 0x400 pins a tail to its birth point: the tail runs back along
`-velocity * tailLength` but never past where the particle was born. 2204 of
3522 TBC tail emitters carry it, many with tailLength 10 s; without the limit
Bloodlust/Summon hand rings and the fire precast hand sparks drew 3-9 yd
spokes absent in the original, while acid breath drops (0.1 s) keep their
short tail. Whether unpinned tails also stop at birth is unverified (about
ten TBC emitters would differ). Evidence:
`_experiments/20261117T233000Z.flag400_spokes` in the workspace.
The particle layout regression runs in `test_m2_ribbon_tracks`.
A ribbon edge is the emitter point moved heightAbove along its bone's +Z and
heightBelow along -Z, through bone and model matrix, so a creature's display
scale widens its trails: Al'ar (display 18945, scale 5) drew 0.28 yd camera-facing
lines where native shows 1.4 yd bands. Colour, alpha, heights and visibility
follow the current sequence; every edge of edgesPerSecond x edgeLifetime is kept
(Al'ar's tail: 100/s for 2 s). Open: Al'ar's wing loops are still narrower than
native's; on phoenix.m2 the trail moves mostly along the bones' Z axes.

### Quest object sparkle

A game object whose `GAMEOBJECT_DYN_FLAGS` carry SPARKLE (0x08) glitters with
the corpse loot model, `Particles\LootFX.m2` (SpellVisualEffectName 14,
"HARDCODED Loot Art"). CMaNGOS sends the bit per player, with ACTIVATE, on a
chest, goober, generic or spell focus a quest of that player needs, and on all
of them to a game master; it clears the bit when the quest is removed,
completed or the chest is looted. Zeppelin Debris (quest 10161) shows none
before `.quest add 10161`, sparkles after it, and stops after `.quest remove`.
The effect sits at the object's model origin, scaled by the height of the
model's M2 header bounding box times the object scale, never below 1:
RuinedSteamTankGear01 (0.55) and Bush_Sansam (no box) at 1, GnomeBigGear 3.78,
AllianceVeteranBanner02 5.79. Gotcha: the scale is fitted to build 8606
captures, not read from the client; a scaled Fel Iron Deposit (box 2.05)
looked nearer 1.5 natively. Particle trails of LootFX's flare emitter still
read stronger than the original's.
Evidence: `_experiments/20261117T2300Z.go_sparkle/` in the workspace.

### Naj'entus Spine

Impaling Spine 39837 shows no kit model on the impaled player: its StateKit
7776 only holds animation 6, its ImpactKit 7682 plays
`nagalordshell_impact` on the chest. The spine is a game object. The stun
aura makes the target cast server-side 39929, which summons Naj'entus Spine
185584 (goober, display 7362 `G_NagaShellMissile`, size 6) at the target.
Its linked trap 185601 (Naj'entus Spine Trap, same display, size 1) appears
with it. Native draws both: the size-6 spine around the player and, on its
own, a small spine. Each server game object has its own M2 instance. The M2
renderer's same-model, same-position merge used to give the spine the size-1
trap's instance whenever the trap's create came first. The model has no bone
keyframes; its slight lean is authored and turns with the target's facing.
Reproduce with `.modify hp 60000` then `.aura 39837` at Black Temple 440.6
817.0 12.5: 39837 deals 4.5-5k and kills a level 40 test character. Gotcha: a
dead player's chat `.revive` did nothing on the pair; `revive <name>` on the
world console works. Spawning Naj'entus outside Black Temple dereferences the
missing instance script in `Reset()`. Evidence:
`_experiments/20261118T140851Z.najentus_spine/` in the workspace.

### Cast failure text

`build/bin/test_tbc_cast_results` checks the result-to-GlobalStrings names.

Every cast failure shows the interface's own GlobalStrings line, named after
the result: TBC 0x14 is `SPELL_FAILED_CASTER_AURASTATE`, "You can't do that
yet" (Riposte without a parry). `include/game/cast_result_text.hpp` holds the
names; the loaded expansion's `GlobalStrings.lua` supplies the words. The
original shows them only on the red error line (UI_ERROR_MESSAGE, colour
1.0/0.1/0.1), never in chat. `SPELL_FAILED_DONT_REPORT` shows nothing.

Parameterised lines, as the 2.4.3 client fills them:

| Result | Example | Argument |
|---|---|---|
| NO_POWER | Not enough energy | `ERR_OUT_OF_<power>` by the spell's Spell.dbc PowerType |
| REQUIRES_SPELL_FOCUS | Requires Forge | SpellFocusObject.dbc name from the packet |
| TOTEMS / TOTEM_CATEGORY | Requires Blacksmith Hammer | first tool of the spell's two not carried |
| REAGENTS | Missing reagent: Copper Ore | item named by the packet |
| EQUIPPED_ITEM_CLASS(_MAINHAND) | Must have a Dagger equipped in the main hand | ItemSubClassMask.dbc name, else ItemSubClass.dbc DisplayName |
| ONLY_SHAPESHIFT | Must be in Stealth | SpellShapeshiftForm.dbc name of the spell's one Stances bit |

The original refuses a missing tool and missing equipment before sending:
Copper Modulator with nothing reads "Requires Blacksmith Hammer" although the
server checks reagents first, and Backstab unarmed reads the dagger line
instead of "You must be behind your target". WoWee checks both locally
(`missingEquipment`, `firstMissingTool`). An interrupted cast shows one
"Interrupted" from the cast result that follows SMSG_SPELL_FAILURE; a broken
channel shows nothing. Gotcha: the parameterised lines need the `Spell`
`Stances`/`Totem*`/`AttributesEx3`, `ItemSubClassMask` and
`SpellShapeshiftForm` layouts in the installed `dbc_layouts.json`.
Evidence: `_experiments/20261117T182446Z.pair7_cast_errors/` in the workspace.

### Inventory and mail results

SMSG_INVENTORY_CHANGE_FAILURE and SMSG_SEND_MAIL_RESULT refusals show the
GlobalStrings line named in `include/game/inventory_result_text.hpp`, on the
error line only. A mail refusal of MAIL_ERR_EQUIP_ERROR carries an
InventoryResult: a second unique item taken from mail reads "You can't carry
any more of those items." (ERR_ITEM_MAX_COUNT). A sent letter shows
ERR_MAIL_SENT on the info line; taking an item or money shows nothing. TBC
MailFrame hears MAIL_FAILED, without an argument, after every mail result,
successes included.

### Keyring button

```sh
build/bin/test_game_replay '[keyring]'
```

`HasKey()` answers from PLAYER_FIELD_KEYRING_SLOT_1's 32 slots only; bank,
bank bag, buyback and vanity pet slots beside them do not count. FrameXML
sets the per-character `SHOW_KEYRING` to 1 the first time HasKey answers
true (PLAYER_ENTERING_WORLD, VARIABLES_LOADED, BAG_UPDATE) and never back:
on 2.4.3 a key added with `.additem 8444` and removed again leaves
`KeyRingButton` shown and `SHOW_KEYRING = 1` saved. Gotcha: a quest key
looted into the ring and used up seconds later (Executioner's Key on a Troll
Cage) leaves no trace in `character_inventory`; the debug log line
`Rebuilt online inventory: ... keyring=N` shows it. Evidence:
`_experiments/20261118T194107.keyring_show/` in the workspace.

### Client notices

The lines the client writes itself use the loaded GlobalStrings.lua through
`GameHandler::systemGlobal` (chat, CHAT_MSG_SYSTEM), `errorGlobal` (red
UI_ERROR_MESSAGE line) and `infoGlobal` (yellow UI_INFO_MESSAGE line). Where
2.4.3 puts each was measured on pair 6 with an event spy that echoes
SYSMSG/UI_ERROR_MESSAGE/UI_INFO_MESSAGE into chat:

| Action | 2.4.3 |
|---|---|
| `.gm on` (SMSG_NOTIFICATION) | error line only: "GM mode is ON" |
| `/afk`, `/dnd` | chat, from PLAYER_FLAGS: MARKED_AFK_MESSAGE, CLEARED_AFK, MARKED_DND, CLEARED_DND (none when DND gives way to AFK) |
| invite / accept / leave / kick | ERR_INVITE_PLAYER_S, ERR_JOINED_GROUP_S (inviter only), ERR_INVITED_TO_GROUP_SS, ERR_LEFT_GROUP_YOU, ERR_LEFT_GROUP_S |
| party command errors | chat: ERR_BAD_PLAYER_NAME_S, ERR_NOT_IN_GROUP, ...; `/invite` self: error line |
| duel | ERR_DUEL_REQUESTED (challenger), DUEL_WINNER_KNOCKOUT in chat; declined: info line ERR_DUEL_CANCELLED |
| trade | ERR_INITIATE_TRADE_S in chat; the one asked gets the window, no line; cancelled: info line |
| channels | CHAT_MSG_CHANNEL_NOTICE(_USER) for ChatFrame to word: "Joined Channel: [6. chantestx]" |
| `/who` | WHO_LIST_FORMAT rows, WHO_NUM_RESULTS |
| `/p` without a party | ERR_NOT_IN_GROUP in chat, nothing sent |
| reputation | one FACTION_STANDING_INCREASED/DECREASED (CHAT_MSG_COMBAT_FACTION_CHANGE) for the packet's first faction, FACTION_STANDING_CHANGED in chat for any rank crossed |
| login, zone change | "Joined Channel: [1. General - Elwynn Forest]" ... after the MOTD; "Changed Channel: [1. General - Stormwind City]" |
| quests | "Quest accepted: X" in chat; "Tough Wolf Meat: 1/8", "Kobold Vermin slain: 1/10" on the info line; nothing on objectives done or abandon |
| ready check | asker: READY_CHECK_START, then READY_CHECK_ALL_READY / RAID_MEMBER_NOT_READY + READY_CHECK_FINISHED / RAID_MEMBERS_AFK; members: ERR_RAID_LEADER_READY_CHECK_START_S |

Gotchas: TBC SMSG_WHO has a gender byte before the zone. JoinPermanentChannel
must answer zone channel 0, not nil, or SlashCmdList JOIN prints "Invalid
channel name" and never lists the channel. The realm answers nothing to
`/friend` of an unknown name; "Player not found." is `/removefriend`'s own.
TBC item links have eight fields (no level). Zone channels are joined by
ChatChannels.dbc id (flag 0x1, plus GuildRecruitment by CVar) and named from
the pattern and the zone under the player; the realm moves them on zone change.
See [Chat channels](#chat-channels).
Notices raised before FrameXML loads (the login roster, loot rules) are held
and printed once it is up. The ready check's asker sends
MSG_RAID_READY_CHECK_FINISHED (all answered or 30 s); the realm only relays it. GM mail is from "Customer
Support" with stationery icon Mail_GMIcon (Stationery.dbc item icon).
Evidence: `_experiments/20261118T074332Z.chatlines_native/` in the workspace.

### Group loot rolls

At most four rolls are announced (START_LOOT_ROLL); TBC FrameXML has four
GroupLootFrames and drops any announcement beyond them. Every roll that ends,
by the player's choice inside RollOnLoot or by the server's result, raises
CANCEL_LOOT_ROLL and then announces the oldest waiting roll whose item is
cached, with the packet's full countdown. Roll ids count from 0 per session.
Gotcha: the announced count is unsigned and also drops for rolls that were
still waiting, as in the original client. Once the server times out waiting
rolls it wraps, and no further roll is announced that session. The original
client measured the same on three later boss drops. Evidence:
`_experiments/20261118T002549Z.loot_roll_queue_pair7` in the workspace root.

### TBC combat feedback

Outgoing melee damage uses white numbers; ability damage uses yellow numbers.
XP appears above the player as `XP: <amount>` in purple, using FRIZQT__.TTF.
Ordinary damage lasts 1.5 seconds; XP lasts 4.5 seconds. The build-8606 popup
parameter table begins at 0xBBF1C0 (28-byte records); XP formatting is at
0x6089E0. Incoming floating combat text follows the `enableCombatText` setting.
Damage numbers over a unit (include/ui/floating_combat_text.hpp) keep a fixed
on-screen size at any distance: 30 interface units, critical 45 after a pop to
twice that over 150 ms. Ability yellow is (255,222,0). An ordinary number is born
with its bottom on the plain name line and climbs 1.4 yd/s; a critical hangs below
the line and stays. Opacity ramps to 0.1 over 150 ms, then is full until 0.76 s
(critical 1.0 s) and fades out by 1.5 s. Shadow: black at half opacity, 3 px right
and 1 px up at 720 lines. Texts over one unit never overlap: the newest keeps its
place, older ones are lifted a row (two font heights) at a time while within one
row of their own height, otherwise set to the right. Check with
`build/bin/test_floating_combat_text`.

Particle births retain their time within each render frame. Existing particles advance
before emission; new particles start at their elapsed age since birth, including
ballistic displacement. Short effects such as LootFX's 0.2-second stars must retain
their authored live population even at low rendering rates.

Terrain height queries can return the selected fan triangle's exact normal.
Walking collision must use that face normal: averaging heights across triangle
creases changes the 50-degree classification. Steep contact currently projects
falling motion onto terrain; the paired rockface route still shows a 3-5 yard
endpoint discrepancy and needs further native support-contact investigation.
WMO wall distance is measured after clipping the triangle to the player's body
span above the step budget, so low ramp edges do not block entry.

Warden scan-boundary validation must include payload semantics. A code-scan
address can decode as a DRIVER opcode by chance; a driver request still requires
a valid nonzero string-table index. Accepting an invented driver consumes later
memory checks and makes the server disconnect on a truncated response.

### Creature support and idle variations

Creature, player and attached-item batches cull back faces unless their M2
material has the two-sided flag 0x04, as world M2s do; an instance whose
transform reflects keeps both sides. Two-sided materials are lit alike on
both sides under TBC light (no normal flip for back faces). Drake.m2 (Azurous,
display 6373; all three materials flags 0) builds each wing from two
one-sided sheets: drawn two-sided, the far sheet's back showed through and,
lit as a back face, painted the membranes dark brown where build 8606 shows
them pink-beige from either side. Chimera.m2's wing materials (2-3) carry
0x04: a Chillwind Chimaera's wings from below were dark grey in WoWee and
are pale in the original. Drake.m2 has two Stand sequences (2 and 5, equal
frequency); 5 is a wing beat, so a drake's wing pose differing between the
clients from moment to moment is the variation draw, not skinning. Paired
captures with the original character morphed to 6373 and 10810:
`_experiments/20261118T211000Z.azurous/` in the workspace.

Units (creatures, players, what they carry) take the TBC light, ambient plus
sun by N.L, times 1 + 2 x AreaTable field 34 (AmbientMultiplier) of the
player's area, before saturating; an area with 0 there uses its parent zone's
(Agama'gor, 0, is lit as The Barrens, 0.3). Native unit pixels divided by the
same texels (WoWee unlit render) against WoWee's own light, ambient and sun
side alike: GM Island (0) 0.85, Stone Cairn Lake in Elwynn (0) 1.0, The
Barrens (0.3) 1.65, Thousand Needles (0.5) 2.0 at 09:00 and at 21:45, Hellfire
Peninsula (0.6) 2.2; terrain in the same frames matches without the gain.
LightParams HighlightSky looked like the switch at first (1 in Thousand
Needles, 0 on GM Island) and is not: Elwynn has 1 and Hellfire 0. The
2.4.3 model vertex shaders (`Shaders\Vertex\model2.bls`) light with nine
spherical-harmonic constants, so the gain sits in what the client feeds
them. Whether it follows the player's or the unit's area, and whether
subzones with their own value (Tarren Mill 0.6 under Old Hillsbrad 0.3)
use it, is not observed. Evidence: `_experiments/20261119T0840Z.creature_light`
in the workspace.

Grounded creatures select the nearest reachable terrain, WMO or M2 floor.
Terrain corrections do not require a previous corrected render position; static
server spawn Z can differ from the ADT triangle. Structure corrections retain
continuity checks for overlapping floors. `WOWEE_CREATURE_GROUND_TRACE=<GUID>`
logs one creature's server position, support position and slope rotation each second.

Looping Stand animations choose among authored M2 sequences using their frequency
weights and repetition bounds. The TBC wolf has four Stand sequences, all with
variationIndex zero; filtering on that field cannot identify its fidgets.
Selection happens at sequence boundaries and uses authored transition blend time.
One-shots draw their sequence by frequency as well: Murloc.m2 AttackUnarmed is
sequence 5 (a leap upright toward the target) or 8 (a low lunge); native 2.4.3
played each in two of four swings. Always taking the first-listed one made
every WoWee murloc swing the leap.

A creature given a new display (`.npc setmodel`, a VALUES display change) is
respawned with the new model but does not play Birth: Tree Disguise (16977,
AshenvaleTreeFalling01.m2, Birth grows bone 1 from 0.09) stands at full height
in native right after setmodel. A display whose CreatureDisplayInfo skin is
empty leaves its texture type 11-13 slots white: Riding Wyvern 20413 (display
19608) and Teron Gorefiend's mount (21263) are white in native, with their
type-0 parts textured. Evidence: `_experiments/20261119T0840Z.sweepfx_verify/`
in the workspace (`base/` is the unchanged build).

Airborne terrain contact compares feet against the terrain height at each end
of the movement segment. Comparing both endpoints against the destination floor
misses uphill landings and leaves jump momentum active after penetration recovery.
Regression target: `test_hill_climbing`.

Moving landings resolve directly to walk/run. Character rendering cross-fades
every change of animation using the destination sequence's authored blend time
(see [Missing animations and cross-fades](#missing-animations-and-cross-fades)).
Outgoing one-shots clamp at their final frame during the blend; looping
sequences wrap. Stationary landings retain JumpEnd.

In the air the lower body keeps the direction it left the ground with until
landing. Native 2.4.3, own character and as seen by others: strafing right,
jump, switch to strafe left (or release, or add forward) mid-air, and the leg
twist stays on the takeoff side; on touching down it turns to the new keys over
about 0.2 s. A standing jump takes the direction of the first key pressed in
the air, which is also the drift direction. Local player: `CameraController`
aims `getTravelYaw` at the airborne momentum (`legsFollowTravel`); remote
players: `rendering::airborneLegDirection` latches the movement-flag
direction at takeoff. `build/bin/test_entity '[animation]'`. Evidence:
`_experiments/20261119T1040.jump_legs/` in the workspace.

Descending floor selection excludes prop/structure candidates above previous
feet. A nearby log rim must not hide terrain crossed beneath it. Terrain retains
its endpoint crossing rule for uphill landings; grounded step-up is separate.

Authored M2 contact uses an upright beveled box. TBC build 8606's shape
constructor at `0x632650` supplies nine points: the sole, four corners at
`radius * 1.8493989706039429`, and four corners at collision height. Its flat
head and tapered foot both participate in contact. Radius/height use the scaled
M2 collision bounds; the dwarf's 0.30556/1.60660-yard dimensions agree with
CreatureModelData's width/height to file rounding.

Continuous separating-axis tests sweep that body against placed triangles,
including triangles classified as local floors. Native collision geometry stays
solid regardless of the name or local orientation of its model. Resolve lateral
motion before vertical motion. Lateral overhang contact preserves feet height;
vertical head contact stops ascent; foot support clears falling velocity.
Grounded step-up remains a separate floor-selection decision.

`test_collision_geometry` covers fast thin-wall crossings in both directions,
ceiling winding, and sustained contact with an overhang without downward drift.

Paired movement tests must wait for teleport acknowledgement and world entry.
Use established movement or obstacle contact before jump input; simultaneous
start/jump events produced different native outcomes across runs. A blocked
jump alone is insufficient evidence: compare valid climbs and landings too.

Outgoing movement packets are stamped with the instant their position belongs
to: a frame's physics integrates up to the instant that frame began
(MovementHandler::beginFrame), so the state synced in the next frame carries
the previous frame's start time. Heartbeats go out exactly 500 ms after the
previous movement packet of any opcode, with the position interpolated between
the two physics samples around that instant; none while standing or only
turning. Measured on native 2.4.3: 7.00 yards per 500 ms running, 4.50
backpedalling, no idle or turn-in-place heartbeats. Stamping send time instead
made observers see speeds from 3.9 to 8.2 yards/s at 17 fps and far worse during
frame hitches. A direction released in the air keeps its flag with
PENDING_STOP 0x4000 / PENDING_STRAFE_STOP 0x8000 until FALL_LAND (native STOP
0x5001, FALL_LAND 0x0). FALL_LAND carries the whole fall time and later packets
keep it, as native does. FALL_LAND is stamped at the touchdown substep and
place within the frame. A running jump keeps its takeoff velocity to the
landing whether keys are held or released. After a standing jump (packet
horizontal speed 0) the first direction pressed in the air drifts the
character at 2.50 yards/s that way until landing, held or released; later
keys leave it (native, run and walk mode). Walk/run toggles send
MSG_MOVE_SET_WALK_MODE / SET_RUN_MODE (flag 0x100) when they happen, moving or
not. Keyboard turning starts from the character's facing, not the camera yaw,
and no SET_FACING follows STOP_TURN. `stream_stats.py` in
`_experiments/20261118T0800.movesend/` prints rates from a debug log.

Water-jump packets use the ballistic clock, excluding time spent swimming.
Takeoff resets that clock; heartbeat position and fallTime describe the same
completed physics sample. START_SWIM reports the current water-contact position
and ends the arc. JUMP already clears swimming, so it needs no STOP_SWIM packet.
The camera compares swimming against the reported network state, which can reset
at login or teleport without a physical water transition.

Captured TBC stationary water jumps advertise a 9.09675 yards/s upward launch
(negative on the wire) and 4.72222 yards/s horizontal speed despite having no
directional movement flags. Preserve the mode speed field; directional flags
determine horizontal travel. Validate outgoing jumps on a native observer as well
as checking packet trajectories. A server session still marked teleporting drops
movement packets even while local physics and outgoing client logs look valid.

A liquid's bed is the highest floor below the swimmer's probe height (feet + 2
yd, as the WMO floor query), and heightfield over a swimmer who is already
under it is a ceiling, not a shore that stops swimming
(`rendering/swim_wall.hpp`, `build/bin/test_swim_wall`). Ahn'Qiraj's desert
lies 300 yd over C'Thun's stomach (server GroundZ 204.8, slime surface -98.0,
bed -106.6): taken as the bed it left WoWee walking on the bottom of the acid,
taken as a shore it froze a swimmer in place; native swims there (`.gps`:
liquid type flags 4 slime, status 4 in water). Evidence:
`_experiments/20261118T1800Z.cthun_beams/` (stomach-00.jpg, acid*-00.jpg).
Open: native draws that acid dark green and nearly opaque from above, WoWee
bright green.

Underwater lighting uses water height at the camera, independently of swimming.
Terrain water and WMO pools both receive underwater fog. Surface swimmers keep
the orbit camera above water; diving below surface float depth keeps it below.
Both limits shorten the pivot-to-eye ray and preserve the requested zoom.
Swimming disables the artificial camera floor at player feet, so underwater
upward views can place the eye below the player. Real geometry still obstructs
the camera. Validate with live pitch sweeps in both swimming states and
camera-only above/below captures while leaving the swimmer submerged.

Swimming movement preserves backward/strafe direction during the locomotion
stop grace period. Swim, swim-backward, swim-left/right, and swim-idle poses
cross-fade using the destination M2 sequence's authored blend time, including
transitions to ground locomotion. `WOWEE_SWIM_ANIMATION_TRACE=1` logs animation
IDs and applied blend durations for live stop/start verification.
Local swim strafing keeps the model aligned with gameplay facing. The authored
swim-left/right poses provide the lateral lean; land-stride travel yaw and
spine counter-rotation must not be applied on top of those poses. Verify both
Q and E underwater, where the whole body is visible.

Player camera fade uses actual camera-to-pivot distance, becoming transparent
between 1.2 and 0.2 yards (build 8606 zoom steps: opaque at 1.31, gone by 0.2). Collision can shorten that distance while requested
zoom stays far away. Camera opacity multiplies ghost/stealth/spawn opacity and
propagates to weapons and their effects. Renderer visibility recovery respects
the camera controller's fully transparent state.

Camera fades render a depth-only pass for solid character batches before color
blending. Blending while writing depth in one pass leaves previously drawn
backfaces visible through the front. The color pass tests equal depth; transparent
cards retain their authored blending and depth behavior.

Every whole-model fade (camera, stealth/invisibility/ghost kit, display alpha,
spawn and despawn fades) uses that path, per instance, in the back-to-front
blended stream: prepass, then colour. Running the prepass with the solid
geometry cut opaque units behind a stealthed one out of the picture (the sand
showed where 2.4.3 shows the other rogue through the faded body). A faded
model's blended material layers test LESS_OR_EQUAL without layer depth bias and
composite over its faded base, so items with stacked layers stay denser than
the body: under Stealth (opacity 0.3) 2.4.3 draws the body at about 0.33 and the
sheathed Warglaive of Azzinoth at about 0.56 (opaque base plus its alpha layer).
The fade comes only from the aura's SpellVisualKit CharProc 14 (Stealth, Prowl,
Vanish, Shadowmeld: kit 312, 0.3; Invisibility: kit 3450, 0.5) times the
CreatureDisplayInfo alpha. UNIT_VIS_FLAG_CREEP does not fade a unit: a creature
under Stealth 31621 (CREEP, no kit) stays opaque in 2.4.3. Evidence:
`_experiments/20261118T201300Z.stealth_alpha/` in the workspace.

### Unit draw distance

2.4.3 draws every unit the server keeps in range; it has no distance or model
size cut of its own for units. What arrives is decided by the server (cmangos
`Visibility.Distance.*`: 100 yd on continents, 170 in instances, 533 in
battlegrounds; LARGE_AOI/GIGANTIC_AOI creatures 200/400, e.g. Lady Jaina 4968,
Frost Wyrm 17907, Archimonde 17968). Far units fade into the fog, which ends
at the `farclip` CVar (477): at farclip 177 a Sea Giant fades out smoothly over
150-180 yd, no pop. Native's `DistCull` (500) and `SmallCull` (0.04) CVars do
not touch units: at DistCull 60 and SmallCull 1 a human 190 yd away is still
drawn. CharacterRenderer culls units at the view distance (the one terrain and
WMOs stop at) and the frustum. Game objects, the emitters of units
(`externalPose`) and spell visuals draw to the view distance as well
(`M2Instance::serverVisibility`), so a unit's glow comes and goes with it.
Gotcha: a unit's name plate is drawn independently of its model; a culled
model leaves a name floating over nothing. Evidence:
`_experiments/20261118T235900Z.unit_distance/` in the workspace.

### Interior fog and far doodads

A WMO's authored fog (MFOG) fogs only what is lit as that interior: its
interior groups, their doodads, units and liquids in them. Terrain, outdoor
doodads and exterior groups seen from inside keep the outdoor Light.dbc fog, and
no outdoor M2 is drawn past the outdoor fog's end. Native: from Black Temple's
stairs to Illidan's door 2.4.3 shows sky where the floating rocks had drawn as
flat teal shapes. Details, evidence and gotchas: [docs/wmo-fog.md](docs/wmo-fog.md).

## TBC scene integration

`GetGamma()`/`SetGamma(v)` use build 8606's scale: the offset of the `gamma`
CVar below 1 (`SetGamma(0.2)` stores `0.800000`, neutral is 0). The TBC Video
options set their -0.5..0.5 Gamma slider to `GetGamma()` on opening, so a
client answering 1 there clamps the slider to 0.5 and writes it straight back.
The CVar is held only; it is the original's display gamma ramp, and the
reference install runs with `DesktopGamma 1`. WoWee's own Brightness setting
(`brightness=50` in `settings.cfg` is neutral, 25 halves the scene) is separate.

WMO area names use the root ID, placement name-set and authored group ID with
`WMOAreaTable.dbc`. A building can have area ID zero: Anvilmar is root 752,
name-set 1, with its building name on the group -1 row. That name overrides
`GetZoneText`; named group rows supply `GetSubZoneText`. An unnamed room in a
named building has an empty subzone. Original TBC FrameXML then displays the
building heading and territory subtitle.

Area membership follows the closest WMO surface below the player's feet,
including outdoor entrance stairs. Terrain above that surface takes precedence.
Indoor lighting and minimap containment remain separate: crossing from outdoor
stairs into the same building's indoor group must not replay the zone title.

Interior minimaps use the authored WMO tiles in `md5translate.trs`, at two pixels
per local yard. Group filenames use the original group index, including empty
groups omitted from rendering. Match minimap zoom before comparing screenshots;
indoor and outdoor zoom levels have different radii.

TBC `SMSG_MONSTER_MOVE` intermediate points must reach entity interpolation.
Ground routes follow linear segments; facing follows each segment. Discarding
those points turns an obstacle-avoiding patrol into a straight line. Stand
variation blends sample the outgoing final pose, rather than restarting it.

World names use per-pixel scene depth. A whole-label anchor test can erase a
visible name when a beam covers only its anchor. Unit picking tests the current
skinned body after the broad sphere test, so a nearby player's sphere does not
steal a vendor hover. Crafting spell-focus objects, including anvils, are not
click targets. Repair takes precedence over vendor when both flags are set.

Unavailable quest status 1 uses the grey world exclamation model and no minimap
dot. Local speech creates its bubble before the duplicated server echo is
suppressed; bubbles use the client tooltip backdrop and tail assets.

### Quest marker rendering and hover

World quest markers use the client M2 materials, including each batch's authored opacity. `TalkToMeGrey.m2` has an opaque gradient base and a 40% alpha-additive highlight. Marker instances skip collision without being classified as spell effects; that classification overrides material blending.

Cursor selection receives quest status separately from the overhead symbol
(`questGiverCursor` in `include/game/quest_giver_status.hpp`, units and quest
giver objects alike). Build 8606: NONE and UNAVAILABLE (grey !) keep the
giver's ordinary cursor (Felix Whindlebolt the plain gauntlet); available,
taken-but-unfinished (grey ?), daily and out-levelled quests show `Quest`;
ready to hand in `QuestTurnIn`, a repeatable ready again (REWARD_REP)
`QuestRepeatable`. Out-levelled is TBC `DIALOG_STATUS_CHAT` (wire 2: every
offered quest more than `Quests.LowLevelHideDiff` levels below): no mark, yet
the Quest cursor - Haughty Modiste, Yeh'kinya and Wanted Poster 142122 to a
level 70 character. Gotcha: `QuestGiverStatus::CHAT` (11) is this client's own
value for it; reading it as NONE gave the vendor/gossip/gear cursor and let
gossip-derived status overwrite it. Evidence:
`_experiments/20261118T181000Z.pair16_quest_cursor/README.md`.

Status refresh: build 8606 sends `CMSG_QUESTGIVER_STATUS_QUERY` for a giver
only as it comes into view, and `CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY` (every
giver in view) whenever its quest log changes: any `PLAYER_QUEST_LOG_*` slot
field (accept, hand-in, removal, a kill counter, `.quest add/complete/remove`)
or the carried count of an objective item (0->9, 9->10, 10->11, 11->9 Cactus
Apples each sent one, Linen Cloth none). Entering the world with quests sends
one; with an empty log none. WoWee does the same (`queryAllQuestGiverStatuses`
in `quest_handler.cpp`). Gotcha: asking only the giver a quest came from left
Gornek with the ! of Cutting Teeth after Your Place In The World (Kaltunk,
complete on accept) instead of the ? of the turn-in. The server itself sends
the multiple status only after a reward and on level-up. Native wire captured
with a logging proxy (header decrypt from the realm's session key):
`_experiments/20261119T102200Z.questmarker_status_refresh/README.md`.

Gossip and greeting lists mark a quest more than 5 levels below the player as
trivial (grey icon, "(low level)"; `questIsTrivial` in
`include/game/level_difficulty.hpp`): measured at levels 17/18, 43-46, 55/56
and 63/64, below 17 unmeasured. Gotcha: this is not `GetQuestGreenRange`,
which build 8606 answers as 4 below level 10, one more every ten levels and 8
from 40 (quest log colours).

### Indoor selection and terrain holes

`scene_pick.hpp` tests terrain surface crossings, not the solid volume beneath
outdoor height samples. Terrain holes break the crossing interval. A camera in
Anvilmar can see NPCs beneath adjacent outdoor terrain without crossing any
rendered terrain surface. WMO walls and foreground M2 geometry still occlude. Cursor rays test the
retained, skinned M2 render triangles, so gaps between candelabra branches
remain clickable. Transparent/additive effect batches do not occlude these
rays. Body and camera collision use their collision meshes.
Click diagnostics in `scene_pick.cpp` identify mesh misses and WMO/M2/terrain
rejection; hover does not emit these logs. Run `build/bin/test_scene_pick` for
selection ordering and terrain-hole regression checks.

### Sunwell Plateau entrance (WMO groups, minimap tiles, rain)

Map 530 at 12574.1, -6774.81, 15.09 (game_tele `SunwellPlateau`), facing west into
`world\wmo\dungeon\qt_sunwell_facades\sunwell_raid_facade.wmo`, compared on a private pair:

- Every WMO group is drawn at every distance; portals alone choose what a camera sees.
  2.4.3 has no distance-only groups. Group 40 "entryfacade" (outdoor, 2406 vertices) is the
  bright arch around the portal; with it missing, the dark outer walls of the indoor portal
  hall (groups 38/39, 0x2000) and the `instanceportal_green.m2` haze showed as a black box
  with a hard-edged green volume. Same model family: `sunwell.wmo` group 39, Shadowfang's
  "towerfacade01", Dire Maul's "facadesatyr", Silvermoon's 0x10000 groups, Stormwind's
  unreachable cathedral copies (checked against native from Cathedral Square and inside the
  cathedral: no overlapping shell; unreachable groups are culled by portals).
- `md5translate.trs` keys use two-digit tile indices (`Expansion01\map44_08`). Any tile with
  an index below 10 (all of Isle of Quel'Danas) needs the padding or it has no minimap.
- Weather particles and weather sound stop only when the camera is in an indoor (0x2000)
  group. The entrance lies inside the box of the facade's outdoor groups (group 53 spans the
  whole model), and the zone's server rain (zone 4080, SMSG_WEATHER state 4) falls there natively.
  Rain appearance still differs: native draws long faint streaks tinted by the scene and
  red splash marks on the ground; WoWee draws short grey-blue streaks with a fixed colour
  (`weather.cpp`) and no splashes.

### Idle loop boundaries

Stand variants blend from their outgoing final pose using the incoming sequence's authored blend time. This also applies when the same variant repeats, either through random selection or its replay count. TBC `DwarfMale.m2` sequence 40 has different first and last spine poses; a direct wrap produces a visible snap.

Every looping animation starts on a variation drawn by M2 frequency and redraws at a sequence end once that sequence's replay count is spent, Stand or not. DoomGuardOutland.m2 has two Walks: 0 (on its feet) and 33 (wings beating, feet off the ground, replay 3-5). Native Illidari Fearbringers (22954) in Black Temple switch between them along their patrol; their create block carries only FORWARD, WALK_MODE and SPLINE_ENABLED (0x08000101), and the spline flag 0x100 is the Runmode CMaNGOS adds to every monster move, so neither flying nor hover state is involved. `WOWEE_TBC_DATA_DIR=... build/bin/test_ready_stance_data`. Evidence: `_experiments/20261118T1350Z.akama_gait` in the workspace (bt1 native, bt2 WoWee).

Walk or Run (rendering::splineGroundGaitWalks) compares the spline speed with the Walk stride at the display's own size: OBJECT_FIELD_SCALE_X enlarges the model but not the stride. Native Akama 21700 (display 20681 x1.3, Walk 2.5) ran at 6.94 yd/s at object scale 1, 2 and 3; Akama 22990 (same display, created at object scale 2) ran at 8.0; El Pollo Grande (display 589 x10) walked at 8.0. For Black Temple Akama 23089 (display 21112 x2.6, run speed 10) the rule gives Walk (stride 6.5, threshold 13); a native 21700 given display 21112 also walked at 10.4. The real Illidan event was not observed natively.

### Missing animations and cross-fades

```sh
WOWEE_TBC_DATA_DIR=client/wowee-data/expansions/tbc build/bin/test_playable_animation
```

A model asked for an animation it has no sequence for shows the stand-in its
own playable animation lookup names (pre-Wrath M2 header 0x2C: uint16 animation,
uint16 flags per AnimationData id, 203 entries in every 2.4.3 creature and
character model). Flags 3 show the stand-in frozen on its last frame (Dead ->
Death, SitGround -> SitGroundDown, LootHold -> Loot), 1 play it backwards
(Walkbackwards -> Walk, LootUp -> Loot); 0, 1 and 3 are the only values. Ids
past the lookup (StealthRun 223, ...) and models without one follow
AnimationData.dbc's Fallback chain, whose Flags give the same modes: 0x20 frozen,
0x10 backwards on the direct Fallback only. The lookup agrees with the chain
except for Fly (135) and SpellKneelLoop (141) -> Stand, and wound, parry, dodge
and block -> Stand on 25 models with a ready stance but no StandWound (dragons,
drakonids, gnolls, NetherDragon.m2, wyrms): there the chain goes StandWound ->
ReadyUnarmed. Native build 8606 follows the lookup: `.npc playemote 39`
(ParryUnarmed) and `43` (ShieldBlock) left JAB Nether Serpent (21329) in Stand,
`44` (ReadyUnarmed, which it has) raised its ready stance; WoWee had shown the
ready stance for all three. `rendering/animation/playable_animation.hpp`.

Every change of animation cross-fades from the shown pose into the new sequence
over that sequence's authored blend time, into and out of one-shots too. Native
Ice Thistle Yeti (7458, Wendigo.m2: no ready stance, AttackUnarmed 1000 ms,
Stand blend 350 ms) moved on 1.27-1.30 s after each swing and eased back into
Stand; WoWee cut from the swing's last frame to Stand's first after 1.0 s, the
reported snap to the idle pose after every attack (Qiraji Gladiator, Wendigo
yetis, every model whose swing ends away from its idle pose). A stand-in frozen
on its last frame (a corpse coming into view) shows at once.

M2 sequence flags 0x2, 0x4 and 0x8 copy AnimationData BodyFlags 0x200, 0x400
and 0x800 (0x2 on every Death and SleepDown, 0x4 on SleepUp, 0x8 on Sleep, Fly
and Dead); nothing observed depends on them yet. Gotcha: pair avatars are hidden
from each other (`debug viewsync hide`); a creature fighting the WoWee character
then attacks an unseen unit on native. `paired_sync.server_request('show')`
before melee observations.

Evidence and tools: `_experiments/20261118T205000Z.anim_fallback` in the workspace.

### Turning in place

The standing local character plays `ShuffleLeft` (11) while the player turns it
left and `ShuffleRight` (12) while turning right, with the turn keys or by
mouse-look (right button). Each shuffle cycle (HumanMale.m2: 500 ms, blend
250 ms) that contained a turn is followed by another; a cycle without one
returns to Stand, so the feet keep stepping briefly after the turn stops and a
one-frame mouse turn still steps once. Moving, jumping and swimming take over
at once. A model without the sequence plays its AnimationData fallback, Stand.
Combat ready stances, emotes and sitting keep their animation. Native 2.4.3
shuffles its own character for keyboard and mouse-look turns, including a
ten-pixel mouse turn. `build/bin/test_locomotion_fsm '[turn]'`. Evidence:
`_experiments/20261118T1952.turn_shuffle` in the workspace.

Other players and creatures turning in place shuffle by the same rule
(`rendering::TurnShuffle`), driven by the per-frame change of their drawn
facing, while they stand alive in Stand (not moving, airborne, swimming,
mounted, in a ready stance, emote or stand state). A model without the
sequence keeps standing. NPC conversation turns keep their own single shuffle.

Another player's facing between packets: a standing keyboard turn sends
`MSG_MOVE_START_TURN_LEFT/RIGHT` with the start facing, no heartbeat, then
`MSG_MOVE_STOP_TURN` with the final facing (measured 3.14 rad/s, the movement
block's turn rate). The facing keeps turning at the mover's turn rate while
`TURN_LEFT/RIGHT` is set; a running player's velocity turns with it, so it
follows the arc between heartbeats. Mouse-look sends one `MSG_MOVE_SET_FACING`
per mouse step, about every 100 ms; any other facing change is drawn over the
mover's packet interval (120 ms after a pause), not as a step. A teleport sets
facing at once. Native draws all of these smoothly with the shuffle.
`build/bin/test_input_integration '[remote-turn]'`,
`build/bin/test_entity '[remote-turn]'`. Gotcha: native draws a WoWee
player's movement about 1.05 s after WoWee sends it (turns, running starts,
backpedals alike; cause not found), while WoWee draws a native player's at
once; compare shapes, not timing. Evidence: `_experiments/20261118T2030.remote_turn`.

### NPC conversation facing

Greeting callbacks play voice audio. Opening a gossip panel plays the emotes
of its npc_text option (SMSG_NPC_TEXT_UPDATE: three delay ms / Emotes.dbc id
pairs); a quest greeting (SMSG_QUESTGIVER_QUEST_LIST) plays its one emote. Native
does this on every open, cached text or not: Marshal Dughan exclaims (npc_text
16211, emote 5), Challe and Gryan Stoutmantle talk (emote 1); an all-zero text
(Gilbarta Grandhammer) and a vendor-only NPC (Brother Danil) play nothing. Each
later emote waits for the previous animation to end, then its delay. Gotcha:
native started Mizli Crankwheel's second emote (delay 3) about 0.6 s after her
talk ended; that gap is not modelled. Quest detail and reward emotes are not
played. `build/bin/test_input_integration '[npc-panel-emote]'`. Other body
emotes come from server animation messages. Local conversation turns play the authored directional `ShuffleLeft`
or `ShuffleRight` once, then return to idle; repeated gossip does not restart
the shuffle. Combat, locomotion, and server emotes keep their animation.
While an NPC service or quest panel is open, its stationary living NPC turns
toward the local player. Closing the panel returns it to the server heading.
Only an NPC in stand state 0 (UNIT_FIELD_BYTES_1 byte 0) without
UNIT_FLAG_STUNNED (0x40000) turns. Native 2.4.3 kept the heading of Marshal
Dughan (240) sitting, in chair states, sleeping, dead, kneeling, submerged and
stunned, and turned him while pacified, in combat, confused, fleeing, possessed,
rooted or with an emote state. Vaelastrasz the Corrupt (13020) lies in stand
state 7 when first talked to and does not turn; standing, the same dragon
model turns. The same rule (game::unitTurnsInPlace) gates turning toward
UNIT_FIELD_TARGET: native Dughan targeting the player turned while standing,
not while sitting or stunned. Measurements: `_experiments/20261118T075345Z.npc_interaction_turn`
in the workspace.
A unit attacking the player is not turned toward it otherwise: standing, it
faces UNIT_FIELD_TARGET; with that cleared it keeps the facing of its last
SMSG_MONSTER_MOVE. During Dark Glare the Eye of C'Thun clears its target and
the server turns it pi/35 a second with facing-only splines (boss_cthun.cpp,
Rotate 360 26009/26136); its CthuneEyeAttack beam (Dark Glare 26029 cast kit,
left hand) sweeps round with it natively. WoWee held every attacker toward
the player each frame, so the beam stayed pointed where the sweep began
(`_experiments/20261118T1800Z.cthun_beams/`, sheet_darkglare_before.jpg, sheet_darkglare_after.jpg).
Facing settles over 50 ms, matching native frame captures. The foot shuffle
finishes at its authored duration (500 ms for DwarfMale). Repeated clicks do not
restart either interval.
This facing is local presentation and never changes entity movement or patrol
orientation. Other clients can see a different facing for the same vendor.
`build/bin/test_input_integration '[npc-facing],[merchant]'` checks turning and merchant
interaction lifetime.

### Corpses keep their death pose

A dead unit stays in Death/Dead whatever animation the server asks of it.
CMaNGOS DB scripts go on talking after the speaker died, each text playing its
emote (Vashj'ir Honor Guard 21218 relays: ONESHOT_TALK, ONESHOT_ROAR), and
UNIT_NPC_EMOTESTATE survives death. Native build 8606 kept a killed Honor Guard
lying through `.npc playemote 15`, `1`, `0` and `10`/`64`, and through an
SMSG_SPELL_GO of Whirlwind 37641 (cast kit animation 126) sent from the corpse
with `.debug send opcode`. WoWee stood those corpses up (Leotheras the Blind
whirling on the ground). `GameHandler::playServerEmote` drops server emotes on
a corpse (the state is still stored); `CharacterRenderer::isInstanceDead`
gates cast, kit, field-channel, swing and hit-reaction animations of remote
units. `build/bin/test_game_replay '[death][emote]'`. Evidence:
`_experiments/20261118T1320Z.death_over_emote` in the workspace. Unverified
against native: swing/hit/channel-field packets aimed at a corpse.

A corpse's own model emitters keep going on the held last frame of Death,
their tracks sampled there, except emitters with pre-264 flag 0x8000
(`kParticleStopsOnHeldFrame`, 39 TBC creature emitters, the death bursts).
Native, pair 15, tempspawned and killed with `.die`: a dead Shivarra (Priestess
of Dementia 22957 / of Delight 22962, `Shivan.m2` #0-#7, flags 0x29-0x239,
ParticleColor 312) keeps the green fel fire in its hands (the Black Temple
Den of Mortal Delights report); a Harvest Golem (36) keeps its smoke (#0, 0x2,
rate keyed 10 at Death's end); a Ravenclaw Apparition (2056, `Ghost.m2`) keeps
its flare (#2, 0x9) but not its sparks and shockwave (#0/#1, 0x8009); a Stone
Rumbler (4528, `ElementalEarth.m2`) shows none of its 0x8001 bursts, which are
keyed 100/30/12/100 past Death's end. Evidence:
`_experiments/20261119T0900Z.corpse_fire` in the workspace.

A display change keeps the death pose too. Native Akama (21700) under Shape of the Bear (42377) killed with `.die` loses the aura with its life: the bear becomes Akama, and that model plays the whole of Death, then lies. A corpse given another display later (`.npc setmodel`) shows the new model lying at once. `EntitySpawner::removeCreatureForNewDisplay` keeps a corpse marked dead through the rebuild: Death on the new model while the old one was still falling, else Dead. A morphed player's creature model follows the same rule. Nalorakk (23576) dying in bear form is the reported case. Evidence: `_experiments/20261118T1620Z.corpse_display_change` in the workspace.

### Sheathed weapons

The unit sheath state (`UNIT_FIELD_BYTES_2` byte 0: 0 unarmed, 1 melee,
2 ranged) puts main and off hand in hand in state 1 and the ranged slot in
state 2; otherwise each item hangs at the attachment named by its Item
SheatheType (Item.dbc, item query, `UNIT_VIRTUAL_ITEM_INFO` word 1 byte 1),
oriented by that attachment's bone alone:

| SheatheType | Main hand / ranged | Off hand | Examples |
|---|---|---|---|
| 1 two-hander | 26 SheathMainHand | 27 SheathOffHand | Battleworn Hammer 2361 |
| 2 staff | 30 LargeWeaponLeft | 31 LargeWeaponRight | Monster - Staff, Ornate Priest Staff 2176 |
| 3 one-hander | 32 HipWeaponLeft | 33 HipWeaponRight | Worn Shortsword 25, Worn Dagger 2092 |
| 4 shield | 28 SheathShield | 28 SheathShield | Worn Wooden Shield 2362 |
| 0, 5, 6, 7 | not drawn | not drawn | bows, guns, rods, fist/held props |

A bow is held in the left hand, a gun in the right.

The local character's state is sent with `CMSG_SETSHEATHED`, so other clients
see it. Z (ToggleSheath) cycles unarmed -> melee -> ranged -> unarmed and skips
a state with no item: melee-only 1 -> 0 -> 1, ranged-only 0 -> 2 -> 0 (native
build 8606). Native keeps weapons drawn after combat ends (90 s observed after
/stopattack and combat end); WoWee does not sheathe on combat end either. A model without the
SheatheType's attachment draws nothing while sheathed (HumanMaleCaster.m2,
Kobold.m2): there is no fallback point. Native observation, build 8606, on
GnomeFemale, HumanMale, HumanMaleCaster and Kobold models:
`_experiments/20261117T190000Z.sheath_attachments` in the workspace.

While a unit plays an animation whose AnimationData.WeaponFlags has 0x04 or
0x10 its weapons show sheathed, whatever its sheath state: spell casts
(ReadySpell*, SpellCast*, ChannelCast*, SpellPrecast 4), swimming and Mount
(4), emotes, SitGround and Kneel (16). NOSHEATHE emotes (Emotes.dbc flag 4,
e.g. 70 ONESHOT_WAVE_NOSHEATHE) keep them drawn. Such an animation starting
on the local character also sends `CMSG_SETSHEATHED 0`, so the weapons stay
away afterwards. Native, pair 1: a remote player at sheath state 1 and a
Scarlet Wizard (staff) show their weapons on the back through Guile of the
Raptor (4153) precast; the native client sends 0 about 0.1 s after its own
SMSG_SPELL_START and on /wave and /cheer.
Unverified: Loot (4) and instant casts on the local character.
Evidence: `_experiments/20261117T233000Z.flag400_spokes` (bursts/edc_*, wzc_fix4).

An animation with WeaponFlags 0x20 (Attack1H/2H/2HL, AttackOff, Ready1H/2H/2HL,
Whirlwind, Special1H/2H, ...) draws the melee weapons at any sheath state, and
they stay drawn for the life of the unit's model (`animationSheathState`).
Leotheras the Blind (21215) has sheath state 0 from creature_template_addon for
his whole fight; native holds his glaives through every swing and Whirlwind,
WoWee had them on his back. A Defias Thug set to sheath state 0 shows its sword
at the hip until its first fight, then holds it through Stand and Walk for 50+ s
after combat and through a server sheath change 1 -> 0. The rule lifts only
sheath state 0: a drawn ranged weapon (state 2, a creature's ranged spell) stays
the one in hand. Native and WoWee, pair 16: Defias Watchman (bow, sheath 1 and 0),
Southsea Cannoneer (gun), Redridge Poacher (crossbow), Defias Smuggler (thrown)
hold only the ranged weapon while shooting and the melee weapon in melee.
Unverified: what ends the drawn state other than re-creating the unit.
Evidence: `_experiments/20261118T1330Z.leotheras_glaives` in the workspace.

### Held weapon grips

Palm attachments 1 and 2 apply the model's `HandsClosed` animation (15) at 1 ms
to right and left finger keybones and their descendants. Wrists and arms retain
their body animation. The shared character renderer applies this to NPCs and
players; removing or sheathing the weapon removes the grip. Shield attachment 0
does not force a closed hand. Models without `HandsClosed` retain their authored
pose.

The animation and keybone convention also appears in
[WoW Model Viewer](https://github.com/wowmodelviewer/wowmodelviewer/blob/develop/Source/games/wow/WoWModel.cpp).
`test_hand_grip` checks finger mapping and hierarchy validation.

### Selection circle on the ground

The ring's size is where a sphere of the selection radius r
(`core/unit_size.hpp`) around the unit's position meets the ground under it
(terrain, WMO or M2 floor). A unit h yards above the ground gets a ring of
radius sqrt(r^2 - h^2) and none once h reaches r: a flying or jumping unit
leaves it on the ground, shrinking. `Textures/UnitSelectTexture.blp` is
projected straight down over the square centre +- that radius onto the drawn
terrain and WMO triangles within r above and below the unit: walls and step
sides get vertical streaks, as native paints Northshire Abbey's wall and stair
block; doodads (M2: the abbey's lantern pillars, a cypress) stay bare. It
draws on the world's own triangles, depth-biased; a grid draped over sampled
floor heights let every WMO crease between grid points poke through as a dark
shard (Huhuran's hall in Ahn'Qiraj). Paired captures:
`_experiments/20261118T174257Z.selection_ring_holes/` in the workspace root. A rider's ring is
centred on the mount. The renderer reads the target's instance position when
it records the ring, the frame its model is drawn; the UI sets the target
after the world is recorded, so the ring used to trail a running unit by a
frame. No ring under the local player, even self-targeted. Measured in the
original on the GM Island hilltop with tempspawned flying creatures:
`_experiments/20261118T084140Z.selection_circle_ground/` in the workspace
root. Not established: native also fades the ring as h grows (Infernal 7950,
r 6.45: ring intensity about 0.6 at 2 yd, 0.2 at 3, 0.08 at 4 of that at 0);
WoWee draws it at full strength until it vanishes. Over WMO liquid (the grey
sludge in Huhuran's hall, group 6 MLIQ of `ahn_qiraj.wmo`) native shows the
ring at full strength; WoWee draws its water after the ring, which dims it
there.

### Ground blob shadows under units

2.4.3 multiplies `Textures/ShadowBlob.blp` (32x32, grey 160 in the middle,
white at the rim) onto the ground under every unit: on the ground directly
under a hovering bat or nether ray as under a standing troglodyte. The quad is
the unit's CreatureModelData GeoBox seen from above (x/y extents times the
unit scale), centred on the unit's position, not on the box's centre, and
turned with its facing. It is fogged with the ground. Fitting the texture to
the native ground darkening of 74 sweep cases gave 0.98 of the box extents
(median) and a centre depth of 0.63 of the plate (160/255); Riding Nether Ray
21152 (box x -4.53..1.99) sits centred on the ray. The shadow is drawn on
ground no further than GeoBox height x scale / sqrt(2) below the unit:
hovering 2852 tempspawns kept a full shadow until the ground left that range,
then lost it pixel by pixel (ray 21152 at h 3 / 3.5, the same at scale 0.5 at
1.5 / 1.75, shark 12195 at 1.25 / 1.5, bat 9750 at 1.25). The cut lies between
0.70 and 0.73 of the box height in all four; WoWee uses 1/sqrt(2), a value
chosen inside that bracket (fitted, not derived; finer height steps would
narrow it). WoWee draws it with the
selection circle's projection (terrain and WMO triangles, not doodads),
after the ground and before units (`rendering/unit_shadow.hpp`,
`OverlaySystem::renderUnitShadows`). Not measured: whether ground above the
unit (uphill) takes it, WMO floors, dead units, and the player's own shadow in
first person. WoWee's sun shadow map still darkens units' surroundings by day;
2.4.3 has no unit shadow other than the blob.
Evidence: `_experiments/20261119T082500Z.orient_shadow/` in the workspace
(blob_fit.py, blob_batch.txt, hoverblob.py, sweep_fix).

### Character-style NPC size and head turn

A character-style NPC (CreatureDisplayInfoExtra) is drawn at its display scale
times its race and sex's player display scale (ChrRaces columns 4/5): tauren
male NPCs are 1.35 times taller than their display says, gnomes 1.15, as
tauren and gnome players are. In the sweep scene 3793 and 1624 (1.0) stood
1.35 times WoWee's height, 1941 (1.2) 1.62; tauren female 1905 and human 4397
as WoWee.

Open: in 2.4.3 a standing NPC turns its head and upper body toward the
player character a few yards away, the hips and legs keep the server facing.
Human 4397 at 5 yd: player 15-40 degrees off its facing, the head looks at
the player and the chest follows part way; at 45 degrees and beyond it does
not turn. The Chaos Gazer (beholder, 23266) turns its whole body, which hangs
under its head bone. WoWee does not do this, which is most of the "faces a
different way" of 23266, 3793 and other humanoids in the visual sweep;
range, angle limit, turn rate and how the turn splits between head and spine
bones are not measured. 830 and 17627 differ only by the Stand variation
drawn at that moment (fidget sequences).
Evidence: `_experiments/20261119T082500Z.orient_shadow/headlook/`.

### Chair clicks and object picking

Right-clicking a chair sends the normal game-object use request. Camera drags
are rejected before picking; a previously selected chair cannot activate from
a click elsewhere. M2 cursor spheres use the local geometry box center and
half-diagonal, transformed without rotation-dependent radius inflation or
extra padding. Native TBC chair hover includes gaps between the chair's parts.
The picked object is excluded from its own collision-hull occlusion test.

TBC `GameObjectDisplayInfo.dbc` contains range-box fields 12 through 17. Chair
reach expands that local box by 3 yards. Object cursor color uses the same range
check as interaction; chairs have no name tooltip. When updating extracted
assets, update their `dbc_layouts.json` as well: the running client reads it from
its expansion asset directory, not the source `Data` directory.

Chair stand states retain the server seat heading while mouse, touch, or idle
orbit moves the camera. Leaving the chair restores normal camera-driven facing.

### World mouse clicks

Mouse press and release edges survive between rendered frames. NPC interaction,
loot, and targeting use the event's press/release coordinates, so a short click
is not lost when both edges arrive before the next frame. Virtual controller
mouse buttons retain their sampled held-state transitions.

`WOWEE_INPUT_TRACE=1` logs SDL button/motion events, sampled cursor coordinates,
button mask, and relative-mode state for diagnosing capture problems.

Mouse-look release restores the pointer to its press position. SDL's accumulated
relative movement remains available to click-versus-drag detection; it does not
change the visible cursor's restoration point.

### Switching loot

`build/bin/test_game_replay '[loot]'`

Original 2.4.3 releases an open corpse loot itself; CMaNGOS answers a client
release of a corpse with nothing and does not release corpse A when corpse B
is looted. Measured wire order (pair 4, through a header-decrypting relay):
right-click B with A open sends `CMSG_LOOT_RELEASE` A, `CMSG_SET_SELECTION` 0,
`CMSG_SET_SELECTION` B, `CMSG_LOOT` B; F1 the same with the player's guid; the
close button, Escape or an auto-loot that empties A sends the release and
`SET_SELECTION` 0; a left click on empty ground sends the release and two
`SET_SELECTION` 0. Releasing loot drops the target only when the target is the
looted unit: a clam opened with a corpse or the player selected keeps it, and
selecting another unit leaves the clam's loot open. A second right-click on
the open corpse sends nothing, and a right-click on bare ground does nothing
with a corpse or a living enemy selected. Gotcha: LootFrame rebuilds its
buttons only in OnShow, so a `LOOT_OPENED` while it is shown keeps the old
corpse's buttons. `SMSG_LOOT_RELEASE_RESPONSE` names its loot; one for another
guid leaves the open window alone. Using a game object with corpse loot open
releases it first (native released it before a refused chest use; `.gobject
add` chests and veins answer "That is already being used" natively, so a
chest's loot after a corpse was not observed).

### Merchant refresh and item usability

`build/bin/test_input_integration '[merchant]'`

A merchant row is red when `GetMerchantItemInfo`'s sixth value, isUsable, is
false. One rule (`GameHandler::itemRequirementsMet` plus the vendor's arena
rating) answers it and colours the item tooltip's requirement lines. Measured
on native 2.4.3 with a level 70 human rogue (pair 4):

| Requirement | Row red when unmet | Tooltip line, red when unmet | Example |
|---|---|---|---|
| Class mask | yes | `Classes: Priest` | Big Zokk Torquewrench: Brutal Gladiator's Satin Gloves |
| Race mask | yes | `Races: ...` | not measured |
| Required level | yes | `Requires Level 70` | not measured at a lower level |
| Skill and rank | yes | `Requires Jewelcrafting (360)` | Karaaz: Design: Crimson Sun |
| Required spell | yes | `Requires Spellfire Tailoring` | Cloth Armor Vendor (26301, spawned): Spellfire Robe |
| Reputation | yes | `Requires The Consortium - Exalted` | Karaaz: neutral red, exalted not |
| Weapon/armour proficiency | yes | subclass (`Gun`) | Karaaz: Consortium Blaster without Guns |
| Vendor arena rating (ItemExtendedCost) | yes | `Requires personal and team arena rating of 2200`, merchant tooltip only | Big Zokk: Brutal Gladiator's Leather Spaulders |
| Money, honor, arena points, tokens | no | - | Tristia's Vindicator's bracers with 0 honor and no marks |
| Unique item already carried | no | - | Karaaz: second Consortium Tabard |
| Recipe already known | no | `Already known` (its colour not measured) | Karaaz: Pattern: Fel Leather Gloves |

An item whose template has not arrived answers on the arena rating alone, as
native does: Big Zokk's gloves for other classes read usable until their page
was shown, his rated items were red from the start. The arena rating is the
best, over the player's arena teams, of the lower of personal rating
(`PLAYER_FIELD_ARENA_TEAM_INFO_1_1`, slot field 5) and team rating
(`SMSG_ARENA_TEAM_STATS`); without a team it is zero. Buying a rated item
anyway draws `ERR_CANT_EQUIP_RATING` from the server's inventory result 80,
lacking points `ERR_NOT_ENOUGH_ARENA_POINTS` (70); a purchase prints no chat
line of its own beyond the item push. `.modify arena 5000` gives points for a
test. Honor rank requirements are not shown or checked: no 2.4.3 vendor sells
such an item. Evidence: `_experiments/20261118T1910Z.merchant_usable/` in the
workspace.

Gotchas: ItemExtendedCost.dbc has 14 columns in 2.4.3 and 16 in 3.3.5 (arena
slot after the points, purchase group at the end); both are layouts in
`dbc_layouts.json`, which an installed data tree needs refreshed, and
`PLAYER_FIELD_ARENA_TEAM_INFO_1_1` (1544) is new in the TBC
`update_fields.json`. Without the layout a token vendor shows coin and honor
only and nothing is red for rating. Pair characters are game masters: CMaNGOS
gives a GM faction template 35, so this client's `UnitFactionGroup("player")`
answers nil there and the honor icon is not drawn, while native answers
Alliance.

Selecting gossip sends one selection packet. NPC-text updates cannot select the
same pending option again, and vendor selection does not send an additional
inventory request. Repeated interaction with the same vendor refreshes inventory
with `MERCHANT_UPDATE`, preserving the open shop and backpack.

### Auction sell slot

`build/bin/test_auction_sell_slot`. The Auctions tab's sell slot
(`include/game/auction_sell_slot.hpp`) names a bag slot; the item stays in the
bag, locked (greyed), until it is taken back onto the cursor, swapped out,
posted, or the auction house closes - the last two fire `ITEM_LOCK_CHANGED`
then `NEW_AUCTION_UPDATE`. Dropping an item on an occupied slot puts the old one
on the cursor. Build 8606 refuses items itself and changes nothing else (item
stays on the cursor): a bound item is `ERR_AUCTION_BOUND_ITEM`, or
`ERR_AUCTION_QUEST_ITEM` when its template binds as a quest item (Gnoll Paw
725, Wailing Essence 6464); conjured (flag 0x2) and has-loot (flag 0x4) items
get their own messages. Gotchas: "bound" is the item's ITEM_FIELD_FLAGS bit,
and `.additem` leaves BoP and quest items unbound, so those are accepted;
item class 12 plays no part. An empty slot answers `GetAuctionSellItemInfo()`
with `nil, nil, 1, -1, nil, 0`; a lone nil breaks
`AuctionSellItemButton_OnEvent` and leaves the old item's count and prices on
the panel. Native measurements: `_experiments/20261118T1935Z.auction_sell_slot/`
in the workspace.

### Online spawn floor

Camera reset preserves the server spawn elevation. Floor searches above outdoor
terrain can select an interior ceiling or roof; they must not relocate an online
spawn. Reset invalidates cached floor contact, then normal movement resolves
contact using streamed collision meshes.

### Tilted doodad floors

M2 floor rays query both local floor and wall triangle grids. A barrel placed
on its side can support a player on triangles classified as walls before
placement. Walkability is evaluated using transformed world-space normals.

### Unit tooltip level and loot lines

The level line is built from GlobalStrings as 2.4.3 draws it: players
`TOOLTIP_UNIT_LEVEL_RACE_CLASS_TYPE` ("Level 60 Human Rogue (Player)"),
creatures `TOOLTIP_UNIT_LEVEL_CLASS[_TYPE]` with the creature type and ELITE
for rank 1 and rare elite 2, BOSS for rank 3 ("Level 55 Humanoid (Elite)",
"Level ?? Demon (Boss)"; UnitLevel answers -1 for rank 3), nothing for rare
(rank 4: "Level 54 Humanoid"). A corpse puts CORPSE in the type's place and
keeps the classification ("Level 55 Corpse (Elite)", "Level ?? Corpse
(Boss)"). The creature type appears only when the player is not a friend
of the unit (`UnitIsFriend("player", unit)`, player's reaction better than
neutral): a faction with a standing counts as a friend at any standing short
of at war. Native: Gadgetzan Bruiser and Quinn at Neutral show "Level 67" /
"Level 35", Katrina Turner (friendly template) "Level 70", Kurenai at
Unfriendly no type; Wastewander Bandit "Level 41 Humanoid", neutral wildlife
"Level 9 Beast", Bloodsail at Hated "Level ?? Humanoid". `UnitCreatureType`
still answers Humanoid for all of them; the rule is in the tooltip. Corpses
keep CORPSE either way. Evidence:
`_experiments/20261118T195000Z.unit_creature_type/`. Type 10
(CREATURE_TYPE_NOT_SPECIFIED, "Not specified") is never in the level line,
though `UnitCreatureType` answers "Not specified": Hakkar "Level ?? (Boss)",
Green Sludge and Hakkari Minion "Level 46". A creature of type 0 (Furbolg,
Rankist) answers nil and has no type either. CreatureType.dbc's last column
does not mark this (1 for Critter, Non-combat Pet, Gas Cloud; 0 for Not
specified). All other types are named for non-friends: "Level 1 Critter",
"Level 11 Mechanical", "Level 69 Totem", "Level 60 Gas Cloud", and wild
Non-combat Pets "Level 1 Non-combat Pet" (Cockroach, Orcish Orphan); an own
companion is a friend ("Level 1"). Evidence:
`_experiments/20261118T225800Z.type_not_specified/`. In a group, a corpse's tooltip adds "Master Looter: <name>" under
master loot and "Loot: <name>" (LOOT, MASTER_LOOTER) from SMSG_LOOT_LIST
(corpse guid, packed master, packed current looter). Group loot, need before
greed and round robin name the packet's looter, alternating kill by kill;
free for all sends no list and shows neither; alive tapped units show none.
Measured on GM Island with Edrike and Edrice grouped:
`_experiments/20261118T094616Z.group_loot_tooltip/` in the workspace root.

### Tooltip text layout

```sh
build/bin/test_native_text_metrics
```

GameTooltip text is measured, wrapped and drawn by the original 2.4.3 rules
(`ui/native_text_metrics.hpp`, `ui/tooltip_layout.hpp`): fonts at whole pixel
heights (`floor(height * pixelsPerUnit + 0.5)`), glyphs on whole-pixel pens,
default wrap 230.4 units, box = content + 20.48 units, right text 38.4 units
past the left. Item 28909 "Greater Inscription of the Orb" on a 720-pixel
window wraps its Use: text into five rows at 216 pixels.

Gotchas:

- The original draws with the install's loose `Fonts\*.ttf`, not the archive
  copies. On this install common.MPQ's `frizqt__.ttf` is a different, wider
  face; measuring with it puts every break in the wrong place. `asset_extract`
  copies the loose fonts over the extracted ones; an older extraction needs
  `fonts/frizqt__.ttf`, `morpheus.ttf`, `skurri.ttf` replaced by hand.
- The tooltip's size is computed during layout, so `GameTooltip:GetWidth()`
  and `GetHeight()` read in the same Lua call as `SetHyperlink` return the
  previous tooltip's size; the original answers immediately.
- Every other FontString, chat line, SimpleHTML page and edit box goes through
  the same measure; see "Interface text layout" below.

### Interface text layout

```sh
build/bin/test_text_wrap '[place]'; build/bin/test_interface_text_lines
```

All interface text (FontString, ScrollingMessageFrame, SimpleHTML, EditBox,
GameTooltip) is measured, wrapped and drawn by the 2.4.3 rules of
`ui/native_text_metrics.hpp`: whole-pixel font height, whole-pixel pens,
rows one pixel height apart. ImGui's own fractional advances drew text about
7% narrower with 1.2-line leading; the quest log wrapped "as if you" one word
early and its lines sat 14.5 pixels apart where native has 12. Measured on
2.4.3 at 0.9375 and 0.5625 pixels per unit (FontProbe addon):

- GetStringWidth adds a right drop shadow, round(x * ppu) pixels, and a wrapped
  shadowed string has that much less room (`shadowPixels`). Every MasterFont
  font (GameFontNormal and family) has a (1, -1) shadow, so their labels are
  one pixel wider than the bare measure; the y offset changes nothing.
- SetSpacing adds ceil(spacing * ppu) pixels between rows (`spacingPixels`).
- A line starts at floor(left) + 2 (LEFT), floor(left) + 2 +
  floor((box - width - shadow) / 2) (CENTER), floor(right) + 1 - width (RIGHT);
  the box top snaps to the next pixel down (`placeTextLine`, `textOriginY`).
- Shadows are drawn whole pixels away from the glyphs.

Gotchas:

- Native rasterizes with hinted FreeType; this client uses ImGui's unhinted
  stb_truetype (no FreeType development files in this environment). Pens and
  line starts match; glyph ink can be a pixel narrower or a pixel higher/lower
  than native (stems are blurrier, e.g. FRIZQT "I" two pixels wide).
- An autosized label anchored by TOP or TOPRIGHT can start one pixel off
  native: there native's line start depends on the anchor, not only on the box
  (FP_Place3 data in the experiment below); not modelled.
- GetStringWidth of a multi-paragraph string can differ by a few pixels
  (quest description: native 1237, here 1234 = widest paragraph).

Evidence: `_experiments/20261119T092759Z.questlog_fonts/` in the workspace.

### Quest text reveal

FontString `SetAlphaGradient(start, length)` applies a character-edge alpha fade
and returns 1 while text remains to reveal, or nil when complete. TBC FrameXML
uses this to reveal quest descriptions, then show objectives and enable Accept.
The Instant Quest Text option uses the same path with a completed reveal.
Widget opacity multiplies through the parent chain, so `TextAlphaDependentFrame`
keeps objectives and rewards hidden until the description finishes, then fades
them in. Local `GetAlpha()` values remain unchanged.
`test_input_integration '[quest-text]'` checks markup-aware completion and reset.
`test_widget_tree '[alpha]'` checks inherited opacity and draw visibility.

### Quest log rows and selection

```sh
build/bin/test_quest_rewards '[quest_rewards]'
```

TBC quest queries are read field by field (`QuestQueryResponseParser::parseTbc`).
The quest log rows live in `QuestHandler::questLogRows()`: headers by name
(QuestSorts among zones), quests by level then title, as 2.4.3 orders them.
The selection is a quest, not a row. `CollapseQuestHeader(i)` with anything
but a visible header row folds every header; QUEST_LOG_UPDATE follows after the
script (`GameHandler::queueAddonEvent`).

Gotchas:

- Every quest-log index is a row index, headers counted. Never subscript
  `getQuestLog()` with one; go through `questAtLogRow` / `questLogIndexOf`.
- Quests under collapsed headers answer at indexes past
  GetNumQuestLogEntries (watch frame, selection). 2.4.3 reorders those hidden
  rows between updates; here they keep header order.
- Watches do not persist: 2.4.3 starts every session with none, leaves an
  accepted quest unwatched, and ends a timed watch (AddQuestWatch(i, seconds),
  the autoQuestWatch watch on objective progress) after its seconds.
- "[1]" before a quest title is QuestLogTitleNGroupMates: how many party
  members are on that quest (IsUnitOnQuest). It reads the member's
  PLAYER_QUEST_LOG quest-id fields, which the server sends only to group
  members in sight; UNIT_QUEST_LOG_CHANGED("partyN") fires when they change.

Evidence: `_experiments/20261118T2010Z.questlog_audit/` in the workspace root.
### $-tokens in server text

Gossip, quest and page text keep the server's tokens ("$N", "$c",
"$gsir:madam;") until a Lua getter hands it out; `addons::serverText` then
applies `game::resolveTextTokens` for the logged-in character. Native 2.4.3
resolves in GetGossipText, GetGreetingText, GetQuestText, GetObjectiveText,
GetProgressText, GetRewardText, GetQuestLogQuestText and ItemTextGetText only.
Titles, gossip options, GetQuestLogLeaderBoard and NPC say/yell/emote/whisper
keep "$N" as two characters on native, so they do here.
Rule: `$n` name, `$c`/`$r` class/race lower-cased (`$C`/`$R` as UnitClass/
UnitRace), `$b` newline, `$gmale:female;`, `$tmale:female;` the honour rank
name (PVP_RANK_0_<team> global string, team by race), `$<id>w` a world state,
`$d`/`$e` always 0 on native (source unknown), anything else left as "$".
`build/bin/test_text_tokens` holds native's strings for a human male rogue and
a blood elf female paladin. Evidence and probe scripts:
`_experiments/20261118T203000Z.text_tokens/` in the workspace.

### Scroll lists, sliders and template anchors

```sh
build/bin/test_input_integration '[faux],[anchor]'
```

Values are held as build 8606 holds them. `ScrollFrame:SetVerticalScroll` is not
clamped to the scroll range and fires OnVerticalScroll once per change. Slider
and StatusBar `SetValue`/`SetMinMaxValues` clamp to the range; a slider snaps to
its step counted from the minimum (round half up), the last whole step being the
maximum: 0..50 by 16 holds 0, 16, 32, 50. A new slider spans 0..0. A bar whose value
SetValue has never moved keeps it through `SetMinMaxValues`/`SetValueStep`, with
no OnValueChanged: a fresh slider given 90..270 reads 0. The options panels set
the range before `SetValue(GetCVar(...))`, and their OnValueChanged writes the CVar. The wheel goes
to the topmost wheel-enabled frame under the cursor, passing over frames that only
take the mouse (a FauxScrollFrame's rows are siblings laid over it).

Gotchas:

- An empty script element (`<OnScrollRangeChanged></OnScrollRangeChanged>` in
  FauxScrollFrameTemplate) replaces the inherited handler; it is emitted as an
  empty function.
- `$parent` in a name or an anchor's `relativeTo` means the nearest named
  ancestor. PartyMemberFrameTemplate anchors its debuff ring to
  `$parentPortrait` from two unnamed frames down.
- The wheel glides a scroll bar through several `SetValue` calls where 2.4.3
  jumps half the bar in one; handlers that redraw before reading the new offset
  (MangAdmin's teleport lists) lag by a fraction of a page here and by a whole
  wheel step there.

Measured with `/script` probes on the 2.4.3 client:
`_experiments/20261118T140302.014047Z.faux_scroll_api/` in the workspace root.

### Camera views, zoom floor and contact

SetView(1-5) as build 8606: 0 yd/0 deg (first person), 5.55/-10, 5.55/-20,
13.88/-30, 13.88/-10, camera put back behind the character. NextView (End)
and PrevView (Home) step 1-5 without wrapping; SaveView keeps the current
distance and pitch; ResetView of the current view applies it. SETVIEW,
SAVEVIEW and RESETVIEW are unbound by default. Zoom runs to 0 yd. The camera
stops 0.111 yd short of the surface its ray hits, along the ray, at any
incidence (Shattrath Terrace wall, offline raycast of native camera rays).
WMO triangles flagged MOPY 0x02 do not stop the camera; every other WMO
triangle does (detail, rendered or hull, from either side), and so does M2
collision geometry (Black Temple: golden statue, banner pole, tapestry).
Contact hold: once any obstacle (terrain, floor, wall) stops the camera while
it is below the pivot, pitching up further keeps the camera in place and turns
only the view; pitching below that orbit pitch moves it again (native Elwynn:
orbit held at 5.5 deg, view to 42; Terrace wall: slid along the ray to orbit
-1, held at +2 with the camera 1.65 yd above the feet). Mouse-look turns
cameraYawMoveSpeed/800 deg of yaw and cameraPitchMoveSpeed/600 of pitch per
pixel (defaults 180 and 90: 0.225 and 0.15, equal at 1280x720 and 800x600).
SetCVar keeps 0.1 to 360 for either (a leading number, "200x" is 200) and
keeps the old value otherwise. Interface -> WoWee -> Camera -> Mouse Look Speed
is the game's own slider: 90 to 270 by 10, writing cameraPitchMoveSpeed at
half. mouseSpeed (Mouse Sensitivity, 0.5 to 1.5) is the cursor's speed in the
original; it changes neither mouse-look nor, on Wine, the cursor, and this
client reads it nowhere. Gotcha: cvars.cfg without `wowee_cvarsversion=1` was
written when all three held this client's own 0.05-1 sensitivity; loading it
drops them to the defaults. settings.cfg `mouse_sensitivity` is ignored.

### Following another character

`/follow Graulberto` resolves a nearby character by name without changing the
selected target. `FollowUnit("target")` and party unit tokens use the same path.
The graphical camera controller owns movement, collision and movement packets;
the headless direct-position driver runs only without that controller. Follow
uses render-space facing, stops within three yards and resumes when the target
moves away. Forward/backward, strafe and turn input cancel follow. Follow does
not latch the independent autorun toggle.

### Confused, feared or charmed

`SMSG_CLIENT_CONTROL_UPDATE` with allowMove 0 hands the character to the
server. Movement keys, jump, autorun and mouse steering stop moving or turning
it; the mouse still orbits the camera. CMaNGOS then walks it with
`SMSG_MONSTER_MOVE` splines for the player's own guid. The character's ground
position and facing follow that spline; height comes from the normal floor
physics. No movement packet or heartbeat is sent until control returns. Gotcha:
the spline's Runmode bit is set on every packet (CMaNGOS PacketBuilder), so it
says nothing about gait. Gait comes from spline speed, which is the walk speed
(confuse) or run speed (fear). Charm uses the same path but was not checked
live.
While the server moves the character, CMaNGOS sends root changes as
`SMSG_SPLINE_MOVE_ROOT`/`UNROOT` for the player's own guid (no ack) instead of
`SMSG_FORCE_MOVE_(UN)ROOT`. Big Bad Wolf (Karazhan) stuns with Wide Swipe
30761 and fears with Terrifying Howl 30752; a stun ending during the fear
arrives as a spline unroot. Ignored, it left the character rooted after the
fear (39 s on pair 5) and standing still while the fear moved it. For other
units the ROOT bit is retained with their other spline-synthesised flags;
nothing draws differently from it yet.

### Morphed players

A player whose `UNIT_FIELD_DISPLAYID` differs from `UNIT_FIELD_NATIVEDISPLAYID`
(TBC index 153) is drawn as that creature display, local player included,
and its portrait follows: Little Red Riding Hood 30753 (transform to creature
17602, display 17629, a gnome girl in a red hood) in the original. The
character instance stays (position, facing, animation, attachment points)
but is not drawn; a creature instance with the display's appearance
(CreatureDisplayInfoExtra clothing and helmet) copies its transform,
animation and first-person visibility each frame. Gotcha: the field is new
in `update_fields.json`; an installed data tree without it draws no morph.
Mouse hover, click and the hover highlight test the creature model, not the
hidden character: hovering Red Riding Hood's body shows the tooltip, the air
where the character's head would be does not. Measured side by side with the
original: a morphed player draws no weapons, also with weapons drawn (Z); a
morphed rider sits on the mount as the creature. Not established: where the
original puts a morphed player's name.

### Projected ground textures

`build/bin/test_projected_texture` checks the draping. An M2 skin batch with
flag 0x4 is a projected texture: in the TBC data every such batch is a single
quad (533 of them, mostly under `Spells/`), such as LavaGround in
Flamestrike_Impact_Base and CrackedGround in ThunderClap_Cast_Base (War
Stomp). The original client lays these onto the ground. WoWee rebuilds each
one per frame as a grid with about 1 yard spacing, set on the terrain or the
WMO floor nearest the quad's height. The grid keeps the quad's texture
coordinates (a vertical projection) and draws through the normal M2
material, blend and animation path. Doodad (M2) surfaces do not receive it,
and vertical faces such as step risers stay bare. In UBRS the original also
paints step sides.

### Kalecgos's Spectral Realm

The realm (spell 46019, map 580 at 1704.31, 927.98, -74.56) is WMO group
`sunwell_046.wmo` (indoor 0x2000, no portals, WMOAreaTable "Inner Veil")
128 yards under the plateau terrain. Terrain is drawn one-sided, so from
below it is not drawn, as in native. Sathrovarr's template 1959 (faction
960) is friendly to players by its template; aura 44852 makes the server send
SMSG_SET_FORCED_REACTIONS 960 -> 1 (hostile), 14 -> 4 (friendly), which
override template and standing (CMaNGOS Unit::GetReactionTo). Gotcha: a level
40 test character dies to Sathrovarr within seconds once the reaction is
forced; entering map 580 needs `.gm on` (level 70 + raid otherwise).
The group a point is in is the one whose surface lies nearest below it,
among the group boxes the vertical through the point crosses; the point is
indoors when that group is indoor (0x2000) and the point lies inside some
group box of the building. Group boxes overlap: Black Temple's indoor Den of
Mortal Delights box covers the outdoor Supremus ramp, the Lion's Pride Inn
kitchen box its porch and roof. Unreachable groups (MOGP 0x80: Black Temple 39/40/46/48, Stormwind's
cathedral copies, a few in Darnassus, Shadowfang, Blackrock Spire) have no
collision and never contain a point. With the camera in a reachable group's
box one is drawn (with its doodads) only through an in-view portal of the
camera's or the character's group that leads to it (Illidan's summit, group 2
-> 48). IsIndoors() and [indoors] additionally need
the group to lack MOGP 0x8000 (Undercity, the Great Forge: nil in 8606).
Game objects, their GM quest sparkle (LootFX) and WMO liquids follow the
same rooms: from 120-200 yards above Orgrimmar (pair 13 against native) none
of the city's interior groups is drawn, nor its Mighty Blaze braziers,
signs or pools. Orgrimmar's valleys are terrain inside interior group boxes,
so a point over no WMO surface belongs to every interior-only group whose
box holds it (WMORenderer::interiorRoomsOf). Gotcha: CMaNGOS turns the
sparkle on for every GENERIC, SPELL_FOCUS and GOOBER object (and ready
chests) while `.gm on`; native draws it too (Mighty Blaze with and without
GM mode), so a city full of sparkles is the GM view, not a quest state.
Native WMO movement collision looked one-sided in Black Temple and
Undercity (walls passed from behind, ceilings fallen through from above);
WoWee's is two-sided.
A camera in an interior group from which no portal path reaches an exterior
group (WMORenderer::findSealedInterior) sees only those groups: terrain,
grass, sky and every model or unit outside their box are not drawn, and the
background is the WMO fog colour. Native shows (59,28,107) there; MFOG 0 is
(56,27,98). Ironforge, Undercity, the Deadmines and Blackrock Depths reach an
exterior group and are unaffected (checked at their teleport points). Units in
the realm are lit by the floor MOCV under them (interiorObjectLight): the
player there gets (0.31,0.31,0.70), and native's player is the same dark
blue. Native creatures facing the camera (Sathrovarr, chest (59,43,28) on
a grey texture) show a warm lit side that WoWee, replacing the light by the
MOCV, does not draw (open). `WOWEE_INTERIOR_LIGHT_TRACE=1` logs each unit's
interior light. Spectral Blast 46648's SpellVisual 10441 plays ImpactAreaKit 9254
(Shadow_Fissure_Base, pink beams and shards) at the SMSG_SPELL_GO destination
besides PersistentAreaKit 9566 on its DynamicObject; TBC column 23 is
ImpactAreaKit.

### Tab targeting

`build/bin/test_tab_target` checks the rule. Tab (TargetNearestEnemy) and
TargetNearestFriend pick only units within 30 degrees of the character's
facing, on screen and within 41 yards (3D), nearest first; each further press
steps one unit farther and wraps. A press more than 3 seconds after the
previous one starts again at the nearest. Corpses are skipped. These values
were measured on the 2.4.3 client; `include/game/tab_target.hpp` lists the
measurements. Gotcha: the native client also sometimes appends units within
about 10 yards outside the cone after the cone units; that set could not be
reduced to a rule and is not reproduced.

### Duel requests

The duel menu casts spell 7266 through the active expansion packet parser.
TBC writes spell ID first, then cast count and target mask; it has no Wrath
cast-flags byte. `test_input_integration '[duel]'` checks the actual Lua menu
API through the outgoing packet, including the opponent GUID.

### One-shot world effects

Duel arbiters play Spawn (animation 145) once, then Stand (animation 0).
M2 sequence order does not identify the idle sequence: DuelingFlag.m2 stores
Spawn first. Level-up effects start Stand at time zero and hold its final pose
until effect cleanup; they do not loop during the particle drain interval.
Both skeletal and particle-only instance clocks honor animation speed and
hold-at-end playback.

Duel hostility follows public PLAYER_DUEL_TEAM and PLAYER_DUEL_ARBITER updates.
Opposing nonzero teams with the same flag are hostile even when factions match.
Zero teams retain normal faction reactions during invitation/countdown and after
completion. Player field updates refresh cached hostility and UNIT_FACTION UI.

Duel countdown uses the localized DUEL_COUNTDOWN chat string once per remaining
second. Right-clicking a hostile player starts auto-attack; active duel opponents
use the red selection circle regardless of PvP flags. Player creation refreshes
hostility for both duel participants, including reconnects where the opponent
arrives before the local player. UnitCanAttack uses the live reaction rather
than cached presentation state. Friendly target cycling excludes opponents.

Each unit's hostility (`Unit::isHostile`: plate colour, name colour,
UnitIsEnemy) is cached from the faction maps when it appears. World packets
are applied as they arrive, so at login the units around the player - a party
member beside them included - exist before the world loader builds the maps,
and against no map every template counts as hostile. Building the maps
(`GameHandler::registerFactionMapBuilder`) uses the player's live faction
template (35 while a game master) and re-judges every existing unit.
`test_input_integration '[faction]'` checks it. Gotcha: when the live template
equals the race's (a non-GM player), nothing else re-judges those units; a
party member's plate drew red and UnitReaction answered 2. Native shows a
grouped player's plate blue.

### Teleports and map transfers

Native 2.4.3, measured on a private pair with `.go xyz`, `.tele`, `.appear`
and `.summon`: a short hop on the same map moves the player at once. A far
teleport on the same map (GM Island to Gadgetzan) shows no loading screen;
the old frame stays on screen until the destination is loaded, then the
loaded destination appears. A map change shows the loading screen. Native
sends MSG_MOVE_WORLDPORT_ACK only when its load is nearly done: the server
cannot find it for the whole loading screen, and a summon during that time
answers "(offline)".

WoWee re-centres terrain streaming on the destination of a same-map teleport
(`TerrainManager::streamFrom`, nearest tile first). It does not hold the old
frame: for about a second after a far hop the character can stand in a
partly streamed world. The worldport acknowledgement waits for the
destination terrain (start of warmup), and a world entry that arrives while a
load runs is deferred to the end of that load.

Gotcha: with `WOWEE_VIEW_SYNC` set, a camera "hold" request pins the camera,
and terrain streams around the camera, not the character. A request file
written before the client started is ignored; one written while it runs
holds until a `release`. A held camera looks like "the world stayed behind
after a teleport": the minimap and tooltips follow the character, the view
does not, and the own model is out of sight.
Evidence: `_experiments/20261118T210500Z.teleport_world/` in the workspace.

### World map (FrameXML WorldMapFrame)

M opens FrameXML's WorldMapFrame (the world map element is handed over); the
ImGui map in `src/ui/map_window.cpp` is only the separate second-monitor
window. WorldMapFrame draws tiles, overlays, landmarks, highlight and party
markers itself; every answer it gets comes from
`rendering/world_map/MapNavigator` (all continents at once, DBC data),
not from the ImGui map's facade. Only the player arrow is drawn by the client
(2.4.3 creates it in code), at the position GetPlayerMapPosition answers,
28 units of the 1002-unit frame.

Native 2.4.3 answers, read through its own Lua API on a private pair:

- GetCurrentMapContinent: -1 cosmic, 0 world, 1 Kalimdor, 2 Eastern
  Kingdoms, 3 Outland. GetMapInfo is nil at world and cosmic level.
  SetMapZoom(c, z) with z past the zone list shows the continent.
- Landmarks: AreaPOI flag 4 on zone maps, 8 on continents, 16 on the world
  sheet, and only when the POI's area is explored (world-state POIs, flag
  128, when their world state is set). Zone-level landmarks answer texture 15,
  an empty POIIcons cell: they are hover names only. GetMapLandmarkInfo
  returns five values in TBC.
- UpdateMapHighlight: zone maps name the explored overlay under the cursor
  (its first area) or a neighbouring zone (by the continent ZMP); continents
  give the zone and its highlight art; the world sheet gives the continent
  rectangle from WorldMapContinent: x = 501 + 12*(column-32) + 16*offsetX
  (scale 0.75), the same for rows with 334. Numbers are 0, not nil, without art.
- ProcessMapClick: world -> continent, continent -> zone; zone maps do nothing.
- Inside an instance the map opens on the zone of its Map.dbc entrance
  (Karazhan -> Deadwind Pass) with no player arrow.

Gotchas: this client's "canonical" X/Y are the server's exchanged
(core::coords::serverToCanonical); the navigator uses the DBC convention (X
north), so bindings swap. Exploration differs per character: compare two
characters only after copying exploredZones between them.
Evidence: `_experiments/20261118T223042.worldmap_audit/`.

## TBC interaction compatibility

`build/bin/test_input_integration '[guard-gossip],[npc-attack],[mail-coin],[mail-stationery],[gm-mail]'`
checks server gossip dispatch, direction-map refresh, NPC attack immunity,
coin artwork boundaries, and the TBC stationery callback.

Creature query `iconName` selects service cursors such as `Directions`.
Reaction color and attack permission differ: neutral NPCs may be immune to
player attacks or belong to a peaceful reputation faction. Gossip selection
sends the selected option to the server; destination labels such as "Auction
House" do not identify service activation requests. Guard destinations enter
both the FrameXML map landmark API and the minimap POI atlas renderer.

A usable game object's cursor, as build 8606 shows it
(`include/game/game_object_cursor.hpp`): a `GO_FLAG_LOCKED` object is
`UnableInteract` whatever the player knows or carries (Battered Footlocker
179486 with Pick Lock, Coilskar Chest 184716 with its key); a quest giver
object shows its status's `Quest`/`QuestTurnIn` (see quest marker section); lock slot 0 naming a
LockType cursor shows it without any skill check (Peacebloom `GatherHerbs`
without Herbalism); otherwise the type's cursor - the gear for chests, `Mail`
for mailboxes, `Buy` for guild vaults. Out of reach adds `Unable`. Gotcha:
only lock slot 0 counts - Alliance Strongbox 4095 (Lock 23, Lockpicking in
slot 1) is the gear. Native table: `_experiments/20261117T195204Z.pair0_go_cursors`.

A unit's cursor (`include/ui/world_cursor.hpp`), measured on spawned NPCs in
build 8606: a lootable corpse shows `Pickup` (the bag; not `LootAll`); a living
NPC its quest status's cursor, then Innkeeper, the server icon (Directions for
guards), RepairNPC, Trainer, Pickup for vendors, Buy for bankers and
auctioneers, Taxi, Speak for gossip and battlemasters; a stable master or a
quest giver with nothing for the player the plain pointer. Beyond 5.5555 yards
(3D, from the unit's position, whatever its size) the service cursor is the
`Unable` one, and right-clicking sends no gossip hello: native opened no
panel there for Marshal Dughan or Vaelastrasz (3D 5.536 yd opened, 5.562 did
not) whatever either unit's UNIT_FIELD_COMBATREACH, UNIT_FIELD_BOUNDINGRADIUS
or the player's scale, while CMaNGOS would answer out to 5 yd plus both combat
reaches (about 19.5 yd for Vaelastrasz). `build/bin/test_game_replay '[gossip]'`.
A game master (`.gm on`: the server makes the player immune to
NPCs) gets the plain pointer over attackable creatures, as in the original.
Gotcha: the corpse loot range is different and not modelled - the original's
bag turns `UnablePickup` at about 6.7 yards for a Clefthoof, 7.0 for a Warmaul
Brute and 11.6 for Gruul; table in
`_experiments/20261117T235000Z.pair8_unit_cursor_cavein/README.md`.

Readable game objects (type 9), including the Valley of Heroes monument
plaques, use `Interface\\Cursor\\Inspect.blp`. Out-of-range readable objects
use `UnableInspect.blp` through the shared interaction-range check.

A game object takes cursor, name tooltip and click only while it regards the
player better than unfriendly (`CGUnit_C::CanInteract`, mirrored in CMaNGOS
`Relations.cpp`): its creator's reaction when a unit created it, else its
`GAMEOBJECT_FACTION` template's, else neutral. The Blackrock Depths rune
doodads (`Doodad_DarkIronDwarfRune_*`, template 114) are hostile to players
and are scenery. Gotcha: this needs `GAMEOBJECT_FACTION`/`GAMEOBJECT_CREATED_BY`
in `update_fields.json` and `Friend0..3` in the `FactionTemplate` layout; an
installed data tree carries copies of both files that do not refresh.

Unit cursor picking intersects the active animation's authored bounding box,
transformed with the rendered instance. Unanimated models use their bind-pose
geometry box. Collision bounds can retain human dimensions on larger creatures
such as ForceofNature and must not determine their hover area. Picking accepts
gaps between limbs. Prop occlusion
uses visible triangles, so openings in objects such as candelabra remain
transparent to picking. Do not replace unit hover volumes with skinned-triangle
hits when changing scenery occlusion.

TBC mail uses `StationeryPopupButton_OnClick(index)`. Coin attachments use
`Interface\Icons\INV_Misc_Coin_01` through `_06`, including separate
single-coin and ten-coin denomination thresholds. WMO area names use the
MOGP authored group ID, which differs from the numbered group filename.

GM stationery (61) identifies support mail, including console mail with sender
GUID zero. Its sender label is `Blizzard Support`; `GetInboxHeaderInfo` reports
`isGM` and disables replies independently of player-name resolution.

## TBC patrols, learned forms, and visual kits

`build/bin/test_input_integration '[tbc-patrol],[learned-forms]'` checks patrol
restoration from create packets and the learned-spell stance-bar refresh.
TBC create splines contain padded control points, elapsed time, and total
travel time. Discarding them leaves moving NPCs at their initial position until
the next segment. Single-segment updates start at the position in their packet.

`SMSG_PLAY_SPELL_VISUAL` carries a `SpellVisualKit.dbc` ID, not a
`SpellVisual.dbc` ID. Kit 406 contains animation 61 and the food-heal effect;
it schedules one eating gesture. A seated gesture preserves the lower-body
pose and waits for sit-down to finish. Food auras do not loop the gesture.
Learning spells emits `UPDATE_SHAPESHIFT_FORMS` so the stance bar refreshes.

Caster animations come from the spell's kits, for the player, other players
and creatures alike: `Spell.SpellVisualID` -> `SpellVisual` Precast/Cast/
ChannelKit -> `SpellVisualKit` column 2 (`AnimationData` id, -1 none). The
precast animation is held while a cast bar fills (`SMSG_SPELL_START` with a
cast time), the cast animation plays once at `SMSG_SPELL_GO` (instant, timed
and triggered casts), the channel animation is held for `MSG_CHANNEL_START`.
Scarlet Wizard Arcane Explosion 8439: no hold, SpellCastOmni (54). Frostbolt
9672: ReadySpellDirected (51), then SpellCastDirected (53). Scarlet Chaplain
Renew 8362 and Power Word: Shield 6066 use ReadySpellOmni/SpellCastOmni (52/54)
even on an ally; the target does not choose the animation. Evocation 12051 holds
ChannelCastOmni (125). `SpellVisualKit` column 1 is not an animation: it holds
0-7 and groups kits by stage. A remote caster's hold is its idle while casting,
so a movement update that stops the unit resumes the cast pose.
A remote unit holds its aura's `StateKit` animation as its idle while the aura
lasts (latest applied aura with one): Rend Blackhand's Whirlwind (13736, kit
370) and Brood Power: Bronze (22291, kit 5769, applied with `.aura`, no cast)
spin with Whirlwind (126) in the original. A held animation other than Stand
also replaces walk/run/jump, for remote units and the local player alike
(`CharacterAnimator::setAuraHoldAnimation`): native spins a running or jumping
player and a following creature under Whirlwind 13736, holds SpellCastOmni (54)
while moving under Consuming Shadows 17715 and ReadyRifle (48) under Hold
Rifle 16723. About 110 spells (Shadowguard 18137) have a Stand state kit; those
units keep walking and running. Casts, swings, hit reactions, stuns and sitting
still play over the hold. Evidence:
`_experiments/20261117T2335Z.aura_state_locomotion/` in the workspace root.
An impact kit's animation plays on the unit it lands on, local player
included: Knockdown (5164/16790) and Backhand (6253) impact kit 348 throw the
target down with Knockdown (121). The local player's kit animation plays once
over its combat state and the stun the spell applies (`CombatFSM::triggerKitAnimation`).

Kit effects on a unit (precast, cast, impact, aura and channel kits) are as
large as their attachment: the unit's model scale (OBJECT_FIELD_SCALE_X x
CreatureDisplayInfo x CreatureModelData scale) times the attachment bone's
authored scale. BaseEffect is attachment 19 (Base), whose bone is 0.5 in
GnomeMale, 1 in HumanMale, 1.8 in Infernal and 5.5 in Dragon; Dragon's Chest
(34) bone is 4.5. Vaelastrasz's Corruption glow (23642, kit 6229,
Shadow_Precast_Uber_Base, display 13992 at 1.5) is therefore several yards
across. Measured with Ice Block's cube on a morphed and `.modify scale`d
creature in the original: cube width / that scale is 3.8-4.4 yd for every
model and scale. WorldEffect (Entangling Roots) stays at the unit's position,
unscaled; native draws it equal on a gnome and a human but far larger on a
dragon, a rule not yet established. Evidence:
`_experiments/20261118T075132Z.fxscale_unit_size/` in the workspace root.

Channel beams come from the channel's fields, not from `MSG_CHANNEL_START`:
`UNIT_CHANNEL_SPELL` (TBC index 165) and `UNIT_FIELD_CHANNEL_OBJECT` (20/21)
stay set for the whole channel, so a unit that was already channelling when it
came into view still shows it. Netherstorm Trigger's Green Beam (33346) at
Forge Camp: Mageddon is cast every ten minutes and was never seen starting.
`SpellVisual.ChannelKit` -> `SpellVisualKit` CharProc 0 names a
`SpellChainEffects.dbc` row in CharParamZero (all 284 such TBC kits do; Green
Beam kit 6674 -> 269 SoulBeam.blp, Drain Life kit 402 -> 8). The beam is a
camera-facing strip from caster to target, both at attachment 34 (Chest);
WoWee does not draw helper models (InvisibleStalker.mdx), so their bind-pose
chest is read from the model file. Measured against the original at Mageddon
(`_experiments/20261117T230336Z.channel_beam_fel_sigil/`): Width is half the
strip, the wave is
`WaveHeight * sin(WaveFreq * s - WaveSpeed * t)` with s yards from the caster,
in the plane of world up turned about the beam by Min/MaxWaveAngle (plus
spin), and colour/alpha/blend come from the record's bytes. The TBC record is
173 bytes: dwords 0-38, then Alpha, Red, Green, Blue, BlendMode bytes at +156.
After it: Combo (string offset) +161, RenderLayer +165, TextureLength (float)
+169, not read. Rules in `include/rendering/chain_effect.hpp`, measured on paired
captures of one server beam (`_experiments/20261118T1328Z.hydross_beam_scroll/`,
`_experiments/20261118T221857Z.chain_rate/`): one copy of the texture spans the
beam at any length and moves TexCoordScale beam lengths a second, toward the
target when positive. The chain textures hold two waves, so a point sees
2 x |TexCoordScale| crests a second: native ManaBeam (chain 4, -2) at 24 yd and
SoulBeam (chain 30, Archimonde's Drain World Tree 39140, +2) show brightness
periods of the whole beam at 2.0 Hz and half the beam at 4.0 Hz; WhiteBeam
(1.66) crests 3.34 Hz. Drawing TextureLength (2.0 on 317 of 320 rows) copies
doubled the crests and their rate: Hydross's beams, Archimonde's soul beam and
the SSC lightning looked "animated way too quickly". Gotcha: on a loaded
machine WoWee rendered 6-9 fps, and an 8 Hz crest rate aliases to about 4 Hz in
a kymograph; compare spatial periods against the beam's pixel length, not
temporal peaks alone. RopeBeam rows 473/510/511 (TextureLength 1.5) are not
measured. Interior vertices stray up to NoiseScale x
length across the beam, redrawn every MinDurBetweenJoints when that is set.
Native bends of a beam between still units stay put (chain 4 at 24 yd: the
bends averaged over 0.5 s change 0.3-1.0 px from one half second to the next
over 5 s). When the units move and the segment count changes, the offsets are
read at the same fractions of the beam; drawing new ones every time made
High Priestess Jeklik's Mind Flay (chain 4) a jagged rod jumping every frame.
A channel spell with EffectChainTarget above 1 runs on from its target through
its other SMSG_SPELL_GO targets (hits, then misses) in packet order: native Jeklik's Mind Flay
23953 (chain targets 5) cast at one of two rooted, stunned Bloodwarder Vindicators (both
listed as misses) runs caster -> first -> second (`mindflay_hop_native.jpg`); the channel fields name
only the first.
The beam bows ArcHeight yards at its middle (half a sine), in the plane of
Min/MaxArcAngle plus spin. Combo is 4-byte entries naming further rows, ID
bits 0-6 in the first byte, bit 7 as 0x02 and bit 8 as 0x20 of the fourth
(214 of 245 TBC entries then name existing rows, nearly all just after their
parent); each named row is drawn whole, with its own shape and look, between
the same ends. Tear of Azzinoth Summon Channel 39857 (237 -> 239 twice): three
interweaving green waves, as native. Sunshatter 45215 / the Black Temple door
channel 41269 (215 -> 211, 209 x4, 217 x3): green bolt, blue bolt, broad
pink-white wave and thin purple bolts, all seen natively; native's come and go
(pulse), here they are always on. Gotcha: `.cast 41269` (script target
23412) on a tempspawned trigger outside Black Temple coincided with a world
server crash; use `.cast 45215` (same chain, dummy aura) on any unit.
Not implemented: pulse, the Combo fourth byte's 0x10, blend 6, chains on
impact kits. Pulse, natively (Open Portal 46801, chain 411: PulseSpeed 100,
PulseOnLength 2.5, PulseFadeLength 15): a strand turning on is lit first at
the caster and its lit part reaches the target in about 0.15-0.2 s
(`openportal_pulse_native_seq.jpg`); WoWee turns whole strands on and off.
Unverified: the arc's shape; a native beam just started (chain 4) is more
jagged in its first 0.1-0.2 s than after (rms bend 4.6 px, then 2-3 px). Open: native draws the Hydross
Beam Helpers (InvisibleStalker, display scale 3) about 11 yd below their server
position (.gps z 38.81, floor 38.75): nameplate ~3 yd and beam start ~5-6 yd
below it, a beam aimed at the helper ends there too; WoWee draws the unit at
the server z, so its beams start ~6 yd above and are ~25% longer. The idle and
channel (124) poses put the chest at +1.8 (x3), so the pose is not it. Open:
the dark quad at ManaFunnel_Impact_Chest's centre is its emitter 1
(Gradient64FlipA tail, flags 0x429, blend 2, colour keys all 0,0,0); native
shows a white core there. Channel kit effects of a channel
known only from the fields are held until the fields clear.

A CastKit's chain is drawn at `SMSG_SPELL_GO` from the caster to every
target in the packet, hit or missed, else to its destination, for as long as
the kit plays (its longest effect model). Kits name the SpellChainEffects row
with CharProc 0 (83 cast kits) or CharProc 12 (33 cast kits, all naming
existing rows: Chain Lightning 421 kit 321 -> 1 Lightning, Chain Heal 1064
kit 3169 -> 2 HealBeam, C'Thun's Eye Beam 26134/32950 kit 6480 -> 18
SoulBeam). Build 8606, a Voidwalker casting Lady Vashj's Forked Lightning
38145 (kit 965, chain 95 ShockLightning, Lightning_Cast_Hand 1 s) at players
it missed: lit 0.92-1.0 s, one bolt per missed player. Native's bolt is
jagged and bows above the line (ArcHeight dword 26: 0.9 here, 5 on chain
260); drawn with the channel beam's noise and arc, not re-measured against
native. Native Eye of C'Thun's Eye Beam (chain targets 60) is a green wave
from the eye to the first player, then on to the second; WoWee drew nothing
before reading CharProc 12 (`_experiments/20261118T1800Z.cthun_beams/`,
hop2_*.jpg). A kit with no effect model lasts the spell's duration: Spite
41376 (Essence of Anger, kit 7979, chain 220, 6 s aura) held its beams 6.1 s.
Gotcha: with neither a model nor a duration the chain is not drawn: Wrath of
the Astromancer 42784 (kit 8183, chain 260), which native shows for
0.24-0.32 s as a tall pink arc from the player to the spell's random
destination. Evidence: `_experiments/20261118T124000Z.astromancer_glow/`
(fork*-pairs.jpg, w84*.jpg, spite2.json). Unverified: CharParamOne of
CharProc 12 (1 on most kits, 3 on Chain Burn 24684) is not read; Eye Beam's
lit time against native was not measured.

Where each SpellVisual stage plays (build 8606, same experiment):

| Stage | Placed at |
|---|---|
| PrecastKit, CastKit, ChannelKit | caster, slots at its attachments |
| ImpactKit | each hit target (missile: when it lands) |
| StateKit | the aura's owner |
| InstantAreaKit (col 22) | `SMSG_SPELL_GO` destination (TARGET_FLAG_DEST_LOCATION), unattached; none without one |
| ImpactAreaKit (col 23) | the destination (missile spell: when the missile gets there) |
| PersistentAreaKit (col 24) | the DynamicObject |

InstantAreaKit examples: Bonechewer Behemoth's Meteor 41276 (flags 0x42,
dest = its victim) drops Meteor_Impact_Base on the player, not the caster;
Smoke Bomb 7964 (0x60) and Summon Burning Exile 4929 (0x40) carry the
caster's own position as destination and play there; Volcanic Geyser 42052
(0x20, source only) plays none. Spell-hand (21/22) and breath (17) slots on
a model lacking that attachment are not drawn (Soul Scream 41545 on
WellofSoulsPain.m2); other slots fall back to the unit position.

A missile spell's ImpactAreaKit waits for the missile to reach the
destination, SPELL_GO + distance / Spell.dbc Speed, also when the spell hit
units (those get their own missiles). Missiles fly the full distance / speed;
there is no cap. Hyjal's Giant Infernals arrive by Infernal 32148 (cast by an
Infernal Relay 18242 at an Infernal Target 21075, visual 7920: Infernal_Ball
missile, speed 20, no ImpactKit, ImpactAreaKit 9322 infernal_impact at
effect scale 2: the green column, shards and burning ground). The server's
TRIGGER_MISSILE then casts 32150 (damage, stun) and 32151 (summons 17908).
Native 2.4.3 (pair 13, relay 116 yd away, `.cast back 32148`): the ball falls
for about 6 s and the column rises where it lands, nothing at SPELL_GO. WoWee
played the kit at SPELL_GO (the spell hit two units) and cut the ball's
flight at 4 s, so on a 187 yd Hyjal relay the column was gone 9 s before the
infernal arrived. Same rule: Infernal Rain 32785/33814, Hellfire's Infernal
33240/35431 (kit 9320, scale 5), engineering grenades (Iron Grenade 4068,
Bomb_ExplosionA). Open: native's area impact starts about 0.4 s after the
missile lands (infernal and Iron Grenade alike), WoWee's at the landing; the
native ball's particles linger after it lands, WoWee removes them with the
ball. Evidence: `_experiments/20261118T221323.261150Z.infernal_landing/`.

Breath (17) and Special (23-25) slot effects take their attachment bone's
full orientation every frame, so a breath follows the head through the cast
animation; other slots turn with the unit's yaw only. Dragons breathe from
SpecialEffect0 at 23 (the mouth): Onyxia's Flame Breath 18435 (cast kit 4450,
dragonflamebreath.m2, SpellCastDirected 53) natively dips with the lowered
head, runs level, then curls upward as the head lifts at the end; it is not
aimed at the target, and from melee range it passes over the victim. With
yaw only it was a level stream throughout. Ebonroc's drake (display 6377)
casting Shadow Flame 22539 (kit 5851) breathes steeply down to the ground in
both clients. Measured on a script-less creature morphed to the display with
`.modify morph` and forced with `.cast target`, so no other cast animation
interrupts 53. Evidence: `_experiments/20261118T1700Z.onyxia_breath/` in the
workspace. Open: native draws the fire as about a dozen separate puffs, WoWee
as a dense continuous stream; Onyxia's Wing Buffet 18500 started right
after the breath replaced WoWee's breath animation with its precast 54 (wing
spread, head down, the fire pointing at the ground); native showed no wing
spread in that one capture (`.cast target` while her AI ran).

Joints (fields 9-18): JointCount points evenly between the ends, each
straying up to JointOffsetRadius yards, times MinorJointScale every
JointsPerMinorJoint-th joint and MajorJointScale every (JointsPerMinorJoint x
MinorJointsPerMajorJoint)-th; the beam runs straight between joints. They walk
to new random spots at JointMoveSpeed / MajorJointMoveSpeed yd/s, redrawn at
most every MinDurBetweenJoints. Flicker (fields 32-35): a row is drawn for
U(Min,MaxFlickerOn) s, hidden for U(Min,MaxFlickerOff) s, repeatedly. Native
Open Portal from Shattrath 46801 (chain 411 drawn eight times: 27 joints,
0.07 / 3.7 / 8.45, flicker 0.3-0.5 on, 0.4-0.65 off) over 24 yd: 1.33 strands
visible per column, spread 0.5-0.9 yd; WoWee before: one straight dense
bundle, after: 1.65 strands, 0.7-1.7 yd (strands wider than native's). The
same rows are Akil'zon's Electrical Storm 43648 (279) and Electrical Arc
43653 (278), not cast natively here. Unverified: radius in yards vs a
fraction of length (only one length measured), whether joints move or jump,
pulse (PulseSpeed/OnLength/FadeLength). Public format docs name fields 16-18
JointSmoothness, MinDurationBetweenJointJumps, MaxDurationBetweenJointJumps;
WoWee reads 18 as the major joints' speed (0.72 on 278/279/411) and ignores
16; not checked against native. Natively a lit chain 411 strand keeps nearly
the same shape over 50-80 ms frames. Hex Lord Malacrass: Siphon Soul 43501
is chain 272 (flicker 0.48-0.66 / 0.26-0.55, joint radius 0) plus 273 from
Combo, so it now breaks up; its beam end height and Spirit Bolts 43383 (aura
23 triggering 43382, AoE enemies within 28 yd at the caster, missile 3435)
are not compared: a tempspawned creature's `.cast target 43383` channelled
but the server sent no 43382.

A channel whose ChannelKit names no beam draws its StateKit's (the channel's
target holds that aura): Black Temple's Sister of Pain (22956) and Sister of
Pleasure (22964) channel Shared Bonds 41363 (visual 8244, StateKit 7030 ->
chain 2 HealBeam) at each other once in combat; native shows the yellow beam
between them (pair 15, `.damage 1` on a tempspawned pair). Open: native shows
7030's HolyDivineShield_State_Base dome only on the first channel's target
(Pleasure) although both hold 41363 (`.list auras`); WoWee shows it on both.
A cast kit beam of a spell with EffectChainTarget above 1 hops caster ->
first hit -> second hit in SMSG_SPELL_GO order (native Sinister Beam 40859,
chain targets 10: two bolts meeting at the first target); others fan out.
Open: BlendMode 4 rows (23, all with black-on-white `*_Inverted`/WebBlack
textures; Mother Shahraz's beams 40827/40859-40861 -> 173-179) natively show
a thin purple bolt with dark edges; WoWee adds the texture and draws a broad
pale band. Adding the inverted texture made it thin but pink-white, so that
rule was not kept.

When a kit effect ends - its one play or the stage length (cast time,
channel, aura, DynamicObject), a Decay, or an interrupted precast - its mesh
and ribbons go at once, its emitters stop, and the particles already out live
out their lifespans (`M2Renderer::retireInstance`). Build 8606, Evocation
12051 interrupted by moving after 3.6 s: CycloneWater_State's cone and the
Lightning_PreCast_Low_Hand ribbons are gone the next frame, the smoke at the
feet (emitter 4, 3 s life) stays and fades over about 0.8 s. Azurous's Frost
Breath 16099 (cast kit 2610, FrostBreath.m2: Stand 2.33 s, mist rate 70,
3 s life) thins out over about 1.9 s after its peak; WoWee cut it from full to
nothing in one frame at 2.33 s and now thins it over about 2.3 s (same breath,
paired capture). Aura removal without Decay is the same: `.unaura` of
Blazerunner's Aura 13913 (CycloneWater_State) leaves the smoke for about
0.8 s. Of the TBC kit
models, 316 cast-kit, 136 impact-kit and 188 channel-kit uses are still
emitting at the end of their Stand. Not compared: missile models at impact
(still removed at once), the drain of a unit's effects when the unit leaves.
Evidence: `_experiments/20261118T211000Z.azurous/` in the workspace.

Every spell effect model is drawn at its `SpellVisualEffectName.Scale` (TBC
field 4), on top of the unit/attachment scale. One file has rows at several
sizes: `Shock_Missile` 1, 4 and 8 - Void Reaver's Arcane Orb (34172, visual
8159, effect 3431) is the 8 and natively a ball several yards across even
when a human casts it (`.cast 34172`); `icebarrier_state` 0.5/1/2/4.
Evidence: `_experiments/20261118T121312Z.glaive_glow/castorb-*` in the
workspace root. Gotcha: needs the `Scale` field in the installed
`dbc_layouts.json`; without it every effect is drawn at 1 (one warning).

A DynamicObject's PersistentAreaKit plays its models at the area's centre for
the object's life, and a kit with CharProc 9 also drops impacts across the
area: CharParamZero picks the model (0 Blizzard_Impact_Base, 1
RainOfFire_Impact_Base, 2 CallLightning_Impact, 3 FlamestrikeSmall_Impact_Base,
4 DeathAndDecay_Area_Base, 5 ArcaneShot_Area, 6 StarShards_Impact_Base, 7
Fel_RainOfFire_Impact_Base, 8 HellfireRaid_Dust_Impact_Base), CharParamOne is
impacts a second, each one play of the model at a random point within the
radius of the spell's PERSISTENT_AREA_AURA effect. Gruul's Cave In (36240,
kit 9373: Smash_Impact_Chest glow at the centre, rocks of index 8 at 5 a
second) and Rain of Fire (5740, about five fireballs a second) measured
against the original: `_experiments/20261117T235000Z.pair8_unit_cursor_cavein/`.
Gotcha: CharParamOne0-3 are new `SpellVisualKit` layout fields; an installed
data tree's `dbc_layouts.json` does not refresh, and without them no impacts
are drawn (one warning). Not implemented: CharProc 9 on other kit stages
(Rain of Fire 42223's InstantAreaKit 9160), impacts on uneven ground (all land
at the area's height), the original's impact positions and orientations
(uniform in the disc, unrotated, evenly spaced in time here).

Small solid M2 props keep their geometry even when they contain multiple
emitters. Spherical emitters use their authored area and radial velocity;
flag 0x100 selects vertical velocity. Ordinary particle colors retain their
authored values. `build/bin/test_m2_classifier_foliage` and
`build/bin/test_m2_ribbon_tracks '[m2][particle]'` cover these rules.

Mounted NPCs use the same mount-display field and mount instances as remote
players. `EntityController` dispatches mount changes for every unit;
`EntitySpawner` resolves either a creature or player render instance. Creature
updates place the horse on the sampled floor, raise the rider to the saddle,
and animate locomotion on the mount while keeping the rider in the mount pose.
Despawn removes both instances. Mounted nameplates use attachment 29 on the
posed rider. General Marcus Jonathan uses mount display 2410.

A mount is drawn at its own display and model scale times its rider's drawn
scale, and the seat (MountMain) rises with it. In the original the Dragonmaw
Wind Reaver (rider display 21546, authored at 2.0) sits on a nether drake
twice a player's; `.npc setmodel 1139` (1.0) on the same unit shrinks the
drake to player size. Players follow the same rule: a mounted player morphed
to display 21546 (`.modify morph`) rides a horse and a gryphon twice the
size, and `.modify scale 1.5` grows the horse with the rider, natively. A
morphed rider's mount follows the morph's creature
(`EntitySpawner::refreshRemoteMountScale`). A character, local or other, is
drawn at its native display's CreatureDisplayInfo scale times its
CreatureModelData scale times OBJECT_FIELD_SCALE_X
(`rendering/character_scale.hpp`): native tauren males (display 59, 1.35) and
gnomes (1563, 1.15) stand and ride larger, their horse and gryphon by the same
factor; the local player's own mount follows its display and the character's
scale now too. ChrRaces float column 7 (night elf 1.2, gnome 0.8, tauren 0.75)
changes nothing visible on a horse natively. The third-person camera pivots
over the feet at the followed model's CreatureModelData CollisionHeight less
0.167, and mounted at the seat (MountMain at the mount display's scale, not the
rider's) plus half of it less 0.167 (`CameraController::nativePivotHeight`).
Fitted on the original's camera, pair 22 in Nagrand, within 0.01 for human
male and female, blood elf, night elf, draenei and dwarf, on foot, on a horse
and on a gryphon; a blood elf pivots exactly as a human, whose CollisionHeight
it shares, not its own skeleton. Not fitted: tauren male (native 1.83 on foot,
rule 1.49), and the scaled races mounted (tauren and gnome 0.12-0.48 higher
natively). The pivot setting moves it in proportion; the default is the
original's. Evidence: `_experiments/20261118T160000Z.player_scale` (fit.py).
A rider sits where its mount's pose carries the mount's MountMain (M2
attachment 0): the attachment's animated bone times its offset, in the
mount's position, facing and scale, and turned with that bone
(`CharacterRenderer::setInstanceSeat`, `riderSeatPose` in
`rendering/animation/rider_pose.hpp`). Every rider takes it: creatures,
other players, the local player and a morph's creature. Only a mount model
without attachment 0 falls back to its height over the mount's origin.
The bind-pose height alone is not the seat: the riding tiger's Stand pose
carries MountMain from (-0.28, 0, 1.24) to (-0.66, 0.04, 1.03), and Tyrande
Whisperwind (17948, 2.0, on tiger 6080 at 1.5) sat 2 yards forward, over
the shoulders, and 0.6 yards high, where natively she sits on the saddle.
The kodo (-1.98 forward), mechanostrider, wyverns, nether drakes and the
nether ray move theirs as well; the horse and ram barely. Gotcha: the camera
pivot keeps the bind-pose height (`cameraSeat`), which is what was fitted
to the original. Evidence: `_experiments/20261118T221740.rider_seat`.
A rider stays seated: its idle, locomotion and melee
stances are Mount (91), and its swings, casts, cast holds and wounds play
from SpineLow up over it (`setInstanceRiding`, `rendering/animation/rider_pose.hpp`).
Natively the Wind Reaver's legs stayed in the seat through every swing and
cast. A display change rebuilds the rider and puts the mount back. A dying
creature loses its mount in the same update (CMaNGOS unmounts on death); the
rider keeps its death pose and lies on the ground, as natively. A unit that
comes into view mid-way along a Flying spline (0x200 in its create block, as
Dragonmaw Sky Stalkers on their HOVER paths) flies, as SMSG_MONSTER_MOVE
already made it. `build/bin/test_rider_pose` covers these rules. Evidence:
`_experiments/20261118T150000Z.npc_mount` in the workspace. Not established:
the Wyrmcaller's orange cast beam. In combat both clients' drakes stand on
four legs, compared by eye only.

A creature whose movement flags carry LEVITATING (0x400) idles in Hover (193)
whatever its height above the ground; CMaNGOS sets the flag from InhabitType
air (`Creature.cpp`, SetLevitate), and in TBC it reaches the client only in
the create block. Native 2.4.3 hovers a levitating Frost Wyrm (17907,
FrostWurm.m2) spawned on flat ground, stands the same creature re-created
after `.debug flight 1 0`, keeps it in Hover through Frost Breath
(DragonSpitHover 183), back in Hover after a Talk emote, in combat and after
a follow move. A model without Hover falls back to Stand through
AnimationData (Gargoyle 17906; Drake.m2, Blackrock Drake 8964, looks the same
in both). This differs from the hover flag, which hovers only after LiftOff
or when the unit is created with it, and drops to Stand after an emote
(Sapphiron). `remoteIdlesInHover` decides it. Evidence:
`_experiments/20261118T220958Z.wyrm_fly` in the workspace.
A rider's weapons follow what plays above its spine, else the seat: Mount
has WeaponFlags 4 and puts them on the back; a swing (WeaponFlags 0x20) has
them in hand for its length (`riderWeaponPoseAnimation`). Native Bonechewer
Raider (16925) and Amani'shi Warbringer (23580) at sheath state 1 sit with
their axes on their backs, fresh, after swings and with combat flags set.
The mount keeps its own idle through the rider's swings, emotes and wounds:
a riding horse (2410), cockatrice (20937) and riding elekk (17063) never
reared or pecked natively in real and fake fights, though the same models
standing alone do. Not established: the Warbringer's war bear (22467,
bearmount.m2) does lunge on some swings natively, in real fights more than
in fake ones; what selects it was not found, and WoWee leaves it in its idle.
Furious Roar (42496) grows rider and bear together over about 1.2 s both
ways in both clients. Evidence: `_experiments/20261118T231500Z.rider_attack`.
A player's mount moves as its rider would on foot: on the ground and in water
the player's LocomotionFSM runs on the mount model's sequences (Walkbackwards
backing up, Shuffle turning, JumpStart/Jump/JumpEnd on a running jump, which is
the ordinary jump and sends MSG_MOVE_JUMP); other players' jumps, falls and
swims play on their mount, not the seated rider. Jump pressed standing rears
the mount (MountSpecial 94) without leaving the ground and sends
CMSG_MOUNTSPECIAL_ANIM; the server shows it to the others only
(SMSG_MOUNTSPECIAL_ANIM), so each client plays its own rear locally. No mount
bob, roll or random idle one-shots: MountSpecial has an M2 frequency and was
picked as an idle fidget. Outdoors-only spells (Attributes 0x8000) are refused
client-side where IsIndoors() is 1, after the moving check: a spell applying
aura 78 says SPELL_FAILED_NO_MOUNTS_ALLOWED, others SPELL_FAILED_ONLY_OUTDOORS
(Black Temple: the server let mounts through). Gotchas: CAN_FLY can go while
FLYING is set (flying mount dismissed in the air), and FLYING must still clear
on landing, or the server treats the player as moving (`debug viewsync align`
refuses). A mount summon shows a smoke cloud natively whose data source was not
found. Evidence: `_experiments/20261119T0900Z.mount_audit`.

TBC MCLQ freshwater preserves per-vertex depth and authored surface height
through both individual and merged terrain meshes. Liquid IDs use the same
1=water, 2=ocean, 3=magma, 4=slime convention as the renderer. Legacy terrain
freshwater uses the animated lake texture and outdoor zone lighting shared
with exterior WMO pools. Terrain ocean rendering remains a separate material.
Terrain freshwater repeats its texture every four liquid cells (16.67 yards),
including merged tile meshes; WMO pools repeat once per liquid cell.
`build/bin/test_adt_truncation '[adt][water]'` checks MCLQ depth, liquid IDs
and magma s,t.

TBC magma and slime follow `psLiquidMagma.bls`/`vsLiquidMagma.bls`: opaque
vertex colour times one animated texture (`XTextures/lava/lava.N.blp`,
`slime/slime.N.blp`, 30 frames per 1.25 s), unlit and fogged. Texture
coordinates are the authored vertex s,t. Fits of native 2.4.3 captures give:
MCLQ (ADT) texture repeats every 256/3 s,t units and drifts +0.025/s along s;
MLIQ (WMO) repeats every 256 units and drifts +0.1/s along t. Vertex colour is
white on terrain and MOMT color2 for interior WMO groups (0x959595: Blackrock
Mountain lava measured 0.585 of its texels). Outland terrain magma (all of it in
Shadowmoon Valley) draws `XTextures/lavagreen`. No TBC data selects that set:
LiquidType.dbc has four fields (ID, name, type, spell) and no textures, and
AreaTable.dbc's LiquidType overrides (fields 29-32: water, ocean, magma, slime)
are used only by Naxxramas slime 21, Hyjal Summit water 61 and Serpentshrine
water 41; the magma override is 0 in all 1643 rows. So the map selects it. Native's extra red brightness on lava is full-screen glow
(`ffxGlow`); with it off native lava equals its texels. Gotcha: magma s,t wrap
by multiples of 256 inside visible quads, which is seamless only at the 256/3
repeat. Evidence and fitting scripts (`uvfit.py`, `wmofit.py`, `burst.py`):
`_experiments/20261118T180500Z.lava_rendering/` in the workspace.

Ground-detail settings preserve fractional internal percentages: native
`groundEffectDensity=24` maps to `groundclutter=56.25` through the CVar scale
150/64. Both login and world settings readers retain that fraction.

Selected entities leaving visibility or despawning must use `clearTarget()`;
raw GUID assignment bypasses `PLAYER_TARGET_CHANGED` and leaves FrameXML showing
stale names with invalid portrait, level and health. Melee ability autoattack
follows Spell.dbc `AttributesEx` bit 0x200, including Sinister Strike ranks.

M2 alpha-key materials use a fixed 224/255 alpha threshold, as in Noggit's
`ModelRenderPass` alpha-key state. Surviving fragments are opaque. Do not boost
alpha with mip level or replace edge RGB with a blurred mip: these change the
authored silhouette and expose dark backing around DXT1 leaves. Reference:
https://github.com/wowdev/noggit3/blob/master/src/noggit/Model.cpp

Trainer services sit under 2.4.3's headers: a class trainer's are the
SkillLine names of each service's SkillLineAbility row (Assassination,
Poisons, Defense), a tradeskill trainer's are GlobalStrings
TRADESKILL_SERVICE_STEP/LEARN. All service APIs resolve the same filtered
rows so selecting or buying a row uses its actual spell. Service states are
recomputed from known and superseded spells, so a learned rank turns the next
one green without a new list. Addon saved globals restore after source files
and before ADDON_LOADED; loading them before source lets Blizzard_TrainerUI
defaults erase saved filters. Gotcha: native return counts matter (a
collapsed header answers three values, GetTrainerSelectionIndex nothing when
none); measured shapes are in
`_experiments/20261117T203052Z.trainer_skill_headers/README.md`.
GetNumTrainerServices counts only the shown rows, but every index API answers
past it with the trainer's other rows (headers too): at Pikkle with only
"available" showing and nothing to learn, the count is 0, row 2 is
"Journeyman Miner" and GetTrainerSelectionIndex is 2. Blizzard_TrainerUI then
selects row 2; answering nil there made it call CollapseTrainerSkillLine(2),
whose "Bad skill line" error stopped ClassTrainerFrame_Update before it set
the title, greeting, portrait and "All" button. The original orders those
extra rows by its earlier filtering; WoWee keeps list order
(`_experiments/20261118T191500Z.trainer_empty_list/` in the workspace).
Recipe item previews follow the spellTrigger=6 taught spell to its crafted item
and reagents rather than displaying the generic Learning spell.

### TBC talents and spell tooltips

`build/bin/test_spell_description_format` checks Spell.dbc description tokens
against native 2.4.3 text ("by $/1000;S1 sec." is "by 0.5 sec.").

The 2.4.3 talent frame (TalentFrameBase.lua) reads GetTalentPrereqs in
threes (tier, column, isLearnable); a fourth value aborts TalentFrame_Update
and leaves every icon coloured with no branches. GetTalentInfo's meetsPrereq
is not the prerequisite talent (native answers 1 for Riposte without
Deflection). Unspent points are PLAYER_CHARACTER_POINTS1. TBC
SMSG_REMOVED_SPELL and SMSG_SUPERCEDED_SPELL carry uint16 spell ids.

Talent and spell tooltip texts come from GlobalStrings (TOOLTIP_TALENT_*,
SPELL_*). Unmet requirements are ff2020, descriptions NORMAL_FONT_COLOR.
Gotcha: Talent.dbc prerequisites, the description columns and
UNIT_FIELD_BASEATTACKTIME live in `dbc_layouts.json`/`update_fields.json`;
an installed data tree keeps old copies. Not modelled: spell modifiers in
descriptions and tooltip values other than cost, and the hidden range of
self-cast spells such as Slice and Dice (native omits its 100 yd range).

The 2.4.3 spellbook has General plus one tab per class skill line:
SkillLine.dbc category 7 whose SkillRaceClassInfo row for the player's race
and class has flag 0x400 (rogue: Assassination, Combat, Subtlety; Internal
once a test spell of it is known). Professions, Lockpicking, Poisons,
racials, another class's spells and spells without a skill line (Hearthstone)
are in General. Tabs and spells are in name order, spells of one name by
id; all known ranks are listed. Gotcha: `SkillRaceClassInfo` is a
`dbc_layouts.json` entry, so an installed data tree needs the new copy.

An M2 batch whose animated colour alpha or texture weight is zero is not
drawn in any blend mode (opaque would otherwise stay solid, additive would
still add). Rookery Egg (175124, G_DragonEggFreeze.m2) shows its ice shell
only in Open/Opened; READY is the bare spiky egg.

Repeated rebuilds of unchanged objects: see [incremental build cache repair](docs/incremental-builds.md).

### Action button usability

`IsUsableAction`/`IsUsableSpell` follow Spell.dbc data, measured against
native 2.4.3 (rogue): Stances/StancesNot with SpellShapeshiftForm.dbc flag 1
(Stealth acts as no form, so Vanish stays usable in it), CasterAuraState and
CasterAuraStateNot against `UNIT_FIELD_AURASTATE` (Riposte needs state 1),
EquippedItemClass with the main/off-hand attributes (Shiv, Fishing), Totem
tools (Pick Lock, Disarm Trap), not-in-combat, finishing-move combo points,
then power. Only power answers `notEnoughMana`; everything else is
(nil, nil): Ambush out of stealth at 30/60 energy is (nil, nil). Cooldowns
are not part of it. Item actions count equipped items and use their on-use
spell (food is unusable in combat). Gotcha: these columns live in the
installed `dbc_layouts.json`, which does not refresh with the build.

`ACTIONBAR_UPDATE_USABLE` fires only when some slot's answer changes
(compared every frame): native fires it as energy crosses a cost, on
combat, stealth and cooldown-event changes, and not for 100 -> 90 -> 80
energy. A spell with attribute 0x02000000 (Stealth, Cold Blood) is
unusable while its own aura is up; its cooldown starts at
SMSG_COOLDOWN_EVENT and `GetActionCooldown` answers enable 0 until then.
`IsActionInRange` is nil for spells not aimed at a unit (Distract, Pick
Lock), harmful spells at an unattackable unit and excluded creature types
(Sap at a deer); a ranged-weapon range (SpellRange flag 2) is 0 inside
melee reach. The global cooldown comes from StartRecoveryCategory 133 and
StartRecoveryTime (1 s for rogue abilities; Stealth, Vanish and Attack
none). Spell buttons count their reagent (Vanish shows Flash Powder).
Evidence and probe addon: `_experiments/20261117T214218Z.usable_action_pair2`
in the workspace.

### Spell and melee range

Range to a unit is 3D, centre to centre, and takes both units'
`UNIT_FIELD_COMBATREACH` (`spellclass::checkSpellRange`,
`GameHandler::unitSpellRange`). Melee (SpellRange flag 1, Sinister Strike)
reaches both reaches plus 4/3 yard, never less than 5: a human (1.5)
reaches Onyxia (23.4) from 26.23 yards. Other ranges are MaxRange plus both
reaches (from Onyxia: Blind 10 -> 34.9, Fireball 35 -> 59.9); a MinRange gets
both reaches too (Charge 8-25 -> 32.9-49.9); a ranged weapon (flag 2: Throw, Shoot) is too
close inside melee reach. Bounding radius and model size play no part.
`IsSpellInRange`, `IsActionInRange` (button colour) and the cast itself use the
same answer: out of range the cast is refused before sending, with
"Out of range." or "Target too close" and `UNIT_SPELLCAST_FAILED`, after the
equipment check. Auto-attack out of melee reach says "You are too far away!"
at once, again for the server's `SMSG_ATTACKSWING_NOTINRANGE`, then every 4 s.
`CheckInteractDistance` is 30 yards for 1 and 4, 11.11 for 2, 10 for 3,
whatever the reaches. Measured on pair 2 with a stunned Mottled Boar whose
reach was set by `.debug setvaluebyname UNIT_FIELD_COMBATREACH`; evidence,
probe addon and scripts: `_experiments/20261118T170000Z.melee_range_pair2` in
the workspace. Gotcha: a GM (`.gm on`) gets nil for every harmful spell, so
range answers need `.gm off`.

### Action bar macros, stances and keys

Measured on pair 2 against native 2.4.3 (rogue). A macro button stands for
the spell or item of its `#showtooltip <name>` line, else of its first
`/cast`, `/use` or `/castsequence` line whose conditions hold (`[stealth]`
picks Ambush in stealth, Sinister Strike out of it). Its icon is its own,
or for the question mark (macro icon 1) that spell's or item's icon;
`IsUsableAction`, `GetActionCooldown`, `IsActionInRange`, `GetActionCount`,
`IsConsumableAction` and `IsCurrentAction` answer for that spell or item.
`/say hi` is usable with enable 0; a name the player lacks
(`/cast Sinister Strike(Rank 1)`, `/use 13` on an empty slot) is unusable.
The tooltip is the spell's with `#showtooltip`, else the macro name.
Macros are listed by name in each half and kept by identity: account n,
character 0x01000000|n (server action type 0x40/0x41, as native). The
interface's index is the place in the sorted half, so a rename moves a
macro's index while its buttons keep it. 2.4.3 indexes are 1-18
(account) and 19-36 (character), and
`CreateMacro(name, iconIndex, body, local, perCharacter)` takes an index
into the 780-entry icon list: the question mark, then Ability_* and
Spell_* icons by name. Gotcha: a character config with 3.3.5 ids (37-54)
keeps those macros, but on TBC the interface no longer lists them.

Account ("General") macros are the account's, character macros the
character's, as on native 2.4.3 (`WTF/Account/<ACCOUNT>/macros-cache.txt`
and `.../<Realm>/<Character>/macros-cache.txt`). WoWee keeps the account's
in `<config root>/accounts/<ACCOUNT>/macros.cfg` and the character's in
`characters/<Name>.cfg`, both as `macro_<id>_text/_name/_icon` keys. Both
halves are read anew whenever a character is selected (native picked up an
edit made to its account file between two logins), so a General macro made
on one character is there on the next one, in the same session or after a
restart, and nothing of one character's own half reaches another. Ids are
one past the highest of the half (native: GenA 1, GenB 2, delete GenA,
GenC is 3). Configs written before the account file kept account macros
per character: the first login with the new layout takes the logging-in
character's and then the account's other characters' account macros into
the account file and removes them from their configs (log lines "Account
macro <id> (<name>) of <character>: ..."). Identical macros become one; a
different macro under an id already taken waits for its own character's
login, where it gets the next free id (or joins an identical macro) and that
character's buttons follow it (CMSG_SET_ACTION_BUTTON). With both halves
full it stays in the character's config. Gotcha: native also uploads both
halves to the server (`account_data` type 4, `character_account_data`
type 5, at the next login) and fetches them when its local copy differs;
WoWee does not exchange account data, so macros made by native on the same
account do not reach WoWee and the other way round.
`build/bin/test_macro_store`; evidence:
`_experiments/20261119T102205Z.macros_account_split` in the workspace.

An item goes onto a button only if it can be equipped or has an on-use
spell; Blinding Powder or Copper Ore stays on the cursor, silently. A
button whose spell the player does not know (a superseded rank stored
by the server) is empty: `HasAction` nil.

The stance bar lists the forms the known spells take (aura 36,
EffectMiscValue); `GetShapeshiftForm` is the place on that bar (1 in
Stealth). An active form spell shows its `ActiveIconID` and counts as
current. A cast also counts as current from `UNIT_SPELLCAST_SENT` until
the server answers it. Default keys: Shift-1..6 page the main bar,
Ctrl-1..0 are the bonus bar, Ctrl-F1..F10 the stance bar.

Using a spell that has a Spell.dbc ActiveIconID while its own aura is on
the player sends CMSG_CANCEL_AURA instead of a cast, from the action bar,
stance bar, spellbook or /cast, before cooldown, power and usability
refusals: Stealth, Shadowmeld, druid forms, Shadowform, Ghost Wolf, paladin
auras, hunter aspects, summoned mounts. If the aura has no buff icon
(AttributesEx 0x10000000: warrior stances) nothing is sent. Spells without an
ActiveIconID cast again (Trueshot Aura, tracking, timed buffs) or refuse as
not ready (Cold Blood). Duration, form and attribute 0x02000000 do not decide
it. While the aura is up the button shows the ActiveIconID, and
ACTIONBAR_SLOT_CHANGED <slot> fires as it lands and goes. `CancelShapeshiftForm`
(/cancelform) cancels the aura holding the current form under the same
buff-icon rule. Measured on pair 13 with a packet-logging proxy:
`_experiments/20261118T205000Z.toggle_aura` in the workspace.

Spell descriptions apply the damage modifier (SpellModOp 0) to direct
damage effects only; a fractional result reads as "316 to 317%". Gotcha:
CMaNGOS sends SMSG_SET_FLAT/PCT_SPELL_MODIFIER only while it applies
auras. A relaunch inside the 30 s logout window resumes the old session
and receives none, so costs and damage read unmodified. Evidence and
probe addon: `_experiments/20261118T074449Z.actionbar_pair2` in the
workspace.

### TBC social panel

FriendsFrame, measured against 2.4.3 on a private pair
(`_experiments/20261118T200713Z.social_audit/` in the workspace;
`build/bin/test_game_replay '[social]'`, `build/bin/test_guild_roster_layout`).
Friends list online first, each half by name; ignores by name; the selection
follows the player. FriendStatus 2 (away) is online: CMaNGOS assigns one status.
Contacts arrive as guids and are name-queried, ignores too. SortWho keeps a
chain of keys (most recent first; the same field reverses, another moves to the
front with its direction; zone, level, name before any) and fires
WHO_LIST_UPDATE; SortGuildRoster fires GUILD_ROSTER_UPDATE and the roster keeps
the realm's order until sorted. The TBC guild roster has a gender byte after
the class. The channel panel is Group / World / Custom headers with the group's
own channel, built-in channels by their ChatChannels name and custom ones;
every channel event carries the panel row, not the chat number. A city-only
channel joined in a capital is named after AreaTable's "City" row. Gotchas:
SetGuildRosterShowOffline and SetWhoToUI get 0 for off, which is true in Lua;
GameTooltip:SetUnit answers nil on TBC, or UnitFrame_UpdateTooltip rebuilds the
tooltip every 0.2 s and drops lines added after it; 2.4.3 has no /reload
(`/script ReloadUI()`).
### Ready check

`build/bin/test_game_replay '[readycheck]'`

Measured against 2.4.3 on pair 5 with a probe addon, party and raid, the
native client asking, answering, as assistant and as plain member
(`_experiments/20261119T094500Z.readycheck_audit/` in the workspace).
CMaNGOS broadcasts MSG_RAID_READY_CHECK (asker's guid) to the group, relays
each answer (sent on MSG_RAID_READY_CHECK with one byte;
MSG_RAID_READY_CHECK_CONFIRM from a client is discarded) to the leader and
assistants only, and relays MSG_RAID_READY_CHECK_FINISHED from the asker.
The asker prints "Starting ready check...", everyone else "Edrike has
initiated a ready check."; READY_CHECK gets the asker's name and the whole
seconds left (29, sometimes 30). GetReadyCheckTimeLeft counts whole seconds
down from 30. The leader and raid assistants keep a status per member
(GetReadyCheckStatus: asker "ready", the rest "waiting"), which is what puts
the icons on PlayerFrame, PartyMemberFrame and the raid buttons; plain members
answer nil. READY_CHECK_CONFIRM gets the member's raid number (party number
in a party) and 1 or 0. A "no" is printed by the asker alone, at once, before
the event ("Edrike is not ready"). When nobody waits any more the leader and
assistants drop statuses and timer; the asker prints "Everyone is Ready" or
"Ready check finished" and sends the finish. After 30 s the asker prints "The
following players are AFK: Edrike" and sends the finish; an assistant just
drops. A plain member's timer runs on after the finish, and its FrameXML popup
prints "You were AFK for a ready check" when its own 29/30 s are up. A new
leader or leaving the group ends the check with no line and no event; the
popup closes on GetReadyCheckTimeLeft() == 0. DoReadyCheck while a check runs:
UI error "You are already running a ready check", nothing sent.
Gotcha: a member out of sight has no entity; names come from the group roster.



### Chat channels

`build/bin/test_game_replay '[channels]'`

Measured on pair 15 against native 2.4.3 with the SocialProbe addon
(`_experiments/20261119T093133Z.chat_channels/` in the workspace).

| ChatChannels.dbc | flags | joined at login |
|---|---|---|
| 1 General - %s | 0x3 | yes |
| 2 Trade - %s | 0x3b | yes; "Trade - City" in a capital (0x20) |
| 22 LocalDefense - %s | 0x10003 | yes |
| 23 WorldDefense | 0x10004 | no |
| 25 GuildRecruitment - %s | 0x20032 | when `guildRecruitmentChannel` is on (default "1") and not in a guild (0x20000); "- City" in a capital |
| 26 LookingForGroup | 0x40001 | yes |

In Tanaris, outside a guild: "1. General - Tanaris", "2. Trade - Tanaris",
"3. LocalDefense - Tanaris", "4. GuildRecruitment - Tanaris", "5.
LookingForGroup". Each join is CMSG_JOIN_CHANNEL with the row's id; a zone
change is the realm's (Player::UpdateLocalChannels: "Changed Channel: [1.
General - Shattrath City]", same number), the client sends nothing.
Joining a guild leaves GuildRecruitment, leaving one joins it again.
SetGuildRecruitmentMode(0/1) leaves/joins it without touching the CVar.

A channel left gives up its number and the others keep theirs; the next join
takes the lowest free number. GetChannelList answers built-in channels by
their ChatChannels name ("General"), GetChannelName(1) the full name.
CHAT_MSG_CHANNEL and the channel notices carry arg4 "1. General - Tanaris",
arg7 the ChatChannels id (0 custom), arg8 the number, arg9 the full name; a
chat window keeps a line whose arg7 is one of its zone channel ids
(GetChatWindowChannels: General 1, Trade 2, LocalDefense 22,
GuildRecruitment 25, LookingForGroup 0 by name) or whose arg9 is one of its
channel names. The Chat tab lists Group (Party / Raid (n), no number), World
(built-in, by number) and Custom headers.

Native keeps per character, in `WTF/.../chat-cache.txt`, which built-in
channels are joined (`ZONECHANNELS`, bit id-1, default 0x3200003) and the custom
channels with their numbers (`CHANNELS`, "zzprobe 1 6"): `/leave 3` in
Shattrath and a relog leaves LocalDefense out, and "zzprobe" comes back as 6.
WoWee does not keep this yet; it joins its chat settings' channels every
login and no custom ones. Gotchas: the stock FrameXML options checkbox and
WoWee's settings row both write the CVar, so a `guildrecruitmentchannel=0`
saved by an earlier build (whose default was "0") still keeps
GuildRecruitment out until it is removed from `cvars.cfg`. A ScrollFrame
measured while hidden reads rows the last layout did not place:
ChannelListScrollFrame reported a range of 621 for nine rows and the tab
opened with a scroll bar; ranges are now measured on resolved rects and only
for shown frames (native has no rect for a hidden frame).
### The held cursor

`build/bin/test_input_integration '[action-cursor]'`

Measured on pair 14 against native 2.4.3 with a probe addon and window
captures (native runs with `gxCursor 0`, so its pointer is in the frame).
Whatever the cursor holds - a spell, a macro, a bag item, an action swapped
off a bar, money - is the pointer: its icon, 32x32 with the top left at the
hotspot, under `Interface\Cursor\Item.blp` (the gold corner). No gauntlet,
no service cursor, and the same picture over every target, valid drop or not.
Money shows `GetCoinIcon` of the amount (5 -> Coin_05, 50 -> Coin_06,
500 -> Coin_03, 4000 -> Coin_04). WoWee derives the picture from the cursor
state each frame (`cursorPicture`); nothing stores it beside the state.

A right-button press anywhere - world, unit, frame, action button, bag
slot - with something held puts it back (bag item: `ITEM_LOCK_CHANGED`,
`ACTIONBAR_HIDEGRID`, `CURSOR_UPDATE`; money to the purse; an action dropped)
and does nothing else: no OnClick, no `UseAction`, no item use, no NPC
interaction, no mouse-look. It is taken on the press, with its release, before
any other reader (`Application`, `returnHeldCursor`). Escape does not touch
the cursor: it closes windows or opens the game menu as usual.

Swaps (`PickupAction`, `PlaceAction`, a click on a button while holding):
the held thing goes down first (`ITEM_LOCK_CHANGED` for a bag item, then
`ACTIONBAR_HIDEGRID`, `CURSOR_UPDATE`), the slot's old action comes up
(`ACTIONBAR_SHOWGRID`), `ACTIONBAR_SLOT_CHANGED` comes last. Dropping the
action a slot already holds, or `PickupSpell` of the spell already held,
empties the cursor and leaves the slot. An item reference off a bar answers
`GetCursorInfo` "item" and `CursorHasItem` nil. Money fires no grid events and
does not go on a bar. `ClearCursor` with nothing held still fires
`CURSOR_UPDATE`. A locked bar lifts only with Shift. Gotcha: native fires
`PLAYER_MONEY` when money is picked up and put back; WoWee does not.
Evidence: `_experiments/20261118T203647Z.action_cursor` in the workspace.

### TBC Lua answer conventions and protected functions

On a TBC login `applyApiConventions` (src/addons/expansion_api_surface.cpp)
wraps measured globals and widget methods: 1/nil instead of true/false
(UnitExists, IsShown, ...), 2.4.3 result counts (UnitClass 2, UnitPowerType 1,
GetQuestLogTitle 8, ...), and refusal of 33 protected functions (TargetUnit,
CastSpellByName, UseAction, ...) from insecure code with
MACRO_ACTION_FORBIDDEN / ADDON_ACTION_FORBIDDEN. Insecure means a Lua frame on
the stack from a RunScript or /run chunk or from a file of an AddOn whose name
starts with neither Blizzard_ nor Wowee. Gotcha: compare answers by truth, not
with `== true`/`== false`; FrameXML compares `IsShown() ~= UnitExists(...)`,
so a function and a widget method must answer in the same kind. Combat-only
restrictions (ADDON_ACTION_BLOCKED) and data taint are not modelled.
Evidence and probe addon: `_experiments/20261118T010104Z.lua_conventions`
in the workspace.

### Frame pacing (maxFPS, maxFPSBk)

The main loop applies build 8606's maxFPS and maxFPSBk CVars (both default 0)
through `core::nativeFrameLimit`. Measured on the original under Wine with a
frame-counting OnUpdate script and another program in front:

| maxFPS | maxFPSBk | in front | behind | minimised |
|---|---|---|---|---|
| 0 | 0 | vsync | vsync | one frame every ~255 ms |
| 0 | 10 | vsync | vsync | vsync |
| 30 | 0 | 30 | 30 | one frame every ~255 ms |
| 30 | 10 | 30 | 10 | 10 |
| 30 | 50 | 30 | 30 | - |

"Behind" means no window of this client has the keyboard. This client's own
frame limit setting still applies; the stricter cap wins. `Frame pacing:`
lines in the log record each change of state and cap. The 2.4.3 cap waits are
sleeps: at maxFPS 10 the original used a tenth of its 60 fps CPU.

Gotchas:

- Native clients on the pairs are pinned to CPUs 0-1 by `processAffinityMask
  "3"` in Config.wtf, and Wow.exe re-pins threads to it itself. With 15-20
  pairs up they share two cores, run 10-25 fps and show about 0.1 core each.
  That is starvation, not pacing. Unpinned, the original costs about 1.1
  cores at 60 fps, focused or not.
- Under Wine 11, losing X focus alone (an override-redirect window) does not
  put the original in the background; only a WM activation of another window
  (`_NET_ACTIVE_WINDOW`) or a minimise does. Wine's `+win` trace shows
  `set_foreground_window` when it happens.
- On Xvfb, `vkQueuePresentKHR` costs 20-26 ms of main-thread CPU per frame
  (FIFO and IMMEDIATE alike): the NVIDIA driver copies the image to the CPU
  for the X server. That, not a busy-wait, is most of an idle client's core.
  `WOWEE_FRAME_PROFILE=1` reports it as `endFrame/present`.
