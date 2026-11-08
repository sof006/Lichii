#!/usr/bin/env python3
"""Summarise a lichii-diag bundle: frame times, vsync pacing, GPU clocks, pipeline builds.

    python3 analyze_bundle.py <bundle-dir-or-zip>

Reads frametimes.jsonl, gpu.csv, pipeline_build.log and eden_log.txt when present.
"""

import argparse
import csv
import json
import os
import statistics
import sys
import zipfile


def load_jsonl(path):
    records = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return records


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def fmt(value, unit="ms"):
    return f"{value:.1f}{unit}"


def section(title):
    print()
    print(title)
    print("-" * len(title))


def analyse_frames(records):
    windows = [r["frame_times"] for r in records
               if isinstance(r.get("frame_times"), dict) and r["frame_times"].get("frames")]
    marks = [r for r in records if r.get("mark")]
    if not windows:
        print("no frame_time windows with frames — the game was probably not presenting")
        return None

    intervals = []
    for window in windows:
        intervals.extend(window.get("intervals_ms") or [])

    fps_values = [float(w.get("fps") or 0.0) for w in windows if w.get("fps")]
    stutters = sum(int(w.get("stutters") or 0) for w in windows)
    frames = sum(int(w.get("frames") or 0) for w in windows)

    section("Frame timing")
    print(f"windows: {len(windows)}   frames counted: {frames}   bookmarks: {len(marks)}")
    if fps_values:
        print(f"fps      median {statistics.median(fps_values):.1f}   "
              f"min {min(fps_values):.1f}   max {max(fps_values):.1f}")
    if intervals:
        print(f"intervals p50 {fmt(percentile(intervals, 0.50))}   "
              f"p90 {fmt(percentile(intervals, 0.90))}   "
              f"p99 {fmt(percentile(intervals, 0.99))}   "
              f"max {fmt(max(intervals))}")
        spread = percentile(intervals, 0.99) - percentile(intervals, 0.50)
        print(f"p99-p50 spread: {fmt(spread)}")
    print(f"stutters (>2x median): {stutters}")

    # vsync buckets: fractions of frames landing at 1/2/3/4+ refresh intervals
    buckets = {}
    for window in windows:
        for key, value in (window.get("vsync_buckets") or {}).items():
            buckets.setdefault(key, []).append(float(value))
    if buckets:
        print("vsync buckets (mean share):", end=" ")
        for key in sorted(buckets, key=lambda k: (len(k), k)):
            print(f"{key}:{statistics.mean(buckets[key]):.2f}", end="  ")
        print()

    section("Worst windows")
    worst = sorted(windows, key=lambda w: float(w.get("max_ms") or 0), reverse=True)[:8]
    for window in worst:
        print(f"  max {fmt(float(window.get('max_ms') or 0)):>9}  "
              f"p99 {fmt(float(window.get('p99_ms') or 0)):>9}  "
              f"fps {float(window.get('fps') or 0):5.1f}  "
              f"stutters {window.get('stutters')}")
    return {"windows": windows, "worst": worst}


