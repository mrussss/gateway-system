#!/usr/bin/env python3
"""Authenticated TCP benchmark with reproducible JSON and telemetry evidence."""

from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import re
import socket
import struct
import threading
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass


VERSION = 1
FIXED_BODY_SIZE = 10
PING, ECHO, LOG_PUSH, STATS, PONG, ECHO_RESP, ERROR_RESP, LOG_ACK, STATS_RESP, AUTH, AUTH_RESP = range(1, 12)
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


@dataclass
class ClientResult:
    success: int
    failed: int
    request_latencies_ms: list[float]
    auth_latency_ms: float | None
    setup_latency_ms: float | None
    error: str | None


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
    body_length = struct.unpack("!I", recv_exact(sock, 4))[0]
    body = recv_exact(sock, body_length)
    version, msg_type, request_id = struct.unpack("!BBQ", body[:FIXED_BODY_SIZE])
    return Response(version, msg_type, request_id, body[FIXED_BODY_SIZE:])


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1))
    return ordered[index]


def read_process_sample(pid: int | None) -> dict[str, float | int] | None:
    if pid is None:
        return None
    try:
        stat_fields = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="utf-8").split()
        cpu_seconds = (int(stat_fields[13]) + int(stat_fields[14])) / os.sysconf("SC_CLK_TCK")
        lines = pathlib.Path(f"/proc/{pid}/status").read_text(encoding="utf-8").splitlines()
        values = {line.split(":", 1)[0]: line.split()[1] for line in lines if ":" in line and len(line.split()) >= 2}
        return {
            "cpu_seconds": cpu_seconds,
            "rss_kib": int(values.get("VmRSS", "0")),
            "peak_rss_kib": int(values.get("VmHWM", "0")),
        }
    except (FileNotFoundError, IndexError, OSError, ValueError):
        return None


def admin_headers(args: argparse.Namespace) -> dict[str, str]:
    if not args.admin_token:
        return {}
    return {"Authorization": f"Bearer {args.admin_token}"}


def register_token(args: argparse.Namespace, client_id: str) -> str:
    payload = json.dumps({"client_id": client_id}).encode("utf-8")
    request = urllib.request.Request(
        f"{args.control_plane}/tokens",
        data=payload,
        headers={"Content-Type": "application/json", **admin_headers(args)},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=5.0) as response:
            body = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"token create HTTP {error.code}: {error.read().decode('utf-8', errors='replace')}") from error
    token = body.get("token")
    if not isinstance(token, str) or not token:
        raise RuntimeError(f"token create did not return one-time secret: {body}")
    return token


def authenticate(sock: socket.socket, client_id: str, token: str) -> float:
    payload = json.dumps({"client_id": client_id, "token": token}).encode("utf-8")
    started = time.perf_counter()
    sock.sendall(packet(AUTH, 1, payload))
    response = recv_response(sock)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    if response.version != VERSION or response.msg_type != AUTH_RESP or response.request_id != 1:
        raise RuntimeError(f"unexpected AUTH response: {response}")
    body = json.loads(response.payload.decode("utf-8"))
    if body.get("allowed") is not True:
        raise RuntimeError(f"AUTH rejected: {body}")
    return elapsed_ms


def build_payload(args: argparse.Namespace) -> bytes:
    text = args.payload if args.payload is not None else "x" * args.payload_size
    if args.message in ("ping", "stats"):
        return b""
    if args.message == "echo":
        return text.encode("utf-8")
    return json.dumps({"level": "INFO", "service": "benchmark_tcp", "message": text}).encode("utf-8")


