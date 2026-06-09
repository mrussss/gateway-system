#!/usr/bin/env python3
"""End-to-end checks for the C++ gateway TCP protocol on localhost:9000."""

import argparse
import json
import socket
import struct
import time
from dataclasses import dataclass


VERSION = 1
FIXED_BODY_SIZE = 1 + 1 + 8
PING = 1
ECHO = 2
LOG_PUSH = 3
STATS = 4
PONG = 5
ECHO_RESP = 6
ERROR_RESP = 7
LOG_ACK = 8
STATS_RESP = 9
AUTH = 10
AUTH_RESP = 11
MAX_BODY_SIZE = 4 * 1024 * 1024 + FIXED_BODY_SIZE


@dataclass
class Response:
    version: int
    msg_type: int
    request_id: int
    payload: bytes


def packet(msg_type: int, request_id: int, payload: bytes = b"") -> bytes:
    body_length = FIXED_BODY_SIZE + len(payload)
    return struct.pack("!IBBQ", body_length, VERSION, msg_type, request_id) + payload


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError(f"connection closed while reading {size} bytes")
        data += chunk
    return data


def recv_response(sock: socket.socket) -> Response:
    header = recv_exact(sock, 4)
    body_length = struct.unpack("!I", header)[0]
    body = recv_exact(sock, body_length)
    version, msg_type, request_id = struct.unpack("!BBQ", body[:FIXED_BODY_SIZE])
    return Response(version, msg_type, request_id, body[FIXED_BODY_SIZE:])


def connect(host: str, port: int) -> socket.socket:
    sock = socket.create_connection((host, port), timeout=5.0)
    sock.settimeout(5.0)
    return sock


def authenticate(sock: socket.socket, request_id: int = 9000) -> None:
    payload = json.dumps({
        "client_id": f"tcp-test-{request_id}",
        "token": "test-token",
    }).encode("utf-8")
    sock.sendall(packet(AUTH, request_id, payload))
    resp = recv_response(sock)
    assert_response(resp, AUTH_RESP, request_id)
    body = json.loads(resp.payload.decode("utf-8"))
    if body.get("allowed") is not True:
        raise AssertionError(f"AUTH rejected unexpectedly: {body}")


def assert_response(resp: Response, msg_type: int, request_id: int, payload: bytes | None = None) -> None:
    if resp.version != VERSION:
        raise AssertionError(f"expected version={VERSION}, got {resp.version}")
    if resp.msg_type != msg_type:
        raise AssertionError(f"expected type={msg_type}, got {resp.msg_type}, payload={resp.payload!r}")
    if resp.request_id != request_id:
        raise AssertionError(f"expected request_id={request_id}, got {resp.request_id}")
    if payload is not None and resp.payload != payload:
        raise AssertionError(f"expected payload={payload!r}, got {resp.payload!r}")


def test_ping(host: str, port: int) -> None:
    with connect(host, port) as sock:
        authenticate(sock, 9001)
        sock.sendall(packet(PING, 1001))
        resp = recv_response(sock)
    assert_response(resp, PONG, 1001)
    body = json.loads(resp.payload.decode("utf-8"))
    if body.get("message") != "pong":
        raise AssertionError(f"unexpected PONG payload: {body}")
    print("[tcp] PASS ping")


def test_echo(host: str, port: int) -> None:
    payload = b"hello gateway"
    with connect(host, port) as sock:
        authenticate(sock, 9002)
        sock.sendall(packet(ECHO, 1002, payload))
        resp = recv_response(sock)
    assert_response(resp, ECHO_RESP, 1002, payload)
    print("[tcp] PASS echo")


def test_stats(host: str, port: int) -> None:
    with connect(host, port) as sock:
        authenticate(sock, 9003)
        sock.sendall(packet(STATS, 1003))
        resp = recv_response(sock)
    assert_response(resp, STATS_RESP, 1003)
    body = json.loads(resp.payload.decode("utf-8"))
    required = {"total_requests", "total_errors", "active_connections"}
    missing = required - body.keys()
    if missing:
        raise AssertionError(f"STATS missing keys: {sorted(missing)}")
    print("[tcp] PASS stats")


