## 커밋 컨벤션

이 저장소는 모노레포 구조를 사용합니다. 모든 서브 모듈(hardware/server/client/shared/docs/tests/.github 등)에 공통으로 아래 커밋 메시지 규칙을 사용합니다.

## 기본 구조

```
<타입>[옵션: 범위(scope)]: <짧은 설명>

[본문 - 자세한 설명, 왜 바꿨는지 등 (선택)]

[꼬리말 - 이슈 번호, BREAKING CHANGE 등 (선택)]
```

### 주요 type 목록

| 타입 | 언제 쓰나? | 프로젝트 예시 |
| --- | --- | --- |
| `feat` | 새로운 기능 추가 | `feat: add face detection and age-gender analysis to camera` |
| `fix` | 버그 수정 | `fix: fix UART timeout in RFID-server communication` |
| `refactor` | 코드 구조 개선 (기능 변화 없음) | `refactor: simplify RTSP stream handling in server` |
| `docs` | 문서(README, 기획서 등) 수정 | `docs: update architecture diagram for monorepo` |
| `test` | 테스트 코드 추가/수정 | `test: add integration tests for laser activation` |
| `chore` | 빌드/설정/잡일 (코드 아닌 것) | `chore: add .gitignore and pre-commit hooks` |
| `style` | 코드 포맷팅 (기능 영향 없음) | `style: format python code with black` |
| `perf` | 성능 개선 | `perf: optimize face detection latency under 500ms` |
| `ci` | CI/CD 설정 변경 | `ci: add github actions for build and test` |
| `build` | 빌드 시스템/의존성 변경 | `build: update opencv to 4.8.0` |

### scope (범위)

어떤 모듈/부분에 영향을 미쳤는지 표시합니다.

- 예: `feat(server): implement age validation logic`
- 예: `fix(client): fix zoom tracking bug on QT interface`
- 예: `refactor(hardware): clean up stm32 laser galvo code`

가능한 한 scope는 아래와 같이 세분화해서 사용합니다.

- `hardware` : `hardware/` 전체 또는 하드웨어 공통
  - `hardware-stm32-rfid-laser` : `hardware/stm32-rfid-laser/`
  - `hardware-camera-pno-a9081r` : `hardware/camera-pno-a9081r/`
- `server` : 라즈베리파이 Python 서버 (`server/`)
- `client` : QT 클라이언트 (`client/`)
- `shared` : 공통 프로토콜, 설정, 유틸 (`shared/`)
- `docs` : 문서 및 다이어그램 (`docs/`)
- `tests` : 통합 테스트 (`tests/`)
- `ci` 또는 `github-actions` : `.github/workflows/` 관련

원한다면 더 세부적인 scope를 붙여도 됩니다.

- 예: `feat(server-auth): add JWT based session`
- 예: `fix(client-ui): fix resolution dropdown bug`

## 예시

### 1. Simple

```
feat: add RFID tag timestamp to server log
```

### 2. Detail (본문 + 이슈 연결)

```
fix(camera): correct age-gender mismatch detection

- Adjust threshold from 0.8 to 0.85 for better accuracy
- Add logging for false positives
- Tested with 50 simulated passengers

Closes #123
```

### 3. Version Upgrade / Breaking Change

```
feat!: switch from UART to WiFi for RFID-server communication

BREAKING CHANGE: All hardware modules need firmware update
```

`!` 느낌표는 **breaking change 표시**로, SemVer를 사용할 경우 자동으로 major 버전 업 조건이 됩니다.

## 디렉터리 구조 가이드

레포 최상단 구조는 다음을 기본으로 합니다.

```
SFEPS/          ← 하나의 repo (monorepo)
├── hardware/
│   ├── stm32-rfid-laser/           ← STM32 코드 + Laser Galvo
│   └── camera-pno-a9081r/          ← Camera 설정/스크립트
├── server/                         ← Raspberry Pi Python 서버
├── client/                         ← QT Client
├── shared/                         ← 공통: protocol 정의, config, utils
├── docs/                           ← 기획서, 아키텍처 다이어그램
├── tests/                          ← 통합 테스트
└── .github/workflows/              ← CI/CD (모듈별 빌드 + 통합 테스트)
```

- **새 모듈 추가** 시에는 위 구조와 비슷한 수준으로 디렉터리를 나누고, 가능하면 `shared/`를 활용해 중복 코드를 최소화합니다.
- **docs**에는 기획서, 요구사항 정의서, 아키텍처/시퀀스 다이어그램을 정리합니다.
- **tests**에는 실제 하드웨어/서버/클라이언트를 아우르는 통합 테스트 스크립트/코드를 배치합니다.

## PR 및 커밋 팁

- 한 PR에는 **하나의 목적**만 담는 것을 권장합니다. (예: 기능 추가, 버그 수정 등)
- 커밋 메시지는 위 컨벤션에 맞추어 작성하고, 변경 이유(왜)를 본문에 간단히 남겨 두면 나중에 추적이 쉬워집니다.
- BREAKING CHANGE가 있는 경우, 설명과 함께 마이그레이션 방법을 `docs/`에 정리하고 커밋/PR 본문에 링크를 남겨주세요.

