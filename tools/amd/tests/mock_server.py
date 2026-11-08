#!/usr/bin/env python3
"""Minimal stand-in for Lichii's live debug server, for testing the diagnostics tools.

Speaks the JSON-lines protocol from docs/nx2/debug-mcp.md on a loopback port and
answers `status` and `frame_times`, with a slow window every few samples so the
analyser has a stutter to find. Used by tests/test_tools.py and by the
end-to-end check of lichii-diag.sh.
"""

import argparse
import json
import socket
import sys
import time


def frame_times_payload(index):
    stutter = index % 4 == 3
    mean_ms = 33.0 if stutter else 16.6
    return {
        "frames": 60,
        "mean_ms": mean_ms,
        "fps": 1000.0 / mean_ms,
        "p50_ms": 16.6,
        "p90_ms": 17.5,
        "p99_ms": mean_ms * 2.5,
        "max_ms": mean_ms * 4.0,
        "min_ms": 16.0,
        "vsync_buckets": {"1": 0.6, "2": 0.3, "3": 0.05, "4+": 0.05},
        "stutters": 12 if stutter else 1,
        "intervals_ms": [16.6] * 8 + [140.0 if stutter else 17.0] * 2,
        "truncated": False,
    }


def serve(port):  # noqa: C901 - small and linear
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", port))
    server.listen(4)
    server.settimeout(0.5)
    index = 0
    print(f"mock debug server on 127.0.0.1:{port}", flush=True)
    while True:
        try:
            connection, _ = server.accept()
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            return
        stream = connection.makefile("rwb")
        try:
            while True:
                line = stream.readline()
                if not line:
                    break
                try:
                    request = json.loads(line)
                except json.JSONDecodeError:
                    break
                method = request.get("method")
                if method == "frame_times":
                    index += 1
                    time.sleep(0.2)
                    result = frame_times_payload(index)
                elif method == "status":
                    result = {
                        "title": "The Legend of Zelda: Ocarina of Time",
                        "program_id": "0400486024DEC000",
                        "fps": 41.0,
                        "paused": False,
                        "renderer": "Vulkan",
                    }
                else:
                    stream.write(json.dumps({"id": request.get("id"),
                                             "error": "unknown method"}).encode() + b"\n")
                    stream.flush()
                    continue
                stream.write(json.dumps({"id": request.get("id"),
                                         "result": result}).encode() + b"\n")
                stream.flush()
        except OSError:
            pass
        finally:
            try:
                stream.close()
                connection.close()
            except OSError:
                pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=6545)
    args = parser.parse_args()
    try:
        serve(args.port)
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
