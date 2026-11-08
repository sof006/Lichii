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
    keys = set()
    stage_hashes = set()
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
                elif part[:3] in ("s1:", "s3:", "s5:"):
                    stage_hashes.add(part)
            if kind == "BEGIN" and key:
                events[key] = True
                keys.add(key)
            elif kind == "END" and key:
                events.pop(key, None)
                if ms is not None:
                    long_builds.append(ms)
    section("Pipeline builds")
    if keys:
        print(f"distinct pipeline keys (as built): {len(keys)}")
        print(f"distinct stage shaders: {len(stage_hashes)}")
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
    zone_re = re.compile(r"\|\s*([A-Za-z][A-Za-z0-9_ ]*?)\s+([\d.]+)\s+ms\s*\(([\d.]+)/s\)")
    busy_re = re.compile(r"host_cpu_busy_ms_per_s=([\d.]+)")
    acquire_re = re.compile(r"acquires\s+([\d.]+)\s+\(([\d.]+) waited\)")

    # With the split on, two threads report: "GPU thread" (front) and
    # "GPU back thread". Their zones share names, so key by thread or the two
    # are averaged into one meaningless table.
    zones = {}
    busy = {}
    acquires = {}
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            head, marker, rest = line.partition("per second:")
            if not marker:
                continue
            thread = head.rsplit(":", 1)[-1].strip() if ":" in head else "GPU thread"
            thread = thread or "GPU thread"
            for name, ms, _ in zone_re.findall(rest):
                zones.setdefault(thread, {}).setdefault(name.strip(), []).append(float(ms))
            match = busy_re.search(rest)
            if match:
                busy.setdefault(thread, []).append(float(match.group(1)))
            match = acquire_re.search(rest)
            if match:
                entry = acquires.setdefault(thread, [0.0, 0.0])
                entry[0] = max(entry[0], float(match.group(1)))
                entry[1] = max(entry[1], float(match.group(2)))

    if not zones and not busy:
        return

    for thread in sorted(zones, key=lambda name: -len(zones[name])):
        section(f"{thread} (mean per 10 s window)")
        ranked = sorted(zones[thread].items(), key=lambda kv: statistics.mean(kv[1]),
                        reverse=True)
        for name, values in ranked[:8]:
            print(f"  {name:26s} {statistics.mean(values):8.1f} ms/s   peak {max(values):7.1f}")
        if thread in busy:
            values = busy[thread]
            average = statistics.mean(values)
            print(f"  {'host CPU busy':26s} {average:8.1f} ms per 1000 ms")
            if average > 950:
                print("  -> saturated: this thread is the CPU limit")
            elif average > 800:
                print("  -> close to the CPU limit; scene complexity will show through")
        if thread in acquires and acquires[thread][0]:
            total, waited = acquires[thread]
            print(f"  {'acquires':26s} {total:8.0f}/s, {waited:.0f} waited "
                  f"({100.0*waited/max(total,1):.0f}%)")
            if waited / max(total, 1.0) > 0.5:
                print("  -> most acquires block here")


def analyse_cache_delta(path):
    if not os.path.exists(path):
        return
    section("Compilation during the run")
    print(open(path, encoding="utf-8", errors="replace").read().strip())


def analyse_run_info(path):
    if not os.path.exists(path):
        return
    text = open(path, encoding="utf-8", errors="replace").read().strip()
    if not text:
        return
    section("Run identity")
    for line in text.splitlines():
        print(f"  {line.strip()}")


def analyse_config(path):
    """Settings that decide whether the fast paths are even active."""
    if not os.path.exists(path):
        return
    import re
    wanted = {}
    accuracy = re.compile(r"Renderer\.gpu_accuracy:\s*(\w+)")
    async_draws = re.compile(r"Renderer\.native_async_draws:\s*(\w+)")
    vsync = re.compile(r"Renderer\.use_vsync:\s*(\w+)")
    resolution = re.compile(r"Renderer\.resolution_setup:\s*(\w+)")
    docked = re.compile(r"System\.use_docked_mode:\s*(\w+)")
    split_started = False
    split_stopped = False
    binaries_state = None
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            for key, pattern in (("gpu_accuracy", accuracy),
                                 ("native_async_draws", async_draws),
                                 ("vsync", vsync),
                                 ("resolution", resolution),
                                 ("docked", docked)):
                match = pattern.search(line)
                if match and key not in wanted:
                    wanted[key] = match.group(1)
            if "GPU thread split: engine methods run on a second thread" in line:
                split_started = True
            if "GPU thread split: stopped" in line:
                split_stopped = True
            if binaries_state is None:
                if "Driver pipeline binaries on:" in line:
                    binaries_state = "on"
                elif "Driver pipeline binaries off" in line:
                    detail = line.split("Driver pipeline binaries off", 1)[1].strip(": \t\r\n")
                    binaries_state = f"off ({detail})"

    section("Configuration that gates the fast paths")
    for key in ("gpu_accuracy", "native_async_draws", "vsync", "resolution", "docked"):
        if key in wanted:
            print(f"  {key:22s} {wanted[key]}")
    if split_started:
        print("  GPU thread split      ON")
    elif split_stopped:
        print("  GPU thread split      STOPPED by the accuracy settings")
    else:
        print("  GPU thread split      never started")
    if binaries_state:
        print(f"  driver binaries       {binaries_state}")
        if binaries_state.startswith("off"):
            print("  -> compiled pipelines are not being saved, so every launch"
                  " recompiles them; this is not a cache that is merely cold")
    elif True:
        print("  driver binaries       unknown (build predates the log line)")
    if wanted.get("gpu_accuracy", "").lower() == "high":
        print("  -> GPU accuracy High disables the split GPU thread"
              " (worth up to +54% frame rate); switching to Low turns it back on")
    if wanted.get("native_async_draws", "").lower() == "wait":
        print("  -> native_async_draws=Wait blocks on an unbuilt pipeline;"
              " Bounded or Skip avoids that hitch")


