#!/usr/bin/env bash
# Usage:
#   ./run_server.sh
set -euo pipefail

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd)"
PRIMARY_BIN_PATH="${SCRIPT_DIR}/build/smart_server.bin"
LEGACY_BIN_PATH="${SCRIPT_DIR}/build/smart_server"

log_info() {
  if [[ "${SFEPS_RUN_VERBOSE:-0}" == "1" ]]; then
    echo "[run_server] $*"
  fi
}

# Auto-load env file(s) from server directory.
# Priority:
# 1) SFEPS_ENV_FILE (if explicitly provided)
# 2) .env.local (optional)
# 3) .env (optional)
load_env_file() {
  local env_file="$1"
  if [[ ! -r "${env_file}" ]]; then
    return 1
  fi
  set -a
  # shellcheck disable=SC1090
  source "${env_file}"
  set +a
  log_info "loaded env file: ${env_file}"
  return 0
}

resolve_bin_path() {
  if [[ -x "${PRIMARY_BIN_PATH}" ]]; then
    echo "${PRIMARY_BIN_PATH}"
    return 0
  fi

  # Backward compatibility: pre-migration builds produced build/smart_server.
  if [[ -x "${LEGACY_BIN_PATH}" ]]; then
    local legacy_real
    legacy_real="$(readlink -f "${LEGACY_BIN_PATH}" || true)"
    if [[ -n "${legacy_real}" && "${legacy_real}" != "${SCRIPT_PATH}" ]]; then
      echo "${LEGACY_BIN_PATH}"
      return 0
    fi
  fi

  return 1
}

if [[ -n "${SFEPS_ENV_FILE:-}" ]]; then
  if ! load_env_file "${SFEPS_ENV_FILE}"; then
    echo "[run_server] SFEPS_ENV_FILE is not readable: ${SFEPS_ENV_FILE}" >&2
    exit 1
  fi
else
  load_env_file "${SCRIPT_DIR}/.env.local" || true
  load_env_file "${SCRIPT_DIR}/.env" || true
fi

# Use defaults unless caller already exported custom paths.
: "${SFEPS_DB_HOST:=localhost}"
# Compatibility only: runtime uses SFEPS_DB_NAME_ANALYTICS single schema.
: "${SFEPS_DB_NAME_AUTH:=${SFEPS_DB_NAME_ANALYTICS:-}}"

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
: "${SFEPS_POSITION_MIN_SEND_MS:=500}"
: "${SFEPS_AUTH_DEAUTH_GRACE_MS:=3000}"
: "${SFEPS_RTSP_URL:=rtsp://127.0.0.1:8554/cam1}"
: "${SFEPS_FRAUD_IMAGE_HTTP_BASE_URL:=http://127.0.0.1:8080/fraud-images}"
: "${SFEPS_VIDEO_RETENTION_SEC:=86400}"
: "${SFEPS_VIDEO_MAX_STORAGE_BYTES:=5368709120}"
: "${SFEPS_VIDEO_STORAGE_RESUME_BYTES:=3221225472}"
: "${SFEPS_PENDING_IMAGE_RETENTION_SEC:=30}"
: "${SFEPS_FRAUD_IMAGE_RETENTION_SEC:=86400}"

# App port TLS (dual-stack migration defaults).
: "${SFEPS_APP_TLS_ENABLE:=0}"
: "${SFEPS_APP_PLAINTEXT_ENABLE:=1}"
: "${SFEPS_AUTH_TLS_PORT:=6555}"
: "${SFEPS_AUDIO_TLS_PORT:=6556}"
: "${SFEPS_ALERT_TLS_PORT:=6557}"
: "${SFEPS_APP_TLS_HANDSHAKE_TIMEOUT_MS:=3000}"
: "${SFEPS_APP_BIND_IP:=0.0.0.0}"

required_envs=(
  SFEPS_DB_USER
  SFEPS_DB_PASS
  SFEPS_DB_NAME_ANALYTICS
)

for var_name in "${required_envs[@]}"; do
  if [[ -z "${!var_name:-}" ]]; then
    echo "[run_server] ${var_name} is not set (fail-closed)." >&2
    exit 1
  fi
done

required_allowlist_envs=(
  SFEPS_AUTH_ALLOW_IPS
  SFEPS_AUDIO_ALLOW_IPS
  SFEPS_ALERT_ALLOW_IPS
)

for var_name in "${required_allowlist_envs[@]}"; do
  if [[ -z "${!var_name:-}" ]]; then
    echo "[run_server] ${var_name} is not set (allowlist fail-closed)." >&2
    exit 1
  fi
done

