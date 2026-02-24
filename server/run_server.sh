#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_PATH="${SCRIPT_DIR}/build/smart_server"
DEFAULT_CA_PATH="/etc/sfeps/pki/ca.crt"

# Use defaults unless caller already exported custom paths.
: "${DB_SSL_CA:=${DEFAULT_CA_PATH}}"
: "${RTSPS_TLS_CA:=${DEFAULT_CA_PATH}}"

# Optional security tuning (safe defaults).
: "${SFEPS_META_MAX_PACKET_BYTES:=65536}"
: "${SFEPS_META_BAD_STREAK_LIMIT:=20}"
: "${SFEPS_META_MAX_LINES_PER_BATCH:=128}"
: "${SFEPS_ANALYTICS_QUEUE_MAX:=200}"
: "${SFEPS_DROP_LOG_INTERVAL:=100}"
: "${SFEPS_AUTH_MAX_BYTES:=256}"
: "${SFEPS_AUDIO_MAX_BYTES:=4194304}"
: "${SFEPS_ALERT_MAX_CLIENTS:=64}"
: "${SFEPS_SOCKET_READ_TIMEOUT_MS:=5000}"

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
export SFEPS_META_MAX_PACKET_BYTES
export SFEPS_META_BAD_STREAK_LIMIT
export SFEPS_META_MAX_LINES_PER_BATCH
export SFEPS_ANALYTICS_QUEUE_MAX
export SFEPS_DROP_LOG_INTERVAL
export SFEPS_AUTH_MAX_BYTES
export SFEPS_AUDIO_MAX_BYTES
export SFEPS_ALERT_MAX_CLIENTS
export SFEPS_SOCKET_READ_TIMEOUT_MS

echo "[run_server] DB_SSL_CA=${DB_SSL_CA}"
echo "[run_server] RTSPS_TLS_CA=${RTSPS_TLS_CA}"
echo "[run_server] SFEPS_DB_HOST=${SFEPS_DB_HOST}"
echo "[run_server] SFEPS_DB_USER=${SFEPS_DB_USER}"
echo "[run_server] SFEPS_DB_NAME_AUTH=${SFEPS_DB_NAME_AUTH}"
echo "[run_server] SFEPS_DB_NAME_ANALYTICS=${SFEPS_DB_NAME_ANALYTICS}"
echo "[run_server] SFEPS_META_MAX_PACKET_BYTES=${SFEPS_META_MAX_PACKET_BYTES}"
echo "[run_server] SFEPS_META_BAD_STREAK_LIMIT=${SFEPS_META_BAD_STREAK_LIMIT}"
echo "[run_server] SFEPS_META_MAX_LINES_PER_BATCH=${SFEPS_META_MAX_LINES_PER_BATCH}"
echo "[run_server] SFEPS_ANALYTICS_QUEUE_MAX=${SFEPS_ANALYTICS_QUEUE_MAX}"
echo "[run_server] SFEPS_DROP_LOG_INTERVAL=${SFEPS_DROP_LOG_INTERVAL}"
echo "[run_server] SFEPS_AUTH_MAX_BYTES=${SFEPS_AUTH_MAX_BYTES}"
echo "[run_server] SFEPS_AUDIO_MAX_BYTES=${SFEPS_AUDIO_MAX_BYTES}"
echo "[run_server] SFEPS_ALERT_MAX_CLIENTS=${SFEPS_ALERT_MAX_CLIENTS}"
echo "[run_server] SFEPS_SOCKET_READ_TIMEOUT_MS=${SFEPS_SOCKET_READ_TIMEOUT_MS}"
if [[ -n "${SFEPS_AUTH_ALLOW_IPS:-}" ]]; then
  echo "[run_server] SFEPS_AUTH_ALLOW_IPS=${SFEPS_AUTH_ALLOW_IPS}"
fi
if [[ -n "${SFEPS_AUDIO_ALLOW_IPS:-}" ]]; then
  echo "[run_server] SFEPS_AUDIO_ALLOW_IPS=${SFEPS_AUDIO_ALLOW_IPS}"
fi
if [[ -n "${SFEPS_ALERT_ALLOW_IPS:-}" ]]; then
  echo "[run_server] SFEPS_ALERT_ALLOW_IPS=${SFEPS_ALERT_ALLOW_IPS}"
fi
echo "[run_server] starting ${BIN_PATH}"

exec "${BIN_PATH}" "$@"
