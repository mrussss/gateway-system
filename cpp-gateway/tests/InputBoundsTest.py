#!/usr/bin/env python3
"""Verify that invalid streams are rejected before buffering their bodies."""

from __future__ import annotations

import json
import signal
import socket
import struct
import sys
import time
from pathlib import Path

from GracefulShutdownTest import (
    ECHO,
    ECHO_RESP,
    STATS,
    STATS_RESP,
    FakeControlPlane,
    GatewayProcess,
    packet,
    recv_packet,
)


def expect_closed(sock: socket.socket) -> None:
    sock.settimeout(1)
    try:
        if sock.recv(1) != b"":
            raise AssertionError("invalid input received an unexpected response")
    except ConnectionResetError:
        pass


def received_bytes(sock: socket.socket, request_id: int) -> int:
    sock.sendall(packet(STATS, request_id))
    message_type, response_id, payload = recv_packet(sock)
    if (message_type, response_id) != (STATS_RESP, request_id):
        raise AssertionError("observer did not receive its STATS response")
    return int(json.loads(payload)["total_recv_bytes"])


def test_prefilled_invalid_stream(executable: Path, control_plane: FakeControlPlane) -> None:
    gateway = GatewayProcess(executable, control_plane.port, shutdown_ms=2000)
    try:
        with gateway.connect_authenticated("input-observer") as observer:
            before = received_bytes(observer, 2)
            with socket.create_connection(("127.0.0.1", gateway.port), timeout=3) as invalid:
                gateway.process.send_signal(signal.SIGSTOP)
                try:
                    deadline = time.monotonic() + 1
                    while "State:\tT" not in Path(f"/proc/{gateway.process.pid}/status").read_text():
                        if time.monotonic() >= deadline:
                            raise AssertionError("gateway did not stop before input prefill")
                        time.sleep(0.005)
                    # 先让内核收齐一批非法输入，消除发送方和 Reactor 的调度竞争。
                    invalid.sendall(struct.pack("!I", 9) + b"x" * 65_532)
                finally:
                    gateway.process.send_signal(signal.SIGCONT)
                expect_closed(invalid)
            consumed = received_bytes(observer, 3) - before - len(packet(STATS, 3))
            if not 0 < consumed <= 4096:
                raise AssertionError(f"invalid stream buffered {consumed} bytes before length validation")
    finally:
        gateway.signal()
        gateway.wait(timeout=4)


def test_runtime_limit_checked_from_header(executable: Path, control_plane: FakeControlPlane) -> None:
    gateway = GatewayProcess(executable, control_plane.port, shutdown_ms=2000)
    try:
        with socket.create_connection(("127.0.0.1", gateway.port), timeout=3) as invalid:
            # 默认 runtime body 上限是 1 MiB；无需发送超限 body 就应拒绝。
            invalid.sendall(struct.pack("!I", 1_048_577))
            expect_closed(invalid)
    finally:
        gateway.signal()
        gateway.wait(timeout=4)


def test_large_frames_survive_incremental_reads(executable: Path, control_plane: FakeControlPlane) -> None:
    gateway = GatewayProcess(executable, control_plane.port, shutdown_ms=2000)
    try:
        with gateway.connect_authenticated("input-large") as sock:
            payload = b"bounded-input" * 8000
            frames = packet(ECHO, 2, payload) + packet(ECHO, 3, b"tail")
            sock.sendall(frames[:7])
            sock.sendall(frames[7:])
            if recv_packet(sock) != (ECHO_RESP, 2, payload):
                raise AssertionError("large fragmented frame changed")
            if recv_packet(sock) != (ECHO_RESP, 3, b"tail"):
                raise AssertionError("sticky trailing frame was lost")
    finally:
        gateway.signal()
        gateway.wait(timeout=4)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/message_server", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1]).resolve()
    failures = 0
    with FakeControlPlane() as control_plane:
        for test in (
            test_prefilled_invalid_stream,
            test_runtime_limit_checked_from_header,
            test_large_frames_survive_incremental_reads,
        ):
            try:
                test(executable, control_plane)
                print(f"[PASS] {test.__name__}")
            except (AssertionError, OSError) as error:
                failures += 1
                print(f"[FAIL] {test.__name__}: {error}", file=sys.stderr)
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
