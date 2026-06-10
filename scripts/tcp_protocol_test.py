#!/usr/bin/env python3
"""End-to-end checks for the C++ gateway TCP protocol on localhost:9000."""

import argparse
import json
import socket
import struct
import threading
import time
import urllib.request
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


def authenticate(sock: socket.socket, request_id: int = 9000, client_id: str | None = None) -> None:
    payload = json.dumps({
        "client_id": client_id or f"tcp-test-{request_id}",
        "token": "test-token",
    }).encode("utf-8")
    sock.sendall(packet(AUTH, request_id, payload))
    resp = recv_response(sock)
    assert_response(resp, AUTH_RESP, request_id)
    body = json.loads(resp.payload.decode("utf-8"))
    if body.get("allowed") is not True:
        raise AssertionError(f"AUTH rejected unexpectedly: {body}")


def expect_closed(sock: socket.socket, message: str) -> None:
    time.sleep(0.2)
    try:
        data = sock.recv(1)
    except (ConnectionResetError, BrokenPipeError):
        data = b""
    if data:
        raise AssertionError(f"{message}, got data={data!r}")


def fetch_clients(control_plane_url: str) -> list[dict]:
    with urllib.request.urlopen(f"{control_plane_url}/clients", timeout=2.0) as resp:
        return json.loads(resp.read().decode("utf-8"))


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
        expect_closed(sock, "expected close before AUTH")

    with connect(host, port) as sock:
        payload = json.dumps({
            "client_id": "tcp-test-invalid",
            "token": "bad-token",
        }).encode("utf-8")
        sock.sendall(packet(AUTH, 4002, payload))
        expect_closed(sock, "expected close after invalid AUTH")

    print("[tcp] PASS auth_required")


def test_auth_invalid_json(host: str, port: int) -> None:
    with connect(host, port) as sock:
        sock.sendall(packet(AUTH, 4101, b"not-json"))
        expect_closed(sock, "expected close after invalid AUTH JSON")

    print("[tcp] PASS auth_invalid_json")


def test_auth_missing_fields(host: str, port: int) -> None:
    cases = [
        (b'{"token":"test-token"}', "missing client_id"),
        (b'{"client_id":"tcp-test-missing-token"}', "missing token"),
    ]
    for payload, name in cases:
        with connect(host, port) as sock:
            sock.sendall(packet(AUTH, 4102, payload))
            expect_closed(sock, f"expected close after AUTH {name}")

    print("[tcp] PASS auth_missing_fields")


def test_auth_invalid_field_types(host: str, port: int) -> None:
    cases = [
        (b'{"client_id":123,"token":"test-token"}', "non-string client_id"),
        (b'{"client_id":"tcp-test-type","token":123}', "non-string token"),
    ]
    for payload, name in cases:
        with connect(host, port) as sock:
            sock.sendall(packet(AUTH, 4103, payload))
            expect_closed(sock, f"expected close after AUTH {name}")

    print("[tcp] PASS auth_invalid_field_types")


def test_auth_duplicate(host: str, port: int) -> None:
    with connect(host, port) as sock:
        authenticate(sock, 4104)
        payload = json.dumps({
            "client_id": "tcp-test-duplicate",
            "token": "test-token",
        }).encode("utf-8")
        sock.sendall(packet(AUTH, 4105, payload))
        resp = recv_response(sock)

    assert_response(resp, ERROR_RESP, 4105)
    body = json.loads(resp.payload.decode("utf-8"))
    if body.get("message") != "already authenticated":
        raise AssertionError(f"unexpected duplicate AUTH response: {body}")

    print("[tcp] PASS auth_duplicate")


def test_clients_reports_authenticated_id(host: str, port: int, control_plane_url: str) -> None:
    client_id = "tcp-test-real-client-id"
    with connect(host, port) as sock:
        authenticate(sock, 4105, client_id=client_id)
        deadline = time.time() + 8.0
        while time.time() < deadline:
            clients = fetch_clients(control_plane_url)
            if any(client.get("client_id") == client_id for client in clients):
                print("[tcp] PASS clients_reports_authenticated_id")
                return
            time.sleep(0.5)

    raise AssertionError(f"/clients did not include authenticated client_id={client_id}")


