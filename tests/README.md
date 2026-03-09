# 테스트 안내 (업데이트)

이 문서는 `tests/` 디렉터리에 포함된 테스트들의 목적과 로컬 실행 방법을 정리합니다. 최근 변경으로 인해 로컬에서 일관되게 실행할 수 있도록 **모크 인증 서버**와 GUI 자동화 도우미가 추가되어 있습니다.

## 요약

- 로그인 관련 테스트: `tests/test_tc_func_login.py`
- 스트리밍 관련 테스트: `tests/test_tc_func_stream.py` (OpenCV 필요)
- 모의 인증 서버: `tests/mock_auth_server.py` (자동 시작 지원)

## 핵심 파일

- `tests/mock_auth_server.py`: 간단한 TCP 인증 스텁(기본: `127.0.0.1:5555`). `tests/conftest.py`의 세션 픽스처가 가능한 경우 자동으로 시작하거나, 이미 포트에 리스너가 있으면 재사용합니다.
- `tests/test_tc_func_login.py`: 인증 프로토콜(PASS/FAIL), 빈 입력 검사, 클라이언트 소스의 로그인 연동을 확인합니다.
- `tests/test_tc_func_stream.py`: RTSP/HTTP 스트림에서 프레임을 읽는 통합 테스트입니다. OpenCV(`cv2`)가 필요합니다. 이 모듈은 클라이언트가 로그인되어 있어야 정상적으로 실행됩니다.
- `tests/start_client_with_env.py`: (선택) 클라이언트 실행을 돕는 스크립트 — 픽스처에서 사용됩니다.
- `tests/gui_login.py`: (선택) `pywinauto` 기반의 GUI 자동화 스크립트로 로그인 입력을 수행합니다. `test_tc_func_stream.py`의 로그인 보장에 사용됩니다.
- `tests/mock_server.log`: 모의 서버의 런타임 로그(테스트에서 PASS 응답 관찰용). 이 파일은 런타임에 생성/갱신됩니다.

> 참고: 이전에 있던 `tests/inspect_ui.py`는 더 이상 필요하지 않아 제거되었습니다.

## 요구사항

- Python 3
- 가상환경 사용 권장
- 필수 패키지 (스트림 테스트 수행 시): `pytest`, `opencv-python` (또는 OpenCV가 설치된 환경)
- GUI 자동화를 사용하는 경우(클라이언트 실행 + 로그인 자동화): `pywinauto`, `psutil`

예시 설치 (가상환경 활성화 후):

```powershell
python -m venv .venv
& .\.venv\Scripts\Activate.ps1
pip install --upgrade pip
pip install pytest opencv-python pywinauto psutil
```

## 로컬 실행 가이드

1) 기본(로그인) 테스트

```powershell
# 가상환경 활성화 후
python -m pytest -q tests/test_tc_func_login.py
```

`tests/conftest.py`는 세션 시작 시 `mock_auth_server` 픽스처로 가능한 경우 모의 서버를 띄우고, 클라이언트 실행 파일이 있으면( `client/build-mingw/appHanwhaVisionSFEPS.exe`) 이를 자동으로 시작합니다.

2) 스트림 테스트

- 사전조건: 테스트 머신에서 대상 RTSP/HTTP 스트림에 접근 가능해야 합니다.
- 스트림 테스트는 OpenCV가 필요합니다.

```powershell
python -m pytest -q tests/test_tc_func_stream.py -r a
```

스트림 테스트는 `tests/test_tc_func_stream.py` 내의 기본값(`rtsp://192.168.0.84:8554/cam1`)을 사용합니다. 필요하면 파일을 편집하거나 환경변수로 대체하도록 테스트를 수정하세요.

3) GUI 자동화 및 로그 확인

- 로그인 자동화가 필요하면 `tests/gui_login.py`가 실행되며, 정상 로그인 시 `tests/mock_server.log`에 `sent: PASS` 항목이 기록됩니다. 로그인 실패 시 로그에 `FAIL`이 남습니다.

```powershell
# 모의 서버 로그 확인 (실행 중)
Get-Content .\tests\mock_server.log -Wait
```

## 문제 해결 노트

- 포트 충돌: `mock_auth_server`는 기본적으로 `127.0.0.1:5555`를 사용합니다. 다른 프로세스가 이미 포트를 점유하면 `tests/conftest.py`가 기존 리스너를 재사용하도록 설계되어 있습니다. 수동으로 포트를 해제하려면:

```powershell
# 점유 프로세스 확인
netstat -ano | findstr ":5555"
# 프로세스 종료
taskkill /PID <PID> /F
```

- 스트림 접근 실패: OpenCV가 스트림을 열지 못하면 네트워크/방화벽 또는 카메라 접근성 문제일 가능성이 큽니다. 로컬에서 VLC/ffmpeg로 먼저 확인해 보세요.

## 기타
- CI에 통합할 때는 `tests/conftest.py`의 자동 시작 동작(모의 서버/클라이언트 시작)을 고려해 실행 노드의 환경을 맞춰 주세요.

문의 사항이나 수정 요청이 있으면 알려주세요.