if [[ "${SFEPS_DB_HOST}" != "localhost" ]]; then
  echo "[run_server] SFEPS_DB_HOST must be localhost (local-only mode)." >&2
  exit 1
fi

if [[ "${SFEPS_APP_TLS_ENABLE}" != "0" && "${SFEPS_APP_TLS_ENABLE}" != "1" ]]; then
  echo "[run_server] SFEPS_APP_TLS_ENABLE must be 0 or 1." >&2
  exit 1
fi

if [[ "${SFEPS_APP_PLAINTEXT_ENABLE}" != "0" && "${SFEPS_APP_PLAINTEXT_ENABLE}" != "1" ]]; then
  echo "[run_server] SFEPS_APP_PLAINTEXT_ENABLE must be 0 or 1." >&2
  exit 1
fi

if [[ "${SFEPS_APP_TLS_ENABLE}" == "0" && "${SFEPS_APP_PLAINTEXT_ENABLE}" == "0" ]]; then
  echo "[run_server] both SFEPS_APP_TLS_ENABLE and SFEPS_APP_PLAINTEXT_ENABLE cannot be 0." >&2
  exit 1
fi

if [[ "${SFEPS_APP_TLS_ENABLE}" == "1" ]]; then
  required_tls_envs=(
    SFEPS_APP_TLS_CERT_FILE
    SFEPS_APP_TLS_KEY_FILE
  )
  for var_name in "${required_tls_envs[@]}"; do
    if [[ -z "${!var_name:-}" ]]; then
      echo "[run_server] ${var_name} is not set (TLS fail-closed)." >&2
      exit 1
    fi
  done

  if [[ ! -r "${SFEPS_APP_TLS_CERT_FILE}" ]]; then
    echo "[run_server] SFEPS_APP_TLS_CERT_FILE is not readable: ${SFEPS_APP_TLS_CERT_FILE}" >&2
    exit 1
  fi
  if [[ ! -r "${SFEPS_APP_TLS_KEY_FILE}" ]]; then
    echo "[run_server] SFEPS_APP_TLS_KEY_FILE is not readable: ${SFEPS_APP_TLS_KEY_FILE}" >&2
    exit 1
  fi
fi

BIN_PATH="$(resolve_bin_path || true)"
if [[ -z "${BIN_PATH}" ]]; then
  echo "[run_server] binary not found or not executable: ${PRIMARY_BIN_PATH}" >&2
  echo "[run_server] fallback checked: ${LEGACY_BIN_PATH}" >&2
  echo "[run_server] build first: cmake -S ${SCRIPT_DIR} -B ${SCRIPT_DIR}/build && cmake --build ${SCRIPT_DIR}/build" >&2
  exit 1
fi

export SFEPS_META_MAX_PACKET_BYTES
export SFEPS_META_BAD_STREAK_LIMIT
export SFEPS_META_MAX_LINES_PER_BATCH
export SFEPS_ANALYTICS_QUEUE_MAX
export SFEPS_DROP_LOG_INTERVAL
export SFEPS_AUTH_MAX_BYTES
export SFEPS_AUDIO_MAX_BYTES
export SFEPS_ALERT_MAX_CLIENTS
export SFEPS_SOCKET_READ_TIMEOUT_MS
export SFEPS_POSITION_MIN_SEND_MS
export SFEPS_AUTH_DEAUTH_GRACE_MS
export SFEPS_RTSP_URL
export SFEPS_FRAUD_IMAGE_HTTP_BASE_URL
export SFEPS_VIDEO_RETENTION_SEC
export SFEPS_VIDEO_MAX_STORAGE_BYTES
export SFEPS_VIDEO_STORAGE_RESUME_BYTES
export SFEPS_PENDING_IMAGE_RETENTION_SEC
export SFEPS_FRAUD_IMAGE_RETENTION_SEC
export SFEPS_APP_TLS_ENABLE
export SFEPS_APP_PLAINTEXT_ENABLE
export SFEPS_AUTH_TLS_PORT
export SFEPS_AUDIO_TLS_PORT
export SFEPS_ALERT_TLS_PORT
export SFEPS_APP_TLS_HANDSHAKE_TIMEOUT_MS
export SFEPS_APP_BIND_IP
if [[ -n "${SFEPS_APP_TLS_CERT_FILE:-}" ]]; then
  export SFEPS_APP_TLS_CERT_FILE
fi
if [[ -n "${SFEPS_APP_TLS_KEY_FILE:-}" ]]; then
  export SFEPS_APP_TLS_KEY_FILE
fi