def test_log_push(host: str, port: int) -> None:
    payload = json.dumps({
        "level": "INFO",
        "service": "tcp-protocol-test",
        "message": "log push smoke test",
    }).encode("utf-8")
    with connect(host, port) as sock:
        authenticate(sock, 9004)
        sock.sendall(packet(LOG_PUSH, 1004, payload))
        resp = recv_response(sock)
    assert_response(resp, LOG_ACK, 1004)
    print("[tcp] PASS log_push")


def test_half_packet(host: str, port: int) -> None:
    payload = b"split payload"
    raw = packet(ECHO, 1005, payload)
    with connect(host, port) as sock:
        authenticate(sock, 9005)
        sock.sendall(raw[:4])
        sock.settimeout(0.4)
        try:
            early = sock.recv(1)
            if early:
                raise AssertionError("server responded before full packet arrived")
        except socket.timeout:
            pass
        sock.settimeout(5.0)
        sock.sendall(raw[4:])
        resp = recv_response(sock)
    assert_response(resp, ECHO_RESP, 1005, payload)
    print("[tcp] PASS half_packet")


def test_sticky_packet(host: str, port: int) -> None:
    expected = {
        2000 + i: f"sticky-{i}".encode("utf-8")
        for i in range(5)
    }
    packets = [packet(ECHO, request_id, payload) for request_id, payload in expected.items()]
    with connect(host, port) as sock:
        authenticate(sock, 9006)
        sock.sendall(b"".join(packets))
        responses = [recv_response(sock) for _ in packets]

    seen = set()
    for resp in responses:
        if resp.request_id not in expected:
            raise AssertionError(f"unexpected sticky response id={resp.request_id}")
        if resp.request_id in seen:
            raise AssertionError(f"duplicate sticky response id={resp.request_id}")
        seen.add(resp.request_id)
        assert_response(resp, ECHO_RESP, resp.request_id, expected[resp.request_id])

    if seen != set(expected):
        raise AssertionError(f"missing sticky response ids={sorted(set(expected) - seen)}")
    print("[tcp] PASS sticky_packet")


def test_invalid_length(host: str, port: int) -> None:
    invalid_lengths = [0, 5, 9, MAX_BODY_SIZE + 1, 999999999]
    for body_length in invalid_lengths:
        with connect(host, port) as sock:
            sock.sendall(struct.pack("!I", body_length) + b"x" * min(body_length, 64))
            time.sleep(0.2)
            sock.settimeout(1.0)
            try:
                data = sock.recv(1)
            except (ConnectionResetError, BrokenPipeError):
                data = b""
            if data:
                raise AssertionError(f"expected close for invalid length {body_length}, got data={data!r}")

    with connect(host, port) as sock:
        authenticate(sock, 9007)
        sock.sendall(packet(PING, 3001))
        resp = recv_response(sock)
    assert_response(resp, PONG, 3001)
    print("[tcp] PASS invalid_length")


def test_auth_required(host: str, port: int) -> None:
    with connect(host, port) as sock:
        sock.sendall(packet(PING, 4001))
        time.sleep(0.2)
        try:
            data = sock.recv(1)
        except (ConnectionResetError, BrokenPipeError):
            data = b""
        if data:
            raise AssertionError(f"expected close before AUTH, got data={data!r}")

    with connect(host, port) as sock:
        payload = json.dumps({
            "client_id": "tcp-test-invalid",
            "token": "bad-token",
        }).encode("utf-8")
        sock.sendall(packet(AUTH, 4002, payload))
        time.sleep(0.2)
        try:
            data = sock.recv(1)
        except (ConnectionResetError, BrokenPipeError):
            data = b""
        if data:
            raise AssertionError(f"expected close after invalid AUTH, got data={data!r}")

    print("[tcp] PASS auth_required")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=9000, type=int)
    args = parser.parse_args()

    tests = [
        test_ping,
        test_echo,
        test_stats,
        test_log_push,
        test_half_packet,
        test_sticky_packet,
        test_invalid_length,
        test_auth_required,
    ]

    for test in tests:
        test(args.host, args.port)

    print("[tcp] PASS all protocol checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
