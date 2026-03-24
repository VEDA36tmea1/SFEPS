# utils

이 폴더는 여러 모듈에서 공통으로 사용하는 작은 헬퍼 함수들을 담고 있습니다. 규모는 작지만 설정 파싱과 로그 안전성에 자주 쓰이는 기반 코드입니다.

## 파일

### `env_utils.cpp`
- `load_env_size_t`
  - 양의 정수형 환경변수를 읽고 범위를 검증합니다.
- `load_env_int`
  - 일반 정수 환경변수를 읽고 최소값을 검증합니다.
- `load_env_bool`
  - `1/0`, `true/false`, `yes/no`, `on/off` 형태를 파싱합니다.
- `load_env_port`
  - 포트 번호를 읽고 `1..65535` 범위를 검사합니다.
- `load_env_string`
  - 문자열 환경변수를 읽고 비어 있으면 기본값을 돌려줍니다.
- `parse_allowlist_env`
  - CSV 문자열을 공백 제거 후 `unordered_set` 형태 allowlist로 변환합니다.

### `text_utils.cpp`
- `trim_copy`
  - 문자열 앞뒤 공백을 제거합니다.
- `to_lower_copy`
  - 대소문자 구분 없는 비교를 위해 소문자로 변환합니다.
- `sanitize_for_log`
  - 개행, 캐리지리턴, 탭을 이스케이프해 로그 줄바꿈 오염을 줄입니다.

## 어디서 쓰이는가

- `config/security_runtime.cpp`가 숫자/불리언/allowlist 환경변수를 읽을 때 `env_utils.cpp`를 사용합니다.
- `env_utils.cpp`는 allowlist 토큰 정리와 불리언 파싱을 위해 `text_utils.cpp`를 재사용합니다.
- 문자열 전처리 유틸리티는 로그 출력과 설정 검증에서 공통 기반 역할을 합니다.
