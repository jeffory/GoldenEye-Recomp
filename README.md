Support me on Ko-fi: https://ko-fi.com/sunjayy

# GoldenEye 007 — PC Recompilation

A native PC port of **GoldenEye 007 (Xbox 360 / XBLA)**, built by *statically
recompiling* the original game into C++ with the
[ReXGlue SDK](https://github.com/jeffory/GoldenEye-Recomp-rexglue). No emulator —
the game runs as a real native executable.

> [!IMPORTANT]
> **This repository contains _no_ game code or assets.** It is only the
> source that wraps the game (menus, hooks, online, post-FX, build
> config). You must find the game files yourself. This game never released publically

## Features

- Runs natively on **Windows** and **Linux** — no emulator, no BIOS — with an
  experimental **Android** (arm64) build.
- Controller support.
- **Online multiplayer** — host or join matches over the internet (LAN, Hamachi,
  playit.gg, or a public server). See [Playing online](#playing-online).
- In-game **pause / settings menu** (ESC): video, resolution, frame limit,
  fullscreen, online setup.
- **Post-FX** filters (brightness, contrast, saturation, vignette, presets…).
- **Weapon quick select** — instant switching via number keys / scroll wheel on
  PC, or a touch menu on the second screen of dual-screen Android handhelds.
  See [Weapon quick select](#weapon-quick-select).
- **Restored music** the Xbox 360 build shipped but never played — the 007
  watch/pause theme, Mission Select music after a mission ends, and the
  Control/Caverns elevator tracks. Plays from your own music banks via the
  game's native audio system.
- Smooth, stable 60 FPS (recompiled, with GPU-pacing fixes for the original's
  frame timing).

## Download & Play

Grab the latest prebuilt release from the **[Releases](../../releases)** page
(**Windows x64**, **Linux x86-64**, and an experimental **Android arm64-v8a**
APK are attached), then supply your own GoldenEye 007 game files —
see **[Game files: what to supply and where](#game-files-what-to-supply-and-where)**.
Run `ge.exe` on Windows or `./run.sh` on Linux; on Android install the
(debug-signed) APK.

- 🎮 **Want to play online?** Someone needs to run a server. Download it here →
  **[GoldenEye-Recomp-Server](https://github.com/SunJaycy/GoldenEye-Recomp-Server)**
- 🛠️ **Want to modify the engine / recompiler?** It's built on a modified ReXGlue
  SDK (with the Linux + Android port) →
  **[GoldenEye-Recomp-rexglue](https://github.com/jeffory/GoldenEye-Recomp-rexglue)**

## Game files: what to supply and where

Neither this repository nor any release contains game data. You supply your own
**GoldenEye 007 (Xbox 360 / XBLA)** dump. The port reads it from a single folder
— the **game data root** — and expects the dump's original layout, unchanged:

```
<game data root>/
├── default.xex                          ← the executable the recomp was built from
├── music.xgs   music.xsb   music.xwb    ← XACT music banks
├── sfx.xgs     sfx.xsb     sfx.xwb      ← XACT sound banks
└── files/
    ├── loc/        english/…
    ├── misc/
    ├── new/        background, char, gun, head, prop, skydome, texture
    ├── original/   background, char, gun, head, prop
    └── texture/
```

That's **1,800 required files, ~700 MB**. If your dump unpacked into a folder
named something like `GoldenEye 007 XBLA`, copy the **contents** of that folder
into the game data root — not the folder itself. Don't rename or flatten
anything; the guest looks these paths up by name (case-insensitively).

### Windows and Linux

The game data root defaults to an **`assets/` folder next to the executable**:

```
GoldenEye-Recomp/
├── ge.exe                 (Windows)
├── ge  run.sh  *.so       (Linux release bundle)
└── assets/
    ├── default.xex
    ├── music.xwb   sfx.xwb   …
    └── files/…
```

To keep the dump elsewhere, point the game at it instead:

```sh
./ge --game_data_root=/path/to/goldeneye-xbla        # any platform
ge.exe --game_data_root=D:\Games\goldeneye-xbla      # Windows
GE_GAME_DATA=/path/to/goldeneye-xbla ./run.sh        # Linux release bundle
```

### Android

No PC required. The app asks where your dump is the first time it runs:

1. Copy the dump anywhere on the device with any file manager — e.g. an
   `Internal storage/GoldenEye` folder. It does **not** have to go into
   `Android/data`.
2. Launch **GoldenEye 007**, tap **Select game folder**, and grant
   **All files access** when Android asks for it.
3. Pick the folder that holds `default.xex`. (Picking its parent works too, as
   long as only one folder inside looks like a dump.)

The dump is then read **in place** — nothing is copied, so leave the folder
where it is and keep the ~700 MB free rather than spending it twice. Your
choice is remembered in
`Android/data/com.sunjaycy.goldeneye/files/user/ge_game_path.txt`; delete that
file, or move the folder, to be asked again.

> [!NOTE]
> The **All files access** permission (`MANAGE_EXTERNAL_STORAGE`) is needed
> because the game reads the dump with ordinary file calls from native code,
> which the folder picker's own temporary access doesn't cover. It is only ever
> used to read the folder you selected.

**Prefer to use a PC?** Stage the dump in the app's own external data directory
instead — that needs no permission at all, and the app uses it automatically
whenever no folder has been picked:

```sh
adb push <dump>/. /sdcard/Android/data/com.sunjaycy.goldeneye/files/
```

```
/sdcard/Android/data/com.sunjaycy.goldeneye/files/
├── default.xex
├── music.xwb   sfx.xwb   …
└── files/…              ← yes: a "files" folder inside "files"
```

### If files are missing

The port verifies the whole manifest before it boots, so a bad install fails
with a list instead of a crash somewhere in the game:

- **Desktop** — an error dialog naming the first few missing files.
- **Android** — the loading screen turns into an error screen naming them, and
  reopening the app returns you to the folder picker so you can select a
  complete dump.

The complete list is written to `ge_missing_files.txt` in the user data folder
(`Android/data/com.sunjaycy.goldeneye/files/user/` on Android). If you're
deliberately running a non-canonical dump, `--no-ge_fatal_on_missing_file`
downgrades the check to a warning and launches anyway.

## Playing online

1. One person runs the **[server](https://github.com/SunJaycy/GoldenEye-Recomp-Server)**
   and shares its address + port.
2. Everyone opens **ESC → ONLINE** in the game, enters their **username**, the
   **server address**, the **port**, ticks *Enable online play*, and hits
   **Save & Restart**.
3. Host a match; the others find and join it.

Because players connect *out* to the server, no port-forwarding is needed for
joiners — only the host's server port has to be reachable.

## Weapon quick select

Switching weapons is **instant** — the port calls the game's own weapon-switch
routine directly instead of cycling through the inventory, so jumping from the
first weapon to the last takes one press. It's enabled by default and there are
two ways to use it, depending on platform:

- **PC (keyboard & mouse):**
  - **Number keys `1`–`9`** — jump straight to the Nth weapon you're carrying
    (in inventory order).
  - **Mouse scroll wheel** — scroll up/down to step to the next/previous
    carried weapon.
- **Android (dual-screen handhelds, e.g. AYN Thor):** the bottom touch panel
  shows a live weapon menu — every carried weapon with its ammo count, with the
  currently equipped one highlighted. **Tap a weapon to switch to it.** The main
  game on the top screen is unaffected. On single-screen devices the menu simply
  doesn't appear; nothing else changes.

The feature can be tuned via cvars: `ge_weapon_select_enable` turns it on/off,
and `ge_key_wpn_next` / `ge_key_wpn_prev` rebind the next/previous keys
(defaults `WheelUp` / `WheelDown`).

## Building from source (advanced)

Most people should just use the [Releases](../../releases). To build it yourself
you need the recompiler toolchain and your own copy of the game.

### Common prerequisites (all platforms)

- The [ReXGlue SDK](https://github.com/jeffory/GoldenEye-Recomp-rexglue) — a local
  checkout that provides the `rexglue` CLI + runtime. The build points at it via
  `-DREXSDK_DIR=/path/to/GoldenEye-Recomp-rexglue`.
- **CMake 3.25+**, a **C++23 Clang** toolchain, and **Python 3** (used by codegen).
- Your own **GoldenEye 007 XBLA game files**, placed in `assets/`
  ([layout](#game-files-what-to-supply-and-where)). Codegen reads
  `assets/default.xex` from there.

Every build starts with the same codegen step, run once from the repo root. It
turns *your* game copy into recompiled C++ under `generated/`:

```sh
rexglue codegen --max_jump_table_entries 2048 ge_config.toml
```

The desktop builds use [CMake presets](CMakePresets.json). Each platform/arch has
`-debug`, `-release`, and `-relwithdebinfo` variants
(`win-amd64`, `win-arm64`, `linux-amd64`, `linux-arm64`); swap the preset name in
the commands below to pick a different target or build type.

### Linux (x86-64)

- **Clang 18+** with **libc++** (`-stdlib=libc++`) — the SDK uses `std::expected` /
  `std::jthread`. The presets invoke plain `clang` / `clang++`; if your distro
  only ships versioned binaries (e.g. `clang-20`), either symlink them or override
  `CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER`.
- **Ninja** (the presets' generator).

```sh
# After running codegen above:
cmake --preset linux-amd64-relwithdebinfo -DREXSDK_DIR=/path/to/GoldenEye-Recomp-rexglue
cmake --build --preset linux-amd64-relwithdebinfo
# Binary: out/build/linux-amd64-relwithdebinfo/ge  → run with ./ge
```

### Windows (x64)

- **Clang** (LLVM, `clang` / `clang++`) inside a **Visual Studio (MSVC)**
  environment — open an *x64 Native Tools* developer prompt so the MSVC headers
  and libs are on `PATH`.
- **Ninja** and **CMake** (both ship with the VS installer's CMake component).

```bat
:: After running codegen above, from the x64 Native Tools prompt:
cmake --preset win-amd64-relwithdebinfo -DREXSDK_DIR=C:\path\to\GoldenEye-Recomp-rexglue
cmake --build --preset win-amd64-relwithdebinfo
:: Binary: out\build\win-amd64-relwithdebinfo\ge.exe
```

### Android (arm64-v8a, experimental)

Builds an APK with Gradle + the NDK, which drives this same CMake. `arm64-v8a`
only (the guest reserves a 4 GiB address space).

- Android SDK + **NDK 27.1.12297006** (Vulkan-capable).
- The ReXGlue SDK checkout, passed as `-PrexSdkDir` (default
  `../../GoldenEye-Recomp-rexglue`).

```sh
# After running codegen above:
cd android
./gradlew assembleRelease -PrexSdkDir=/path/to/GoldenEye-Recomp-rexglue
# APK: android/app/build/outputs/apk/release/
```

Install the (debug-signed) APK on a controller-equipped device. On-device
game-asset delivery is still being wired up, and cold boot uses an auto-retry
watchdog. See [`android/README.md`](android/README.md) for how the
NativeActivity / Vulkan-surface shell wires together, and
[`docs/boot-startup-race.md`](docs/boot-startup-race.md) for the boot race.

> [!NOTE]
> **Continuous integration.** Every push is compile-verified on **Linux, Windows,
> and Android** via GitHub Actions
> ([`.github/workflows/build.yml`](.github/workflows/build.yml)): it checks the
> hand-written engine sources against the SDK headers on all three platforms. CI
> can't produce the final `ge` binary — that needs generated PPC code from *your*
> own game copy.

source lives in [`src/`](src/):
`ge_app` (app + window/menus glue), `ge_menu` (pause/settings menu),
`ge_hooks` (mid-asm fixups), `ge_postfx` (filters). `ge_manifest.toml` /
`ge_config.toml` drive the recompiler.

## Credits

Huge thanks to **[mrfox-1](https://github.com/mrfox-1)** for the community fixes
this build ships:

- **Restored native XACT music transitions** — the watch/pause theme, Mission
  Select music, and the Control/Caverns elevator tracks. The Xbox 360 build left
  the `music_xtrack_play` / `music_xtrack_stop` script opcodes as printf-only
  stubs; mrfox-1 reverse-engineered the mission-music state machine and the
  logical-cue translation table to wire them back up, so the music plays from
  your own banks through the game's own audio system.
  ([upstream PR #114](https://github.com/SunJaycy/GoldenEye-Recomp/pull/114))
- **Tank turret mouse aiming** — horizontal aim while mounted, which the ordinary
  mouse path could never do because the game rebuilds the camera yaw from the
  turret every tick.
  ([upstream PR #116](https://github.com/SunJaycy/GoldenEye-Recomp/pull/116))

Both were adapted to this fork; the reverse-engineering behind them is theirs.
Original work: [GoldenEye-Recomp-Watch-Music-Fix](https://github.com/mrfox-1/GoldenEye-Recomp-Watch-Music-Fix).

The community DATA/code bug-fixes are from the **BeanTools / GoldenEye
Community Edition** patch set.

## Legal

GoldenEye 007 and all related assets are property of their respective rights
holders. This project ships **none** of that — no ROM, XEX, textures, audio, or
recompiled game code. It only automates turning a copy *you already own* into a
PC build. Don't ask for or share game files.

## License

The original code in this repository is released into the **public domain**
([The Unlicense](LICENSE)). The ReXGlue SDK it builds against has its own
(BSD-3) license.
