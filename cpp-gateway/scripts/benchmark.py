#!/usr/bin/env python3
"""
Benchmark script for the C++ epoll MessageServer.

Sends requests over long-lived TCP connections, measures latency and throughput,
and outputs a JSON result file.

Usage:
    python3 scripts/benchmark.py --clients 100 --requests 100 --type echo
    python3 scripts/benchmark.py --clients 1 --requests 1000 --type echo --output results/echo_1x1000.json
"""

import argparse
import json
import os
import socket
import struct
import sys
import threading
import time

# ── Protocol constants ──────────────────────────────────────────────
FIXED_BODY_SIZE = 1 + 1 + 8          # version + type + request_id

MESSAGE_TYPE_MAP = {
    "ping": 1,
    "echo": 2,
    "log_push": 3,
    "stats": 4,
}

EXPECTED_RESP_TYPE = {
    1: 5,   # PING  → PONG
    2: 6,   # ECHO  → ECHO_RESP
    3: 8,   # LOG_PUSH → LOG_ACK
    4: 9,   # STATS → STATS_RESP
}

MESSAGE_TYPE_NAMES = {
    1: "PING",
    2: "ECHO",
    3: "LOG_PUSH",
    4: "STATS",
    5: "PONG",
    6: "ECHO_RESP",
    7: "ERROR_RESP",
    8: "LOG_ACK",
    9: "STATS_RESP",
}


# ── Protocol helpers ────────────────────────────────────────────────

def make_packet(msg_type: int, req_id: int, payload: bytes) -> bytes:
    """Build a binary request packet."""
    body_length = FIXED_BODY_SIZE + len(payload)
    header = struct.pack("!I", body_length)             # 4 bytes
    meta = struct.pack("!BBQ", 1, msg_type, req_id)     # version(1) + type(1) + id(8)
    return header + meta + payload


def recv_exact(sock: socket.socket, n: int, timeout: float):
    """Read exactly *n* bytes from *sock* (or return None on timeout/disconnect)."""
    sock.settimeout(timeout)
    data = b""
    while len(data) < n:
        try:
            chunk = sock.recv(n - len(data))
        except socket.timeout:
            return None
        except OSError:
            return None
        if not chunk:
            return None
        data += chunk
    return data


def recv_response(sock: socket.socket, timeout: float):
    """Read one response packet.

    Returns (dict | None, error_str | None).
    """
    header = recv_exact(sock, 4, timeout)
    if header is None:
        return None, "disconnect_or_timeout"

    body_length = struct.unpack("!I", header)[0]
    body = recv_exact(sock, body_length, timeout)
    if body is None:
        return None, "incomplete_body"

    version, rtype, req_id = struct.unpack("!BBQ", body[:10])
    payload = body[10:].decode("utf-8", errors="replace")
    return {"version": version, "type": rtype, "request_id": req_id, "payload": payload}, None


# ── Per-thread worker ───────────────────────────────────────────────

def _worker(host, port, msg_type, payload_bytes, num_requests, worker_id, out_list):
    """Run *num_requests* on one TCP connection and append results to *out_list*."""
    latencies = []
    successes = 0
    failures = 0
    timeouts = 0
    mismatches = 0
    expected_resp = EXPECTED_RESP_TYPE.get(msg_type, 7)

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.connect((host, port))
    except OSError as e:
        out_list.append({"error": str(e), "worker": worker_id})
        return

    for i in range(num_requests):
        req_id = worker_id * 1_000_000 + i
        packet = make_packet(msg_type, req_id, payload_bytes)

        try:
            start = time.monotonic()
            sock.sendall(packet)
            resp, err = recv_response(sock, timeout=5.0)
            elapsed_ms = (time.monotonic() - start) * 1000
        except OSError:
            failures += 1
            continue

        if err:
            if err == "disconnect_or_timeout":
                timeouts += 1
            else:
                failures += 1
            continue

        if resp["request_id"] != req_id or resp["type"] != expected_resp:
            mismatches += 1
            failures += 1
        else:
            successes += 1
            latencies.append(elapsed_ms)

    try:
        sock.close()
    except OSError:
        pass

    out_list.append({
        "successes": successes,
        "failures": failures,
        "timeouts": timeouts,
        "mismatches": mismatches,
        "latencies": latencies,
    })


# ── CLI ─────────────────────────────────────────────────────────────

