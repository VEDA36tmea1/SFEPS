## camera_CSRT (CSRT/KCF 기반 bbox 추적) 사용 메모 (2026-03-19)

`camera_CSRT.cpp`는 ONVIF 메타데이터에서 사람이 탐지된 bbox를 그려주고, 사용자가 **클릭한 bbox**를 초기 ROI로 잡아 OpenCV 트래커로 계속 추적한다.  
트래커 bbox에서 `ratio` 위치를 타겟으로 잡고 기존 `camera_RBF.cpp`와 동일하게 RBF(TPS)로 `SET_PWM,PAN=...,TILT=...`를 stdout에 출력한다.

주요 목적:
- ONVIF bbox가 프레임마다 바뀌는 상황에서, “클릭한 객체”를 끊김 없이 추적해 PWM을 안정화
- KCF/CSRT 및 ROI crop/다운스케일을 조합해서 속도/안정성 트레이드오프를 실험

---

### 1) 빌드
```bash
cd /home/ros2man/Desktop/SFEPS/Camera/get_metadata
make camera_CSRT
```

---

### 2) 실행 예시

1. 기본값 (보통 권장: KCF + ROI crop, track-size downscale 비활성)
```bash
./camera_CSRT
```

2. CSRT로 640x360 다운스케일 테스트 (속도 비교용)
```bash
./camera_CSRT --tracker-mode 1 --track-size 640 360
```

3. KCF로 640x360 테스트 (속도 우선)
```bash
./camera_CSRT --tracker-mode 2 --track-size 640 360
```

4. KCF + ROI crop + 640x360 테스트 (기본 실험 모드)
```bash
./camera_CSRT --tracker-mode 3 --track-size 640 360 --crop-scale 1.5
```

5. (성능 튜닝) 그리드 오버레이 끄기
```bash
./camera_CSRT --no-grid
```

---

### 3) 모드별 인자 정리

#### 3.1 `--tracker-mode <1|2|3>`
- `1`: `CSRT` (crop 미사용)
- `2`: `KCF` (crop 미사용)
- `3`: `KCF + ROI crop` (bbox 주변 영역을 확장 crop해서 트래킹)

#### 3.2 `--track-size <W> <H>`
- 트래커가 동작할 프레임 해상도를 다운스케일한다.
- 효과: FPS를 올리고(특히 CSRT) 트래커 성능을 빠르게 관찰 가능
- 단점: 작은 bbox/빠른 움직임에서는 정밀도가 떨어질 수 있음

예:
```bash
./camera_CSRT --tracker-mode 1 --track-size 640 360
```

#### 3.3 `--crop-scale <f>`
- `tracker-mode 3`에서만 의미가 있음
- 클릭/업데이트된 bbox를 중심으로 `crop-scale`만큼 확장한 뒤 그 영역에서 KCF를 수행
- 기본값: `1.5`
- 권장 튜닝:
  - 흔들리면 `1.3 ~ 1.5` 쪽으로 줄이기
  - 객체가 자주 crop 밖으로 나가면 `1.7 ~ 2.0`으로 늘리기

---

### 4) 출력/연동

stdout 포맷:
```text
SET_PWM,PAN=1234,TILT=1350
```

파이프 건강 체크:
- 트래커가 비활성(선택 해제/실패)일 때는 빈 줄 heartbeat를 30프레임마다 전송한다.

---

### 5) 현재 한계 (현장에서 “쓰기 힘들다” 판단한 포인트)

#### 5.1 객체가 겹치면(근접/교차) 놓칠 수 있음
- CSRT/KCF 트래커는 “외형(appearance)” 기반이라, 여러 객체가 겹치거나 형태가 비슷하면 **트래커가 다른 객체로 전환(switch)** 하거나 **추적 실패(update false)** 할 수 있다.
- 특히 ONVIF bbox와 트래커 bbox가 동시에 변화하는데, 서로 겹치는 구간에서는 “클릭한 객체”라는 제약이 트래커만으로는 충분히 유지되지 않을 수 있다.

#### 5.2 FPS가 낮아질 수 있음
- CSRT는 KCF보다 훨씬 무겁기 때문에 FPS가 떨어진다.
- 또한 매 프레임 RBF 계산 + OpenCV 그리기(그리드 포함) 때문에 프레임이 누적되면 반응이 둔해질 수 있다.

현장 대응 방향(문서상 권장):
- CSRT가 필요 없으면 `--tracker-mode 2 또는 3`로 시작
- CSRT/KCF 모두 `--track-size 640 360` 같은 다운스케일을 적극 사용
- 시각화 오버헤드 줄이기: `--no-grid`

---

### 6) 다음 개선 아이디어(코드 TODO 성격)
- 트래커 bbox가 ONVIF bbox 중 다른 객체와 교차가 크게 늘어나는 경우 “확률적으로 전환”으로 판단해서:
  - 추적 중단(heartbeat만) 또는
  - 마지막 유효 bbox 기준으로 재-init 재시도
- FPS 측면에서는:
  - 그리드 오버레이를 기본 끄기(옵션화)
  - `--send-every`를 1보다 크게(예: 2~3) 조정

