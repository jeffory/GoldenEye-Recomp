#!/usr/bin/env bash
# Fail if any binary requires a glibc/libstdc++ symbol version above the release floor.
#
# Shipped Linux bundles link against the *player's* libc/libm/libstdc++, so the build
# host's symbol-version floor leaks into the release. Issue #12: a Fedora 44 build
# (glibc 2.43 / GCC 16) would not load on a Steam Deck. This gate catches that class of
# regression at release time instead of on a player's device.
#
# Usage: scripts/check-abi-floor.sh <binary> [<binary> ...]
# Env:   ABI_MAX_GLIBC   (default 2.39)     ABI_MAX_GLIBCXX (default 3.4.33)
set -euo pipefail

MAX_GLIBC="${ABI_MAX_GLIBC:-2.39}"
MAX_GLIBCXX="${ABI_MAX_GLIBCXX:-3.4.33}"

[ $# -gt 0 ] || { echo "usage: $0 <binary> [<binary> ...]" >&2; exit 2; }
command -v objdump >/dev/null 2>&1 || { echo "error: objdump not found (install binutils)" >&2; exit 2; }

# ver_gt A B -> true when A is strictly newer than B, compared component-wise.
# `sort -V` is what makes 2.9 < 2.39 and 3.4.9 < 3.4.33 come out right; a plain
# string compare gets both backwards.
ver_gt() {
  [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]
}

fail=0
for bin in "$@"; do
  [ -f "$bin" ] || { echo "error: no such file: $bin" >&2; exit 2; }

  # The "Version References" block lists one required version per line, e.g.
  #     0x069691a3 0x00 22 GLIBC_2.43
  # Other lines in the block ("required from libm.so.6:") end in a library name
  # and fall through the case below untouched.
  reqs="$(objdump -p "$bin" | sed -n '/Version References:/,/^$/p' | awk '{print $NF}')"

  for req in $reqs; do
    case "$req" in
      GLIBC_[0-9]*)   name=GLIBC;   ver="${req#GLIBC_}";   max="$MAX_GLIBC" ;;
      GLIBCXX_[0-9]*) name=GLIBCXX; ver="${req#GLIBCXX_}"; max="$MAX_GLIBCXX" ;;
      # GLIBC_ABI_GNU2_TLS, CXXABI_*, GCC_* and block headers are not gated.
      *) continue ;;
    esac
    if ver_gt "$ver" "$max"; then
      echo "FAIL: $(basename "$bin") requires ${name}_${ver} (floor ${name}_${max})" >&2
      objdump -T "$bin" | grep -F "($req)" | awk '{print "        " $NF}' | sort -u >&2
      fail=1
    fi
  done
done

if [ "$fail" -ne 0 ]; then
  {
    echo
    echo "These binaries exceed the release ABI floor and will not load on older distros"
    echo "(this is what broke the Steam Deck in issue #12)."
    echo "Build the release in the container instead: scripts/build-linux-container.sh"
  } >&2
  exit 1
fi

echo "ABI floor OK (<= GLIBC_$MAX_GLIBC, GLIBCXX_$MAX_GLIBCXX): $*"
