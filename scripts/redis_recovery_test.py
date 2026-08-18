#!/usr/bin/env python3
"""Verify Redis outage/recovery semantics against the Docker Compose stack."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import time
import urllib.error
import urllib.request

from tcp_protocol_test import (
    AUTH,
    AUTH_RESP,
    ECHO,
    ECHO_RESP,
    admin_headers,
    assert_response,
    connect,
    default_config,
    packet,
    recv_response,
    register_token,
    update_config,
)


ROOT = pathlib.Path(__file__).resolve().parents[1]
CONTROL_PLANE_URL = os.environ.get("CONTROL_PLANE_URL", "http://127.0.0.1:8080")
GATEWAY_TOKEN = os.environ.get("GATEWAY_SHARED_TOKEN", "local-gateway-change-me")


def request_json(path: str, *, admin: bool = False) -> tuple[int, dict]:
    headers = admin_headers() if admin else {}
    request = urllib.request.Request(f"{CONTROL_PLANE_URL}{path}", headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=4.0) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read().decode("utf-8"))


def wait_http(path: str, expected_status: int, timeout: float = 30.0) -> dict:
    deadline = time.monotonic() + timeout
    last_status = 0
    while time.monotonic() < deadline:
        try:
            last_status, body = request_json(path)
            if last_status == expected_status:
                return body
        except (OSError, urllib.error.URLError, json.JSONDecodeError):
            pass
        time.sleep(0.5)
    raise AssertionError(f"{path} did not reach HTTP {expected_status}; last={last_status}")


def wait_gateway_config(version: int, timeout: float = 25.0) -> None:
    deadline = time.monotonic() + timeout
    last_version = -1
    while time.monotonic() < deadline:
        try:
            status, body = request_json("/gateways/gateway-001/status", admin=True)
            last_version = int(body.get("runtime_config_version", -1))
            if status == 200 and last_version >= version:
                return
        except (OSError, urllib.error.URLError, ValueError):
            pass
        time.sleep(1.0)
    raise AssertionError(
        f"gateway did not report config version {version}; last={last_version}"
    )


def authenticate_existing(sock, client_id: str, token: str, request_id: int) -> dict:
    payload = json.dumps({"client_id": client_id, "token": token}).encode("utf-8")
    sock.sendall(packet(AUTH, request_id, payload))
    response = recv_response(sock)
    assert_response(response, AUTH_RESP, request_id)
    return json.loads(response.payload.decode("utf-8"))


def compose(*arguments: str) -> None:
    subprocess.run(["docker", "compose", *arguments], cwd=ROOT, check=True)


def main() -> int:
    wait_http("/health/ready", 200)
    client_id = f"redis-recovery-{os.getpid()}"
    token = register_token(CONTROL_PLANE_URL, client_id)

    update_config(CONTROL_PLANE_URL, default_config())
    _, active_config = request_json("/config", admin=True)
    expected_version = int(active_config["version"])
    wait_gateway_config(expected_version)

    established = connect("127.0.0.1", 9000)
    if authenticate_existing(established, client_id, token, 7001).get("code") != "OK":
        raise AssertionError("initial AUTH failed")

    redis_paused = False
    try:
        compose("pause", "redis")
        redis_paused = True
        wait_http("/health/ready", 503)
        wait_http("/health/live", 200)

        payload = b"survives redis outage"
        established.sendall(packet(ECHO, 7002, payload))
        assert_response(recv_response(established), ECHO_RESP, 7002, payload)

        with connect("127.0.0.1", 9000) as unauthenticated:
            result = authenticate_existing(unauthenticated, client_id, token, 7003)
            if result.get("allowed") is not False or result.get("code") != "AUTH_UNAVAILABLE":
                raise AssertionError(f"new AUTH did not fail closed: {result}")
    finally:
        if redis_paused:
            compose("unpause", "redis")

    wait_http("/health/ready", 200)
    with connect("127.0.0.1", 9000) as recovered:
        result = authenticate_existing(recovered, client_id, token, 7004)
        if result.get("code") != "OK":
            raise AssertionError(f"AUTH did not recover: {result}")

    _, recovered_config = request_json("/config", admin=True)
    if int(recovered_config["version"]) != expected_version:
        raise AssertionError(
            f"active config changed across Redis outage: {recovered_config}"
        )
    wait_gateway_config(expected_version)
    established.close()
    print("[redis-recovery] PASS live/ready, fail-closed AUTH, established ECHO, recovery, config retention")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
