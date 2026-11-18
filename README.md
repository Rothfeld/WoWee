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
Matrix-created WMO doodad particles use authored placement scale for sprite
width. See [authored particle motion](docs/authored-particles.md).
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
pose. Silvermyst crystal glow cards are an example. Visible
character and M2 bones update every frame; distance/frustum culling still
limits work. Character animation uses the same radius as character drawing.
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
Flag 0x1000 (XYQuad) sprites lie flat in the model XY plane:
`WaterElemental.m2` #16/#21 are the ground rings under an Aqua Guardian.
Flag 0x400 pins a tail to its birth point: the tail runs back along
`-velocity * tailLength` but never past where the particle was born. 2204 of
3522 TBC tail emitters carry it, many with tailLength 10 s; without the limit
Bloodlust/Summon hand rings and the fire precast hand sparks drew 3-9 yd
spokes absent in the original, while acid breath drops (0.1 s) keep their
short tail. Whether unpinned tails also stop at birth is unverified (about
ten TBC emitters would differ). Evidence:
`_experiments/20261117T233000Z.flag400_spokes` in the workspace.
The particle layout regression runs in `test_m2_ribbon_tracks`.

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

Grounded creatures select the nearest reachable terrain, WMO or M2 floor.
Terrain corrections do not require a previous corrected render position; static
server spawn Z can differ from the ADT triangle. Structure corrections retain
continuity checks for overlapping floors. `WOWEE_CREATURE_GROUND_TRACE=<GUID>`
logs one creature's server position, support position and slope rotation each second.

Looping Stand animations choose among authored M2 sequences using their frequency
weights and repetition bounds. The TBC wolf has four Stand sequences, all with
variationIndex zero; filtering on that field cannot identify its fidgets.
Selection happens at sequence boundaries and uses authored transition blend time.

Airborne terrain contact compares feet against the terrain height at each end
of the movement segment. Comparing both endpoints against the destination floor
misses uphill landings and leaves jump momentum active after penetration recovery.
Regression target: `test_hill_climbing`.

Moving landings resolve directly to walk/run. Character rendering cross-fades
jump, landing and locomotion sequences using the destination sequence's authored
blend time. Outgoing one-shots clamp at their final frame during the blend;
looping sequences wrap. Stationary landings retain JumpEnd.

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
between 1.5 and 0.5 yards. Collision can shorten that distance while requested
zoom stays far away. Camera opacity multiplies ghost/stealth/spawn opacity and
propagates to weapons and their effects. Renderer visibility recovery respects
the camera controller's fully transparent state.

Camera fades render a depth-only pass for solid character batches before color
blending. Blending while writing depth in one pass leaves previously drawn
backfaces visible through the front. The color pass tests equal depth; transparent
cards retain their authored blending and depth behavior.

## TBC scene integration

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

Cursor selection receives quest status separately from the overhead symbol. An unavailable quest can show a grey exclamation mark while keeping the NPC's ordinary cursor. Felix Whindlebolt uses the plain gauntlet in this state.

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

### Idle loop boundaries

Stand variants blend from their outgoing final pose using the incoming sequence's authored blend time. This also applies when the same variant repeats, either through random selection or its replay count. TBC `DwarfMale.m2` sequence 40 has different first and last spine poses; a direct wrap produces a visible snap.

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
Facing settles over 50 ms, matching native frame captures. The foot shuffle
finishes at its authored duration (500 ms for DwarfMale). Repeated clicks do not
restart either interval.
This facing is local presentation and never changes entity movement or patrol
orientation. Other clients can see a different facing for the same vendor.
`build/bin/test_input_integration '[npc-facing],[merchant]'` checks turning and merchant
interaction lifetime.

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

The ring is where a sphere of the selection radius (`core/unit_size.hpp`)
around the unit's position meets the ground under it, projected straight down
onto terrain, WMO and M2 floors so it wraps slopes. A unit h yards above the
ground gets a ring of radius sqrt(r^2 - h^2) and none once h reaches r: a
flying or jumping unit leaves it on the ground, shrinking. A rider's ring is
centred on the mount. The renderer reads the target's instance position when
it records the ring, the frame its model is drawn; the UI sets the target
after the world is recorded, so the ring used to trail a running unit by a
frame. No ring under the local player, even self-targeted. Measured in the
original on the GM Island hilltop with tempspawned flying creatures:
`_experiments/20261118T084140Z.selection_circle_ground/` in the workspace
root. Not established: native also fades the ring as h grows (Infernal 7950,
r 6.45: ring intensity about 0.6 at 2 yd, 0.2 at 3, 0.08 at 4 of that at 0);
WoWee draws it at full strength until it vanishes.

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

