# Qt 클라이언트 — MediaPipe Pose 워커 (Windows)

최종 갱신: 2026-03-31  

Qt 빌드(`CAMERA_RBF_QT_MODE` + OpenCV)에서 레이저 조준에 **어깨 기반 Pose**를 쓰려면, C++가 `mediapipe_pose_worker.py`를 **별도 Python 프로세스**로 띄워 JPEG crop을 넘기고, 랜드마크로 조준점(u, v)을 받습니다.

관련 코드:

- `client_msvc/src/videobackend.cpp` — `QtPoseWorker`, `onPwmTick()`에서 crop 제출·`rbfqt_set_pose_aim`
- `Camera/get_metadata/src/mediapipe_pose_worker.py` — MediaPipe Tasks `PoseLandmarker`, stdin/stdout IPC

---

## 1. 전제 조건

| 항목 | 설명 |
|------|------|
| OS | Windows (Qt 클라이언트가 `CreateProcess`로 Python 실행) |
| Python | **3.10~3.12** 권장 (MediaPipe Tasks wheel 호환성). 3.13 이상은 패키지 미지원일 수 있음 |
| 스크립트 위치 | `Camera/get_metadata/src/mediapipe_pose_worker.py` (저장소 기준 고정 경로) |

---

## 2. Python 인터프리터 선택 순서 (코드 동작)

`videobackend.cpp`의 `QtPoseWorker::resolve_python_exe()`는 실행 파일(`appHanwhaVisionSFEPS.exe`) 폴더를 기준으로, 상위 디렉터리를 최대 6단계까지 올라가며 다음을 **순서대로** 찾습니다.

1. `<exeDir>\.venv\Scripts\python.exe`
2. `<repo>\Camera\get_metadata\.venv\Scripts\python.exe` (예: `...\SFEPS\Camera\get_metadata\.venv\Scripts\python.exe`)

둘 다 없으면 **`python`** (PATH에 등록된 인터프리터)을 사용합니다.

**권장:** 저장소 루트 또는 `Camera/get_metadata` 아래에 가상환경을 만들고, 그 안에 의존성을 설치합니다.

```powershell
cd C:\Users\2-16\Desktop\SFEPS
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install mediapipe opencv-python numpy
```

또는:

```powershell
cd C:\Users\2-16\Desktop\SFEPS\Camera\get_metadata
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install mediapipe opencv-python numpy
```

---

## 3. `mediapipe_pose_worker.py` 의존성

스크립트가 직접 import 하는 패키지:

- `mediapipe` (Tasks API: `mediapipe.tasks.python.vision.pose_landmarker`)
- `cv2` (`opencv-python`)
- `numpy`

표준 라이브러리: `struct`, `json` 없음, `urllib`로 모델 다운로드 시 사용.

---

## 4. 모델 파일 (자동 다운로드)

스크립트와 같은 디렉터리에 `pose_landmarker_lite.task`가 없거나 너무 작으면, Google 스토리지에서 **lite** 모델을 자동 다운로드합니다.

- 경로: `Camera/get_metadata/src/pose_landmarker_lite.task`
- 최초 실행 시 네트워크·디스크 쓰기 권한 필요

오프라인 환경이면 해당 `.task` 파일을 같은 폴더에 미리 복사해 두면 됩니다.

---

## 5. MediaPipe가 정상 동작하는지 확인하는 방법

### 5-1. Python 단독에서 import 확인

```powershell
& "C:\Users\2-16\Desktop\SFEPS\.venv\Scripts\python.exe" -c "import mediapipe as mp; print('mediapipe', mp.__version__)"
& "C:\Users\2-16\Desktop\SFEPS\.venv\Scripts\python.exe" -c "import cv2; print('opencv', cv2.__version__)"
```

### 5-2. 워커 스크립트 기동·모델 로드 확인

워커는 **stdin으로 JPEG 길이(4바이트 LE) + JPEG 바이트**를 기대합니다. 수동 테스트는 번거로우므로, 아래처럼 **최소한 한 번** 실행해 모델 로드까지 에러가 없는지 보는 것이 좋습니다.

```powershell
cd C:\Users\2-16\Desktop\SFEPS\Camera\get_metadata\src
& "C:\Users\2-16\Desktop\SFEPS\.venv\Scripts\python.exe" .\mediapipe_pose_worker.py
```

