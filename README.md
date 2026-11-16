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
TBC group finder: [LFG/LFM protocol and validation](docs/tbc-lfg.md).
Party frames and map transfers: [protocol and input lifecycle](docs/party-stats-and-map-input.md).
Interior minimaps: [authored room tiles and validation](docs/interior-minimap.md).
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
The particle layout regression runs in `test_m2_ribbon_tracks`.

### TBC combat feedback

Outgoing melee damage uses white numbers; ability damage uses yellow numbers.
XP appears above the player as `XP: <amount>` in purple, using FRIZQT__.TTF.
Ordinary damage lasts 1.5 seconds; XP lasts 4.5 seconds. The build-8606 popup
parameter table begins at 0xBBF1C0 (28-byte records); XP formatting is at
0x6089E0. Incoming floating combat text follows the `enableCombatText` setting.

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

Greeting callbacks play voice audio. Body emotes come from server animation
messages. Local conversation turns play the authored directional `ShuffleLeft`
or `ShuffleRight` once, then return to idle; repeated gossip does not restart
the shuffle. Combat, locomotion, and server emotes keep their animation.
While an NPC service or quest panel is open, its stationary living NPC turns
toward the local player. Closing the panel returns it to the server heading.
Facing settles over 50 ms, matching native frame captures. The foot shuffle
finishes at its authored duration (500 ms for DwarfMale). Repeated clicks do not
restart either interval.
This facing is local presentation and never changes entity movement or patrol
orientation. Other clients can see a different facing for the same vendor.
`build/bin/test_input_integration '[npc-facing],[merchant]'` checks turning and merchant
interaction lifetime.

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

Small solid M2 props keep their geometry even when they contain multiple
emitters. Spherical emitters use their authored area and radial velocity;
flag 0x100 selects vertical velocity. Ordinary particle colors retain their
authored values. `build/bin/test_m2_classifier_foliage` and
`build/bin/test_m2_ribbon_tracks '[m2][particle]'` cover these rules.
