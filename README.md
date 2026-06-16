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
(**Windows x64**, **Linux x86-64**, and an experimental **Android arm64-v8a**
APK are attached), then drop your own GoldenEye 007 game files into the `assets/`
folder next to the binary. Run `ge.exe` on Windows or `./ge` on Linux; on Android
install the (debug-signed) APK.

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

### Common prerequisites (all platforms)

- The [ReXGlue SDK](https://github.com/jeffory/GoldenEye-Recomp-rexglue) — a local
  checkout that provides the `rexglue` CLI + runtime. The build points at it via
  `-DREXSDK_DIR=/path/to/GoldenEye-Recomp-rexglue`.
- **CMake 3.25+**, a **C++23 Clang** toolchain, and **Python 3** (used by codegen).
- Your own **GoldenEye 007 XBLA game files**, placed in `assets/`.

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

## Legal

GoldenEye 007 and all related assets are property of their respective rights
holders. This project ships **none** of that — no ROM, XEX, textures, audio, or
recompiled game code. It only automates turning a copy *you already own* into a
PC build. Don't ask for or share game files.

## License

The original code in this repository is released into the **public domain**
([The Unlicense](LICENSE)). The ReXGlue SDK it builds against has its own
(BSD-3) license.
