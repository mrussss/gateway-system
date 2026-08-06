#!/usr/bin/env python3
"""Black-box checks for eventfd wakeups and bounded graceful shutdown."""

from __future__ import annotations

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


FIXED_BODY_SIZE = 10
AUTH = 10
AUTH_RESP = 11
ECHO = 2
ECHO_RESP = 6
STATS = 4
STATS_RESP = 9


def packet(message_type: int, request_id: int, payload: bytes = b"") -> bytes:
    return struct.pack("!IBBQ", FIXED_BODY_SIZE + len(payload), 1, message_type, request_id) + payload


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise RuntimeError(f"connection closed with {remaining} bytes still expected")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_packet(sock: socket.socket) -> tuple[int, int, bytes]:
    body_length = struct.unpack("!I", recv_exact(sock, 4))[0]
    body = recv_exact(sock, body_length)
    _, message_type, request_id = struct.unpack("!BBQ", body[:FIXED_BODY_SIZE])
    return message_type, request_id, body[FIXED_BODY_SIZE:]


def unused_port() -> int:
    with closing(socket.socket()) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class ControlPlaneHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format: str, *_args: object) -> None:
        return

    def _reply(self, value: object) -> None:
        body = json.dumps(value).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.server.path_requests[self.path] = self.server.path_requests.get(self.path, 0) + 1
        if self.path == "/config":
            self._reply(
                {
                    "version": 1,
                    "max_payload_size": 4 * 1024 * 1024 + FIXED_BODY_SIZE,
                    "max_connections_per_client": 10,
                    "max_requests_per_client_per_second": 100_000,
                    "slow_client_output_limit": 8 * 1024 * 1024,
                    "log_level": "INFO",
                }
            )
            return
        self._reply({"success": True})

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.server.path_requests[self.path] = self.server.path_requests.get(self.path, 0) + 1
        content_length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(content_length)
        if self.path == "/auth/check":
            request = json.loads(body)
            self.server.auth_requests += 1
            time.sleep(getattr(self.server, "auth_delay", 0.0))
            self._reply(
                {
                    "allowed": request.get("token") == "test-token",
                    "reason": "ok",
                }
            )
            return
        self._reply({"success": True})


class ReusableThreadingHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True


class FakeControlPlane:
    def __init__(self, port: int = 0) -> None:
        self.bind_port = port

    def __enter__(self) -> "FakeControlPlane":
        self.server = ReusableThreadingHTTPServer(("127.0.0.1", self.bind_port), ControlPlaneHandler)
        self.server.auth_delay = 0.0
        self.server.auth_requests = 0
        self.server.path_requests = {}
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *_args: object) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    @property
    def port(self) -> int:
        return int(self.server.server_address[1])

    def set_auth_delay(self, delay: float) -> None:
        self.server.auth_delay = delay

    def reset_auth_requests(self) -> None:
        self.server.auth_requests = 0

    def auth_requests(self) -> int:
        return int(self.server.auth_requests)

    def request_count(self, path: str) -> int:
        return int(self.server.path_requests.get(path, 0))


