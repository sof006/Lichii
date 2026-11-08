#!/usr/bin/env python3
"""Sample Lichii's live debug server and write frame-time records as JSONL.

The emulator must be running with EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545 (see
docs/nx2/debug-mcp.md). Each sample asks the `frame_times` tool for a short
window and stores the statistics it returns, so a stutter shows up as a spike in
one window instead of being averaged away.

Send SIGUSR1 to mark a stutter; the mark is written into the stream so a
recording can be lined up with what you saw.
"""

import argparse
import json
import os
import signal
import socket
import sys
import time

BOOKMARK = {"at": None}
MAX_CONSECUTIVE_FAILURES = 600


def _on_signal(signum, frame):
    BOOKMARK["at"] = time.time()
    print("bookmark requested", flush=True)


class Client:
    """One request at a time over the JSON-lines socket."""

    def __init__(self, host, port, timeout):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.stream = None
        self.next_id = 1

    def connect(self):
        self.close()
        self.sock = socket.create_connection((self.host, self.port), timeout=10.0)
        self.sock.settimeout(self.timeout)
        self.stream = self.sock.makefile("rwb")

    def close(self):
        if self.stream is not None:
            try:
                self.stream.close()
            except OSError:
                pass
            self.stream = None
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def call(self, method, params=None):
        if self.stream is None:
            self.connect()
        request = {"id": self.next_id, "method": method}
        if params:
            request["params"] = params
        self.next_id += 1
        self.stream.write((json.dumps(request) + "\n").encode("utf-8"))
        self.stream.flush()
        line = self.stream.readline()
        if not line:
            raise ConnectionError("server closed the connection")
        message = json.loads(line)
        if isinstance(message, dict) and message.get("error"):
            raise RuntimeError(str(message["error"]))
        return message.get("result")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="127.0.0.1:6545")
    parser.add_argument("--out", required=True, help="JSONL output path")
    parser.add_argument("--interval", type=float, default=1.0,
                        help="seconds between samples (default 1)")
    parser.add_argument("--window", type=float, default=5.0,
                        help="frame_times window in seconds, max 60 (default 5)")
    parser.add_argument("--refresh-hz", type=float, default=60.0)
    parser.add_argument("--duration", type=float, default=0.0,
                        help="stop after this many seconds; 0 runs until killed")
    parser.add_argument("--sample-timeout", type=float, default=90.0)
    args = parser.parse_args()

    window = max(1.0, min(60.0, args.window))
    host, _, port = args.server.rpartition(":")
    if not host:
        host, port = "127.0.0.1", args.server

    signal.signal(signal.SIGUSR1, _on_signal)

    client = Client(host, int(port), args.sample_timeout)
    started = time.time()
    failures = 0
    samples = 0

    with open(args.out, "a", buffering=1, encoding="utf-8") as out:
        print(f"probe: {args.server} -> {args.out}", flush=True)
        while True:
            if args.duration and time.time() - started >= args.duration:
                break

            record = {
                "t": time.time(),
                "iso": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime()),
            }
            if BOOKMARK["at"] is not None:
                record["bookmark"] = round(BOOKMARK["at"], 3)
                BOOKMARK["at"] = None

            try:
                record["status"] = client.call("status")
                record["frame_times"] = client.call(
                    "frame_times",
                    {"seconds": window, "refresh_hz": args.refresh_hz},
                )
                failures = 0
                samples += 1
                ft = record["frame_times"] or {}
                print(
                    "fps={fps:.1f} p50={p50:.1f} p99={p99:.1f} max={mx:.1f} "
                    "stutters={st}".format(
                        fps=float(ft.get("fps") or 0.0),
                        p50=float(ft.get("p50_ms") or 0.0),
                        p99=float(ft.get("p99_ms") or 0.0),
                        mx=float(ft.get("max_ms") or 0.0),
                        st=ft.get("stutters"),
                    ),
                    flush=True,
                )
            except (OSError, ConnectionError) as error:
                record["error"] = f"{type(error).__name__}: {error}"
                client.close()
                failures += 1
                if failures == 1:
                    print(f"waiting for the debug server ({error})", flush=True)
                if failures > MAX_CONSECUTIVE_FAILURES:
                    print("giving up: server never came back", file=sys.stderr)
                    break
                time.sleep(min(2.0, args.interval))
                continue
            except RuntimeError as error:
                record["error"] = str(error)

            out.write(json.dumps(record, sort_keys=True) + "\n")

            if BOOKMARK["at"] is not None:
                out.write(json.dumps({
                    "t": BOOKMARK["at"],
                    "iso": time.strftime("%Y-%m-%dT%H:%M:%S",
                                         time.localtime(BOOKMARK["at"])),
                    "mark": True,
                }, sort_keys=True) + "\n")
                BOOKMARK["at"] = None

            time.sleep(args.interval)

    client.close()
    print(f"probe done: {samples} samples", flush=True)


if __name__ == "__main__":
    main()
