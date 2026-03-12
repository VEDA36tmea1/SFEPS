#!/usr/bin/env python3
"""
50.png 체커보드에서 외부 파라미터(R, t) 추출 스크립트.

전제:
- 내부 파라미터(K, 왜곡계수)는 calibrationSession.mat 안에 저장되어 있음.
- 체커보드 패턴 크기(코너 개수)와 한 칸 사이즈는 아래 상수로 직접 맞춰줘야 함.

필요 패키지:
- pip install opencv-python scipy numpy
"""

import os
import sys
from typing import Tuple

import cv2
import numpy as np
from scipy.io import loadmat


# ===== 사용자가 반드시 확인/수정해야 하는 부분 =====

# 체커보드 내부 코너 개수 (가로, 세로)  -> 예: 9x6 코너면 (9, 6)
PATTERN_SIZE: Tuple[int, int] = (9, 6)

# 체커보드 한 칸의 실제 길이 (미터 단위 권장; mm 쓰면 t도 mm 단위가 됨)
SQUARE_SIZE: float = 0.043  # 2.5cm 예시

# 파일 경로
HERE = os.path.dirname(os.path.abspath(__file__))
IMG_PATH = os.path.join(HERE, "54.png")
MAT_PATH = os.path.join(HERE, "cameraParams_opencv.mat")


def load_intrinsics_from_mat(mat_path: str) -> Tuple[np.ndarray, np.ndarray]:
    """
    calibrationSession.mat 에서 카메라 내부 파라미터를 읽어온다.

    상황마다 .mat 구조가 다를 수 있어서, 가장 일반적인 두 가지 케이스를 시도:
    1) 'cameraMatrix', 'distCoeffs' 키가 바로 있는 경우 (OpenCV 스타일)
    2) 'cameraParams' 라는 MATLAB cameraParameters 객체가 저장된 경우
       - IntrinsicMatrix, RadialDistortion, TangentialDistortion 사용
    """
    data = loadmat(mat_path, squeeze_me=True, struct_as_record=False)

    # 1) OpenCV 스타일 (cameraMatrix, distCoeffs)
    if "cameraMatrix" in data and "distCoeffs" in data:
        K = np.array(data["cameraMatrix"], dtype=float)
        dist = np.array(data["distCoeffs"], dtype=float).reshape(-1)
        return K, dist

    # 2) MATLAB cameraParameters 스타일 (최상위에 바로 cameraParams 가 있는 경우)
    if "cameraParams" in data:
        cam = data["cameraParams"]
        # MATLAB cameraParameters.IntrinsicMatrix: 3x3, 보통 transposed 형태
        # OpenCV는 [fx 0 cx; 0 fy cy; 0 0 1] 이므로 transpose 필요할 수 있음.
        K = np.array(cam.IntrinsicMatrix, dtype=float).T

        radial = np.array(getattr(cam, "RadialDistortion", [0, 0]), dtype=float).reshape(-1)
        tang = np.array(getattr(cam, "TangentialDistortion", [0, 0]), dtype=float).reshape(-1)

        # OpenCV 왜곡계수 형태: (k1, k2, p1, p2, k3[, k4, k5, k6])
        # 여기서는 k1, k2, p1, p2, k3=0 으로 구성
        if radial.size >= 2:
            k1, k2 = radial[0], radial[1]
            k3 = radial[2] if radial.size >= 3 else 0.0
        else:
            k1 = k2 = k3 = 0.0

        p1 = tang[0] if tang.size >= 1 else 0.0
        p2 = tang[1] if tang.size >= 2 else 0.0

        dist = np.array([k1, k2, p1, p2, k3], dtype=float)
        return K, dist

    # 3) MATLAB Camera Calibrator App 세션 구조 안에 cameraParams 가 들어있는 경우
    #    예: calibrationSession.cameraParams 또는 다른 struct 필드 등
    for key, val in data.items():
        if key.startswith("__"):
            continue

        # (a) 최상위 struct 가 cameraParams 필드를 갖는 경우
        cam = getattr(val, "cameraParams", None)
        if cam is not None and hasattr(cam, "IntrinsicMatrix"):
            K = np.array(cam.IntrinsicMatrix, dtype=float).T

            radial = np.array(getattr(cam, "RadialDistortion", [0, 0]), dtype=float).reshape(-1)
            tang = np.array(getattr(cam, "TangentialDistortion", [0, 0]), dtype=float).reshape(-1)

            if radial.size >= 2:
                k1, k2 = radial[0], radial[1]
                k3 = radial[2] if radial.size >= 3 else 0.0
            else:
                k1 = k2 = k3 = 0.0

            p1 = tang[0] if tang.size >= 1 else 0.0
            p2 = tang[1] if tang.size >= 2 else 0.0

            dist = np.array([k1, k2, p1, p2, k3], dtype=float)
            return K, dist

        # (b) val 자체가 cameraParameters 객체인 경우 (이름이 cameraParams 가 아닐 수도 있음)
        if hasattr(val, "IntrinsicMatrix") and hasattr(val, "RadialDistortion"):
            K = np.array(val.IntrinsicMatrix, dtype=float).T

            radial = np.array(getattr(val, "RadialDistortion", [0, 0]), dtype=float).reshape(-1)
            tang = np.array(getattr(val, "TangentialDistortion", [0, 0]), dtype=float).reshape(-1)

            if radial.size >= 2:
                k1, k2 = radial[0], radial[1]
                k3 = radial[2] if radial.size >= 3 else 0.0
            else:
                k1 = k2 = k3 = 0.0

            p1 = tang[0] if tang.size >= 1 else 0.0
            p2 = tang[1] if tang.size >= 2 else 0.0

            dist = np.array([k1, k2, p1, p2, k3], dtype=float)
            return K, dist

    raise KeyError(
        "calibrationSession.mat 안에서 내부 파라미터(cameraMatrix/distCoeffs 또는 "
        "cameraParams)를 찾지 못했습니다. MATLAB 쪽에서 저장 형식을 한 번 확인해 주세요."
    )


