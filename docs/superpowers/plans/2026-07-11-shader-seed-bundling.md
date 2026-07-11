# Bundled Shader-Storage Seed Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship grown shader/pipeline storage (`.xsh`/`.xpso`) inside the APK and copy it to the cache dir on first boot, so first-install users precompile pipelines behind the loader screen instead of hitching in combat.

**Architecture:** Game repo only. Seed binaries live in `android/app/src/main/assets/shader_seed/` (default gradle assets dir — no build config needed); a refresh script pulls them from the Thor; `cut-release.sh` warns when they age past 30 days; `GoldenEyeActivity.onCreate` stream-copies each seed file to `<external-files>/cache/shaders/shareable/` only when absent (tmp+rename, never fatal). The SDK's existing boot precompile + item-1 debounce do the rest. Spec: `docs/superpowers/specs/2026-07-11-shader-seed-bundling-design.md`.

**Tech Stack:** Java (Android), bash, gradle, adb.

## Global Constraints

- Game repo only (`/home/keith/Projects/GoldenEye-Recomp`), branch `feat/shader-seed` off `develop`. No SDK changes, no worktrees.
- Seed filenames are exactly `584108A9.xsh` and `584108A9.fbo.vk.xpso`; destination is `<external-files>/cache/shaders/shareable/<name>`.
- Copy-if-absent ONLY (existing files never overwritten); copy via `<name>.tmp` + rename; all failures log a warning and continue (never fatal); one info log per copied file: `shader seed: copied <name> (<bytes> bytes)`.
- Thor adb serial: `192.168.1.182:41285` (multiple devices attached — always pass `-s` or `ANDROID_SERIAL`). Device log: `/sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log`; Java logs land in logcat under tag `GEBOOT`.
- Android build: `cd android && ./gradlew :app:installDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue`.
- Acceptance instrument: GESPIKE `pcomp=` field (from the 2A counters). Combat acceptance = no combat-time spike with `pcomp` > 10ms on a fresh-seeded install.
- If the Thor's screen may be asleep, wake it before `monkey` launches: `input keyevent KEYCODE_WAKEUP` + `svc power stayon true` (a sleeping screen silently kills the app ~17s after launch).

---

### Task 1: Seed assets + refresh script

**Files:**
- Create: `scripts/refresh-shader-seed.sh` (executable)
- Create: `android/app/src/main/assets/shader_seed/584108A9.xsh` (pulled binary)
- Create: `android/app/src/main/assets/shader_seed/584108A9.fbo.vk.xpso` (pulled binary)

**Interfaces:**
- Produces: the two committed seed binaries at the exact paths above (Task 3's copy code and Task 4's packaging check depend on the `shader_seed/` asset dir name).

- [ ] **Step 1: Create the branch**

```bash
cd /home/keith/Projects/GoldenEye-Recomp && git checkout develop && git checkout -b feat/shader-seed
```

- [ ] **Step 2: Write the refresh script**

Create `scripts/refresh-shader-seed.sh`:

```bash
#!/usr/bin/env bash
# Pull the device-grown shader/pipeline storage into the APK seed assets.
# Run after playing new content on the device, then commit the result.
# The seed gives first-install users a boot-time precompile instead of
# first-shot pipeline-compile hitches (see
# docs/superpowers/specs/2026-07-11-shader-seed-bundling-design.md).
#
# Usage: scripts/refresh-shader-seed.sh [adb-serial]
set -euo pipefail

DEVICE="${1:-${ANDROID_SERIAL:-}}"
ADB=(adb)
[ -n "$DEVICE" ] && ADB=(adb -s "$DEVICE")

SRC=/sdcard/Android/data/com.sunjaycy.goldeneye/files/cache/shaders/shareable
DEST="$(cd "$(dirname "$0")/.." && pwd)/android/app/src/main/assets/shader_seed"
mkdir -p "$DEST"

for f in 584108A9.xsh 584108A9.fbo.vk.xpso; do
  old=$(stat -c%s "$DEST/$f" 2>/dev/null || echo 0)
  "${ADB[@]}" pull "$SRC/$f" "$DEST/$f"
  new=$(stat -c%s "$DEST/$f")
  echo "$f: ${old} -> ${new} bytes"
done
echo "Done. Review and commit android/app/src/main/assets/shader_seed/"
```

