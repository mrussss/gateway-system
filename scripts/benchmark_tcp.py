#!/usr/bin/env python3
"""Lightweight TCP benchmark for the gateway protocol."""

import argparse
import json
import math
import socket
import struct
import threading
import time
import urllib.error
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

MESSAGE_TYPES = {
    "ping": (PING, PONG),
    "echo": (ECHO, ECHO_RESP),
    "log_push": (LOG_PUSH, LOG_ACK),
    "stats": (STATS, STATS_RESP),
}


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


def register_token(control_plane_url: str, client_id: str, token: str) -> None:
    payload = json.dumps({
        "client_id": client_id,
        "token": token,
    }).encode("utf-8")
    request = urllib.request.Request(
        f"{control_plane_url}/tokens",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=2.0) as resp:
        body = json.loads(resp.read().decode("utf-8"))
    if body.get("success") is not True:
        raise RuntimeError(f"token registration failed: {body}")


def authenticate(sock: socket.socket, client_id: str, token: str) -> None:
    payload = json.dumps({
        "client_id": client_id,
        "token": token,
    }).encode("utf-8")
    sock.sendall(packet(AUTH, 1, payload))
    resp = recv_response(sock)
    if resp.version != VERSION or resp.msg_type != AUTH_RESP or resp.request_id != 1:
        raise RuntimeError(f"unexpected auth response: {resp}")
    body = json.loads(resp.payload.decode("utf-8"))
    if body.get("allowed") is not True:
        raise RuntimeError(f"auth rejected: {body}")


def build_payload(message: str, payload_text: str) -> bytes:
    if message == "ping" or message == "stats":
        return b""
    if message == "echo":
        return payload_text.encode("utf-8")
    return json.dumps({
        "level": "INFO",
        "service": "benchmark_tcp",
        "message": payload_text,
    }).encode("utf-8")


def percentile_95(latencies_ms: list[float]) -> float:
    if not latencies_ms:
        return 0.0
    sorted_values = sorted(latencies_ms)
    index = max(0, min(len(sorted_values) - 1, math.ceil(len(sorted_values) * 0.95) - 1))
    return sorted_values[index]


def run_client(
    client_index: int,
    args: argparse.Namespace,
    latencies_ms: list[float],
    counters: dict[str, int],
    lock: threading.Lock,
) -> None:
    client_id = f"{args.client_id_prefix}-{client_index:04d}"
    token = f"benchmark-token-{client_index:04d}"
    request_type, expected_response = MESSAGE_TYPES[args.message]
    request_payload = build_payload(args.message, args.payload)

    try:
        register_token(args.control_plane, client_id, token)
        with socket.create_connection((args.host, args.port), timeout=5.0) as sock:
            sock.settimeout(5.0)
            authenticate(sock, client_id, token)
            local_success = 0
            local_failed = 0
            local_latencies: list[float] = []

            for request_offset in range(args.requests_per_client):
                request_id = 1000 + client_index * args.requests_per_client + request_offset
                started = time.perf_counter()
                try:
                    sock.sendall(packet(request_type, request_id, request_payload))
                    response = recv_response(sock)
                    if response.version != VERSION:
                        raise RuntimeError(f"unexpected version {response.version}")
                    if response.request_id != request_id:
                        raise RuntimeError(f"unexpected request id {response.request_id}")
                    if response.msg_type != expected_response:
                        if response.msg_type == ERROR_RESP:
                            payload = response.payload.decode("utf-8", errors="replace")
                            raise RuntimeError(f"gateway returned error response: {payload}")
                        raise RuntimeError(f"unexpected response type {response.msg_type}")
                    local_success += 1
                    local_latencies.append((time.perf_counter() - started) * 1000.0)
                except Exception:
                    local_failed += 1

            with lock:
                counters["success"] += local_success
                counters["failed"] += local_failed
                latencies_ms.extend(local_latencies)
    except Exception:
        with lock:
            counters["failed"] += args.requests_per_client


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a lightweight TCP benchmark against the gateway.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--control-plane", default="http://127.0.0.1:8080")
    parser.add_argument("--clients", type=int, default=1)
    parser.add_argument("--requests-per-client", type=int, default=1)
    parser.add_argument("--message", choices=sorted(MESSAGE_TYPES.keys()), default="echo")
    parser.add_argument("--payload", default="benchmark payload")
    parser.add_argument("--client-id-prefix", default="bench-client")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.clients <= 0 or args.requests_per_client <= 0:
        raise SystemExit("clients and requests-per-client must be positive")

    counters = {
        "success": 0,
        "failed": 0,
    }
    latencies_ms: list[float] = []
    lock = threading.Lock()
    threads = []
    started = time.perf_counter()

    for client_index in range(args.clients):
        thread = threading.Thread(
            target=run_client,
            args=(client_index, args, latencies_ms, counters, lock),
            daemon=False,
        )
        thread.start()
        threads.append(thread)

    for thread in threads:
        thread.join()

    elapsed_seconds = time.perf_counter() - started
    total_requests = args.clients * args.requests_per_client
    avg_latency_ms = sum(latencies_ms) / len(latencies_ms) if latencies_ms else 0.0
    requests_per_second = counters["success"] / elapsed_seconds if elapsed_seconds > 0 else 0.0

    print(f"total_requests={total_requests}")
    print(f"success={counters['success']}")
    print(f"failed={counters['failed']}")
    print(f"elapsed_seconds={elapsed_seconds:.3f}")
    print(f"requests_per_second={requests_per_second:.2f}")
    print(f"avg_latency_ms={avg_latency_ms:.2f}")
    print(f"p95_latency_ms={percentile_95(latencies_ms):.2f}")
    return 0 if counters["failed"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
