#!/usr/bin/env bash
set -euo pipefail

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd)"

BIN_PATH="${SFEPS_MTX_BIN:-${SCRIPT_DIR}/bin/mediamtx}"
DEFAULT_CONFIG_PATH="${SFEPS_MTX_CONFIG:-${SCRIPT_DIR}/mediamtx.yml}"
if [[ -z "${SFEPS_MTX_BIN:-}" && ! -x "${BIN_PATH}" && -x "/usr/local/bin/mediamtx" ]]; then
  BIN_PATH="/usr/local/bin/mediamtx"
fi
if [[ -z "${SFEPS_MTX_CONFIG:-}" && ! -r "${DEFAULT_CONFIG_PATH}" && -r "/etc/mediamtx/mediamtx.yml" ]]; then
  DEFAULT_CONFIG_PATH="/etc/mediamtx/mediamtx.yml"
fi
CONFIG_PATH="${DEFAULT_CONFIG_PATH}"
DEBUG_SOURCE="${SFEPS_MTX_DEBUG_SOURCE:-0}"
DEBUG_SOURCE_ONLY=0
PROBE_PATHS_8554_ONLY=0

usage() {
  cat <<'EOF'
Usage:
  ./run_mediamtx.sh [config-path] [--debug-source]
  ./run_mediamtx.sh --debug-source [config-path]
  ./run_mediamtx.sh --debug-source-only [config-path]
  ./run_mediamtx.sh --probe-paths-8554 [config-path]

Options:
  --debug-source       Print source URL debug probes before start.
  --debug-source-only  Run source debug probes and exit.
  --probe-paths-8554   Probe common RTSP path candidates on source host:8554 and exit.
  -h, --help       Show this help.

Env:
  SFEPS_MTX_DEBUG_SOURCE=1 enables source debug by default.
EOF
}

parse_source_values() {
  local cfg="$1"
  awk '
    /^[[:space:]]*#/ { next }
    /^[[:space:]]*source:[[:space:]]*/ {
      line=$0
      sub(/^[[:space:]]*source:[[:space:]]*/, "", line)
      sub(/[[:space:]]+#.*/, "", line)
      if (line ~ /^".*"$/ || line ~ /^\047.*\047$/) {
        line = substr(line, 2, length(line) - 2)
      }
      if (length(line) > 0) print line
    }
  ' "${cfg}"
}

debug_source_urls() {
  local cfg="$1"
  if ! command -v python3 >/dev/null 2>&1; then
    echo "[run_mediamtx][debug] python3 not found; skip source probing." >&2
    return 0
  fi

  mapfile -t source_values < <(parse_source_values "${cfg}")
  if [[ "${#source_values[@]}" -eq 0 ]]; then
    echo "[run_mediamtx][debug] no source entries found in ${cfg}" >&2
    return 0
  fi

  local idx=0
  local src
  for src in "${source_values[@]}"; do
    idx=$((idx + 1))
    python3 - "${idx}" "${src}" <<'PY'
import base64
import socket
import sys
from urllib.parse import urlsplit

idx = sys.argv[1]
url = sys.argv[2]

parts = urlsplit(url)
scheme = (parts.scheme or "rtsp").lower()
host = parts.hostname
port = parts.port
if port is None:
    if scheme in ("rtsp", "rtsps"):
        port = 8554
path = parts.path or "/"
if parts.query:
    path += "?" + parts.query

user = parts.username or ""
has_pw = parts.password is not None
masked_auth = ""
if user:
    masked_auth = f"{user}:{'***' if has_pw else ''}@"

masked_url = f"{scheme}://{masked_auth}{host or '?'}:{port if port else '?'}{path}"
print(f"[run_mediamtx][debug] source[{idx}] {masked_url}")

if not host or not port:
    print(f"[run_mediamtx][debug] source[{idx}] parse_error (host/port)")
    sys.exit(0)

# TCP reachability check
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(2.0)
try:
    sock.connect((host, int(port)))
    print(f"[run_mediamtx][debug] source[{idx}] tcp_connect ok ({host}:{port})")
except Exception as exc:
    print(f"[run_mediamtx][debug] source[{idx}] tcp_connect fail ({host}:{port}): {exc}")
    sys.exit(0)
finally:
    try:
        sock.close()
    except Exception:
        pass

if scheme not in ("rtsp", "rtsps"):
    print(f"[run_mediamtx][debug] source[{idx}] skip rtsp probe (scheme={scheme})")
    sys.exit(0)

auth_header = ""
if user:
    raw = f"{user}:{parts.password or ''}".encode()
    auth_header = "Authorization: Basic " + base64.b64encode(raw).decode() + "\r\n"

def probe(method):
    req = (
        f"{method} {scheme}://{host}:{port}{path} RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "User-Agent: run_mediamtx_debug\r\n"
    )
    if method == "DESCRIBE":
        req += "Accept: application/sdp\r\n"
    if auth_header:
        req += auth_header
    req += "\r\n"

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.5)
    try:
        s.connect((host, int(port)))
        s.sendall(req.encode())
        data = s.recv(4096).decode("latin1", "replace")
        first = data.splitlines()[0] if data.splitlines() else "NO_RESPONSE"
        return first
    except Exception as exc:
        return f"ERR {type(exc).__name__}: {exc}"
    finally:
        try:
            s.close()
        except Exception:
            pass

print(f"[run_mediamtx][debug] source[{idx}] OPTIONS  -> {probe('OPTIONS')}")
print(f"[run_mediamtx][debug] source[{idx}] DESCRIBE -> {probe('DESCRIBE')}")
PY
  done
}