Then: `chmod +x scripts/refresh-shader-seed.sh`

- [ ] **Step 3: Run it against the Thor (this is the test)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
./scripts/refresh-shader-seed.sh 192.168.1.182:41285
ls -la android/app/src/main/assets/shader_seed/
```
Expected: both files pulled; `.xsh` roughly 30–150KB, `.xpso` roughly 30–150KB (the Thor has menu + Dam-combat coverage from the 2026-07-11 sessions). If the game is currently running on the device that's fine — the SDK flushes these files continuously.

- [ ] **Step 4: Commit**

```bash
git add scripts/refresh-shader-seed.sh android/app/src/main/assets/shader_seed/
git commit -m "feat(android): bundle shader-storage seed + refresh script"
```

---

### Task 2: cut-release staleness warning

**Files:**
- Modify: `scripts/cut-release.sh` (insert before the line `# --- build: Android signed release APK -------------------------------------`, currently line 95)

**Interfaces:**
- Consumes: Task 1's `android/app/src/main/assets/shader_seed/` directory (checks its last git-commit date).

- [ ] **Step 1: Insert the warning block**

Directly above the `# --- build: Android signed release APK ---...` section header, add:

```bash
# --- shader seed freshness (warn only) --------------------------------------
# The bundled first-install shader seed should track real playthrough coverage;
# refresh with scripts/refresh-shader-seed.sh after playing new content.
SEED_DIR="android/app/src/main/assets/shader_seed"
if [ -d "$SEED_DIR" ]; then
  SEED_COMMIT_TS=$(git log -1 --format=%ct -- "$SEED_DIR" 2>/dev/null || echo "")
  if [ -n "$SEED_COMMIT_TS" ]; then
    SEED_AGE_DAYS=$(( ( $(date +%s) - SEED_COMMIT_TS ) / 86400 ))
    if [ "$SEED_AGE_DAYS" -gt 30 ]; then
      echo "WARNING: shader seed is ${SEED_AGE_DAYS} days old -- consider scripts/refresh-shader-seed.sh" >&2
    fi
  fi
fi
```

