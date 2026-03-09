# Tests README

`tests/` 디렉터리 자동 테스트는 현재 **로그인 기능**(`tests/test_tc_func_login.py`) 기준으로 정리되어 있습니다.
테스트 실행 시 `tests/conftest.py`가 `server/build/smart_server*` 바이너리를 사용해 실제 SFEPS 서버를 기동합니다.

## 현재 사용 파일

- `tests/test_tc_func_login.py`: 로그인 PASS/FAIL 및 클라이언트 소스 가드 검증
- `tests/conftest.py`: 실서버 자동 기동/종료 및 포트(127.0.0.1:5555) 준비
- `tests/real_server.log`: 테스트 중 실서버 로그 출력 파일

## 1) 가상환경(venv) 생성 및 준비

```bash
cd /home/iam/SFEPS
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install pytest
```

## 2) 실서버 기동에 필요한 조건

아래 조건이 충족되어야 로그인 테스트가 실제 서버로 실행됩니다.

- 서버 바이너리 존재: `server/build/smart_server.bin` (또는 `smart_server`, `smart_server.exe`)
- DB 환경변수 설정:
  - `SFEPS_DB_USER`
  - `SFEPS_DB_PASS`
  - `SFEPS_DB_NAME_ANALYTICS`

환경변수는 쉘에서 export 하거나 `server/.env.local`에 설정할 수 있습니다.

## 3) 테스트 실행

```bash
cd /home/iam/SFEPS
source .venv/bin/activate
python -m pytest -q tests/test_tc_func_login.py -r a
```

## 4) 로그 확인

```bash
tail -f /home/iam/SFEPS/tests/real_server.log
```

## 5) 종료

```bash
deactivate
```

## 참고

- `tests/README.md`는 현재 구현/운영 중인 로그인 자동 테스트 기준으로만 유지합니다.
- 아직 구현되지 않았거나 현재 사용하지 않는 스트림/GUI 테스트 내용은 문서에서 제거했습니다.
