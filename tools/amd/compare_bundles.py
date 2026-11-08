#!/usr/bin/env python3
"""Compare two lichii-diag bundles and report which way each metric moved.

    python3 compare_bundles.py run-baseline run-candidate

Both arguments are bundle directories or .zip files. Prints a table of the
control and the candidate with the relative change, then a short verdict for
the metrics that decide whether a change helped.
"""

import argparse
import csv
import json
import os
import re
import statistics
import sys
import tempfile
import zipfile


def open_bundle(path):
    if os.path.isfile(path) and zipfile.is_zipfile(path):
        target = tempfile.mkdtemp(prefix="lichii-cmp-")
        with zipfile.ZipFile(path) as archive:
            archive.extractall(target)
        entries = os.listdir(target)
        if len(entries) == 1 and os.path.isdir(os.path.join(target, entries[0])):
            return os.path.join(target, entries[0])
        return target
    return path


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def read_jsonl(path):
    records = []
    if not os.path.exists(path):
        return records
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return records


def read_gpu(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            try:
                sclk = float(row.get("sclk_mhz") or 0)
                busy = float(row.get("gpu_busy_percent") or 0)
            except (TypeError, ValueError):
                continue
            if sclk > 0:
                rows.append({"sclk": sclk, "busy": busy})
    return rows


def read_profile(path):
    """Peak GPU-thread zones, host CPU busy and acquire waits from eden_log.txt."""
    out = {"busy": None, "acquire_wait_pct": None, "top_zone": None, "top_zone_ms": 0.0}
    if not os.path.exists(path):
        return out
    zone_re = re.compile(r"\|\s*([A-Za-z][A-Za-z0-9_ ]*?)\s+([\d.]+)\s+ms\s+\(([\d.]+)/s\)")
    busy_re = re.compile(r"host_cpu_busy_ms_per_s=([\d.]+)")
    acq_re = re.compile(r"acquires\s+([\d.]+)\s+\(([\d.]+) waited\)")
    busy = []
    zones = {}
    wait_share = None
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if "per second:" not in line:
                continue
            for name, ms, _ in zone_re.findall(line):
                zones[name.strip()] = max(zones.get(name.strip(), 0.0), float(ms))
            match = busy_re.search(line)
            if match:
                busy.append(float(match.group(1)))
            match = acq_re.search(line)
            if match:
                acquired, waited = float(match.group(1)), float(match.group(2))
                if acquired > 0:
                    share = 100.0 * waited / acquired
                    wait_share = share if wait_share is None else max(wait_share, share)
    if busy:
        out["busy"] = statistics.mean(busy)
    if zones:
        name = max(zones, key=zones.get)
        out["top_zone"] = name
        out["top_zone_ms"] = zones[name]
    out["acquire_wait_pct"] = wait_share
    return out


def read_pipelines(path):
    out = {"count": 0, "over_50ms": 0, "max_ms": 0.0}
    if not os.path.exists(path):
        return out
    times = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not line.startswith("END "):
                continue
            for part in line.split():
                if part.startswith("ms="):
                    try:
                        times.append(float(part[3:]))
                    except ValueError:
                        pass
    if times:
        out["count"] = len(times)
        out["over_50ms"] = sum(1 for value in times if value > 50.0)
        out["max_ms"] = max(times)
    return out


def read_cache_delta(path):
    """How many cache files appeared or grew during the run, i.e. pipelines built mid-play."""
    if not os.path.exists(path):
        return 0
    total = 0
    pattern = re.compile(r"(?:new cache files during the run|existing cache files that grew):\s*(\d+)")
    with open(path, encoding="utf-8", errors="replace") as handle:
        for match in pattern.finditer(handle.read()):
            total += int(match.group(1))
    return total


def read_config(path):
    """The settings that decide whether a comparison is between like runs."""
    out = {}
    if not os.path.exists(path):
        return out
    patterns = {
        "gpu_accuracy": re.compile(r"Renderer\.gpu_accuracy:\s*(\w+)"),
        "native_async_draws": re.compile(r"Renderer\.native_async_draws:\s*(\w+)"),
        "vsync": re.compile(r"Renderer\.use_vsync:\s*(\w+)"),
        "resolution": re.compile(r"Renderer\.resolution_setup:\s*(\w+)"),
        "docked": re.compile(r"System\.use_docked_mode:\s*(\w+)"),
        "astc": re.compile(r"Renderer\.astc_recompression:\s*(\w+)"),
    }
    split = "never started"
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            for key, pattern in patterns.items():
                if key in out:
                    continue
                match = pattern.search(line)
                if match:
                    out[key] = match.group(1)
            if "GPU thread split: engine methods run on a second thread" in line:
                split = "on"
            elif "GPU thread split: stopped" in line and split != "on":
                split = "stopped"
    out["split"] = split
    return out


def collect(root):
    records = read_jsonl(os.path.join(root, "frametimes.jsonl"))
    windows = [r["frame_times"] for r in records
               if isinstance(r.get("frame_times"), dict) and r["frame_times"].get("frames")]
    intervals = []
    for window in windows:
        intervals.extend(window.get("intervals_ms") or [])

    metrics = {
        "windows": len(windows),
        "frames": sum(int(w.get("frames") or 0) for w in windows),
        "fps_median": 0.0,
        "fps_min": 0.0,
        "p50_ms": percentile(intervals, 0.50),
        "p99_ms": percentile(intervals, 0.99),
        "max_ms": max(intervals) if intervals else 0.0,
        "stutter_per_1k": 0.0,
        "vsync_2plus": 0.0,
    }
    fps = [float(w.get("fps") or 0.0) for w in windows if w.get("fps")]
    if fps:
        metrics["fps_median"] = statistics.median(fps)
        metrics["fps_min"] = min(fps)
    if metrics["frames"]:
        stutters = sum(int(w.get("stutters") or 0) for w in windows)
        metrics["stutter_per_1k"] = 1000.0 * stutters / metrics["frames"]
    buckets = [w.get("vsync_buckets") or {} for w in windows]
    if buckets:
        metrics["vsync_2plus"] = 100.0 * statistics.mean(
            float(b.get("2") or 0) + float(b.get("3") or 0) + float(b.get("4+") or 0)
            for b in buckets)

    gpu = read_gpu(os.path.join(root, "gpu.csv"))
    metrics["sclk_median"] = statistics.median(r["sclk"] for r in gpu) if gpu else 0.0
    metrics["gpu_busy"] = statistics.mean(r["busy"] for r in gpu) if gpu else 0.0

    profile = read_profile(os.path.join(root, "eden_log.txt"))
    metrics["cpu_busy_ms_s"] = profile["busy"] or 0.0
    metrics["acquire_wait_pct"] = profile["acquire_wait_pct"] or 0.0
    metrics["top_zone"] = profile["top_zone"] or "-"
    metrics["top_zone_ms"] = profile["top_zone_ms"]

    pipeline = read_pipelines(os.path.join(root, "pipeline_build.log"))
    metrics["builds"] = pipeline["count"]
    metrics["builds_over_50ms"] = pipeline["over_50ms"]
    metrics["build_max_ms"] = pipeline["max_ms"]
    metrics["compiled_during_play"] = read_cache_delta(
        os.path.join(root, "shader-cache-delta.txt"))
    return metrics


ROWS = [
    ("windows sampled", "windows", None),
    ("frames counted", "frames", None),
    ("fps, median", "fps_median", "up"),
    ("fps, worst window", "fps_min", "up"),
    ("frame time p50 (ms)", "p50_ms", "down"),
    ("frame time p99 (ms)", "p99_ms", "down"),
    ("frame time max (ms)", "max_ms", "down"),
    ("stutters per 1000 frames", "stutter_per_1k", "down"),
    ("frames not at 1x refresh (%)", "vsync_2plus", "down"),
    ("GPU shader clock, median (MHz)", "sclk_median", None),
    ("GPU busy (%)", "gpu_busy", None),
    ("GPU thread host CPU (ms/s)", "cpu_busy_ms_s", "down"),
    ("acquires that blocked (%)", "acquire_wait_pct", "down"),
    ("pipeline builds completed", "builds", None),
    ("cache files built during play", "compiled_during_play", "down"),
    ("builds over 50 ms", "builds_over_50ms", "down"),
    ("slowest build (ms)", "build_max_ms", "down"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    args = parser.parse_args()

    left_dir = open_bundle(args.baseline)
    right_dir = open_bundle(args.candidate)
    left = collect(left_dir)
    right = collect(right_dir)

    # Compare the settings first: an A/B whose two runs differ in gpu_accuracy
    # is measuring the setting, not the build.
    left_cfg = read_config(os.path.join(left_dir, "eden_log.txt"))
    right_cfg = read_config(os.path.join(right_dir, "eden_log.txt"))

    print(f"control  : {args.baseline}")
    print(f"candidate: {args.candidate}")
    print()

    if left_cfg or right_cfg:
        keys = ["gpu_accuracy", "split", "native_async_draws", "vsync", "resolution", "docked"]
        print(f"{'setting':24s} {'control':>18s} {'candidate':>18s}")
        print("-" * 62)
        differences = []
        for key in keys:
            a = left_cfg.get(key, "-")
            b = right_cfg.get(key, "-")
            marker = ""
            if a != b and a != "-" and b != "-":
                marker = "   <- differs"
                differences.append(f"{key}: {a} -> {b}")
            print(f"{key:24s} {a:>18s} {b:>18s}{marker}")
        print()
        if differences:
            print("warning: the two runs are not configured the same, so a difference in the "
                  "numbers below may be the setting rather than the build:")
            for line in differences:
                print(f"  {line}")
            print()
        else:
            print("both runs used the same settings; the numbers below are comparable")
            print()

    print(f"{'metric':34s} {'control':>10s} {'candidate':>10s} {'change':>9s}  better")
    print("-" * 78)
    verdict = []
    flat = 0
    for label, key, better in ROWS:
        a, b = left.get(key), right.get(key)
        if isinstance(a, str) or isinstance(b, str):
            print(f"{label:34s} {str(a):>10s} {str(b):>10s} {'':>9s}")
            continue
        change = ""
        if a:
            change = f"{100.0 * (b - a) / a:+.1f}%"
        elif b:
            change = "new"
        print(f"{label:34s} {a:10.2f} {b:10.2f} {change:>9s}  {better or ''}")
        if better is not None:
            if a == 0 and b == 0:
                flat += 1
                continue
            if a == 0:
                # Nothing to make a percentage of; moving off zero is the signal.
                verdict.append((label, better == "up", a, b))
                continue
            relative = (b - a) / a
            if abs(relative) < 0.005:
                flat += 1
                continue
            verdict.append((label, (relative > 0) if better == "up" else (relative < 0), a, b))

    print()
    if verdict or flat:
        wins = sum(1 for _, improved, _, _ in verdict if improved)
        losses = len(verdict) - wins
        print(f"verdict: {wins} improved, {losses} worse, {flat} unchanged "
              f"(of {wins + losses + flat} directional metrics)")
        for label, improved, a, b in verdict:
            if not improved:
                print(f"  worse: {label}  {a:.2f} -> {b:.2f}")
    if left.get("top_zone") != right.get("top_zone"):
        print(f"  hottest GPU-thread zone moved: {left.get('top_zone')} -> {right.get('top_zone')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