log_info "SFEPS_DB_HOST=${SFEPS_DB_HOST}"
log_info "SFEPS_DB_USER=${SFEPS_DB_USER}"
log_info "SFEPS_DB_NAME_ANALYTICS=${SFEPS_DB_NAME_ANALYTICS}"
log_info "SFEPS_DB_NAME_AUTH=${SFEPS_DB_NAME_AUTH} (compat only)"
log_info "SFEPS_META_MAX_PACKET_BYTES=${SFEPS_META_MAX_PACKET_BYTES}"
log_info "SFEPS_META_BAD_STREAK_LIMIT=${SFEPS_META_BAD_STREAK_LIMIT}"
log_info "SFEPS_META_MAX_LINES_PER_BATCH=${SFEPS_META_MAX_LINES_PER_BATCH}"
log_info "SFEPS_ANALYTICS_QUEUE_MAX=${SFEPS_ANALYTICS_QUEUE_MAX}"
log_info "SFEPS_DROP_LOG_INTERVAL=${SFEPS_DROP_LOG_INTERVAL}"
log_info "SFEPS_AUTH_MAX_BYTES=${SFEPS_AUTH_MAX_BYTES}"
log_info "SFEPS_AUDIO_MAX_BYTES=${SFEPS_AUDIO_MAX_BYTES}"
log_info "SFEPS_ALERT_MAX_CLIENTS=${SFEPS_ALERT_MAX_CLIENTS}"
log_info "SFEPS_SOCKET_READ_TIMEOUT_MS=${SFEPS_SOCKET_READ_TIMEOUT_MS}"
log_info "SFEPS_AUTH_DEAUTH_GRACE_MS=${SFEPS_AUTH_DEAUTH_GRACE_MS}"
log_info "SFEPS_RTSP_URL=${SFEPS_RTSP_URL}"
log_info "SFEPS_FRAUD_IMAGE_HTTP_BASE_URL=${SFEPS_FRAUD_IMAGE_HTTP_BASE_URL}"
log_info "SFEPS_VIDEO_RETENTION_SEC=${SFEPS_VIDEO_RETENTION_SEC}"
log_info "SFEPS_VIDEO_MAX_STORAGE_BYTES=${SFEPS_VIDEO_MAX_STORAGE_BYTES}"
log_info "SFEPS_VIDEO_STORAGE_RESUME_BYTES=${SFEPS_VIDEO_STORAGE_RESUME_BYTES}"
log_info "SFEPS_PENDING_IMAGE_RETENTION_SEC=${SFEPS_PENDING_IMAGE_RETENTION_SEC}"
log_info "SFEPS_FRAUD_IMAGE_RETENTION_SEC=${SFEPS_FRAUD_IMAGE_RETENTION_SEC}"
log_info "SFEPS_APP_TLS_ENABLE=${SFEPS_APP_TLS_ENABLE}"
log_info "SFEPS_APP_PLAINTEXT_ENABLE=${SFEPS_APP_PLAINTEXT_ENABLE}"
log_info "SFEPS_AUTH_TLS_PORT=${SFEPS_AUTH_TLS_PORT}"
log_info "SFEPS_AUDIO_TLS_PORT=${SFEPS_AUDIO_TLS_PORT}"
log_info "SFEPS_ALERT_TLS_PORT=${SFEPS_ALERT_TLS_PORT}"
log_info "SFEPS_APP_TLS_HANDSHAKE_TIMEOUT_MS=${SFEPS_APP_TLS_HANDSHAKE_TIMEOUT_MS}"
log_info "SFEPS_APP_BIND_IP=${SFEPS_APP_BIND_IP}"
if [[ -n "${SFEPS_APP_TLS_CERT_FILE:-}" ]]; then
  log_info "SFEPS_APP_TLS_CERT_FILE=${SFEPS_APP_TLS_CERT_FILE}"
fi
if [[ "${SFEPS_APP_TLS_ENABLE}" == "1" ]]; then
  log_info "SFEPS_APP_TLS_KEY_FILE=[set]"
fi
if [[ -n "${SFEPS_AUTH_ALLOW_IPS:-}" ]]; then
  log_info "SFEPS_AUTH_ALLOW_IPS=${SFEPS_AUTH_ALLOW_IPS}"
fi
if [[ -n "${SFEPS_AUDIO_ALLOW_IPS:-}" ]]; then
  log_info "SFEPS_AUDIO_ALLOW_IPS=${SFEPS_AUDIO_ALLOW_IPS}"
fi
if [[ -n "${SFEPS_ALERT_ALLOW_IPS:-}" ]]; then
  log_info "SFEPS_ALERT_ALLOW_IPS=${SFEPS_ALERT_ALLOW_IPS}"
fi
log_info "starting ${BIN_PATH}"

exec "${BIN_PATH}" "$@"