probe_paths_on_8554() {
  local cfg="$1"
  if ! command -v python3 >/dev/null 2>&1; then
    echo "[run_mediamtx][probe] python3 not found; skip path probing." >&2
    return 0
  fi

  mapfile -t source_values < <(parse_source_values "${cfg}")
  if [[ "${#source_values[@]}" -eq 0 ]]; then
    echo "[run_mediamtx][probe] no source entries found in ${cfg}" >&2
    return 0
  fi

  local idx=0
  local src
  for src in "${source_values[@]}"; do
    idx=$((idx + 1))
    python3 - "${idx}" "${src}" <<'PY'
import base64
import socket
import sys
from urllib.parse import urlsplit

idx = sys.argv[1]
url = sys.argv[2]
parts = urlsplit(url)
host = parts.hostname
user = parts.username or ""
password = parts.password or ""
port = 8554

if not host:
    print(f"[run_mediamtx][probe] source[{idx}] parse_error (host)")
    sys.exit(0)

print(f"[run_mediamtx][probe] source[{idx}] target={host}:{port} user={(user if user else '(none)')}")

def rtsp_call(path: str, method: str = "DESCRIBE") -> str:
    auth_header = ""
    if user:
        token = base64.b64encode(f"{user}:{password}".encode()).decode()
        auth_header = f"Authorization: Basic {token}\r\n"

    req = (
        f"{method} rtsp://{host}:{port}{path} RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "User-Agent: run_mediamtx_probe\r\n"
    )
    if method == "DESCRIBE":
        req += "Accept: application/sdp\r\n"
    if auth_header:
        req += auth_header
    req += "\r\n"

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.5)
    try:
        s.connect((host, port))
        s.sendall(req.encode())
        data = s.recv(4096).decode("latin1", "replace")
        return data.splitlines()[0] if data.splitlines() else "NO_RESPONSE"
    except Exception as exc:
        return f"ERR {type(exc).__name__}: {exc}"
    finally:
        try:
            s.close()
        except Exception:
            pass

print(f"[run_mediamtx][probe] source[{idx}] OPTIONS -> {rtsp_call('/', 'OPTIONS') if host else 'parse_error'}")

