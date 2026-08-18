#!/usr/bin/env python3
"""Static contract checks for the checked-in Kubernetes manifests."""

from __future__ import annotations

import pathlib
import sys

try:
    import yaml
except ImportError as error:  # pragma: no cover - environment diagnostic
    raise SystemExit("PyYAML is required: python3 -m pip install PyYAML") from error


ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST_DIR = ROOT / "deploy" / "kubernetes"


def documents() -> list[dict]:
    result: list[dict] = []
    for path in sorted(MANIFEST_DIR.glob("*.yaml")):
        if path.name == "secret.example.yaml":
            continue
        for document in yaml.safe_load_all(path.read_text(encoding="utf-8")):
            if document:
                result.append(document)
    return result


def find(resources: list[dict], kind: str, name: str) -> dict:
    for resource in resources:
        if resource.get("kind") == kind and resource.get("metadata", {}).get("name") == name:
            return resource
    raise AssertionError(f"missing {kind}/{name}")


def container(workload: dict, name: str) -> dict:
    containers = workload["spec"]["template"]["spec"]["containers"]
    return next(value for value in containers if value["name"] == name)


def assert_hardened(workload: dict, container_name: str) -> None:
    pod_spec = workload["spec"]["template"]["spec"]
    pod_security = pod_spec["securityContext"]
    assert pod_security["runAsNonRoot"] is True
    assert pod_security["seccompProfile"]["type"] == "RuntimeDefault"
    value = container(workload, container_name)
    security = value["securityContext"]
    assert security["allowPrivilegeEscalation"] is False
    assert security["readOnlyRootFilesystem"] is True
    assert security["capabilities"]["drop"] == ["ALL"]
    assert value["resources"]["requests"]
    assert value["resources"]["limits"]


def main() -> int:
    resources = documents()
    for resource in resources:
        if resource["kind"] != "Namespace":
            assert resource["metadata"].get("namespace") == "gateway-system", resource

    gateway = find(resources, "Deployment", "gateway")
    gateway_spec = gateway["spec"]
    assert gateway_spec["replicas"] == 2
    assert gateway_spec["strategy"]["rollingUpdate"] == {
        "maxUnavailable": 0,
        "maxSurge": 1,
    }
    pod_spec = gateway_spec["template"]["spec"]
    assert pod_spec["terminationGracePeriodSeconds"] == 30
    gateway_container = container(gateway, "gateway")
    assert gateway_container["startupProbe"]["tcpSocket"]["port"] == "tcp"
    assert gateway_container["readinessProbe"]["tcpSocket"]["port"] == "tcp"
    assert gateway_container["livenessProbe"]["exec"]
    pre_stop = " ".join(gateway_container["lifecycle"]["preStop"]["exec"]["command"])
    assert "kill -TERM 1" in pre_stop and "sleep 3" in pre_stop
    assert_hardened(gateway, "gateway")

    config = find(resources, "ConfigMap", "gateway-system-config")["data"]
    shutdown_seconds = int(config["SHUTDOWN_TIMEOUT_MS"]) / 1000
    assert 30 > 3 + shutdown_seconds

    control_plane = find(resources, "Deployment", "control-plane")
    control_container = container(control_plane, "control-plane")
    assert control_container["startupProbe"]["httpGet"]["path"] == "/health/live"
    assert control_container["livenessProbe"]["httpGet"]["path"] == "/health/live"
    assert control_container["readinessProbe"]["httpGet"]["path"] == "/health/ready"
    assert_hardened(control_plane, "control-plane")

    redis = find(resources, "StatefulSet", "redis")
    assert redis["spec"]["replicas"] == 1
    assert redis["spec"]["volumeClaimTemplates"][0]["spec"]["resources"]["requests"]["storage"]
    assert_hardened(redis, "redis")

    for name in ("gateway", "control-plane"):
        pdb = find(resources, "PodDisruptionBudget", name)
        assert pdb["spec"]["minAvailable"] == 1

    print(f"[k8s-manifest] PASS {len(resources)} resources")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, KeyError, StopIteration) as error:
        print(f"[k8s-manifest] FAIL: {error}", file=sys.stderr)
        raise SystemExit(1) from error
