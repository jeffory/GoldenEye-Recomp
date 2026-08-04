#!/usr/bin/env bash
# Verifies check-abi-floor.sh both rejects a too-new binary and accepts a compliant one.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECK="$ROOT/scripts/check-abi-floor.sh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

fails=0
ran=0
skipped=0
note() { printf '%s\n' "$*"; }

# --- Case 1: the known-bad shipped v1.6.0 artifact must be REJECTED ----------
# It requires GLIBC_2.43 (sqrtf, acosf, asinf, atan2f, log10f) and
# GLIBCXX_3.4.35 (std::__detail::__wait_impl and friends). See issue #12.
#
# This fixture is NOT stable: `dist/bundle/` is `cut-release.sh`'s output dir, so
# after a successful container release it holds a compliant binary (case 1 would
# then have nothing to reject), and after a `--no-container` run it holds a bad
# one again. Probe what's actually there before asserting anything, rather than
# assuming it's still the known-bad artifact.
BAD="$ROOT/dist/bundle/librexruntimerd.so"
if [ ! -f "$BAD" ] || ! objdump -p "$BAD" 2>/dev/null | grep -q 'GLIBC_2\.43'; then
  note "SKIP (fixture not the known-bad artifact): $BAD is missing or no longer requires GLIBC_2.43 (dist/bundle/ reflects whatever was built last — rebuild the pre-container native bundle to exercise this case)"
  skipped=$((skipped + 1))
else
  ran=$((ran + 1))
  if out="$("$CHECK" "$BAD" 2>&1)"; then
    note "FAIL case 1: checker accepted a GLIBC_2.43 binary"; fails=1
  else
    for want in GLIBC_2.43 GLIBCXX_3.4.35 sqrtf; do
      case "$out" in *"$want"*) ;; *) note "FAIL case 1: output missing '$want'"; fails=1 ;; esac
    done
    [ "$fails" -eq 0 ] && note "ok case 1: rejected the known-bad bundle"
  fi
fi

# --- Case 2: a trivially-compliant binary must be ACCEPTED -------------------
# /bin/true from the host is ancient-ABI and always within any sane floor.
ran=$((ran + 1))
if "$CHECK" /bin/true >/dev/null 2>&1; then
  note "ok case 2: accepted /bin/true"
else
  note "FAIL case 2: checker rejected /bin/true"; fails=1
fi

# --- Case 3: version compare must be numeric, not lexical -------------------
# With a 2.9 ceiling, a binary needing GLIBC_2.34 must be rejected; lexical
# comparison would wrongly accept it because "2.34" < "2.9" as a string.
ran=$((ran + 1))
if ABI_MAX_GLIBC=2.9 "$CHECK" /bin/true >/dev/null 2>&1; then
  note "FAIL case 3: lexical comparison bug (2.34 treated as <= 2.9)"; fails=1
else
  note "ok case 3: numeric version comparison"
fi

# --- Case 4: missing file is a usage error (exit 2), not a pass -------------
ran=$((ran + 1))
"$CHECK" "$TMP/nope" >/dev/null 2>&1 && rc=0 || rc=$?
if [ "${rc:-0}" -eq 2 ]; then note "ok case 4: missing file -> exit 2"
else note "FAIL case 4: missing file gave exit ${rc:-0}, want 2"; fails=1; fi

note "ran $ran/4 cases, skipped $skipped"
[ "$fails" -eq 0 ] && note "ALL PASS" || note "FAILURES"
exit "$fails"
