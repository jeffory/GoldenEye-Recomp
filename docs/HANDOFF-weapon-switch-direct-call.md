# Follow-up: switch weapons by calling the guest function directly (RE later)

**Status:** deferred optimization. The shipping feature switches weapons by
**injecting the native Y button** (see the scrollwheel/number weapon-select
work). This note captures the reverse-engineering done toward the *direct
function-call* approach so a future pass can pick it up for instant jump-to-N.

## Why we deferred it
Injecting Y (cycle-to-target) is guaranteed-correct and shipped today. The
direct-call path would give instant multi-step jumps but needs more RE and
crash-prone runtime trials. Net: not worth blocking the feature.

## What we learned (desktop, confirmed)
- `E` (= 360 **Y** button, `BTN_Y = 0x8000` in the slot-0 pad at `0x830C8B9C`)
  is the game's native weapon switch; one press = one forward cycle.
- The equipped-weapon block is at fixed guest addresses:
  `0x447f10b0` equipped id, `0x447f10c8` a second "agreement" copy,
  `0x447f10f4` clip, `0x447f10c0` weapon-def ptr, `0x447f1104` another id mirror.
  **Writing any of these does NOT switch** — `0x447f10b0` desyncs the agreement
  gate (snapshot goes invalid); `0x447f1104` sticks but nothing happens. The game
  sets these *from* its switch routine; it does not poll them.
- Player struct: `+0x928` (2344) = current weapon (also seen in
  `ge_ce_remote_weapon_sfx`). Weapon-stats array @ `0x82421968` (stride 0x38).

## The function trail (generated recomp)
The recompiled code keeps original PPC asm as comments — grep those.
- `sub_820A7508` (guest `0x820A7508`) writes current-weapon: `stw r27,2344(r11)`
  plus `+0x954` and `+0xB04`. It is a **per-hand weapon applier**.
  - Signature observed at callers: `sub_820A7508(r3 = hand [1 then 0],
    r4 = resolved weapon)`, where `r4` comes from `sub_820A0D30(hand)` and is a
    **resolved weapon object/struct, not a raw id**.
  - 7 callers total; e.g. `sub_820BC030` (ge_recomp.1.cpp:33183/33195) calls it
    in a hand-1 then hand-0 pair as part of a broader refresh.
- **Not yet found:** the higher-level "select weapon N" entry the Y button
  invokes (the one that runs the full switch incl. draw animation / viewmodel).
  That is the function to call. Start by tracing upward from the pad-Y read to
  whatever eventually reaches `sub_820A7508`, and identify how a target weapon
  is resolved (`sub_820A0D30`).

## Tooling added for this (kept)
- `memscan` gained a live `write <ga> <16|32> <val>` command (ge_gamestate.cpp,
  behind `ge_gamestate_diag`, desktop-only) — poke guest fields at runtime.
  Drive via `/tmp/ge_scan.cmd`, output `/tmp/ge_scan.out`.

## Build/run gotcha
The desktop binary is **`out/build/linux-amd64-relwithdebinfo/GoldenEye`**
(CMake `OUTPUT_NAME "GoldenEye"`), NOT `ge`. `CLAUDE.md`'s run recipe says `ge`
and is wrong.