- [ ] **Step 2: Test the logic both ways**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
bash -n scripts/cut-release.sh   # syntax check
# Fresh seed (committed in Task 1 minutes ago) -> no warning:
bash -c 'SEED_DIR="android/app/src/main/assets/shader_seed"; SEED_COMMIT_TS=$(git log -1 --format=%ct -- "$SEED_DIR"); SEED_AGE_DAYS=$(( ( $(date +%s) - SEED_COMMIT_TS ) / 86400 )); echo "age=${SEED_AGE_DAYS}d"; [ "$SEED_AGE_DAYS" -gt 30 ] && echo WARN || echo OK'
# Simulate stale (timestamp 40 days back) -> warning fires:
bash -c 'SEED_COMMIT_TS=$(( $(date +%s) - 40*86400 )); SEED_AGE_DAYS=$(( ( $(date +%s) - SEED_COMMIT_TS ) / 86400 )); [ "$SEED_AGE_DAYS" -gt 30 ] && echo "WARN fires (${SEED_AGE_DAYS}d)" || echo "BUG: no warn"'
```
Expected: `age=0d` + `OK`, then `WARN fires (40d)`.

- [ ] **Step 3: Commit**

```bash
git add scripts/cut-release.sh
git commit -m "feat(release): warn when the bundled shader seed is stale (>30 days)"
```

---

### Task 3: First-boot copy in GoldenEyeActivity

**Files:**
- Modify: `android/app/src/main/java/com/sunjaycy/goldeneye/GoldenEyeActivity.java` — imports (after line 31 `import java.io.FileReader;`) and `onCreate` (insert call after the ge.log-deletion try block, before `super.onCreate(savedInstanceState);` at line 134), plus one new private method.

**Interfaces:**
- Consumes: APK assets under `shader_seed/` (Task 1).
- Produces: logcat/`GEBOOT` lines `shader seed: copied <name> (<bytes> bytes)` — Task 4 greps for these.

- [ ] **Step 1: Add imports**

After `import java.io.FileReader;` add:

```java
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
```

- [ ] **Step 2: Call the seeder from onCreate**

Directly after the ge.log-deletion `try { ... } catch (Throwable t) { // best-effort }` block (before `super.onCreate(savedInstanceState);`), add:

```java
        // First-install shader/pipeline storage seed: gives the runtime's
        // boot-time precompile something to chew on, so first-shot pipeline
        // compiles happen behind the loader screen instead of mid-combat.
        seedShaderStorageFromAssets();
```

- [ ] **Step 3: Add the method**

Add as a private method in `GoldenEyeActivity` (e.g. directly after `onCreate`):

```java
    /**
     * Copy bundled shader-storage seed files (assets/shader_seed/*) into the
     * runtime's cache dir, only when absent. tmp+rename so a mid-copy kill
     * can't leave a truncated file; every failure is non-fatal (the game just
     * boots seedless, as before this feature). See
     * docs/superpowers/specs/2026-07-11-shader-seed-bundling-design.md.
     */
    private void seedShaderStorageFromAssets() {
        try {
            String[] names = getAssets().list("shader_seed");
            if (names == null || names.length == 0) {
                return;
            }
            File destDir = new File(getExternalFilesDir(null), "cache/shaders/shareable");
            for (String name : names) {
                File dest = new File(destDir, name);
                if (dest.exists()) {
                    continue;
                }
                if (!destDir.isDirectory() && !destDir.mkdirs()) {
                    Log.w(TAG, "shader seed: cannot create " + destDir);
                    return;
                }
                File tmp = new File(destDir, name + ".tmp");
                long bytes = 0;
                try (InputStream in = getAssets().open("shader_seed/" + name);
                     OutputStream out = new FileOutputStream(tmp)) {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                        bytes += n;
                    }
                }
                if (tmp.renameTo(dest)) {
                    Log.i(TAG, "shader seed: copied " + name + " (" + bytes + " bytes)");
                } else {
                    Log.w(TAG, "shader seed: rename failed for " + name);
                    tmp.delete();
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "shader seed: copy failed (continuing without)", t);
        }
    }
```

- [ ] **Step 4: Build**

```bash
cd /home/keith/Projects/GoldenEye-Recomp/android
ANDROID_SERIAL=192.168.1.182:41285 ./gradlew :app:assembleDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue
```
Expected: BUILD SUCCESSFUL.

- [ ] **Step 5: Packaging check (spec test #4)**

```bash
unzip -l /home/keith/Projects/GoldenEye-Recomp/android/app/build/outputs/apk/debug/app-debug.apk | grep shader_seed
```
Expected: both `assets/shader_seed/584108A9.xsh` and `assets/shader_seed/584108A9.fbo.vk.xpso` listed with nonzero sizes.

- [ ] **Step 6: Commit**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
git add android/app/src/main/java/com/sunjaycy/goldeneye/GoldenEyeActivity.java
git commit -m "feat(android): copy bundled shader seed into the cache dir on first boot"
```

---

### Task 4: Thor verification

**Files:** none (verification only).

**NOTE FOR THE CONTROLLER:** step 4 (combat acceptance) needs the USER to play one short Dam session; everything else is adb-automatable.

- [ ] **Step 1: Install + fresh-install simulation (keep it cheap — no uninstall)**

Simulate "first install" by wiping the entire shader cache dir (storage + driver blob) rather than uninstalling (uninstall would wipe game assets and force the 603MB tar-push restore):

```bash
ADB="adb -s 192.168.1.182:41285"
cd /home/keith/Projects/GoldenEye-Recomp/android
ANDROID_SERIAL=192.168.1.182:41285 ./gradlew :app:installDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue
adb -s 192.168.1.182:41285 shell am force-stop com.sunjaycy.goldeneye
adb -s 192.168.1.182:41285 shell rm -rf /sdcard/Android/data/com.sunjaycy.goldeneye/files/cache/shaders
adb -s 192.168.1.182:41285 shell input keyevent KEYCODE_WAKEUP
adb -s 192.168.1.182:41285 shell svc power stayon true
adb -s 192.168.1.182:41285 logcat -c
adb -s 192.168.1.182:41285 shell monkey -p com.sunjaycy.goldeneye 1
sleep 8
adb -s 192.168.1.182:41285 logcat -d -s GEBOOT | grep "shader seed"
adb -s 192.168.1.182:41285 shell ls -la /sdcard/Android/data/com.sunjaycy.goldeneye/files/cache/shaders/shareable/
```
Expected: two `shader seed: copied ...` logcat lines; both files present with sizes matching the bundled seed; no `.tmp` remnants. Then poll `grep -ac "rendered#" .../ge.log` until >0 (boot completes; note it may take a bit longer than usual — the precompile is compiling every seeded pipeline with a cold driver cache).

- [ ] **Step 2: Existing-install no-op**

```bash
adb -s 192.168.1.182:41285 shell am force-stop com.sunjaycy.goldeneye
adb -s 192.168.1.182:41285 logcat -c
adb -s 192.168.1.182:41285 shell monkey -p com.sunjaycy.goldeneye 1
sleep 8
adb -s 192.168.1.182:41285 logcat -d -s GEBOOT | grep -c "shader seed: copied" || echo "0 copies (correct)"
```
Expected: `0 copies (correct)` — files exist, nothing overwritten.

- [ ] **Step 3: Confirm the driver cache repopulated**

```bash
adb -s 192.168.1.182:41285 shell 'grep -a "Stored persistent VkPipelineCache" /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log | head -1'
```
(Run during/after Step 1's boot session, once the game has been up >10s past the precompile.) Expected: a store line — the seeded precompile fed the item-1 debounce.

- [ ] **Step 4: Combat acceptance (USER plays)**

With the Step-1 state (seed copied, driver cache freshly rebuilt from the seed): user plays Dam, shoots several fresh guard types. Then:

```bash
adb -s 192.168.1.182:41285 pull /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log out/seed-acceptance-ge.log
python3 - <<'EOF'
import sys
sys.path.insert(0, "scripts")
import perf_report as pr
worst = 0.0
for line in open("out/seed-acceptance-ge.log", errors="replace"):
    m = pr.GESPIKE_RE.search(line)
    if m:
        d = {k: (float(v) if v is not None else 0.0) for k, v in m.groupdict().items()}
        worst = max(worst, d.get("pipeline_compile_us", 0.0))
print(f"worst pcomp in any spike: {worst/1000:.1f}ms")
print("PASS" if worst <= 10000 else "FAIL (combat compile hitch still present)")
EOF
```
Expected: `PASS` (worst combat `pcomp` ≤ 10ms). If a spike >10ms appears, check whether its timestamp is during the boot/load screen (gpu≈0, acceptable) before calling it a failure.

- [ ] **Step 5: Record results in the task report** (no commit)

---

### Task 5: Finish

- [ ] **Step 1: Review the branch diff**

```bash
git -C /home/keith/Projects/GoldenEye-Recomp diff develop...feat/shader-seed --stat
```

- [ ] **Step 2: Merge**

```bash
cd /home/keith/Projects/GoldenEye-Recomp && git checkout develop && \
  git merge --no-ff feat/shader-seed -m "merge: bundled shader-storage seed (first-install precompile)" && \
  git branch -d feat/shader-seed
```

- [ ] **Step 3: Commit spec + plan docs**

```bash
git add docs/superpowers/specs/2026-07-11-shader-seed-bundling-design.md \
        docs/superpowers/plans/2026-07-11-shader-seed-bundling.md
git commit -m "docs: shader-seed bundling spec + plan (perf deep-dive item 2B part 1)"
```
