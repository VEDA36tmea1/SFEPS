#!/bin/bash
# RC522 드라이버 테스트 스크립트
# 사용법: sudo ./run_test.sh
# (디바이스 트리 오버레이 적용 후 재부팅했거나, SPI에 RC522가 연결된 상태)

set -e
cd "$(dirname "$0")"

echo "=== 1. 모듈 로드 ==="
if [ -f build/rc522.ko ]; then
	insmod build/rc522.ko || true
else
	insmod rc522.ko || true
fi
sleep 1

echo ""
echo "=== 2. 로드 확인 ==="
lsmod | grep rc522 || echo "rc522 모듈 없음 (디바이스 트리 오버레이·재부팅 확인)"
ls -l /dev/rc522 2>/dev/null || echo "/dev/rc522 없음"

echo ""
echo "=== 3. 커널 로그 (rc522) ==="
dmesg | grep -i rc522 | tail -10

echo ""
if [ ! -c /dev/rc522 ]; then
	echo "경고: /dev/rc522 가 없습니다. 다음을 확인하세요:"
	echo "  - /boot/config.txt 에 dtoverlay=rc522-overlay 추가 후 재부팅"
	echo "  - 또는 sudo dtoverlay rc522-overlay 로 오버레이 로드 후 sudo insmod build/rc522.ko"
	exit 1
fi
echo "=== 4. 테스트 프로그램 실행 (태그를 리더에 갖다 대세요) ==="
./test_rc522

echo ""
echo "=== 완료 ==="
