#!/usr/bin/env python3
"""Stress the XFined MCP socket lifetime and shutdown paths.

For the timeout case, launch the editor with
XFINED_MCP_REQUEST_TIMEOUT_MS=250 and pass --timeout-scene <scene.level>.
Use --shutdown or --shutdown-via-palette only when it is safe for this script
to close the editor.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import ctypes
from ctypes import wintypes
import json
import socket
import struct
import sys
import threading
import time
from typing import Any


MAX_RESPONSE_BYTES = 4 * 1024 * 1024


def connect(host: str, port: int, timeout: float) -> socket.socket:
    client = socket.create_connection((host, port), timeout=timeout)
    client.settimeout(timeout)
    return client


def receive_line(client: socket.socket, timeout: float) -> dict[str, Any]:
    client.settimeout(timeout)
    data = bytearray()
    while b"\n" not in data:
        block = client.recv(4096)
        if not block:
            raise ConnectionError("MCP closed before returning a response")
        data.extend(block)
        if len(data) > MAX_RESPONSE_BYTES:
            raise ValueError("MCP response exceeded the lifecycle-test limit")
    line, _, _ = data.partition(b"\n")
    return json.loads(line)


def send_request(client: socket.socket, payload: dict[str, Any], timeout: float) -> dict[str, Any]:
    wire = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"
    client.sendall(wire)
    return receive_line(client, timeout)


def one_request(host: str, port: int, payload: dict[str, Any], timeout: float) -> dict[str, Any]:
    with connect(host, port, timeout) as client:
        return send_request(client, payload, timeout)


def one_wire_request(host: str, port: int, wire: bytes, timeout: float) -> dict[str, Any]:
    with connect(host, port, timeout) as client:
        client.sendall(wire + b"\n")
        return receive_line(client, timeout)


def require_ping(host: str, port: int, timeout: float) -> None:
    response = one_request(host, port, {"cmd": "ping"}, timeout)
    if not response.get("ok") or response.get("name") != "XFined Editor":
        raise AssertionError(f"unexpected ping response: {response!r}")


def test_concurrent_clients(host: str, port: int, timeout: float, clients: int, repeats: int) -> None:
    barrier = threading.Barrier(clients)

    def run_client(index: int) -> int:
        completed = 0
        with connect(host, port, timeout) as client:
            barrier.wait(timeout=timeout)
            for sequence in range(repeats):
                response = send_request(client, {"cmd": "ping", "client": index, "sequence": sequence}, timeout)
                if not response.get("ok"):
                    raise AssertionError(f"client {index} ping failed: {response!r}")
                completed += 1
        return completed

    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as executor:
        counts = list(executor.map(run_client, range(clients)))
    expected = clients * repeats
    if sum(counts) != expected:
        raise AssertionError(f"completed {sum(counts)} concurrent requests, expected {expected}")


def reset_close(client: socket.socket) -> None:
    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("hh", 1, 0))
    client.close()


def test_disconnects(host: str, port: int, timeout: float, repeats: int) -> None:
    for index in range(repeats):
        client = connect(host, port, timeout)
        payload = b'{"cmd":"ping","disconnect":' + str(index).encode("ascii") + b"}\n"
        client.sendall(payload if index % 2 else payload[: len(payload) // 2])
        reset_close(client)
    time.sleep(0.25)
    require_ping(host, port, timeout)


def test_json_arguments(host: str, port: int, timeout: float) -> None:
    cyrillic = "я" * 127
    response = one_request(
        host, port, {"cmd": "command_palette", "action": "query", "query": cyrillic}, timeout
    )
    if not response.get("ok") or response.get("query") != cyrillic:
        raise AssertionError(f"escaped UTF-8 argument changed: {response!r}")

    raw = json.dumps(
        {"cmd": "command_palette", "action": "query", "query": cyrillic},
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")
    response = one_wire_request(host, port, raw, timeout)
    if not response.get("ok") or response.get("query") != cyrillic:
        raise AssertionError(f"raw UTF-8 argument changed: {response!r}")

    for query in ("я" * 128, "😀" * 64):
        response = one_request(
            host, port, {"cmd": "command_palette", "action": "query", "query": query}, timeout
        )
        if response.get("query") not in (None, ""):
            raise AssertionError(f"oversized UTF-8 argument was truncated: {response!r}")

    malformed = (
        b'{"cmd":"command_palette","action":"query","query":"\\ud800"}',
        b'{"cmd":"command_palette","action":"query","query":"\\udc00"}',
        b'{"cmd":"command_palette","action":"query","query":"bad\\xescape"}',
        b'{"cmd":"command_palette","action":"query","query":"unterminated}',
        b'{"cmd":"command_palette","action":"query","query":"\xc0\xaf"}',
    )
    for wire in malformed:
        response = one_wire_request(host, port, wire, timeout)
        if response.get("ok"):
            raise AssertionError(f"malformed JSON argument was accepted: {response!r}")

    response = one_wire_request(
        host,
        port,
        b'{"cmd":"command_palette","action":"query","query":null,"next":"sentinel"}',
        timeout,
    )
    if response.get("query") == "sentinel":
        raise AssertionError(f"non-string value captured the next field: {response!r}")

    response = one_wire_request(
        host,
        port,
        b'{"cmd":"property_inspector","meta":{"filter":"wrong"},"action":"filter",'
        b'"target":"world","filter":"lighting"}',
        timeout,
    )
    if not response.get("ok") or response.get("filter") != "lighting":
        raise AssertionError(f"nested field shadowed a top-level argument: {response!r}")

    require_ping(host, port, timeout)


def test_overflow(host: str, port: int, timeout: float) -> None:
    with connect(host, port, timeout) as client:
        client.sendall(b'{"cmd":"ping","padding":"' + b"x" * (1024 * 1024 + 1) + b'"}\n')
        response = receive_line(client, timeout)
        if response.get("ok") or "1 MiB" not in response.get("error", ""):
            raise AssertionError(f"request bound was not enforced: {response!r}")
    require_ping(host, port, timeout)


def test_timeout(host: str, port: int, timeout: float, scene: str, recovery_timeout: float) -> None:
    with connect(host, port, recovery_timeout) as client:
        response = send_request(client, {"cmd": "open_scene", "scene": scene}, recovery_timeout)
        if response.get("ok") or "timeout" not in response.get("error", ""):
            raise AssertionError(
                "open_scene did not time out; launch with "
                f"XFINED_MCP_REQUEST_TIMEOUT_MS below the scene load time: {response!r}"
            )

        deadline = time.monotonic() + recovery_timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                response = send_request(client, {"cmd": "ping"}, timeout)
                if response.get("ok"):
                    return
            except (ConnectionError, OSError, TimeoutError, ValueError) as error:
                last_error = error
            time.sleep(0.1)
        raise AssertionError(f"same connection did not recover after timed-out scene load: {last_error!r}")


def process_exited(pid: int, timeout: float) -> bool:
    synchronize = 0x00100000
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    handle = kernel32.OpenProcess(synchronize, False, pid)
    if not handle:
        return True
    try:
        return kernel32.WaitForSingleObject(handle, int(timeout * 1000)) == 0
    finally:
        kernel32.CloseHandle(handle)


def test_shutdown(
    host: str,
    port: int,
    timeout: float,
    idle_count: int,
    pid: int | None,
    via_palette: bool,
) -> None:
    idle_clients = [connect(host, port, timeout) for _ in range(idle_count)]
    for client in idle_clients:
        client.sendall(b'{"cmd":"pin')

    try:
        with connect(host, port, timeout) as quitter:
            request = (
                b'{"cmd":"command_palette","action":"execute","id":"COMMAND_EXIT"}\n'
                if via_palette
                else b'{"cmd":"exec_command","name":"COMMAND_QUIT"}\n'
            )
            quitter.sendall(request)
            try:
                receive_line(quitter, timeout)
            except (ConnectionError, OSError, TimeoutError):
                pass

        deadline = time.monotonic() + timeout
        for client in idle_clients:
            client.settimeout(max(0.1, deadline - time.monotonic()))
            try:
                if client.recv(1):
                    raise AssertionError("idle client received data instead of shutdown EOF")
            except (ConnectionResetError, ConnectionAbortedError):
                pass
    finally:
        for client in idle_clients:
            client.close()

    if pid and not process_exited(pid, timeout):
        raise AssertionError(f"editor process {pid} did not exit within {timeout:.1f}s")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=28016)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--repeats", type=int, default=20)
    parser.add_argument("--disconnects", type=int, default=64)
    parser.add_argument("--skip-overflow", action="store_true")
    parser.add_argument("--timeout-scene")
    parser.add_argument("--recovery-timeout", type=float, default=30.0)
    parser.add_argument("--shutdown", action="store_true")
    parser.add_argument("--shutdown-via-palette", action="store_true")
    parser.add_argument("--shutdown-idle-clients", type=int, default=8)
    parser.add_argument("--pid", type=int)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    require_ping(args.host, args.port, args.timeout)
    test_concurrent_clients(args.host, args.port, args.timeout, args.clients, args.repeats)
    test_disconnects(args.host, args.port, args.timeout, args.disconnects)
    test_json_arguments(args.host, args.port, args.timeout)
    if not args.skip_overflow:
        test_overflow(args.host, args.port, args.timeout)
    if args.timeout_scene:
        test_timeout(args.host, args.port, args.timeout, args.timeout_scene, args.recovery_timeout)
    if args.shutdown or args.shutdown_via_palette:
        test_shutdown(
            args.host,
            args.port,
            args.timeout,
            args.shutdown_idle_clients,
            args.pid,
            args.shutdown_via_palette,
        )
    print("XFined MCP lifecycle stress passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"XFined MCP lifecycle stress failed: {error}", file=sys.stderr)
        raise