class GatewayProcess:
    def __init__(
        self,
        executable: Path,
        control_port: int,
        shutdown_ms: int,
        request_capacity: int = 10000,
        response_capacity: int = 10000,
        auth_capacity: int = 32,
        auth_workers: int = 2,
    ) -> None:
        self.port = unused_port()
        self.readiness_file = Path("/tmp/gateway-ready")
        self.readiness_file.unlink(missing_ok=True)
        control_plane_timeout_ms = min(1000, (shutdown_ms - 100) // 2)
        if control_plane_timeout_ms < 100:
            raise ValueError("shutdown_ms cannot satisfy the startup timeout contract")
        environment = os.environ.copy()
        environment.update(
            {
                "GATEWAY_PORT": str(self.port),
                "CONTROL_PLANE_HOST": "127.0.0.1",
                "CONTROL_PLANE_PORT": str(control_port),
                "CONTROL_PLANE_TIMEOUT_MS": str(control_plane_timeout_ms),
                "REQUEST_QUEUE_CAPACITY": str(request_capacity),
                "RESPONSE_QUEUE_CAPACITY": str(response_capacity),
                "SHUTDOWN_TIMEOUT_MS": str(shutdown_ms),
                "WORKER_COUNT": "1",
                "AUTH_QUEUE_CAPACITY": str(auth_capacity),
                "AUTH_WORKER_COUNT": str(auth_workers),
                "GATEWAY_LOG_PATH": "/tmp/gateway-system-graceful-test.log",
            }
        )
        self.process = subprocess.Popen(
            [str(executable)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self._wait_until_listening()
        if not self.readiness_file.is_file():
            self._raise_failure("gateway did not create readiness file")

    def _wait_until_listening(self) -> None:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self._raise_failure("gateway exited during startup")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.05):
                    return
            except OSError:
                time.sleep(0.01)
        self._raise_failure("gateway did not start listening")

    def connect_authenticated(self, client_id: str) -> socket.socket:
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        auth = json.dumps({"client_id": client_id, "token": "test-token"}).encode()
        sock.sendall(packet(AUTH, 1, auth))
        message_type, request_id, _ = recv_packet(sock)
        if message_type != AUTH_RESP or request_id != 1:
            raise AssertionError("authentication response mismatch")
        return sock

    def signal(self) -> None:
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)

    def wait(self, timeout: float) -> tuple[str, str]:
        try:
            output = self.process.communicate(timeout=timeout)
            if self.readiness_file.exists():
                raise AssertionError("gateway did not remove readiness file")
            return output
        except subprocess.TimeoutExpired:
            self.process.kill()
            self._raise_failure(f"gateway did not exit within {timeout}s")

    def _raise_failure(self, reason: str) -> None:
        if self.process.poll() is None:
            self.process.kill()
        stdout, stderr = self.process.communicate()
        raise AssertionError(f"{reason}\nstdout:\n{stdout}\nstderr:\n{stderr}")


def test_idle_and_repeated_stop(executable: Path, control_port: int) -> None:
    gateway = GatewayProcess(executable, control_port, shutdown_ms=1000)
    gateway.signal()
    time.sleep(0.01)
    gateway.signal()
    gateway.wait(timeout=3)
    if gateway.process.returncode != 0:
        raise AssertionError("idle shutdown returned non-zero")


def test_queued_requests_are_drained(
    executable: Path, control_plane: FakeControlPlane
) -> None:
    # Slow synchronous auth creates a deterministic AUTH-queue backlog while
    # the Reactor remains free to accept and decode all connections.
    control_plane.set_auth_delay(0.01)
    gateway = GatewayProcess(
        executable,
        control_plane.port,
        shutdown_ms=5000,
        auth_capacity=256,
        auth_workers=1,
    )
    connection_count = 120
    clients = [
        socket.create_connection(("127.0.0.1", gateway.port), timeout=3)
        for _ in range(connection_count)
    ]
    for index, sock in enumerate(clients):
        auth = json.dumps(
            {"client_id": f"queued-client-{index}", "token": "test-token"}
        ).encode()
        sock.sendall(packet(AUTH, index + 1, auth))
        sock.settimeout(7)

    # The Reactor can enqueue these tiny packets well before the single delayed
    # Auth Worker drains them; most tasks are still queued when SIGTERM arrives.
    time.sleep(0.25)
    gateway.signal()
    for index, sock in enumerate(clients):
        message_type, request_id, _ = recv_packet(sock)
        if message_type != AUTH_RESP or request_id != index + 1:
            raise AssertionError(f"queued AUTH response mismatch at index {index}")
        sock.close()
    gateway.wait(timeout=7)
    control_plane.set_auth_delay(0.0)
    if gateway.process.returncode != 0:
        raise AssertionError("queued-request shutdown returned non-zero")


def test_slow_client_is_bounded_by_deadline(executable: Path, control_port: int) -> None:
    gateway = GatewayProcess(executable, control_port, shutdown_ms=350)
    slow = gateway.connect_authenticated("slow-client")
    slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
    # Stay below the local fail-safe config while the asynchronous config puller
    # initializes; this is still far larger than the test socket receive window.
    large_payload = b"x" * 900_000
    slow.sendall(packet(ECHO, 2, large_payload) + packet(ECHO, 3, large_payload))

    # A second connection's later STATS response proves the single worker has
    # already produced both large responses for the slow connection.
    observer = gateway.connect_authenticated("observer")
    observer.settimeout(5)
    observer.sendall(packet(STATS, 2))
    message_type, _, _ = recv_packet(observer)
    if message_type != STATS_RESP:
        raise AssertionError("observer did not receive STATS response")
    observer.close()

    started = time.monotonic()
    gateway.signal()
    _, stderr = gateway.wait(timeout=3)
    elapsed = time.monotonic() - started
    slow.close()
    if elapsed > 2.0:
        raise AssertionError(f"deadline shutdown took too long: {elapsed:.3f}s")
    if "deadline reached" not in stderr:
        raise AssertionError("slow-client scenario did not exercise forced deadline")


def test_request_queue_overload(executable: Path, control_plane: FakeControlPlane) -> None:
    control_plane.set_auth_delay(0.0)
    gateway = GatewayProcess(
        executable,
        control_plane.port,
        shutdown_ms=3000,
        request_capacity=1,
        response_capacity=32,
        auth_capacity=1,
        auth_workers=1,
    )
    ordinary = gateway.connect_authenticated("ordinary-client")
    ordinary.settimeout(1)
    control_plane.set_auth_delay(0.15)
    clients = [
        socket.create_connection(("127.0.0.1", gateway.port), timeout=3)
        for _ in range(12)
    ]
    for index, sock in enumerate(clients):
        payload = json.dumps(
            {"client_id": f"overload-client-{index}", "token": "test-token"}
        ).encode()
        sock.sendall(packet(AUTH, index + 1, payload))
        sock.settimeout(5)

    started = time.monotonic()
    ordinary.sendall(packet(ECHO, 2, b"bulkhead-ok"))
    message_type, request_id, payload = recv_packet(ordinary)
    elapsed = time.monotonic() - started
    if (message_type, request_id, payload) != (ECHO_RESP, 2, b"bulkhead-ok"):
        raise AssertionError("ordinary request failed while AUTH executor was saturated")
    if elapsed > 0.2:
        raise AssertionError(f"ordinary request was delayed by AUTH saturation: {elapsed:.3f}s")
    ordinary.close()

    overloaded = 0
    allowed = 0
    for sock in clients:
        message_type, _, payload = recv_packet(sock)
        if message_type != AUTH_RESP:
            raise AssertionError("overload path returned wrong response type")
        body = json.loads(payload)
        if body.get("code") == "AUTH_OVERLOADED":
            overloaded += 1
        elif body.get("allowed") is True:
            allowed += 1
        else:
            raise AssertionError(f"unexpected overload response: {body}")
        sock.close()
    gateway.signal()
    gateway.wait(timeout=5)
    control_plane.set_auth_delay(0.0)
    if overloaded == 0 or allowed == 0:
        raise AssertionError(
            f"expected both admitted and rejected requests, got allowed={allowed} overloaded={overloaded}"
        )


def test_control_plane_outage(executable: Path) -> None:
    with FakeControlPlane() as control_plane:
        gateway = GatewayProcess(executable, control_plane.port, shutdown_ms=2000)
        existing = gateway.connect_authenticated("existing-client")
        control_port = control_plane.port

    # The fake control plane is now down. Existing authenticated data-plane
    # traffic remains independent of it.
    existing.settimeout(3)
    existing.sendall(packet(ECHO, 2, b"still-available"))
    message_type, request_id, payload = recv_packet(existing)
    if (message_type, request_id, payload) != (ECHO_RESP, 2, b"still-available"):
        raise AssertionError("existing authenticated connection failed during control-plane outage")

    newcomer = socket.create_connection(("127.0.0.1", gateway.port), timeout=3)
    newcomer.settimeout(3)
    auth = json.dumps({"client_id": "new-client", "token": "test-token"}).encode()
    newcomer.sendall(packet(AUTH, 1, auth))
    message_type, _, payload = recv_packet(newcomer)
    if message_type != AUTH_RESP or json.loads(payload).get("allowed") is not False:
        raise AssertionError("new AUTH did not fail closed while control plane was unavailable")
    newcomer.close()

    with FakeControlPlane(control_port) as recovered:
        recovered_client = socket.create_connection(("127.0.0.1", gateway.port), timeout=3)
        recovered_client.settimeout(3)
        recovered_client.sendall(packet(AUTH, 1, auth))
        message_type, _, payload = recv_packet(recovered_client)
        if message_type != AUTH_RESP or json.loads(payload).get("allowed") is not True:
            raise AssertionError("new AUTH did not recover after the control plane returned")
        recovered_client.close()

        recovery_deadline = time.monotonic() + 6
        while time.monotonic() < recovery_deadline:
            if recovered.request_count("/config") > 0 and recovered.request_count("/metrics/report") > 0 and recovered.request_count("/clients/report") > 0:
                break
            time.sleep(0.05)
        else:
            raise AssertionError("background config/report loops did not recover")

    # A reporting/config failure must not terminate the Gateway process.
    existing.sendall(packet(ECHO, 3, b"still-running"))
    message_type, request_id, payload = recv_packet(existing)
    if (message_type, request_id, payload) != (ECHO_RESP, 3, b"still-running"):
        raise AssertionError("gateway exited after control-plane reporting failure")
    existing.close()
    gateway.signal()
    gateway.wait(timeout=4)
    if gateway.process.returncode != 0:
        raise AssertionError(f"control-plane outage gateway failed; port={control_port}")


def test_cancelled_queued_auth(executable: Path, control_plane: FakeControlPlane) -> None:
    control_plane.set_auth_delay(0.3)
    control_plane.reset_auth_requests()
    gateway = GatewayProcess(
        executable,
        control_plane.port,
        shutdown_ms=2000,
        auth_capacity=4,
        auth_workers=1,
    )

    active = socket.create_connection(("127.0.0.1", gateway.port), timeout=3)
    active.sendall(
        packet(
            AUTH,
            1,
            json.dumps({"client_id": "active-auth", "token": "test-token"}).encode(),
        )
    )
    deadline = time.monotonic() + 1
    while control_plane.auth_requests() == 0 and time.monotonic() < deadline:
        time.sleep(0.005)
    if control_plane.auth_requests() != 1:
        raise AssertionError("first AUTH did not enter the control plane")

    cancelled = socket.create_connection(("127.0.0.1", gateway.port), timeout=3)
    cancelled.sendall(
        packet(
            AUTH,
            2,
            json.dumps({"client_id": "cancelled-auth", "token": "test-token"}).encode(),
        )
    )
    cancelled.close()
    active.settimeout(2)
    recv_packet(active)
    active.close()
    time.sleep(0.1)
    if control_plane.auth_requests() != 1:
        raise AssertionError("cancelled queued AUTH still called the control plane")

    gateway.signal()
    gateway.wait(timeout=4)
    control_plane.set_auth_delay(0.0)


def test_deep_queue_deadline(executable: Path, control_plane: FakeControlPlane) -> None:
    control_plane.set_auth_delay(0.5)
    gateway = GatewayProcess(
        executable,
        control_plane.port,
        shutdown_ms=350,
        auth_capacity=64,
        auth_workers=1,
    )
    clients = [
        socket.create_connection(("127.0.0.1", gateway.port), timeout=3)
        for _ in range(30)
    ]
    for index, sock in enumerate(clients):
        payload = json.dumps(
            {"client_id": f"deadline-client-{index}", "token": "test-token"}
        ).encode()
        sock.sendall(packet(AUTH, index + 1, payload))
    time.sleep(0.15)

    started = time.monotonic()
    gateway.signal()
    _, stderr = gateway.wait(timeout=2.5)
    elapsed = time.monotonic() - started
    for sock in clients:
        sock.close()
    control_plane.set_auth_delay(0.0)
    if elapsed > 1.5:
        raise AssertionError(f"deep-queue deadline took too long: {elapsed:.3f}s")
    if "discarded_auth_tasks=" not in stderr:
        raise AssertionError("deep-queue scenario did not abort pending AUTH tasks at deadline")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/message_server", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1]).resolve()
    with FakeControlPlane() as control_plane:
        test_idle_and_repeated_stop(executable, control_plane.port)
        test_queued_requests_are_drained(executable, control_plane)
        test_request_queue_overload(executable, control_plane)
        test_cancelled_queued_auth(executable, control_plane)
        test_deep_queue_deadline(executable, control_plane)
        test_slow_client_is_bounded_by_deadline(executable, control_plane.port)
    test_control_plane_outage(executable)
    print("graceful shutdown scenarios passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