(입력이 없으면 대기 종료될 수 있음 — 정상. stderr에 다운로드/로드 메시지가 있으면 확인.)

### 5-3. Qt 실행 후 로그로 확인

자동 추적 중 sticky bbox가 있고, OpenCV 프레임이 준비되면 `onPwmTick`이 주기적으로 crop을 제출합니다. 앱 로그에 다음이 보이면 **Pose 추론 결과가 들어온 뒤** stale 조건을 통과한 것입니다.

- `[PoseAim] got aim=(...)` … `staleOk=true`

`staleOk=false`이거나 `no aim yet`만 반복되면: Python 미설치·경로 오류·프로세스 실패·프레임 없음·스크립트 경로 불일치 등을 의심합니다.

---

## 6. `run_client.ps1`에서 조절하는 환경 변수 (Pose 관련)

현재 Qt 쪽에서 **Pose 조준점의 세로 오프셋**만 환경 변수로 읽습니다.

| 변수 | 기본(예시) | 의미 |
|------|------------|------|
| `SFEPS_POSE_DOWN_RATIO` | `0.35` (스크립트에서 `run_client.ps1` 예시는 `0.30`) | 어깨 중심에서 **아래로** 얼마나 내릴지. `0`에 가까우면 어깨 근처, `1`에 가까우면 sticky bbox의 **아래쪽(bbox bottom)** 쪽에 가깝게 조준. C++에서 `shoulder_y + (bboxBottom - shoulder_y) * ratio` 로 계산합니다. |

`run_client.ps1` 예시:

```powershell
[Environment]::SetEnvironmentVariable("SFEPS_POSE_DOWN_RATIO", "0.30", "Process")
```

**참고:** `SFEPS_RBF_PREDICT_MS`는 Kalman 예측 시간(카메라/스트림 지연 보정)용이며, MediaPipe 설치·동작과는 별개입니다. 같은 `run_client.ps1` 블록에 있을 수 있습니다.

---

## 7. 코드에만 있는 상수 (PS1로는 안 바뀜)

다음은 `videobackend.cpp`의 `onPwmTick()` 내부 상수입니다. 빌드 없이 바꾸려면 PS1이 아니라 **소스 수정** 또는 향후 환경 변수화가 필요합니다.

| 항목 | 대략적 값 | 의미 |
|------|-----------|------|
| `kPoseEveryTicks` | `5` | PWM 타이머(약 33ms) 기준으로 **몇 번에 한 번** pose 요청을 보낼지 (≈ 0.165s 간격) |
| `kPoseStaleMs` | `800` | pose 결과가 이보다 오래되면 무시하고 bbox 기반으로 복귀 |
| `kPosePadRatio` | `0.15` | sticky bbox crop에 붙이는 패딩 비율 |
| `kPoseJpegQuality` | `80` | JPEG 인코딩 품질 |

---

## 8. 자주 나는 문제

| 증상 | 조치 |
|------|------|
| `ModuleNotFoundError: mediapipe` | 해당 `.venv`에 `pip install mediapipe` 후, exe와 같은 탐색 경로에 `.venv`가 있는지 확인 |
| `python`이 시스템 3.13 등 | 3.10~3.12로 venv 재생성 |
| `mediapipe_pose_worker.py`를 못 찾음 | 빌드 산출물을 다른 폴더로만 옮긴 경우 — 저장소의 `Camera\get_metadata\src\` 트리가 exe 기준 상위에 보이도록 배치하거나, 소스 트리에서 실행 |
| Pose 로그 없음 | `CAMERA_RBF_QT_MODE` + OpenCV 캡처·sticky bbox·fraud 자동 추적 경로가 켜져 있는지 확인 |

---

## 9. 한 줄 요약

- **설치:** Python venv + `pip install mediapipe opencv-python numpy`
- **위치:** `client_msvc` 옆 `.venv` 또는 `Camera/get_metadata/.venv` 권장
- **PS1에서 Pose 조절:** 주로 `SFEPS_POSE_DOWN_RATIO`
- **정상 여부:** import 테스트 + Qt 로그 `[PoseAim] ... staleOk=true`
