#!/usr/bin/env bash
# Fail if a built librexruntimerd.so is missing capabilities the native build has.
#
# SDL3 is vendored and its CMake auto-detects optional backends (audio, input hotplug, ...)
# at configure time from whatever -dev packages/pkg-config modules are on the build image,
# and silently compiles out anything it can't find — no error, no warning, just a smaller
# binary. That is how the Ubuntu 24.04 release container (issue #12's fix) shipped with no
# working audio backend: it had the GTK/X11/XInput2 dev packages the SDK's own UI code needs
# but nothing for SDL3's audio or udev backends, so SDL_InitSubSystem(SDL_INIT_AUDIO) had
# nothing to bind to, the guest's audio registration failed, and boot deadlocked with no
# window (confirmed on a Steam Deck; forcing SDL_AUDIODRIVER=dummy unblocked it). Losing
# udev also threatens gamepad hotplug detection, which matters a lot on a handheld. The ABI
# floor gate catches symbol-version regressions; this gate catches silently missing features,
# which is a different failure mode entirely and was not caught by anything until now.
#
# Usage: scripts/check-build-parity.sh <librexruntimerd.so path>
set -euo pipefail

BIN="${1:-}"
[ -n "$BIN" ] || { echo "usage: $0 <librexruntimerd.so path>" >&2; exit 2; }
[ -f "$BIN" ] || { echo "error: no such file: $BIN" >&2; exit 2; }
command -v nm >/dev/null 2>&1 || { echo "error: nm not found (install binutils)" >&2; exit 2; }
command -v strings >/dev/null 2>&1 || { echo "error: strings not found (install binutils)" >&2; exit 2; }

fail=0

# Materialize both dumps fully into variables up front, rather than piping straight into
# `grep -q` per check below: `nm/strings ... | grep -q` exits the moment grep finds its first
# match, which SIGPIPEs the still-writing nm/strings/awk process, and under `pipefail` that
# SIGPIPE (exit 141) — not grep's own 0 — becomes the pipeline's reported status. That turns
# every successful match into a spurious FAIL. `awk` and command substitution read each
# producer to EOF with no early-exiting consumer attached, so this capture step is safe; the
# `grep -qx ... <<<` membership tests below then run against the already-complete string, so
# grep exiting early has nothing upstream left to SIGPIPE.
dynsyms="$(nm -D --defined-only "$BIN" 2>/dev/null | awk '{print $NF}')"
allstrings="$(strings "$BIN")"

# check_symbol NAME IMPACT -> fail if NAME is not a defined dynamic symbol in $BIN.
check_symbol() {
  local name="$1" impact="$2"
  if ! grep -qx "$name" <<< "$dynsyms"; then
    echo "FAIL: missing SDL driver bootstrap symbol $name -- $impact" >&2
    fail=1
  fi
}

# check_soname PATTERN IMPACT -> fail if no dlopen soname string in $BIN matches PATTERN
# (an ERE, e.g. 'libudev\.so\.[0-9]+' to accept either udev SONAME).
check_soname() {
  local pattern="$1" impact="$2"
  if ! grep -qE "^${pattern}\$" <<< "$allstrings"; then
    echo "FAIL: missing dlopen soname matching '$pattern' -- $impact" >&2
    fail=1
  fi
}

check_symbol ALSA_bootstrap       "no ALSA audio backend -- SDL_INIT_AUDIO can fail on Linux desktops without PulseAudio"
check_symbol PULSEAUDIO_bootstrap "no PulseAudio audio backend -- SDL_INIT_AUDIO fails on Steam Deck / most desktop distros, deadlocking boot with no window (issue #12 regression)"
check_soname 'libpulse\.so\.0'    "PulseAudio backend cannot dlopen its runtime library even if compiled in"
check_soname 'libasound\.so\.2'   "ALSA backend cannot dlopen its runtime library even if compiled in"
check_soname 'libudev\.so\.[0-9]+' "no udev backend -- gamepad hotplug detection is degraded or absent, which matters a lot on a handheld"

if [ "$fail" -ne 0 ]; then
  {
    echo
    echo "This binary is missing capabilities the native release build has (feature parity"
    echo "regression, not an ABI regression -- see scripts/check-abi-floor.sh for that class)."
    echo "Likely cause: a -dev package SDL3's CMake probes for is missing from the build"
    echo "image (see docker/linux-release.Dockerfile) so SDL3 silently compiled the backend"
    echo "out instead of failing the build."
  } >&2
  exit 1
fi

echo "BUILD PARITY OK (ALSA + PulseAudio + udev present): $BIN"
