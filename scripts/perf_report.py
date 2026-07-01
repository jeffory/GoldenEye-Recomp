#!/usr/bin/env python3
"""GoldenEye-Recomp performance report.

Analyzes the game's perf telemetry and names the likeliest bottlenecks:
  - ge.log      GEFPS / GESHOWN / GESPIKE lines (ge_fps_log / ge_spike_log)
  - ge_perf.csv per-frame rex::perf counter dump (pause menu > VIDEO >
                "Record Perf CSV", or --perf_log_csv=<path> on desktop)

Usage:
  adb pull /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log
  adb pull /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge_perf.csv
  python3 scripts/perf_report.py ge.log ge_perf.csv

Any mix of .log and .csv files may be passed; each is auto-detected.
stdlib-only, no dependencies.
"""

import csv
import re
import statistics
import sys

# Stage columns (CSV names / GESPIKE keys) that partition frame time.
# NOTE: raw cp_execute_us CONTAINS cp_wait_reg_mem_us (WAIT_REG_MEM executes as
# a packet inside ExecutePrimaryBuffer), so the report derives a net value --
# otherwise "CP execute" reads as a full frame when the CP is really parked in
# the CPU<->GPU fence for most of it (pacing, not work).
STAGES = {
    "cp_execute_net_us": "CP work (PM4 translation, net of fence wait)",
    "cp_idle_us": "CP idle (waiting for guest work)",
    "cp_wait_reg_mem_us": "CP fence wait (WAIT_REG_MEM pacing/sync)",
    "present_block_us": "Present (UI-thread paint+present block)",
    "guest_gpu_wait_us": "Guest GPU-wait (thread-time, can exceed wall)",
    "gpu_frame_us": "GPU execution (Vulkan timestamps)",
}


def derive_stages(fr):
    """Add derived stage fields to a frame/spike dict (mutates + returns it)."""
    # max(0, ...) keeps the operand type: int for CSV rows, float for log lines.
    fr["cp_execute_net_us"] = max(
        0, fr.get("cp_execute_us", 0) - fr.get("cp_wait_reg_mem_us", 0))
    return fr

GESPIKE_RE = re.compile(
    r"GESPIKE dt=(?P<dt>[\d.]+)ms med=(?P<med>[\d.]+)ms "
    r"cpexec=(?P<cp_execute_us>\d+)us cpidle=(?P<cp_idle_us>\d+)us "
    r"wrm=(?P<cp_wait_reg_mem_us>\d+)us present=(?P<present_block_us>\d+)us "
    r"gwait=(?P<guest_gpu_wait_us>\d+)us gpu=(?P<gpu_frame_us>\d+)us "
    r"draws=(?P<draws>\d+) stalls=(?P<stalls>\d+) starved=(?P<starved>\d+)")
GEFPS_RE = re.compile(
    r"GEFPS avg=(?P<avg>[\d.]+) low1=(?P<low1>[\d.]+) worst=(?P<worst>[\d.]+) "
    r"hitch=(?P<hitch>\d+) gaps=(?P<gaps>\d+) maxgap=(?P<maxgap>\d+)ms "
    r"n=(?P<n>\d+) dur=(?P<dur>[\d.]+)s")
GESHOWN_RE = re.compile(
    r"GESHOWN shown/s=(?P<shown>[\d.]+) new/s=(?P<new>[\d.]+) "
    r"refresh/s=(?P<refresh>[\d.]+) drop/s=(?P<drop>[\d.]+) "
    r"submit/s=(?P<submit>[\d.]+) paint=(?P<paint>[\d.]+)ms")


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    k = min(len(sorted_vals) - 1, max(0, int(round(p / 100.0 * (len(sorted_vals) - 1)))))
    return sorted_vals[k]


def fps(ms):
    return 1000.0 / ms if ms > 0 else 0.0


