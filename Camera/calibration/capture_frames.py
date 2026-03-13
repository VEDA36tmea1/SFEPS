#!/usr/bin/env python3
"""
RTSP 영상을 실시간으로 보여주고, 스페이스바를 누르면 현재 프레임을
Camera/calibration 폴더에 숫자 파일명(0.png, 1.png, ...)으로 저장한다.
종료: q 또는 ESC
"""

import os
import sys

import cv2

# RTSP URL
cameraURL = "rtsp://admin:CCgbdCCgbd@192.168.0.84/profile2/media.smp"

# 스크립트가 있는 폴더 = 저장 경로
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def get_next_save_index(save_dir: str) -> int:
    """저장 폴더에 있는 N.png 중 최대 N을 찾아, 다음 번호(N+1)를 반환. 없으면 0."""
    if not os.path.isdir(save_dir):
        return 0
    max_n = -1
    for name in os.listdir(save_dir):
        if name.endswith(".png"):
            base = name[:-4]  # .png 제거
            if base.isdigit():
                max_n = max(max_n, int(base))
    return max_n + 1


def main() -> None:
    cap = cv2.VideoCapture(cameraURL)
    if not cap.isOpened():
        print(f"[ERROR] 영상 열기 실패: {cameraURL}", file=sys.stderr)
        sys.exit(1)

    count = get_next_save_index(SCRIPT_DIR)
    saved_this_run = 0
    window_name = "calibration - SPACE: save, q/ESC: quit"

    print(f"저장 경로: {SCRIPT_DIR}")
    print(f"다음 저장 번호: {count} (기존 파일 이어서)")
    print("스페이스바: 프레임 저장 | q 또는 ESC: 종료")

    while True:
        ret, frame = cap.read()
        if not ret or frame is None:
            print("[WARN] 프레임 읽기 실패", file=sys.stderr)
            continue

        # 다음 저장할 파일 번호 표시
        cv2.putText(
            frame,
            f"Next: {count}.png (SPACE=save, q=quit)",
            (10, 30),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.8,
            (0, 255, 0),
            2,
        )
        cv2.imshow(window_name, frame)

        key = cv2.waitKey(1) & 0xFF
        if key == ord("q") or key == 27:  # q or ESC
            break
        if key == ord(" "):  # 스페이스바
            path = os.path.join(SCRIPT_DIR, f"{count}.png")
            cv2.imwrite(path, frame)
            print(f"저장: {path}")
            count += 1
            saved_this_run += 1

    cap.release()
    cv2.destroyAllWindows()
    print(f"종료. 이번 실행에서 {saved_this_run}장 저장됨. (다음 시작 번호: {count})")


if __name__ == "__main__":
    main()