def analyse_gpu(path):
    if not os.path.exists(path):
        return
    rows = []
    with open(path, encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            try:
                rows.append({
                    "t": float(row["t"]),
                    "busy": float(row.get("gpu_busy_percent") or 0),
                    "sclk": float(row.get("sclk_mhz") or 0),
                    "power": float(row.get("power_w") or 0),
                })
            except (KeyError, TypeError, ValueError):
                continue
    rows = [r for r in rows if r["sclk"] > 0]
    if not rows:
        print("gpu.csv present but empty (no readable amdgpu telemetry)")
        return
    sclk = [r["sclk"] for r in rows]
    busy = [r["busy"] for r in rows]
    section("GPU telemetry")
    print(f"samples {len(rows)}   sclk min {min(sclk):.0f} max {max(sclk):.0f} MHz   "
          f"busy mean {statistics.mean(busy):.0f}%")
    low = sum(1 for value in sclk if value < max(sclk) * 0.6)
    print(f"seconds below 60% of max clock: {low} "
          f"({100.0 * low / len(sclk):.0f}% of the run)")
    if low > len(sclk) * 0.25:
        print("  -> clocks are not staying up; check the power profile/limit before "
              "blaming the emulator")


def analyse_pipelines(path):
    if not os.path.exists(path):
        return
    events = {}
    long_builds = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            kind = parts[0]
            key = None
            ms = None
            for part in parts[1:]:
                if part.startswith("pipeline="):
                    key = part[len("pipeline="):]
                elif part.startswith("ms="):
                    try:
                        ms = float(part[3:])
                    except ValueError:
                        pass
            if kind == "BEGIN" and key:
                events[key] = True
            elif kind == "END" and key:
                events.pop(key, None)
                if ms is not None:
                    long_builds.append(ms)
    section("Pipeline builds")
    if long_builds:
        long_builds.sort()
        print(f"completed builds {len(long_builds)}   "
              f"median {long_builds[len(long_builds) // 2]:.1f}ms   "
              f"p99 {long_builds[int(len(long_builds) * 0.99)]:.1f}ms   "
              f"max {long_builds[-1]:.1f}ms")
        slow = [ms for ms in long_builds if ms > 50]
        print(f"builds over 50 ms (visible hitches): {len(slow)}")
    else:
        print("no completed builds recorded")
    if events:
        print(f"unfinished builds (driver stuck or still compiling): {len(events)}")


def analyse_log(path):
    """Parse the EDEN_NX2_GPU_PROFILE lines written every 10 s to the GPU thread log.

    Shape: "GPU thread per second: | Draw 12.3 ms (1234/s) | ... |
            host_cpu_busy_ms_per_s=812.4 window_wall_ms=10000.0"
    """
    if not os.path.exists(path):
        return
    import re
    zone_re = re.compile(r"\|\s*([A-Za-z][A-Za-z0-9_ ]*?)\s+([\d.]+)\s+ms\s+\(([\d.]+)/s\)")
    busy_re = re.compile(r"host_cpu_busy_ms_per_s=([\d.]+)")
    acquire_re = re.compile(r"acquires\s+([\d.]+)\s+\(([\d.]+) waited\)")

    peaks = {}
    busy = []
    acquired = 0.0
    waited = 0.0
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if "per second:" not in line:
                continue
            for name, ms, calls in zone_re.findall(line):
                name = name.strip()
                entry = peaks.setdefault(name, {"ms": 0.0, "calls": 0.0})
                entry["ms"] = max(entry["ms"], float(ms))
                entry["calls"] = max(entry["calls"], float(calls))
            match = busy_re.search(line)
            if match:
                busy.append(float(match.group(1)))
            match = acquire_re.search(line)
            if match:
                acquired = max(acquired, float(match.group(1)))
                waited = max(waited, float(match.group(2)))

    if not peaks and not busy:
        return

    section("GPU thread (peak per 10 s window)")
    for name, entry in sorted(peaks.items(), key=lambda kv: kv[1]["ms"], reverse=True)[:10]:
        print(f"  {name:26s} {entry['ms']:8.1f} ms/s   {entry['calls']:9.0f} calls/s")
    if busy:
        print(f"  host CPU busy: mean {statistics.mean(busy):.0f} ms per 1000 ms wall, "
              f"peak {max(busy):.0f}")
        if statistics.mean(busy) > 800:
            print("  -> the GPU thread is CPU-bound; frame rate will track scene complexity")
    if acquired:
        print(f"  acquires/sec {acquired:.0f}   of which waited {waited:.0f} "
              f"({100.0 * waited / max(acquired, 1.0):.0f}%)")
        if acquired and waited / acquired > 0.5:
            print("  -> most acquires block; look at split-front waits and fences")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", help="bundle directory or .zip")
    args = parser.parse_args()

    root = args.bundle
    if os.path.isfile(root) and zipfile.is_zipfile(root):
        import tempfile
        target = tempfile.mkdtemp(prefix="lichii-diag-")
        with zipfile.ZipFile(root) as archive:
            archive.extractall(target)
        entries = os.listdir(target)
        if len(entries) == 1 and os.path.isdir(os.path.join(target, entries[0])):
            root = os.path.join(target, entries[0])
        else:
            root = target

    if not os.path.isdir(root):
        print(f"not a bundle: {args.bundle}", file=sys.stderr)
        return 2

    summary = os.path.join(root, "SUMMARY.txt")
    if os.path.exists(summary):
        print(open(summary, encoding="utf-8", errors="replace").read().strip())
    else:
        print(f"bundle: {root}")

    records = load_jsonl(os.path.join(root, "frametimes.jsonl"))
    analyse_frames(records)
    analyse_gpu(os.path.join(root, "gpu.csv"))
    analyse_pipelines(os.path.join(root, "pipeline_build.log"))
    analyse_log(os.path.join(root, "eden_log.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