def run_client(
    client_index: int,
    args: argparse.Namespace,
    result_slots: list[ClientResult | None],
    ready_condition: threading.Condition,
    ready_count: list[int],
    start_event: threading.Event,
    setup_gate: threading.Semaphore,
) -> None:
    run_id = args.run_id
    client_id = f"{args.client_id_prefix}-{run_id}-{client_index:04d}"
    request_type, expected_response = MESSAGE_TYPES[args.message]
    request_payload = build_payload(args)
    slow_client_count = math.ceil(args.clients * args.slow_client_ratio)
    slow_reader = client_index < slow_client_count
    setup_started = time.perf_counter()

    try:
        setup_gate.acquire()
        try:
            token = register_token(args, client_id)
            sock = socket.create_connection((args.host, args.port), timeout=5.0)
            sock.settimeout(10.0)
            auth_latency_ms = authenticate(sock, client_id, token)
            setup_latency_ms = (time.perf_counter() - setup_started) * 1000.0
        finally:
            setup_gate.release()
        with sock:
            if args.mode == "steady":
                with ready_condition:
                    ready_count[0] += 1
                    ready_condition.notify_all()
                start_event.wait()

            success = 0
            failed = 0
            latencies: list[float] = []
            for offset in range(args.requests_per_client):
                request_id = 1000 + client_index * args.requests_per_client + offset
                started = time.perf_counter()
                try:
                    sock.sendall(packet(request_type, request_id, request_payload))
                    if slow_reader and args.slow_read_delay_ms > 0:
                        time.sleep(args.slow_read_delay_ms / 1000.0)
                    response = recv_response(sock)
                    if response.version != VERSION or response.request_id != request_id:
                        raise RuntimeError("response identity mismatch")
                    if response.msg_type != expected_response:
                        details = response.payload.decode("utf-8", errors="replace")
                        raise RuntimeError(f"response type={response.msg_type} payload={details}")
                    success += 1
                    latencies.append((time.perf_counter() - started) * 1000.0)
                except (OSError, RuntimeError, ValueError, json.JSONDecodeError):
                    failed += 1
            result_slots[client_index] = ClientResult(
                success, failed, latencies, auth_latency_ms, setup_latency_ms, None
            )
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        result_slots[client_index] = ClientResult(
            0, args.requests_per_client, [], None, None, str(error)
        )
        if args.mode == "steady" and not start_event.is_set():
            with ready_condition:
                ready_count[0] += 1
                ready_condition.notify_all()


def fetch_gateway_stats(args: argparse.Namespace) -> dict:
    client_id = f"{args.client_id_prefix}-{args.run_id}-stats-{time.time_ns()}"
    token = register_token(args, client_id)
    with socket.create_connection((args.host, args.port), timeout=5.0) as sock:
        sock.settimeout(5.0)
        authenticate(sock, client_id, token)
        sock.sendall(packet(STATS, 2))
        response = recv_response(sock)
        if response.msg_type != STATS_RESP:
            raise RuntimeError(f"unexpected STATS response: {response}")
        return json.loads(response.payload.decode("utf-8"))


def fetch_redis_latency(args: argparse.Namespace) -> dict[str, float | int | str]:
    request = urllib.request.Request(f"{args.control_plane}/metrics")
    try:
        with urllib.request.urlopen(request, timeout=5.0) as response:
            text = response.read().decode("utf-8")
    except (OSError, urllib.error.URLError) as error:
        return {"error": str(error)}
    sums = [float(value) for value in re.findall(r"^control_plane_redis_operation_duration_seconds_sum(?:\{[^}]*\})? ([0-9.eE+-]+)$", text, re.MULTILINE)]
    counts = [float(value) for value in re.findall(r"^control_plane_redis_operation_duration_seconds_count(?:\{[^}]*\})? ([0-9.eE+-]+)$", text, re.MULTILINE)]
    total_count = int(sum(counts))
    return {
        "observations": total_count,
        "total_seconds": sum(sums),
        "average_ms": (sum(sums) / total_count * 1000.0) if total_count else 0.0,
    }