def build_object_points(pattern_size: Tuple[int, int], square_size: float) -> np.ndarray:
    """체커보드 월드 좌표 (Z=0 평면 상)를 생성."""
    cols, rows = pattern_size
    objp = np.zeros((rows * cols, 3), np.float32)
    # (0,0), (1,0), ... 그리드 생성
    objp[:, :2] = np.mgrid[0:cols, 0:rows].T.reshape(-1, 2)
    objp *= square_size
    return objp


def main() -> None:
    if not os.path.exists(IMG_PATH):
        print(f"[ERROR] 이미지 파일을 찾을 수 없습니다: {IMG_PATH}", file=sys.stderr)
        sys.exit(1)
    if not os.path.exists(MAT_PATH):
        print(f"[ERROR] calibrationSession.mat 을 찾을 수 없습니다: {MAT_PATH}", file=sys.stderr)
        sys.exit(1)

    print(f"[INFO] 이미지: {IMG_PATH}")
    print(f"[INFO] 내부 파라미터: {MAT_PATH}")

    # 1. 내부 파라미터 로드
    K, dist = load_intrinsics_from_mat(MAT_PATH)
    print("[INFO] Camera matrix (K):")
    print(K)
    print("[INFO] Distortion coeffs:")
    print(dist)

    # 2. 이미지 로드 및 그레이 변환
    img = cv2.imread(IMG_PATH)
    if img is None:
        print(f"[ERROR] cv2.imread 실패: {IMG_PATH}", file=sys.stderr)
        sys.exit(1)

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

    # 3. 체커보드 코너 검출
    pattern_flags = cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE
    ret, corners = cv2.findChessboardCorners(gray, PATTERN_SIZE, flags=pattern_flags)

    if not ret:
        print(f"[ERROR] 체커보드 코너를 찾지 못했습니다. PATTERN_SIZE={PATTERN_SIZE} 가 맞는지 확인하세요.", file=sys.stderr)
        sys.exit(1)

    # 4. 코너 서브픽셀 정제
    criteria = (
        cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER,
        30,
        0.001,
    )
    corners_refined = cv2.cornerSubPix(
        gray,
        corners,
        winSize=(11, 11),
        zeroZone=(-1, -1),
        criteria=criteria,
    )

    # 5. 월드 좌표 생성
    objp = build_object_points(PATTERN_SIZE, SQUARE_SIZE)

    # 6. PnP 로 R, t 추정
    ret, rvec, tvec = cv2.solvePnP(objp, corners_refined, K, dist)
    if not ret:
        print("[ERROR] solvePnP 실패", file=sys.stderr)
        sys.exit(1)

    # Rodrigues -> 회전행렬
    R, _ = cv2.Rodrigues(rvec)

    print("\n===== Extrinsic parameters (World -> Camera) =====")
    print("R (3x3 회전행렬):")
    print(R)
    print("\nt (3x1 이동벡터, SQUARE_SIZE 단위):")
    print(tvec)

    # 원하면 4x4 포즈 행렬도 출력
    T = np.eye(4, dtype=float)
    T[:3, :3] = R
    T[:3, 3] = tvec.reshape(3)

    print("\nT (4x4 homogeneous transform, World -> Camera):")
    print(T)

    # 코너 검출 결과 시각화
    vis = img.copy()
    cv2.drawChessboardCorners(vis, PATTERN_SIZE, corners_refined, ret)
    cv2.imshow("Chessboard detection (50.png)", vis)
    print("\n아무 키나 누르면 창이 닫힙니다.")
    cv2.waitKey(0)
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()