### Merchant refresh and item usability

Merchant items report level, class/race, equipment proficiency, skill, and
reputation eligibility to FrameXML. Its red icons and row backgrounds indicate
unusable items; lack of money does not make otherwise usable items red.

Selecting gossip sends one selection packet. NPC-text updates cannot select the
same pending option again, and vendor selection does not send an additional
inventory request. Repeated interaction with the same vendor refreshes inventory
with `MERCHANT_UPDATE`, preserving the open shop and backpack.

### Online spawn floor

Camera reset preserves the server spawn elevation. Floor searches above outdoor
terrain can select an interior ceiling or roof; they must not relocate an online
spawn. Reset invalidates cached floor contact, then normal movement resolves
contact using streamed collision meshes.

### Tilted doodad floors

M2 floor rays query both local floor and wall triangle grids. A barrel placed
on its side can support a player on triangles classified as walls before
placement. Walkability is evaluated using transformed world-space normals.

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
- Other FontStrings (labels, quest text, chat) still measure and draw with
  ImGui's metrics. The bare FontString wrap thresholds in
  `tests/test_native_text_metrics.cpp` are the measured target for them.

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
Native draws nothing of the plateau above the realm and a blue-purple sky;
WoWee still draws the plateau's models above and its own sky (open).

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
object with a quest marker shows `Quest`/`QuestTurnIn`; lock slot 0 naming a
LockType cursor shows it without any skill check (Peacebloom `GatherHerbs`
without Herbalism); otherwise the type's cursor - the gear for chests, `Mail`
for mailboxes, `Buy` for guild vaults. Out of reach adds `Unable`. Gotcha:
only lock slot 0 counts - Alliance Strongbox 4095 (Lock 23, Lockpicking in
slot 1) is the gear. Native table: `_experiments/20261117T195204Z.pair0_go_cursors`.

A unit's cursor (`include/ui/world_cursor.hpp`), measured on spawned NPCs in
build 8606: a lootable corpse shows `Pickup` (the bag; not `LootAll`); a living
NPC its quest marker's cursor, then Innkeeper, the server icon (Directions for
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
strip, TexCoordScale is texture repeats over the whole beam, the wave is
`WaveHeight * sin(WaveFreq * s - WaveSpeed * t)` with s yards from the caster,
in the plane of world up turned about the beam by Min/MaxWaveAngle (plus
spin), and colour/alpha/blend come from the record's bytes. The TBC record is
173 bytes: dwords 0-38, then Alpha, Red, Green, Blue, BlendMode bytes at +156.
Not implemented: joints, noise, arcs, flicker, pulse, Combo rows, blend 6,
chains on cast/impact kits (Chain Lightning). Channel kit effects of a channel
known only from the fields are held until the fields clear.

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

TBC MCLQ freshwater preserves per-vertex depth and authored surface height
through both individual and merged terrain meshes. Liquid IDs use the same
1=water, 2=ocean, 3=magma, 4=slime convention as the renderer. Legacy terrain
freshwater uses the animated lake texture and outdoor zone lighting shared
with exterior WMO pools. Terrain ocean rendering remains a separate material.
Terrain freshwater repeats its texture every four liquid cells (16.67 yards),
including merged tile meshes; WMO pools repeat once per liquid cell.
`build/bin/test_adt_truncation '[adt][water]'` checks MCLQ depth and liquid IDs.

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
2.4.3 macro ids are 1-18 (account) and 19-36 (character), and
`CreateMacro(name, iconIndex, body, local, perCharacter)` takes an index
into the 780-entry icon list: the question mark, then Ability_* and
Spell_* icons by name. Gotcha: a character config with 3.3.5 ids (37-54)
keeps those macros, but on TBC the interface no longer lists them.

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

Spell descriptions apply the damage modifier (SpellModOp 0) to direct
damage effects only; a fractional result reads as "316 to 317%". Gotcha:
CMaNGOS sends SMSG_SET_FLAT/PCT_SPELL_MODIFIER only while it applies
auras. A relaunch inside the 30 s logout window resumes the old session
and receives none, so costs and damage read unmodified. Evidence and
probe addon: `_experiments/20261118T074449Z.actionbar_pair2` in the
workspace.

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
