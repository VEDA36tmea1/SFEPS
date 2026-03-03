# Login 테스트 안내

이 파일은 `tests/test_tc_func_login.py`에 정의된 로그인 관련 기능 테스트를 요약하고, 로컬에서 실행하는 방법을 안내합니다.

## 개요

- 테스트는 간단한 TCP 기반 인증 서버와의 상호작용을 통해 로그인 동작을 검증합니다.
- 또한 클라이언트 소스 코드의 빈 입력 검사와 UI 연동(wiring)을 정적 검사합니다.

## 환경 요구사항

- Python 3
- `pytest` (가상환경 사용 권장)
- 인증 서버(기본: `192.168.0.92:5555`)에 접근 가능해야 합니다.

## 환경 변수

- `SFEPS_AUTH_HOST` — 인증 서버 호스트(옵션)
- `SFEPS_AUTH_PORT` — 인증 서버 포트(옵션)

## 예시

PowerShell (Windows):

```powershell
$env:SFEPS_AUTH_HOST = "192.168.0.92"
$env:SFEPS_AUTH_PORT = "5555"
pytest tests/test_tc_func_login.py -q
```

## 테스트에서 확인하는 내용

- 네트워크 연결: 테스트는 먼저 설정한 인증 서버에 TCP 연결을 시도하며, 실패하면 관련 테스트들을 건너뜁니다.
- 프로토콜: `user:password` 형태의 UTF-8 바이트열을 전송하고, 서버로부터 `PASS` 또는 `FAIL` 응답을 기대합니다.
- 클라이언트 입력 검사: `client/src/authmanager.cpp`에 빈 입력을 검사하고 에러 메시지를 emit 한 뒤 소켓 연결을 시도하지 않는지 확인합니다.
- UI 연동: `client/src/main.cpp`와 `client/src/views/LoginView.qml`이 로그인 성공 시 메인 화면으로 전환되도록 연결되어 있는지 확인합니다.

## 참고 파일

- 테스트: [tests/test_tc_func_login.py](tests/test_tc_func_login.py)
- 클라이언트 구현 참조: [client/src/authmanager.cpp](../client/src/authmanager.cpp), [client/src/main.cpp](../client/src/main.cpp), [client/src/views/LoginView.qml](../client/src/views/LoginView.qml)

## 실행 (Windows, `.\venv` 사용)

1. 프로젝트 루트로 이동한 뒤 가상환경을 생성하고 활성화합니다 (PowerShell):

```powershell
python -m venv venv
& .\venv\Scripts\activate.ps1
```

활성화되면 프롬프트에 `(venv)`가 표시됩니다. 활성화가 차단된 경우(ExecutionPolicy 관련) 관리자 권한 PowerShell에서 한 번만 다음을 실행하세요:

```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

2. 종속성 설치 (가상환경이 활성화된 상태에서):

```powershell
pip install --upgrade pip
pip install pytest
```

3. 인증 서버 준비 또는 환경 변수 설정:

```powershell
$env:SFEPS_AUTH_HOST = "192.168.0.92"
$env:SFEPS_AUTH_PORT = "5555"
```

4. 테스트 실행:

```powershell
pytest tests/test_tc_func_login.py -q
```

## 비고

- 인증 서버가 없으면 네트워크 의존 테스트는 pytest에서 skip 처리됩니다.
- 테스트는 검증 목적으로 간단한 텍스트 기반 TCP 프로토콜을 사용합니다.