def analyse_frame_trace(path):
    """EDEN_DKB_FRAME_TRACE=1: where each frame's wall time actually goes.

    acquire_frame -> waiting for a swapchain image (the GPU is behind),
    record_submit -> CPU recording the frame, present_call -> blocked in
    vkQueuePresentKHR. Whichever dominates names the limit.
    """
    if not os.path.exists(path):
        return
    import re
    field = re.compile(r"(\w+)=(-?[\d.]+)")
    keys = ("cpu_frame_wall_ms", "cpu_composite_wall_ms", "acquire_frame_wall_ms",
            "record_submit_wall_ms", "present_call_wall_ms", "frame_end_wall_ms")
    samples = {key: [] for key in keys}
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if "DKB renderer" not in line:
                continue
            for name, value in field.findall(line):
                if name in samples:
                    try:
                        samples[name].append(float(value))
                    except ValueError:
                        pass
    samples = {key: values for key, values in samples.items() if values}
    if not samples:
        return

    section("Where the frame time goes (EDEN_DKB_FRAME_TRACE)")
    for key in keys:
        if key in samples:
            values = samples[key]
            print(f"  {key:26s} mean {statistics.mean(values):7.2f} ms   "
                  f"p95 {percentile(values, 0.95):7.2f}")

    parts = {key: statistics.mean(samples[key]) for key in
             ("acquire_frame_wall_ms", "record_submit_wall_ms", "present_call_wall_ms")
             if key in samples}
    if parts:
        total = sum(parts.values())
        if total > 0:
            worst = max(parts, key=parts.get)
            share = 100.0 * parts[worst] / total
            reading = {
                "record_submit_wall_ms": "CPU recording the frame — the renderer's CPU side is the limit",
                "present_call_wall_ms": "blocked in vkQueuePresentKHR — the GPU or the display is the limit",
                "acquire_frame_wall_ms": "waiting for a free swapchain image — the GPU is behind",
            }[worst]
            print(f"\n  dominant: {worst} at {share:.0f}% of acquire+record+present")
            print(f"  -> {reading}")


def analyse_cpu(path):
    """Per-thread CPU time of the emulator process, as a percentage of one core."""
    if not os.path.exists(path):
        return
    try:
        import os as _os
        hz = _os.sysconf("SC_CLK_TCK")
    except (ImportError, ValueError, OSError, AttributeError):
        hz = 100

    samples = {}
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parts = line.rstrip("\n").split(",")
            if len(parts) < 4 or not parts[0].replace(".", "", 1).isdigit():
                continue
            try:
                t = float(parts[0])
                tid = parts[1]
                comm = parts[2].strip('"')
                ticks = float(parts[3])
            except ValueError:
                continue
            samples.setdefault((tid, comm), []).append((t, ticks))

    usage = {}
    for (tid, comm), points in samples.items():
        points.sort()
        if len(points) < 2:
            continue
        first, last = points[0], points[-1]
        seconds = last[0] - first[0]
        if seconds <= 0.5:
            continue
        share = (last[1] - first[1]) / hz / seconds
        usage[(tid, comm)] = usage.get((tid, comm), 0.0) + share

    if not usage:
        return

    section("CPU use per thread (% of one core)")
    ranked = sorted(usage.items(), key=lambda kv: kv[1], reverse=True)[:10]
    for (tid, comm), share in ranked:
        print(f"  {comm:26s} tid {tid:>6s}  {100.0 * share:6.1f}%")
    total = sum(usage.values())
    print(f"  {'total':26s}          {100.0 * total:6.1f}% across {len(usage)} threads")
    hot = [(comm, share) for (_, comm), share in usage.items() if share >= 0.9]
    if hot:
        print(f"  -> {len(hot)} thread(s) at 90%+ of a core: "
              f"{', '.join(sorted({c for c, _ in hot}))}")
        print("     a saturated thread is a good candidate for the limit")


