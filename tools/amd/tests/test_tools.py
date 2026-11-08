#!/usr/bin/env python3
"""Tests for the Lichii stutter diagnostics tools.

Runs a mock Lichii debug server (the JSON-lines protocol from
docs/nx2/debug-mcp.md) and checks that frametime_probe.py records usable
samples, survives a dropped connection, and waits for a late server. Also
feeds synthetic data through analyze_bundle.py.

    python3 -m unittest discover -s tools/amd/tests -v
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
PROBE = os.path.join(TOOLS, "frametime_probe.py")
ANALYZE = os.path.join(TOOLS, "analyze_bundle.py")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def frame_times_payload(frames=120, mean_ms=16.6):
    return {
        "frames": frames,
        "mean_ms": mean_ms,
        "fps": 1000.0 / mean_ms if mean_ms else 0.0,
        "p50_ms": mean_ms,
        "p90_ms": mean_ms * 1.2,
        "p99_ms": mean_ms * 2.0,
        "max_ms": mean_ms * 3.0,
        "min_ms": mean_ms * 0.9,
        "vsync_buckets": {"1": 0.9, "2": 0.05, "3": 0.03, "4+": 0.02},
        "stutters": 3,
        "intervals_ms": [round(mean_ms, 1)] * 10,
        "truncated": False,
    }


class MockServer(threading.Thread):
    """Answers the probe the way nx2_debug_server.cpp does."""

    def __init__(self, port, drop_after=None, delay=0.0):
        super().__init__(daemon=True)
        self.port = port
        self.drop_after = drop_after
        self.delay = delay
        self.requests = []
        self._stop = threading.Event()
        self._sock = socket.socket()
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", port))
        self._sock.listen(4)
        self._sock.settimeout(0.2)

    def run(self):
        if self.delay:
            time.sleep(self.delay)
        while not self._stop.is_set():
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self._serve(conn)

    def _serve(self, conn):
        conn.settimeout(5.0)
        stream = conn.makefile("rwb")
        served = 0
        try:
            while not self._stop.is_set():
                line = stream.readline()
                if not line:
                    return
                try:
                    request = json.loads(line)
                except json.JSONDecodeError:
                    return
                self.requests.append(request)
                served += 1
                if self.drop_after is not None and served > self.drop_after:
                    return  # simulate the emulator exiting mid-recording
                method = request.get("method")
                if method == "frame_times":
                    result = frame_times_payload()
                elif method == "status":
                    result = {"title": "The Legend of Zelda: Ocarina of Time",
                              "program_id": "0400486024DEC000",
                              "fps": 60.2, "paused": False}
                elif method == "capture_frame":
                    result = {"directory": "/tmp/mock-capture-1", "frame": 7,
                              "entries": {"colour": 12}, "command": "frame_dump_index.py"}
                else:
                    stream.write(json.dumps({"id": request.get("id"),
                                             "error": "unknown method"}).encode() + b"\n")
                    stream.flush()
                    continue
                stream.write(json.dumps({"id": request.get("id"),
                                         "result": result}).encode() + b"\n")
                stream.flush()
        finally:
            try:
                stream.close()
                conn.close()
            except OSError:
                pass

    def stop(self):
        self._stop.set()
        try:
            self._sock.close()
        except OSError:
            pass


def run_probe(port, out_path, duration, interval=0.4):
    return subprocess.run(
        [sys.executable, PROBE, "--server", f"127.0.0.1:{port}",
         "--out", out_path, "--interval", str(interval),
         "--window", "1", "--duration", str(duration),
         "--sample-timeout", "5"],
        capture_output=True, text=True, timeout=120,
    )


def read_records(path):
    records = []
    if not os.path.exists(path):
        return records
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    return records


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="lichii-probe-test-")
        self.out = os.path.join(self.dir, "frametimes.jsonl")

    def test_records_samples(self):
        port = free_port()
        server = MockServer(port)
        server.start()
        try:
            completed = run_probe(port, self.out, duration=4)
        finally:
            server.stop()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        records = read_records(self.out)
        self.assertGreaterEqual(len(records), 2, "expected repeated samples")
        for record in records:
            self.assertIn("frame_times", record)
            self.assertEqual(record["frame_times"]["frames"], 120)
            self.assertAlmostEqual(record["frame_times"]["fps"], 60.24, places=1)
            self.assertIn("status", record)
            self.assertEqual(record["status"]["program_id"], "0400486024DEC000")

    def test_survives_disconnect(self):
        port = free_port()
        # Answer two requests, then drop the connection repeatedly.
        server = MockServer(port, drop_after=2)
        server.start()
        try:
            completed = run_probe(port, self.out, duration=5)
        finally:
            server.stop()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        records = read_records(self.out)
        good = [r for r in records if "frame_times" in r]
        self.assertGreaterEqual(len(good), 2, "probe should reconnect and keep sampling")

    def test_waits_for_late_server(self):
        port = free_port()
        server = MockServer(port, delay=1.5)
        server.start()
        try:
            completed = run_probe(port, self.out, duration=6)
        finally:
            server.stop()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertGreaterEqual(len(read_records(self.out)), 1,
                                "probe should wait for the server to appear")

    def test_bookmark_signal(self):
        port = free_port()
        server = MockServer(port)
        server.start()
        process = subprocess.Popen(
            [sys.executable, PROBE, "--server", f"127.0.0.1:{port}",
             "--out", self.out, "--interval", "0.3", "--window", "1",
             "--duration", "5", "--sample-timeout", "5"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(1.5)
            if os.name == "posix":
                import signal
                process.send_signal(signal.SIGUSR1)
                time.sleep(1.5)
        finally:
            process.wait(timeout=60)
            server.stop()
        if os.name != "posix":
            self.skipTest("SIGUSR1 is POSIX only")
        marks = [r for r in read_records(self.out) if r.get("mark")]
        self.assertTrue(marks, "a SIGUSR1 should be recorded as a mark")

    def test_capture_signal(self):
        """SIGUSR2 must ask the emulator for a frame dump and record where it landed."""
        if os.name != "posix":
            self.skipTest("SIGUSR2 is POSIX only")
        import signal
        port = free_port()
        server = MockServer(port)
        server.start()
        process = subprocess.Popen(
            [sys.executable, PROBE, "--server", f"127.0.0.1:{port}",
             "--out", self.out, "--interval", "0.3", "--window", "1",
             "--duration", "6", "--sample-timeout", "5"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(1.5)
            process.send_signal(signal.SIGUSR2)
            time.sleep(2.0)
        finally:
            process.wait(timeout=60)
            server.stop()

        captures = read_records(os.path.join(os.path.dirname(self.out), "captures.jsonl"))
        self.assertTrue(captures, "a SIGUSR2 should produce a captures.jsonl entry")
        self.assertTrue(captures[0].get("ok"), captures[0])
        self.assertIn("directory", captures[0]["result"])


class AnalyzerTests(unittest.TestCase):
    def test_parses_synthetic_bundle(self):
        bundle = tempfile.mkdtemp(prefix="lichii-bundle-test-")
        records = []
        for index in range(6):
            payload = frame_times_payload(mean_ms=16.6 + index)
            payload["intervals_ms"] = [16.6, 16.6, 16.6, 120.0]
            records.append({"t": 1700000000 + index, "frame_times": payload,
                            "status": {"title": "Ocarina of Time"}})
        records.append({"t": 1700000010, "mark": True})
        with open(os.path.join(bundle, "frametimes.jsonl"), "w", encoding="utf-8") as handle:
            for record in records:
                handle.write(json.dumps(record) + "\n")

        with open(os.path.join(bundle, "gpu.csv"), "w", encoding="utf-8") as handle:
            handle.write("t,gpu_busy_percent,sclk_mhz,power_w,mclk_mhz\n")
            for index in range(6):
                handle.write(f"{1700000000 + index},95,3000,250,1200\n")
                handle.write(f"{1700000001 + index},40,600,80,800\n")

        with open(os.path.join(bundle, "pipeline_build.log"), "w", encoding="utf-8") as handle:
            handle.write("BEGIN graphics pipeline=00000000deadbeef thread=1\n")
            handle.write("END graphics pipeline=00000000deadbeef thread=1 ms=250.5\n")
            handle.write("BEGIN graphics pipeline=00000000cafebabe thread=1\n")

        with open(os.path.join(bundle, "eden_log.txt"), "w", encoding="utf-8") as handle:
            handle.write(
                "HW.GPU <Info> GPU thread per second: | Draw 120.0 ms (4000/s) "
                "| FlushWork 30.0 ms (50/s) | host_cpu_busy_ms_per_s=812.4 "
                "window_wall_ms=10000.0 | acquires 500 (400 waited)\n")

        completed = subprocess.run([sys.executable, ANALYZE, bundle],
                                   capture_output=True, text=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        output = completed.stdout

        self.assertIn("Frame timing", output)
        self.assertIn("stutters", output)
        self.assertIn("Worst windows", output)
        self.assertIn("GPU telemetry", output)
        self.assertIn("Pipeline builds", output)
        # The 250 ms build and the dangling BEGIN must both be called out.
        self.assertIn("builds over 50 ms", output)
        self.assertIn("unfinished builds", output)
        # The zone line and the CPU-limit verdict.
        self.assertIn("Draw", output)
        self.assertIn("CPU limit", output)
        self.assertIn("most acquires block", output)


class CompareTests(unittest.TestCase):
    def _make_bundle(self, root, mean_ms, spike_ms, stutters, cpu_busy, waited, build_ms):
        os.makedirs(root, exist_ok=True)
        records = []
        for index in range(5):
            payload = frame_times_payload(frames=600, mean_ms=mean_ms)
            payload["p99_ms"] = spike_ms
            payload["max_ms"] = spike_ms
            payload["intervals_ms"] = [mean_ms] * 18 + [spike_ms] * 2
            payload["stutters"] = stutters
            records.append({"t": 1700000000 + index, "frame_times": payload})
        with open(os.path.join(root, "frametimes.jsonl"), "w", encoding="utf-8") as handle:
            for record in records:
                handle.write(json.dumps(record) + "\n")
        with open(os.path.join(root, "gpu.csv"), "w", encoding="utf-8") as handle:
            handle.write("t,gpu_busy_percent,sclk_mhz,power_w,mclk_mhz\n")
            for index in range(5):
                handle.write(f"{1700000000 + index},90,3000,250,1200\n")
        with open(os.path.join(root, "pipeline_build.log"), "w", encoding="utf-8") as handle:
            handle.write(f"END graphics pipeline=0000000000000001 thread=1 ms={build_ms}\n")
        with open(os.path.join(root, "eden_log.txt"), "w", encoding="utf-8") as handle:
            handle.write(
                f"HW.GPU <Info> GPU thread per second: | Draw 100.0 ms (4000/s) "
                f"| host_cpu_busy_ms_per_s={cpu_busy} window_wall_ms=10000.0 "
                f"| acquires 500 ({waited} waited)\n")

    def test_reports_improvement_direction(self):
        base = tempfile.mkdtemp(prefix="lichii-base-")
        cand = tempfile.mkdtemp(prefix="lichii-cand-")
        self._make_bundle(base, mean_ms=21.0, spike_ms=150.0, stutters=40,
                          cpu_busy=900.0, waited=400, build_ms=180.0)
        self._make_bundle(cand, mean_ms=16.7, spike_ms=60.0, stutters=5,
                          cpu_busy=300.0, waited=40, build_ms=20.0)

        completed = subprocess.run(
            [sys.executable, os.path.join(TOOLS, "compare_bundles.py"), base, cand],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        output = completed.stdout
        self.assertIn("verdict:", output)
        self.assertIn("unchanged", output)
        self.assertNotIn("worse:", output, "no directional metric should regress here")
        self.assertIn("0 worse", output)


    def test_zero_to_positive_counts_as_regression(self):
        """Compiling pipelines mid-play when the control compiled none must not read as neutral."""
        base = tempfile.mkdtemp(prefix="lichii-base0-")
        cand = tempfile.mkdtemp(prefix="lichii-cand0-")
        self._make_bundle(base, mean_ms=16.7, spike_ms=60.0, stutters=5,
                          cpu_busy=300.0, waited=40, build_ms=20.0)
        self._make_bundle(cand, mean_ms=16.7, spike_ms=60.0, stutters=5,
                          cpu_busy=300.0, waited=40, build_ms=20.0)
        with open(os.path.join(cand, "shader-cache-delta.txt"), "w", encoding="utf-8") as handle:
            handle.write("new cache files during the run: 2 (4096 bytes)\n")
            handle.write("existing cache files that grew: 0 (0 bytes)\n")
            handle.write("pipelines were built while playing: compilation stutter is present\n")

        completed = subprocess.run(
            [sys.executable, os.path.join(TOOLS, "compare_bundles.py"), base, cand],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("worse: cache files built during play", completed.stdout)


    def test_reports_hot_cpu_thread(self):
        """A thread pinned at a full core must be called out as the likely limit."""
        bundle = tempfile.mkdtemp(prefix="lichii-cpu-test-")
        with open(os.path.join(bundle, "frametimes.jsonl"), "w", encoding="utf-8") as handle:
            handle.write(json.dumps({"t": 1.0, "frame_times": frame_times_payload()}) + "\n")
        rows = ["t,tid,comm,ticks\n",
                '1000.0,101,"GPU thread",0\n',
                '1002.0,101,"GPU thread",200\n',
                '1002.0,102,"Cpu Core 0",50\n']
        with open(os.path.join(bundle, "cpu.csv"), "w", encoding="utf-8") as handle:
            handle.writelines(rows)

        completed = subprocess.run([sys.executable, ANALYZE, bundle],
                                   capture_output=True, text=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        output = completed.stdout
        self.assertIn("CPU use per thread", output)
        self.assertIn("GPU thread", output)
        self.assertIn("100.0%", output)
        self.assertIn("thread(s) at 90%+ of a core", output)

    def test_reports_disk_read_burst(self):
        """A hitch during a content load must be visible as a read burst."""
        bundle = tempfile.mkdtemp(prefix="lichii-io-test-")
        with open(os.path.join(bundle, "frametimes.jsonl"), "w", encoding="utf-8") as handle:
            handle.write(json.dumps({"t": 1.0, "frame_times": frame_times_payload()}) + "\n")
        rows = ["t,read_bytes,write_bytes\n"]
        base = 1_000_000.0
        for index in range(6):
            # Quiet, then a 10 MB/s burst for one second, then quiet again.
            read = base + (index * 10_000)
            if index >= 3:
                read += 10_000_000
            rows.append(f"{1000.0 + index:.1f},{read:.0f},0\n")
        with open(os.path.join(bundle, "io.csv"), "w", encoding="utf-8") as handle:
            handle.writelines(rows)

        completed = subprocess.run([sys.executable, ANALYZE, bundle],
                                   capture_output=True, text=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        output = completed.stdout
        self.assertIn("Disk I/O", output)
        self.assertIn("peak", output)
        self.assertIn("read more than 4 MB", output)

    def test_reports_pipeline_binary_state(self):
        """The analyser must say whether the driver binary cache engaged, and flag 'off'."""
        bundle = tempfile.mkdtemp(prefix="lichii-pb-test-")
        with open(os.path.join(bundle, "frametimes.jsonl"), "w", encoding="utf-8") as handle:
            handle.write(json.dumps({"t": 1.0, "frame_times": frame_times_payload()}) + "\n")
        with open(os.path.join(bundle, "eden_log.txt"), "w", encoding="utf-8") as handle:
            handle.write("[   1.0] Render.Vulkan <Info> video_core/x.cpp:9:f: "
                         "Driver pipeline binaries off: setting=true extension=false nvidia=false\n")

        completed = subprocess.run([sys.executable, ANALYZE, bundle],
                                   capture_output=True, text=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        output = completed.stdout
        self.assertIn("driver binaries", output)
        self.assertIn("off (setting=true extension=false nvidia=false)", output)
        self.assertIn("not a cache that is merely cold", output)

    def test_reads_frame_trace(self):
        """EDEN_DKB_FRAME_TRACE lines must be attributed to the right component."""
        bundle = tempfile.mkdtemp(prefix="lichii-trace-test-")
        with open(os.path.join(bundle, "frametimes.jsonl"), "w", encoding="utf-8") as handle:
            handle.write(json.dumps({"t": 1.0, "frame_times": frame_times_payload()}) + "\n")
        lines = []
        for frame in (100, 160, 220):
            lines.append(
                "[  10.000] Render.Vulkan <Info> renderer_vulkan.cpp:283:DKB renderer "
                f"renderer_frame={frame} host_pacing_mode=0 vsync_mode=2 "
                "cpu_frame_wall_ms=19.520 cpu_composite_wall_ms=18.900 capture_wall_ms=0.010 "
                "acquire_frame_wall_ms=0.400 record_submit_wall_ms=16.200 "
                "present_call_wall_ms=1.900 frame_end_wall_ms=0.400\n")
        with open(os.path.join(bundle, "eden_log.txt"), "w", encoding="utf-8") as handle:
            handle.writelines(lines)

        completed = subprocess.run([sys.executable, ANALYZE, bundle],
                                   capture_output=True, text=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        output = completed.stdout
        self.assertIn("Where the frame time goes", output)
        self.assertIn("record_submit_wall_ms", output)
        self.assertIn("dominant: record_submit_wall_ms", output)
        self.assertIn("CPU recording the frame", output)

    def test_flags_differing_configuration(self):
        """An A/B that changed gpu_accuracy is measuring the setting, not the build."""
        base = tempfile.mkdtemp(prefix="lichii-cfg-a-")
        cand = tempfile.mkdtemp(prefix="lichii-cfg-b-")
        for root, accuracy, split in ((base, "High", False), (cand, "Low", True)):
            with open(os.path.join(root, "frametimes.jsonl"), "w", encoding="utf-8") as handle:
                handle.write(json.dumps({"t": 1.0, "frame_times": frame_times_payload()}) + "\n")
            lines = ["[   1.0] Config <Info> common/settings.cpp:139:LogSettings: "
                     f"-- Renderer.gpu_accuracy: {accuracy}\n"]
            if split:
                lines.append("[   2.0] Render.Vulkan <Info> gpu_split.cpp:124:MaybeStart: "
                             "GPU thread split: engine methods run on a second thread\n")
            with open(os.path.join(root, "eden_log.txt"), "w", encoding="utf-8") as handle:
                handle.writelines(lines)

        completed = subprocess.run(
            [sys.executable, os.path.join(TOOLS, "compare_bundles.py"), base, cand],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        output = completed.stdout
        self.assertIn("gpu_accuracy", output)
        self.assertIn("<- differs", output)
        self.assertIn("not configured the same", output)
        self.assertIn("High -> Low", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