def test_clients_excludes_unauthenticated(host: str, port: int, control_plane_url: str) -> None:
    unauthenticated_id = "client_"
    with connect(host, port) as sock:
        deadline = time.time() + 8.0
        while time.time() < deadline:
            clients = fetch_clients(control_plane_url)
            if any(str(client.get("client_id", "")).startswith(unauthenticated_id) for client in clients):
                raise AssertionError("/clients included an unauthenticated placeholder client_id")
            time.sleep(0.5)

    print("[tcp] PASS clients_excludes_unauthenticated")


def test_repeated_auth_ping_close(host: str, port: int) -> None:
    for i in range(5):
        with connect(host, port) as sock:
            authenticate(sock, 5000 + i)
            sock.sendall(packet(PING, 5100 + i))
            resp = recv_response(sock)
        assert_response(resp, PONG, 5100 + i)

    print("[tcp] PASS repeated_auth_ping_close")


def test_concurrent_auth_echo(host: str, port: int) -> None:
    errors: list[str] = []

    def worker(index: int) -> None:
        payload = f"concurrent-{index}".encode("utf-8")
        try:
            with connect(host, port) as sock:
                authenticate(sock, 5200 + index)
                sock.sendall(packet(ECHO, 5300 + index, payload))
                resp = recv_response(sock)
            assert_response(resp, ECHO_RESP, 5300 + index, payload)
        except Exception as exc:  # noqa: BLE001
            errors.append(f"worker {index}: {exc}")

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(5)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    if errors:
        raise AssertionError("; ".join(errors))

    print("[tcp] PASS concurrent_auth_echo")


def test_auth_pending_second_request_closes(host: str, port: int) -> None:
    payload = json.dumps({
        "client_id": "tcp-test-pending-close",
        "token": "test-token",
    }).encode("utf-8")
    with connect(host, port) as sock:
        sock.sendall(packet(AUTH, 5401, payload) + packet(PING, 5402))
        expect_closed(sock, "expected close after request sent while AUTH pending")

    print("[tcp] PASS auth_pending_second_request_closes")


def test_clients_remove_disconnected_client(host: str, port: int, control_plane_url: str) -> None:
    client_id = "tcp-test-disconnect-cleanup"
    with connect(host, port) as sock:
        authenticate(sock, 5501, client_id=client_id)
        deadline = time.time() + 8.0
        while time.time() < deadline:
            clients = fetch_clients(control_plane_url)
            if any(client.get("client_id") == client_id for client in clients):
                break
            time.sleep(0.5)
        else:
            raise AssertionError(f"/clients did not include authenticated client_id={client_id}")

    deadline = time.time() + 8.0
    while time.time() < deadline:
        clients = fetch_clients(control_plane_url)
        if not any(client.get("client_id") == client_id for client in clients):
            print("[tcp] PASS clients_remove_disconnected_client")
            return
        time.sleep(0.5)

    raise AssertionError(f"/clients still included disconnected client_id={client_id}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=9000, type=int)
    parser.add_argument("--control-plane-url", default="http://127.0.0.1:8080")
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
        test_auth_invalid_json,
        test_auth_missing_fields,
        test_auth_invalid_field_types,
        test_auth_duplicate,
    ]

    for test in tests:
        test(args.host, args.port)
    test_clients_reports_authenticated_id(args.host, args.port, args.control_plane_url)
    test_clients_excludes_unauthenticated(args.host, args.port, args.control_plane_url)
    test_repeated_auth_ping_close(args.host, args.port)
    test_concurrent_auth_echo(args.host, args.port)
    test_auth_pending_second_request_closes(args.host, args.port)
    test_clients_remove_disconnected_client(args.host, args.port, args.control_plane_url)

    print("[tcp] PASS all protocol checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
