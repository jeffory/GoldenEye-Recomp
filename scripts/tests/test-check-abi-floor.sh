#!/usr/bin/env bash
# Verifies check-abi-floor.sh both rejects a too-new binary and accepts a compliant one.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECK="$ROOT/scripts/check-abi-floor.sh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

fails=0
note() { printf '%s\n' "$*"; }

# --- Case 1: the known-bad shipped v1.6.0 artifact must be REJECTED ----------
# It requires GLIBC_2.43 (sqrtf, acosf, asinf, atan2f, log10f) and
# GLIBCXX_3.4.35 (std::__detail::__wait_impl and friends). See issue #12.
BAD="$ROOT/dist/bundle/librexruntimerd.so"
if [ ! -f "$BAD" ]; then
  note "SKIP case 1: $BAD not present (rebuild a bundle to exercise it)"
else
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
if "$CHECK" /bin/true >/dev/null 2>&1; then
  note "ok case 2: accepted /bin/true"
else
  note "FAIL case 2: checker rejected /bin/true"; fails=1
fi

# --- Case 3: version compare must be numeric, not lexical -------------------
# With a 2.9 ceiling, a binary needing GLIBC_2.34 must be rejected; lexical
# comparison would wrongly accept it because "2.34" < "2.9" as a string.
if ABI_MAX_GLIBC=2.9 "$CHECK" /bin/true >/dev/null 2>&1; then
  note "FAIL case 3: lexical comparison bug (2.34 treated as <= 2.9)"; fails=1
else
  note "ok case 3: numeric version comparison"
fi

# --- Case 4: missing file is a usage error (exit 2), not a pass -------------
"$CHECK" "$TMP/nope" >/dev/null 2>&1 && rc=0 || rc=$?
if [ "${rc:-0}" -eq 2 ]; then note "ok case 4: missing file -> exit 2"
else note "FAIL case 4: missing file gave exit ${rc:-0}, want 2"; fails=1; fi

[ "$fails" -eq 0 ] && note "ALL PASS" || note "FAILURES"
exit "$fails"