def analyze_log(path):
    gefps, geshown, gespikes = [], [], []
    with open(path, errors="replace") as f:
        for line in f:
            m = GEFPS_RE.search(line)
            if m:
                gefps.append({k: float(v) for k, v in m.groupdict().items()})
                continue
            m = GESHOWN_RE.search(line)
            if m:
                geshown.append({k: float(v) for k, v in m.groupdict().items()})
                continue
            m = GESPIKE_RE.search(line)
            if m:
                gespikes.append(
                    derive_stages({k: float(v) for k, v in m.groupdict().items()}))

    print(f"\n=== {path} (ge.log telemetry) ===")
    if gefps:
        last = gefps[-1]
        print(f"Session: {last['dur']:.0f}s, {int(last['n'])} frames, "
              f"avg {last['avg']:.1f}fps, 1%-low {last['low1']:.1f}, "
              f"worst {last['worst']:.1f}, hitches {int(last['hitch'])}, "
              f"gaps {int(last['gaps'])} (max {int(last['maxgap'])}ms)")
    if geshown:
        shown = sorted(s["shown"] for s in geshown)
        drops = sum(s["drop"] for s in geshown)
        paints = sorted(s["paint"] for s in geshown)
        print(f"Displayed: median {pct(shown, 50):.1f} shown/s "
              f"(p10 {pct(shown, 10):.1f}), total dropped {drops:.0f} frames, "
              f"paint block p50 {pct(paints, 50):.2f}ms p95 {pct(paints, 95):.2f}ms")
        prod = sorted(s["refresh"] for s in geshown)
        if pct(prod, 50) - pct(shown, 50) > 3:
            print(f"  !! produced ({pct(prod, 50):.1f}/s) > shown "
                  f"({pct(shown, 50):.1f}/s): the present path is dropping frames")
    if gespikes:
        cluster_spikes(gespikes)
    else:
        print("No GESPIKE lines (no frame exceeded 2x median, or ge_spike_log off).")


def cluster_spikes(spikes):
    print(f"Spikes: {len(spikes)} GESPIKE lines")
    clusters = {}
    for s in spikes:
        stage_vals = {k: s.get(k, 0.0) for k in STAGES}
        dominant = max(stage_vals, key=stage_vals.get)
        if stage_vals[dominant] <= 0:
            dominant = "(unattributed -- stage feeders inactive?)"
        clusters.setdefault(dominant, []).append(s)
    for stage, items in sorted(clusters.items(), key=lambda kv: -len(kv[1])):
        dts = sorted(i["dt"] for i in items)
        label = STAGES.get(stage, stage)
        print(f"  {len(items):4d}x dominated by {label}: "
              f"dt p50 {pct(dts, 50):.1f}ms max {dts[-1]:.1f}ms")
        starved = sum(1 for i in items if i.get("starved", 0) > 0)
        if starved:
            print(f"        ({starved} spikes had CP-starvation episodes recorded)")


def analyze_csv(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print(f"\n=== {path}: empty ===")
        return
    frames = []
    for r in rows:
        try:
            frames.append(
                derive_stages({k: int(v) for k, v in r.items() if v not in ("", None)}))
        except ValueError:
            continue
    fts = sorted(fr.get("frame_time_us", 0) / 1000.0 for fr in frames)
    fts = [v for v in fts if v > 0]

    print(f"\n=== {path} (per-frame CSV, {len(frames)} frames) ===")
    if fts:
        print("Frametime  p50 {:6.2f}ms ({:5.1f}fps)   p90 {:6.2f}ms   "
              "p95 {:6.2f}ms   p99 {:6.2f}ms ({:5.1f}fps)".format(
                  pct(fts, 50), fps(pct(fts, 50)), pct(fts, 90),
                  pct(fts, 95), pct(fts, 99), fps(pct(fts, 99))))

    print("Per-stage time (us/frame):")
    stage_p95 = {}
    for col, label in STAGES.items():
        vals = sorted(fr.get(col, 0) for fr in frames)
        if not any(vals):
            print(f"  {label:52s} (no data)")
            continue
        stage_p95[col] = pct(vals, 95)
        print(f"  {label:52s} p50 {pct(vals, 50):7d}  p95 {pct(vals, 95):7d}  "
              f"max {vals[-1]:7d}")

    # Spikes: frames beyond 2x median, clustered by dominant stage.
    if fts:
        med_us = statistics.median(fr.get("frame_time_us", 0) for fr in frames)
        spikes = [fr for fr in frames if fr.get("frame_time_us", 0) > 2 * med_us]
        if spikes:
            print(f"Spike frames (> 2x median of {med_us / 1000.0:.1f}ms): {len(spikes)}")
            clusters = {}
            for fr in spikes:
                vals = {k: fr.get(k, 0) for k in STAGES}
                dom = max(vals, key=vals.get)
                if vals[dom] <= 0:
                    dom = "(unattributed)"
                clusters.setdefault(dom, 0)
                clusters[dom] += 1
            for dom, cnt in sorted(clusters.items(), key=lambda kv: -kv[1]):
                print(f"  {cnt:4d}x dominated by {STAGES.get(dom, dom)}")

    # Top-3 candidates: stages with the largest p95 budget share.
    if stage_p95:
        print("Top bottleneck candidates (by p95 share of a 16.7ms budget):")
        for col, v in sorted(stage_p95.items(), key=lambda kv: -kv[1])[:3]:
            print(f"  {STAGES[col]:52s} {v / 16667.0 * 100:5.1f}%")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    for path in argv[1:]:
        if path.endswith(".csv"):
            analyze_csv(path)
        else:
            analyze_log(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
