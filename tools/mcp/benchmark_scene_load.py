#!/usr/bin/env python3
"""Measure synchronous scene loads and first-frame readiness through MCP."""

import argparse
import json
import socket
import statistics
import sys
import time


def editor_call(host: str, port: int, timeout: float, payload: dict) -> dict:
    with socket.create_connection((host, port), timeout=timeout) as connection:
        connection.settimeout(timeout)
        connection.sendall((json.dumps(payload) + "\n").encode())
        response = bytearray()
        while not response.endswith(b"\n"):
            chunk = connection.recv(1 << 20)
            if not chunk:
                break
            response.extend(chunk)
    return json.loads(response.decode())


def checked_call(host: str, port: int, timeout: float, payload: dict) -> dict:
    response = editor_call(host, port, timeout, payload)
    if not response.get("ok"):
        raise RuntimeError(response.get("error", f"request failed: {response!r}"))
    return response


def percentile(samples: list[float], fraction: float) -> float:
    ordered = sorted(samples)
    return ordered[min(len(ordered) - 1, max(0, int(len(ordered) * fraction + 0.999999) - 1))]


def summarize(samples: list[float]) -> dict:
    return {
        "samples_ms": [round(sample, 3) for sample in samples],
        "min_ms": round(min(samples), 3),
        "median_ms": round(statistics.median(samples), 3),
        "p95_ms": round(percentile(samples, 0.95), 3),
        "max_ms": round(max(samples), 3),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", help="scene path accepted by xfined_open_scene")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--expected-total", type=int)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=28016)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    if args.repeats < 1 or args.warmup < 0:
        parser.error("--repeats must be positive and --warmup cannot be negative")

    checked_call(args.host, args.port, args.timeout, {"cmd": "ping"})
    load_timings = []
    ready_timings = []
    object_total = 0
    for index in range(args.warmup + args.repeats):
        started = time.perf_counter()
        checked_call(args.host, args.port, args.timeout, {"cmd": "open_scene", "scene": args.scene})
        load_ms = (time.perf_counter() - started) * 1000.0
        info = checked_call(args.host, args.port, args.timeout, {"cmd": "scene_info"})
        ready_ms = (time.perf_counter() - started) * 1000.0
        object_total = sum(tool.get("objects", 0) for tool in info.get("tools", []))
        if args.expected_total is not None and object_total != args.expected_total:
            raise RuntimeError(f"object total changed: expected {args.expected_total}, got {object_total}")
        if index >= args.warmup:
            load_timings.append(load_ms)
            ready_timings.append(ready_ms)
            print(
                f"sample {index - args.warmup + 1}: load {load_ms:.3f} ms, "
                f"ready {ready_ms:.3f} ms ({object_total} objects)"
            )

    summary = {
        "scene": args.scene,
        "load": summarize(load_timings),
        "ready": summarize(ready_timings),
        "object_total": object_total,
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, json.JSONDecodeError, RuntimeError) as error:
        print(f"benchmark failed: {error}", file=sys.stderr)
        raise SystemExit(2)
