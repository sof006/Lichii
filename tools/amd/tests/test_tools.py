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
        # The zone line and CPU-bound verdict.
        self.assertIn("Draw", output)
        self.assertIn("CPU-bound", output)
        self.assertIn("most acquires block", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
