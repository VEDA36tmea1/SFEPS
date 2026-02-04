## 커밋 컨벤션

이 저장소는 모노레포 구조를 사용합니다. 모든 서브 모듈(hardware/server/client/shared/docs/tests/.github 등)에 공통으로 아래 커밋 메시지 규칙을 사용합니다.

## 기본 구조

```
<타입>[옵션: 범위(scope)]: <짧은 설명>

[본문 - 자세한 설명, 왜 바꿨는지 등 (선택)]

[꼬리말 - 이슈 번호, BREAKING CHANGE 등 (선택)]
```

### 주요 type 목록

| **이모지** | **깃모지 코드** | **타입** | **의미 (언제 쓰나?)** | **실전 커밋 예시** |
| --- | --- | --- | --- | --- |
| 🎉 | `:tada:` | `init` | 프로젝트 시작 (첫 커밋) | `:tada: init: start SFEPS project structure` |
| ✨ | `:sparkles:` | `feat` | 새로운 기능 추가 | `:sparkles: feat: add face detection to camera` |
| 🐛 | `:bug:` | `fix` | 버그 수정 | `:bug: fix: fix UART timeout in RFID communication` |
| ♻️ | `:recycle:` | `refactor` | 코드 구조 개선 | `:recycle: refactor: simplify RTSP stream handling` |
| 📝 | `:memo:` | `docs` | 문서 작성 및 수정 | `:memo: docs: update architecture diagram` |
| 🧪 | `:test_tube:` | `test` | 테스트 코드 추가/수정 | `:test_tube: test: add integration tests for laser` |
| 🧹 | `:broom:` | `chore` | 빌드/설정/단순 잡일 | `:broom: chore: add .gitignore and hooks` |
| 🎨 | `:art:` | `style` | 코드 스타일/포맷팅 | `:art: style: format C++ code with clang-format` |
| ⚡ | `:zap:` | `perf` | 성능 개선 (속도 등) | `:zap: perf: optimize detection latency under 500ms` |
| 🚦 | `:vertical_traffic_light:` | `ci` | CI/CD 설정 변경 | `:vertical_traffic_light: ci: add github actions for build` |
| 📦 | `:package:` | `build` | 빌드 시스템/의존성 변경 | `:package: build: update opencv to 4.10.0` |
| 🛠️ | `:hammer_and_wrench:` | `config` | 개발 환경/설정 변경 | `:hammer_and_wrench: config: update CMakeLists for cross-build` |

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

## 브랜치 전략

이 레포는 다음과 같은 브랜치 네이밍 규칙을 권장합니다.

| 브랜치 | 용도 | 예시 |
| --- | --- | --- |
| `master`  | 전체 통합 코드  | `master` |
| `feature/` | 새로운 기능 개발 | `feature/123-camera-face-detection` |
| `fix/` | 버그 수정 | `fix/45-laser-galvo-not-activating` |
| `refactor/` | 코드 리팩토링 (기능 변경 없음) | `refactor/78-server-rtsp-handling` |
| `docs/` | 문서 수정 | `docs/19-update-architecture-diagram` |
| `test/` | 테스트 코드 추가/수정 | `test/102-add-rfid-integration-tests` |
| `chore/` | 빌드, 설정, CI 등 잡일 | `chore/15-setup-github-actions` |
| `hotfix/` | 프로덕션에 즉시 적용해야 하는 긴급 수정 | `hotfix/urgent-rfid-uart-crash` |

- 브랜치 이름에는 **이슈 번호나 간단한 설명**을 포함하는 것을 추천합니다.
- 긴급 수정(`hotfix/`)은 가능한 빠르게 `master`(또는 실제 운영 브랜치)와 필요한 하위 브랜치에 머지합니다.

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