def analyse_captures(path):
    """Frame dumps requested by pressing c during the run."""
    if not os.path.exists(path):
        return
    captures = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                captures.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    if not captures:
        return
    section("Frame captures")
    for entry in captures:
        if entry.get("ok"):
            result = entry.get("result") or {}
            print(f"  ok   frame {result.get('frame')} -> {result.get('directory')}")
            counts = result.get("entries")
            if isinstance(counts, dict):
                print(f"       entries: {counts}")
            if result.get("command"):
                print(f"       index it: {result['command']}")
        else:
            print(f"  failed: {entry.get('error')}")


def analyse_io(path):
    """Bytes the emulator read from and wrote to disk, to spot content loading."""
    if not os.path.exists(path):
        return
    samples = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parts = line.rstrip("\n").split(",")
            if len(parts) < 3 or not parts[0].replace(".", "", 1).isdigit():
                continue
            try:
                samples.append((float(parts[0]), float(parts[1]), float(parts[2])))
            except ValueError:
                continue
    if len(samples) < 3:
        return
    reads, writes = [], []
    for (t0, r0, w0), (t1, r1, w1) in zip(samples, samples[1:]):
        delta = t1 - t0
        if delta <= 0:
            continue
        reads.append(max(0.0, r1 - r0) / delta)
        writes.append(max(0.0, w1 - w0) / delta)
    if not reads:
        return
    section("Disk I/O")
    print(f"  read  mean {statistics.mean(reads)/1e6:7.2f} MB/s   "
          f"peak {max(reads)/1e6:7.2f} MB/s")
    print(f"  write mean {statistics.mean(writes)/1e6:6.2f} MB/s   "
          f"peak {max(writes)/1e6:7.2f} MB/s")
    busy = [index for index, value in enumerate(reads) if value > 4 * 1e6]
    if busy:
        print(f"  {len(busy)} seconds read more than 4 MB "
              f"(a hitch during one of these is loading, not rendering)")


def analyse_present_pacing(path):
    """EDEN_DKB_FRAME_TRACE also logs the present word the guest sends.

    Word 12 is (swap interval << 16) | refresh rate, so interval 1 is 60 fps and
    interval 2 is 30 fps. If the game asks for 30, a steady 60 falling to a
    steady 30 is the guest switching mode, not the emulator throttling.
    """
    if not os.path.exists(path):
        return
    import re
    field = re.compile(r"(\w+)=([^\s]+)")
    samples = []
    switches = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if "DKB frame present=" not in line:
                continue
            recorded = dict(field.findall(line))
            samples.append(recorded)
            switched = recorded.get("last_switch")
            if switched and not switched.startswith("0x0->"):
                switches.append(switched)
    if not samples:
        return

    section("Present pacing (guest request)")
    last = samples[-1]
    print(f"  samples {len(samples)}   requested_hz {last.get('requested_hz')}"
          f"   effective_hz {last.get('effective_hz')}")
    intervals = {}
    for sample in samples:
        key = sample.get("requested_interval", "?")
        intervals[key] = intervals.get(key, 0) + 1
    print("  requested swap interval over the run: "
          + ", ".join(f"{k}: {v} samples" for k, v in sorted(intervals.items())))
    if switches:
        print(f"  mode switches seen: {len(switches)}   last {switches[-1]}")
    if "2" in intervals:
        print("  -> the guest asked for interval 2 (30 fps): a steady drop to 30 is the game"
              " switching mode, not the emulator")
    elif intervals:
        print("  -> the guest asked for interval 1 throughout: a drop to 30 is then"
              " half-rate vsync on frames that miss the 60 Hz deadline")


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
    analyse_run_info(os.path.join(root, "run-info.txt"))
    analyse_config(os.path.join(root, "eden_log.txt"))
    analyse_frames(records)
    analyse_gpu(os.path.join(root, "gpu.csv"))
    analyse_cpu(os.path.join(root, "cpu.csv"))
    analyse_io(os.path.join(root, "io.csv"))
    analyse_pipelines(os.path.join(root, "pipeline_build.log"))
    analyse_cache_delta(os.path.join(root, "shader-cache-delta.txt"))
    analyse_captures(os.path.join(root, "captures.jsonl"))
    analyse_frame_trace(os.path.join(root, "eden_log.txt"))
    analyse_present_pacing(os.path.join(root, "eden_log.txt"))
    analyse_log(os.path.join(root, "eden_log.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
