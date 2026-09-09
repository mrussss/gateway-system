#!/usr/bin/env python3
"""Black-box checks for readable data surviving a TCP peer half-close."""

from __future__ import annotations

import json
import socket
import sys
from pathlib import Path

from GracefulShutdownTest import (
    AUTH,
    AUTH_RESP,
    ECHO,
    ECHO_RESP,
    PING,
    FakeControlPlane,
    GatewayProcess,
    packet,
    recv_packet,
)

PONG = 5


def assert_eof(sock: socket.socket) -> None:
    sock.settimeout(3)
    if sock.recv(1) != b"":
        raise AssertionError("expected gateway to close after draining half-closed output")


def authenticated_socket(gateway: GatewayProcess, client_id: str) -> socket.socket:
    sock = gateway.connect_authenticated(client_id)
    sock.settimeout(3)
    return sock


def test_single_request(executable: Path, control_plane: FakeControlPlane) -> None:
    gateway = GatewayProcess(executable, control_plane.port, shutdown_ms=2000)
    sock = authenticated_socket(gateway, "half-close-single")
    try:
        sock.sendall(packet(ECHO, 2, b"half-close"))
        sock.shutdown(socket.SHUT_WR)
        message_type, request_id, payload = recv_packet(sock)
        if (message_type, request_id, payload) != (ECHO_RESP, 2, b"half-close"):
            raise AssertionError("single request was not answered after SHUT_WR")
        assert_eof(sock)
    finally:
        sock.close()
        gateway.signal()
        gateway.wait(timeout=4)


def test_sticky_requests(executable: Path, control_plane: FakeControlPlane) -> None:
    gateway = GatewayProcess(executable, control_plane.port, shutdown_ms=2000)
    sock = authenticated_socket(gateway, "half-close-sticky")
    try:
        sock.sendall(
            packet(ECHO, 2, b"one")
            + packet(ECHO, 3, b"two")
            + packet(ECHO, 4, b"three")
        )
        sock.shutdown(socket.SHUT_WR)
        responses = [recv_packet(sock) for _ in range(3)]
        if [response[1] for response in responses] != [2, 3, 4]:
            raise AssertionError(f"sticky responses were incomplete: {responses}")
        if [response[2] for response in responses] != [b"one", b"two", b"three"]:
            raise AssertionError(f"sticky response payloads changed: {responses}")
        assert_eof(sock)
    finally:
        sock.close()
        gateway.signal()
        gateway.wait(timeout=4)


def test_auth_request(executable: Path, control_plane: FakeControlPlane) -> None:
    gateway = GatewayProcess(executable, control_plane.port, shutdown_ms=2000)
    sock = socket.create_connection(("127.0.0.1", gateway.port), timeout=3)
    try:
        payload = json.dumps({"client_id": "half-close-auth", "token": "test-token"}).encode()
        sock.sendall(packet(AUTH, 1, payload))
        sock.shutdown(socket.SHUT_WR)
        message_type, request_id, response_payload = recv_packet(sock)
        if message_type != AUTH_RESP or request_id != 1:
            raise AssertionError("AUTH response was not written after SHUT_WR")
        if json.loads(response_payload).get("allowed") is not True:
            raise AssertionError("half-closed AUTH was rejected")
        assert_eof(sock)
    finally:
        sock.close()
        gateway.signal()
        gateway.wait(timeout=4)


def test_truncated_tail(executable: Path, control_plane: FakeControlPlane) -> None:
    gateway = GatewayProcess(executable, control_plane.port, shutdown_ms=2000)
    sock = authenticated_socket(gateway, "half-close-truncated")
    try:
        complete = packet(PING, 2)
        truncated = packet(ECHO, 3, b"tail")[:5]
        sock.sendall(complete + truncated)
        sock.shutdown(socket.SHUT_WR)
        message_type, request_id, _ = recv_packet(sock)
        if (message_type, request_id) != (PONG, 2):
            raise AssertionError("complete frame was lost before truncated EOF")
        assert_eof(sock)
    finally:
        sock.close()
        gateway.signal()
        gateway.wait(timeout=4)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/message_server", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1]).resolve()
    with FakeControlPlane() as control_plane:
        test_single_request(executable, control_plane)
        test_sticky_requests(executable, control_plane)
        test_auth_request(executable, control_plane)
        test_truncated_tail(executable, control_plane)
    print("half-close scenarios passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
