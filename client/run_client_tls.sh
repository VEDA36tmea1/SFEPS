#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CA_PATH="${SCRIPT_DIR}/certs/auth_ca.pem"

if [[ ! -r "${CA_PATH}" ]]; then
  echo "[ERROR] CA file not found or not readable: ${CA_PATH}" >&2
  exit 1
fi

# Auth channel defaults (TLS-first)
export AUTH_TLS_ENABLE="${AUTH_TLS_ENABLE:-1}"
export AUTH_TLS_PORT="${AUTH_TLS_PORT:-6555}"
export AUTH_PLAINTEXT_PORT="${AUTH_PLAINTEXT_PORT:-5555}"
export AUTH_ALLOW_PLAINTEXT_FALLBACK="${AUTH_ALLOW_PLAINTEXT_FALLBACK:-0}"
export AUTH_TLS_CA_FILE="${AUTH_TLS_CA_FILE:-${CA_PATH}}"

echo "[INFO] AUTH_TLS_CA_FILE=${AUTH_TLS_CA_FILE}"
echo "[INFO] AUTH_TLS_ENABLE=${AUTH_TLS_ENABLE}, AUTH_TLS_PORT=${AUTH_TLS_PORT}, AUTH_PLAINTEXT_PORT=${AUTH_PLAINTEXT_PORT}, AUTH_ALLOW_PLAINTEXT_FALLBACK=${AUTH_ALLOW_PLAINTEXT_FALLBACK}"

if [[ $# -eq 0 ]]; then
  echo "[USAGE] $0 <qt-app-binary> [args...]" >&2
  echo "[EXAMPLE] $0 ./build/appHanwhaVisionSFEPS" >&2
  exit 2
fi

exec "$@"
