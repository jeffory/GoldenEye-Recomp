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
- Smooth, stable 60 FPS (recompiled, with GPU-pacing fixes for the original's
  frame timing).

## Download & Play

Grab the latest prebuilt release from the **[Releases](../../releases)** page
(a **Linux x86-64** build is attached; Windows/Android — see the release notes),
then drop your own GoldenEye 007 game files into the `assets/` folder next to the
binary. Run `ge.exe` on Windows or `./ge` on Linux.

- 🎮 **Want to play online?** Someone needs to run a server. Download it here →
  **[GoldenEye-Recomp-Server](https://github.com/SunJaycy/GoldenEye-Recomp-Server)**
- 🛠️ **Want to modify the engine / recompiler?** It's built on a modified ReXGlue
  SDK (with the Linux + Android port) →
  **[GoldenEye-Recomp-rexglue](https://github.com/jeffory/GoldenEye-Recomp-rexglue)**

## Playing online

1. One person runs the **[server](https://github.com/SunJaycy/GoldenEye-Recomp-Server)**
   and shares its address + port.
2. Everyone opens **ESC → ONLINE** in the game, enters their **username**, the
   **server address**, the **port**, ticks *Enable online play*, and hits
   **Save & Restart**.
3. Host a match; the others find and join it.

Because players connect *out* to the server, no port-forwarding is needed for
joiners — only the host's server port has to be reachable.

## Building from source (advanced)

Most people should just use the [Releases](../../releases). To build it yourself
you need the recompiler toolchain and your own copy of the game.

**Prerequisites**
- The [ReXGlue SDK](https://github.com/jeffory/GoldenEye-Recomp-rexglue) (provides the `rexglue` CLI + runtime).
- CMake 3.25+, a C++23 **Clang** toolchain, Python 3.
  - **Windows:** Clang inside a Visual Studio (MSVC) environment.
  - **Linux:** Clang 18+ with **libc++** (`-stdlib=libc++`) — the SDK uses
    `std::expected` / `std::jthread`.
  - **Android:** the NDK (r27) — see [`android/README.md`](android/README.md).
- Your own GoldenEye 007 XBLA game files, placed in `assets/`.

**Steps — desktop (Windows / Linux)**
```sh
# 1. Generate the recompiled game code from your copy (creates generated/).
rexglue codegen --max_jump_table_entries 2048 ge_config.toml

# 2. Configure for your platform, pointing at your local ReXGlue SDK checkout.
#    Presets: win-amd64 / win-arm64 / linux-amd64 / linux-arm64,
#    each in -debug / -release / -relwithdebinfo.
cmake --preset linux-amd64-relwithdebinfo -DREXSDK_DIR=/path/to/GoldenEye-Recomp-rexglue

# 3. Build.
cmake --build --preset linux-amd64-relwithdebinfo
```

**Android (arm64-v8a, experimental)** builds an APK with Gradle + the NDK, which
drives this same CMake — see [`android/README.md`](android/README.md). On-device
game-asset delivery is still being wired up, and cold boot uses an auto-retry
watchdog (background in [`docs/boot-startup-race.md`](docs/boot-startup-race.md)).

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

## Legal

GoldenEye 007 and all related assets are property of their respective rights
holders. This project ships **none** of that — no ROM, XEX, textures, audio, or
recompiled game code. It only automates turning a copy *you already own* into a
PC build. Don't ask for or share game files.

## License

The original code in this repository is released into the **public domain**
([The Unlicense](LICENSE)). The ReXGlue SDK it builds against has its own
(BSD-3) license.
