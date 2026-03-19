## get_point_pixel (캘리브레이션 포인트 픽셀 찍기)

### 목적

카메라 영상 위에서 **15개 실측 월드 포인트(p0~p14)** 에 대응하는 **픽셀 좌표(u,v)** 를 순차로 찍어 저장합니다.

- 좌클릭: 현재 포인트 픽셀 저장(즉시 저장) → 다음 포인트로 이동
- 우클릭: 직전 저장 1개 취소(즉시 저장) → 해당 포인트로 되돌아감
- 재실행: 기존 파일이 있으면 **다음 미완료 인덱스부터 자동으로 이어서 진행**

저장 파일은 OpenCV `FileStorage` YAML 형식(`calib_pixels.yml`)입니다.

---

### 빌드

```bash
cd /home/ros2man/Desktop/SFEPS/hardware/stm32-laser/get_point_pixel
mkdir -p build
cd build
cmake ..
make -j4
```

---

### 실행

기본 RTSP 소스(코드 기본값)로 실행:

```bash
./get_point_pixel
```

소스/출력파일 지정:

```bash
./get_point_pixel "rtsp://..." calib_pixels.yml
```

---

### 출력 파일

`calib_pixels.yml` 안에:

- `source`: 입력 소스 문자열
- `points`: p0~p14 각각에 대해:
  - `id`, `X_cm`, `Y_cm`
  - `done` (0/1)
  - `u_px`, `v_px`

이 파일을 이후 Homography/LUT/world-track 파이프라인의 입력으로 사용하면 됩니다.

