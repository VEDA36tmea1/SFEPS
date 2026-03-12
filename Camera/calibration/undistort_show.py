#!/usr/bin/env python3
"""
55.png 를 카메라 내부 파라미터(K, dist)로 undistort 한 뒤 화면에 표시.
내부 파라미터는 cameraParams_opencv.mat (cameraMatrix, distCoeffs) 에서 로드.
"""

import os
import sys

import cv2
import numpy as np
from scipy.io import loadmat

HERE = os.path.dirname(os.path.abspath(__file__))
IMG_PATH = os.path.join(HERE, "74.png")
MAT_PATH = os.path.join(HERE, "cameraParams_opencv.mat")


def load_intrinsics(mat_path: str):
    """OpenCV 스타일 .mat 에서 K, dist 로드."""
    data = loadmat(mat_path, squeeze_me=True, struct_as_record=False)
    if "cameraMatrix" not in data or "distCoeffs" not in data:
        raise KeyError(f"{mat_path} 에 cameraMatrix, distCoeffs 가 없습니다.")
    K = np.array(data["cameraMatrix"], dtype=np.float64)
    dist = np.array(data["distCoeffs"], dtype=np.float64).reshape(-1)
    return K, dist


def main():
    if not os.path.exists(IMG_PATH):
        print(f"[ERROR] 이미지 없음: {IMG_PATH}", file=sys.stderr)
        sys.exit(1)
    if not os.path.exists(MAT_PATH):
        print(f"[ERROR] .mat 없음: {MAT_PATH}", file=sys.stderr)
        sys.exit(1)

    K, dist = load_intrinsics(MAT_PATH)
    img = cv2.imread(IMG_PATH)
    if img is None:
        print(f"[ERROR] imread 실패: {IMG_PATH}", file=sys.stderr)
        sys.exit(1)

    h, w = img.shape[:2]

    # alpha=1.0 으로 새 카메라 행렬 계산: 가능한 한 많이 살리고, 검은 여백이 생겨도 크롭을 최소화
    new_K, roi = cv2.getOptimalNewCameraMatrix(K, dist, (w, h), alpha=1.0, newImgSize=(w, h))

    # 왜곡 보정 (확장된 시야, 여백 포함)
    undistorted = cv2.undistort(img, K, dist, None, new_K)

    # 원본 | 보정본 나란히 표시
    combined = np.hstack([img, undistorted])
    cv2.putText(combined, "original", (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
    cv2.putText(combined, "undistorted (alpha=1)", (w + 10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
    cv2.namedWindow("original | undistorted", cv2.WINDOW_NORMAL)
    cv2.imshow("original | undistorted", undistorted)
    print("아무 키나 누르면 종료.")
    cv2.waitKey(0)
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
