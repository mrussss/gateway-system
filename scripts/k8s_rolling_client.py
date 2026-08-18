#!/usr/bin/env python3
"""Continuously ECHO through a reconnecting Service port-forward during rollout."""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
import time

from tcp_protocol_test import AUTH, AUTH_RESP, ECHO, ECHO_RESP, assert_response, connect, packet, recv_response, register_token


def authenticate_existing(sock: socket.socket, client_id: str, token: str, request_id: int) -> None:
    payload = json.dumps({"client_id": client_id, "token": token}).encode("utf-8")
    sock.sendall(packet(AUTH, request_id, payload))
    response = recv_response(sock)
    assert_response(response, AUTH_RESP, request_id)
    body = json.loads(response.payload.decode("utf-8"))
    if body.get("allowed") is not True:
        raise AssertionError(f"AUTH failed: {body}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=19000)
    parser.add_argument("--control-plane-url", default="http://127.0.0.1:18080")
    parser.add_argument("--duration", type=float, default=45.0)
    parser.add_argument("--ready-file", required=True)
    parser.add_argument("--result-file", required=True)
    args = parser.parse_args()

    client_id = f"k8s-rolling-{int(time.time())}"
    token = register_token(args.control_plane_url, client_id)
    deadline = time.monotonic() + args.duration
    request_id = 8000
    successes = 0
    failures = 0
    reconnects = 0
    outage_started: float | None = None
    max_outage = 0.0
    sock: socket.socket | None = None

    while time.monotonic() < deadline:
        try:
            if sock is None:
                sock = connect(args.host, args.port)
                authenticate_existing(sock, client_id, token, request_id)
                request_id += 1
                reconnects += 1
                pathlib.Path(args.ready_file).touch()
                if outage_started is not None:
                    max_outage = max(max_outage, time.monotonic() - outage_started)
                    outage_started = None

            payload = f"rolling-{request_id}".encode("utf-8")
            sock.sendall(packet(ECHO, request_id, payload))
            assert_response(recv_response(sock), ECHO_RESP, request_id, payload)
            successes += 1
            request_id += 1
            time.sleep(0.05)
        except (AssertionError, OSError, RuntimeError, TimeoutError):
            failures += 1
            if outage_started is None:
                outage_started = time.monotonic()
            if sock is not None:
                sock.close()
                sock = None
            time.sleep(0.2)

    if sock is not None:
        sock.close()
    if outage_started is not None:
        max_outage = max(max_outage, time.monotonic() - outage_started)

    result = {
        "successes": successes,
        "failures": failures,
        "connections": reconnects,
        "max_outage_seconds": round(max_outage, 3),
    }
    pathlib.Path(args.result_file).write_text(json.dumps(result), encoding="utf-8")
    if successes < 20 or reconnects < 2 or max_outage > 10.0:
        raise AssertionError(f"rolling client acceptance failed: {result}")
    print(f"[k8s-rolling-client] PASS {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
