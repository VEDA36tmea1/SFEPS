#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_PATH="${SCRIPT_DIR}/build/smart_server"
DEFAULT_CA_PATH="/etc/sfeps/pki/ca.crt"

# Use defaults unless caller already exported custom paths.
: "${DB_SSL_CA:=${DEFAULT_CA_PATH}}"
: "${RTSPS_TLS_CA:=${DEFAULT_CA_PATH}}"

required_envs=(
  SFEPS_DB_HOST
  SFEPS_DB_USER
  SFEPS_DB_PASS
  SFEPS_DB_NAME_AUTH
  SFEPS_DB_NAME_ANALYTICS
)

for var_name in "${required_envs[@]}"; do
  if [[ -z "${!var_name:-}" ]]; then
    echo "[run_server] ${var_name} is not set (fail-closed)." >&2
    exit 1
  fi
done

if [[ ! -x "${BIN_PATH}" ]]; then
  echo "[run_server] binary not found or not executable: ${BIN_PATH}" >&2
  echo "[run_server] build first: cmake --build ${SCRIPT_DIR}/build" >&2
  exit 1
fi

if [[ ! -r "${DB_SSL_CA}" ]]; then
  echo "[run_server] DB_SSL_CA is not readable: ${DB_SSL_CA}" >&2
  exit 1
fi

if [[ ! -r "${RTSPS_TLS_CA}" ]]; then
  echo "[run_server] RTSPS_TLS_CA is not readable: ${RTSPS_TLS_CA}" >&2
  exit 1
fi

export DB_SSL_CA
export RTSPS_TLS_CA

echo "[run_server] DB_SSL_CA=${DB_SSL_CA}"
echo "[run_server] RTSPS_TLS_CA=${RTSPS_TLS_CA}"
echo "[run_server] SFEPS_DB_HOST=${SFEPS_DB_HOST}"
echo "[run_server] SFEPS_DB_USER=${SFEPS_DB_USER}"
echo "[run_server] SFEPS_DB_NAME_AUTH=${SFEPS_DB_NAME_AUTH}"
echo "[run_server] SFEPS_DB_NAME_ANALYTICS=${SFEPS_DB_NAME_ANALYTICS}"
echo "[run_server] starting ${BIN_PATH}"

exec "${BIN_PATH}" "$@"