def redis_latency_delta(before: dict, after: dict) -> dict[str, float | int | str]:
    if "error" in before:
        return {"error": f"before benchmark: {before['error']}"}
    if "error" in after:
        return {"error": f"after benchmark: {after['error']}"}
    count = int(after.get("observations", 0)) - int(before.get("observations", 0))
    total = float(after.get("total_seconds", 0.0)) - float(before.get("total_seconds", 0.0))
    if count < 0 or total < 0:
        return {"error": "Prometheus Redis counters reset during benchmark"}
    return {
        "observations": count,
        "total_seconds": total,
        "average_ms": (total / count * 1000.0) if count else 0.0,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--control-plane", default="http://127.0.0.1:8080")
    parser.add_argument("--admin-token", default=os.environ.get("CONTROL_PLANE_ADMIN_TOKEN", ""))
    parser.add_argument("--clients", type=int, default=1)
    parser.add_argument("--requests-per-client", type=int, default=100)
    parser.add_argument("--message", choices=sorted(MESSAGE_TYPES), default="echo")
    parser.add_argument("--payload", help="literal payload; overrides --payload-size")
    parser.add_argument("--payload-size", type=int, default=128)
    parser.add_argument("--slow-client-ratio", type=float, default=0.0)
    parser.add_argument("--slow-read-delay-ms", type=float, default=25.0)
    parser.add_argument("--setup-concurrency", type=int, default=16)
    parser.add_argument("--client-id-prefix", default="bench-client")
    parser.add_argument("--run-id", default=f"{int(time.time())}-{os.getpid()}")
    parser.add_argument("--mode", choices=("steady", "full"), default="steady")
    parser.add_argument("--gateway-pid", type=int, help="local PID for CPU/RSS sampling")
    parser.add_argument("--build-mode", default="unknown")
    parser.add_argument("--worker-count", type=int, help="record the startup WORKER_COUNT")
    parser.add_argument("--request-queue-capacity", type=int, help="record startup Request Queue capacity")
    parser.add_argument("--response-queue-capacity", type=int, help="record startup Response Queue capacity")
    parser.add_argument("--output", help="write the complete JSON result to this path")
    parser.add_argument(
        "--allow-request-failures",
        action="store_true",
        help="return success after recording request failures (for deliberate overload matrices)",
    )
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    if args.clients <= 0 or args.requests_per_client <= 0 or args.payload_size < 0 or args.setup_concurrency <= 0:
        raise SystemExit("clients/requests must be positive and payload-size non-negative")
    if not 0.0 <= args.slow_client_ratio <= 1.0 or args.slow_read_delay_ms < 0:
        raise SystemExit("slow-client-ratio must be 0..1 and delay must be non-negative")
    if len(f"{args.client_id_prefix}-{args.run_id}-{args.clients:04d}") > 128:
        raise SystemExit("generated client_id exceeds the API limit")


def main() -> int:
    args = parse_args()
    validate_args(args)
    before_stats: dict = {}
    try:
        before_stats = fetch_gateway_stats(args)
    except Exception as error:  # noqa: BLE001 - reported as benchmark evidence
        before_stats = {"error": str(error)}
    redis_before = fetch_redis_latency(args)

    results: list[ClientResult | None] = [None] * args.clients
    ready_condition = threading.Condition()
    ready_count = [0]
    start_event = threading.Event()
    setup_gate = threading.Semaphore(args.setup_concurrency)
    threads: list[threading.Thread] = []
    benchmark_started = time.perf_counter() if args.mode == "full" else 0.0
    process_before = read_process_sample(args.gateway_pid) if args.mode == "full" else None

    for index in range(args.clients):
        thread = threading.Thread(
            target=run_client,
            args=(index, args, results, ready_condition, ready_count, start_event, setup_gate),
        )
        thread.start()
        threads.append(thread)

    if args.mode == "steady":
        with ready_condition:
            ready_condition.wait_for(lambda: ready_count[0] == args.clients, timeout=60.0)
        if ready_count[0] != args.clients:
            raise RuntimeError("timed out preparing steady-state clients")
        process_before = read_process_sample(args.gateway_pid)
        benchmark_started = time.perf_counter()
        start_event.set()

    for thread in threads:
        thread.join()
    elapsed_seconds = time.perf_counter() - benchmark_started
    process_after = read_process_sample(args.gateway_pid)

    completed = [value for value in results if value is not None]
    latencies = [latency for value in completed for latency in value.request_latencies_ms]
    auth_latencies = [value.auth_latency_ms for value in completed if value.auth_latency_ms is not None]
    setup_latencies = [value.setup_latency_ms for value in completed if value.setup_latency_ms is not None]
    success = sum(value.success for value in completed)
    failed = sum(value.failed for value in completed) + (args.clients - len(completed)) * args.requests_per_client

    after_stats: dict = {}
    try:
        after_stats = fetch_gateway_stats(args)
    except Exception as error:  # noqa: BLE001 - reported as benchmark evidence
        after_stats = {"error": str(error)}

    process = {"before": process_before, "after": process_after}
    if process_before and process_after:
        cpu_delta = max(0.0, float(process_after["cpu_seconds"]) - float(process_before["cpu_seconds"]))
        process["cpu_seconds_delta"] = cpu_delta
        process["cpu_percent"] = cpu_delta / elapsed_seconds * 100.0 if elapsed_seconds else 0.0
        process["rss_kib_delta"] = int(process_after["rss_kib"]) - int(process_before["rss_kib"])

    result = {
        "schema_version": 1,
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "parameters": {
            "mode": args.mode,
            "build_mode": args.build_mode,
            "host": args.host,
            "port": args.port,
            "message": args.message,
            "payload_bytes": len(build_payload(args)),
            "clients": args.clients,
            "requests_per_client": args.requests_per_client,
            "slow_client_ratio": args.slow_client_ratio,
            "slow_read_delay_ms": args.slow_read_delay_ms,
            "setup_concurrency": args.setup_concurrency,
            "worker_count": args.worker_count,
            "request_queue_capacity": args.request_queue_capacity,
            "response_queue_capacity": args.response_queue_capacity,
        },
        "requests": {
            "attempted": args.clients * args.requests_per_client,
            "success": success,
            "failed": failed,
            "elapsed_seconds": elapsed_seconds,
            "qps": success / elapsed_seconds if elapsed_seconds else 0.0,
            "latency_ms": {
                "average": sum(latencies) / len(latencies) if latencies else 0.0,
                "p50": percentile(latencies, 0.50),
                "p95": percentile(latencies, 0.95),
                "p99": percentile(latencies, 0.99),
                "max": max(latencies, default=0.0),
            },
        },
        "auth_latency_ms": {
            "observations": len(auth_latencies),
            "average": sum(auth_latencies) / len(auth_latencies) if auth_latencies else 0.0,
            "p50": percentile(auth_latencies, 0.50),
            "p95": percentile(auth_latencies, 0.95),
            "p99": percentile(auth_latencies, 0.99),
        },
        "setup_latency_ms": {
            "observations": len(setup_latencies),
            "average": sum(setup_latencies) / len(setup_latencies) if setup_latencies else 0.0,
        },
        "process": process,
        "gateway_stats_before": before_stats,
        "gateway_stats_after": after_stats,
        "redis_operation_latency": redis_latency_delta(redis_before, fetch_redis_latency(args)),
        "clients": [asdict(value) for value in completed],
    }

    rendered = json.dumps(result, indent=2, sort_keys=True)
    if args.output:
        output = pathlib.Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(rendered + "\n", encoding="utf-8")
        print(f"result_file={output}")
    print(f"success={success} failed={failed} qps={result['requests']['qps']:.2f}")
    print(
        "latency_ms="
        f"p50:{result['requests']['latency_ms']['p50']:.2f} "
        f"p95:{result['requests']['latency_ms']['p95']:.2f} "
        f"p99:{result['requests']['latency_ms']['p99']:.2f}"
    )
    print(
        "auth_latency_ms="
        f"p50:{result['auth_latency_ms']['p50']:.2f} "
        f"p95:{result['auth_latency_ms']['p95']:.2f} "
        f"p99:{result['auth_latency_ms']['p99']:.2f}"
    )
    return 0 if failed == 0 or args.allow_request_failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
