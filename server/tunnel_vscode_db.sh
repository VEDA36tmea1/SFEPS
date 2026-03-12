#!/usr/bin/env bash
# Usage:
#   ./tunnel_vscode_db.sh
#   ./tunnel_vscode_db.sh 192.168.0.80
#
# Create SSH tunnel for local VSCode DB extension access to Raspberry Pi MariaDB.

set -euo pipefail

PI_HOST="${1:-192.168.0.80}"
PI_USER="${SFEPS_PI_USER:-pi}"
LOCAL_PORT="${SFEPS_DB_TUNNEL_LOCAL_PORT:-33060}"
PI_DB_HOST="${SFEPS_DB_TUNNEL_REMOTE_HOST:-127.0.0.1}"
PI_DB_PORT="${SFEPS_DB_TUNNEL_REMOTE_PORT:-3306}"

echo "[DB Tunnel] target: ${PI_USER}@${PI_HOST}"
echo "[DB Tunnel] local: 127.0.0.1:${LOCAL_PORT}"
echo "[DB Tunnel] remote: ${PI_DB_HOST}:${PI_DB_PORT}"
echo "[DB Tunnel] Ctrl-C to stop"

ssh -N -L "${LOCAL_PORT}:${PI_DB_HOST}:${PI_DB_PORT}" "${PI_USER}@${PI_HOST}"