candidates = [
    "/cam1", "/cam2", "/live", "/live.sdp", "/stream", "/stream1", "/stream2",
    "/main", "/sub", "/video", "/h264", "/h265", "/test",
    "/profile1/media.smp", "/profile2/media.smp", "/profile3/media.smp",
    "/Streaming/Channels/101", "/Streaming/Channels/102",
    "/ISAPI/Streaming/channels/101", "/ISAPI/Streaming/channels/102",
    "/ch0_0.264", "/ch1/main/av_stream", "/h264/ch1/main/av_stream",
    "/axis-media/media.amp", "/onvif1", "/onvif2",
]

hits = []
for path in candidates:
    status = rtsp_call(path, "DESCRIBE")
    if "404" not in status:
        hits.append((path, status))

if hits:
    print(f"[run_mediamtx][probe] source[{idx}] non-404 candidates:")
    for path, status in hits:
        print(f"[run_mediamtx][probe]   {path:<36} -> {status}")
else:
    print(f"[run_mediamtx][probe] source[{idx}] no non-404 candidate found on :8554")
PY
  done
}

POSITIONAL_CONFIG=""
for arg in "$@"; do
  case "${arg}" in
    --debug-source)
      DEBUG_SOURCE=1
      ;;
    --debug-source-only)
      DEBUG_SOURCE=1
      DEBUG_SOURCE_ONLY=1
      ;;
    --probe-paths-8554)
      PROBE_PATHS_8554_ONLY=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --*)
      echo "[run_mediamtx] unknown option: ${arg}" >&2
      usage >&2
      exit 1
      ;;
    *)
      if [[ -z "${POSITIONAL_CONFIG}" ]]; then
        POSITIONAL_CONFIG="${arg}"
      else
        echo "[run_mediamtx] too many positional arguments." >&2
        usage >&2
        exit 1
      fi
      ;;
  esac
done

if [[ -n "${POSITIONAL_CONFIG}" ]]; then
  CONFIG_PATH="${POSITIONAL_CONFIG}"
fi

if [[ ! -x "${BIN_PATH}" ]]; then
  echo "[run_mediamtx] binary not found or not executable: ${BIN_PATH}" >&2
  exit 1
fi

if [[ ! -r "${CONFIG_PATH}" ]]; then
  echo "[run_mediamtx] config not readable: ${CONFIG_PATH}" >&2
  usage >&2
  exit 1
fi

if [[ "${DEBUG_SOURCE}" == "1" ]]; then
  echo "[run_mediamtx][debug] source probe enabled"
  debug_source_urls "${CONFIG_PATH}"
  if [[ "${DEBUG_SOURCE_ONLY}" == "1" ]]; then
    echo "[run_mediamtx][debug] source probe done (debug-only mode)."
    exit 0
  fi
fi

if [[ "${PROBE_PATHS_8554_ONLY}" == "1" ]]; then
  echo "[run_mediamtx][probe] scanning common path candidates on :8554"
  probe_paths_on_8554 "${CONFIG_PATH}"
  echo "[run_mediamtx][probe] scan done."
  exit 0
fi

if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet mediamtx; then
  echo "[run_mediamtx] mediamtx.service is active." >&2
  echo "[run_mediamtx] stop service first to avoid port conflicts:" >&2
  echo "[run_mediamtx]   sudo systemctl stop mediamtx" >&2
  exit 1
fi

CURRENT_USER="$(id -un)"
if [[ "${CURRENT_USER}" != "mediamtx" ]]; then
  if id mediamtx >/dev/null 2>&1 && command -v sudo >/dev/null 2>&1 && \
      sudo -n -u mediamtx true 2>/dev/null; then
    echo "[run_mediamtx] switching user: ${CURRENT_USER} -> mediamtx"
    exec sudo -u mediamtx "${BIN_PATH}" "${CONFIG_PATH}"
  fi
  echo "[run_mediamtx] warning: running as ${CURRENT_USER} (not mediamtx)." >&2
  # echo "[run_mediamtx] if TLS key permission error occurs, run:" >&2
  # echo "[run_mediamtx]   sudo -u mediamtx $0 ${CONFIG_PATH}" >&2
fi

echo "[run_mediamtx] starting ${BIN_PATH} ${CONFIG_PATH}"
exec "${BIN_PATH}" "${CONFIG_PATH}"
