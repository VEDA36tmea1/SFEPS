#!/usr/bin/env bash
# Raspi-driver 폴더만 Raspberry Pi로 rsync 동기화
# 사용: ./sync-to-pi.sh [pi_user@host]
# 예:   ./sync-to-pi.sh pi@192.168.0.10

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="${1:-pi@raspberrypi}"

echo "Syncing $SCRIPT_DIR -> $TARGET:~/SFEPS/hardware/Raspi-driver/"
rsync -avz --delete \
  "$SCRIPT_DIR/" \
  "$TARGET:~/SFEPS/hardware/Raspi-driver/"

echo "Done."