def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Benchmark for C++ epoll MessageServer")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--clients", type=int, default=100, help="number of concurrent TCP connections")
    p.add_argument("--requests", type=int, default=100, help="requests per connection")
    p.add_argument("--type", default="echo", choices=["ping", "echo", "log_push", "stats"],
                   help="message type to send")
    p.add_argument("--payload-size", type=int, default=128, help="payload size in bytes")
    p.add_argument("--output", default=None, help="path to output JSON file (under results/)")
    p.add_argument("--connect-timeout", type=float, default=5.0)
    return p.parse_args(argv)


# ── JSON helpers ────────────────────────────────────────────────────

def _percentile(sorted_data, pct):
    if not sorted_data:
        return 0.0
    idx = max(0, int(len(sorted_data) * pct / 100) - 1)
    return sorted_data[idx]


def _output_path(clients, requests, msg_type_name):
    os.makedirs("results", exist_ok=True)
    return f"results/{msg_type_name.lower()}_{clients}x{requests}.json"


# ── Main ────────────────────────────────────────────────────────────

def main():
    args = parse_args()
    msg_type = MESSAGE_TYPE_MAP[args.type]
    msg_type_name = args.type.upper()

    # Build payload
    if args.type == "log_push":
        body = "x" * max(0, args.payload_size - 50)
        payload_str = f'{{"level":"INFO","service":"benchmark","message":"{body}"}}'
    else:
        payload_str = "x" * args.payload_size
    payload_bytes = payload_str.encode("utf-8")

    total_requests = args.clients * args.requests
    print(f"Benchmark: {args.clients} clients × {args.requests} requests = {total_requests} total")
    print(f"  Type:  {msg_type_name}  (msg_type={msg_type})")
    print(f"  Host:  {args.host}:{args.port}")
    print(f"  Payload size: {len(payload_bytes)} bytes")
    print()

    # Launch workers
    threads = []
    results = []
    start_ts = time.monotonic()

    for wid in range(args.clients):
        t = threading.Thread(
            target=_worker,
            args=(args.host, args.port, msg_type, payload_bytes, args.requests, wid, results),
        )
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    elapsed = time.monotonic() - start_ts

    # Aggregate
    total_success = sum(r.get("successes", 0) for r in results)
    total_failures = sum(r.get("failures", 0) for r in results)
    total_timeouts = sum(r.get("timeouts", 0) for r in results)
    total_mismatches = sum(r.get("mismatches", 0) for r in results)
    all_latencies = sorted([ms for r in results for ms in r.get("latencies", [])])

    avg_lat = sum(all_latencies) / len(all_latencies) if all_latencies else 0.0
    p50 = _percentile(all_latencies, 50)
    p95 = _percentile(all_latencies, 95)
    p99 = _percentile(all_latencies, 99)
    max_lat = all_latencies[-1] if all_latencies else 0.0
    qps = total_success / elapsed if elapsed > 0 else 0.0

    # Summary
    print(f"  Elapsed:     {elapsed:.2f} s")
    print(f"  Success:     {total_success}")
    print(f"  Failures:    {total_failures}  (timeout={total_timeouts}, mismatch={total_mismatches})")
    print(f"  QPS:         {qps:.1f}")
    print(f"  Avg latency: {avg_lat:.2f} ms")
    print(f"  P50 latency: {p50:.2f} ms")
    print(f"  P95 latency: {p95:.2f} ms")
    print(f"  P99 latency: {p99:.2f} ms")
    print(f"  Max latency: {max_lat:.2f} ms")

    # Build result dict
    result = {
        "host": args.host,
        "port": args.port,
        "message_type": msg_type_name,
        "concurrent_clients": args.clients,
        "requests_per_client": args.requests,
        "total_requests": total_requests,
        "successful_responses": total_success,
        "failed_responses": total_failures,
        "timeout_responses": total_timeouts,
        "mismatched_request_id": total_mismatches,
        "total_time_seconds": round(elapsed, 3),
        "qps": round(qps, 1),
        "avg_latency_ms": round(avg_lat, 2),
        "p50_latency_ms": round(p50, 2),
        "p95_latency_ms": round(p95, 2),
        "p99_latency_ms": round(p99, 2),
        "max_latency_ms": round(max_lat, 2),
    }

    # Write output
    output = args.output or _output_path(args.clients, args.requests, args.type)
    with open(output, "w") as f:
        json.dump(result, f, indent=2)
    print(f"\nResult saved to {output}")


if __name__ == "__main__":
    main()
