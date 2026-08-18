#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
output="${1:-$ROOT_DIR/results/environment/$timestamp.txt}"
mkdir -p "$(dirname "$output")"

command_version() {
  local name="$1"
  shift
  if command -v "$name" >/dev/null 2>&1; then
    "$@" 2>&1 | head -5 || true
  else
    echo "$name: unavailable"
  fi
}

{
  echo "captured_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_head=$(git -C "$ROOT_DIR" rev-parse HEAD)"
  echo "working_tree=$(git -C "$ROOT_DIR" status --short --untracked-files=no | wc -l | tr -d ' ') tracked changes"
  echo
  echo "[kernel]"
  uname -a
  echo
  echo "[cpu]"
  command_version lscpu lscpu
  echo
  echo "[memory]"
  command_version free free -h
  echo
  echo "[toolchains]"
  command_version g++ g++ --version
  command_version cmake cmake --version
  command_version go go version
  command_version python3 python3 --version
  echo
  echo "[containers_and_cluster]"
  command_version docker docker version
  command_version kubectl kubectl version --client
  command_version kind kind version
} >"$output"

echo "$output"
