# SFEPS Server - 문서 색인

## 📚 전체 문서 가이드

```
/home/iam/finalProject/SFEPS/server/
├── README.md              ← 🔴 먼저 읽을 것! 프로젝트 개요
├── MODULES.md             ← 각 모듈의 상세 명세 및 구현
├── BUILD_GUIDE.md         ← 빌드, 배포, 운영 가이드
├── CODE_REVIEW.md         ← 코드 품질, 보안, 개선 사항
├── API.md                 ← API 명세, 통신 프로토콜
├── INDEX.md (이 파일)     ← 문서 네비게이션
├── include/               ← 헤더 파일 (.h)
│   ├── auth.h
│   ├── cleanup.h
│   ├── log.h
│   ├── recorder.h
│   └── rfid_monitor.h
├── src/                   ← 소스 파일 (.cpp)
│   ├── auth.cpp
│   ├── cleanup.cpp
│   ├── log.cpp
│   ├── main.cpp
│   ├── recorder.cpp
│   └── rfid_monitor.cpp
├── CMakeLists.txt         ← CMake 빌드 설정
├── build/                 ← 빌드 산출물
│   └── smart_server       ← 실행 파일
└── videos/                ← 녹화된 영상 저장소
```

---

## 🎯 문서 선택 가이드

### 🔴 신규 개발자 온보딩

**읽는 순서:**

1. **[README.md](README.md)** (15분)
   - 프로젝트 개요
   - 전체 구조
   - 모듈 간 통신
   - 의존성 정보

2. **[MODULES.md](MODULES.md)** 중 필요한 모듈만 (30분)
   - [인증](/MODULES.md#1-인증-모듈-authh--authcpp)
   - [로깅](/MODULES.md#2-로깅-모듈-logh--logcpp)
   - [녹화](/MODULES.md#3-비디오-녹화-모듈-recorderh--recordercpp)
   - [정리](/MODULES.md#4-파일-정리-모듈-cleanh--cleanupcpp)
   - [RFID](/MODULES.md#5-rfid-모니터링-rfid_monitorh--rfid_monitorcpp)

3. **[BUILD_GUIDE.md](BUILD_GUIDE.md)** (10분)
   - 빌드 환경 설정
   - 컴파일 및 실행

### 🟡 기능 추가/수정

**필요 문서:**

- **[MODULES.md](MODULES.md)** - 해당 모듈 상세 이해
- **[CODE_REVIEW.md](CODE_REVIEW.md)** - 보안 및 품질 체크리스트
- **[API.md](API.md)** - 통신 프로토콜 확인

### 🟠 배포 및 운영

**필요 문서:**

- **[BUILD_GUIDE.md](BUILD_GUIDE.md)** - 빌드, systemd 등록, 모니터링
- **[CODE_REVIEW.md](CODE_REVIEW.md)** - 보안 체크리스트

### 🔵 API 연동

**필요 문서:**

- **[API.md](API.md)** - 포트, 형식, 예제
- **[MODULES.md](MODULES.md)** - 각 모듈의 enqueue 함수

### ⚫ 버그 수정 / 성능 개선

**필요 문서:**

- **[CODE_REVIEW.md](CODE_REVIEW.md)** - 보안 이슈, 개선 권장사항
- **[BUILD_GUIDE.md](BUILD_GUIDE.md)** - 프로파일링 도구

---

## 📖 상세 문서 목차

### [README.md](README.md) - 프로젝트 개요 (★필독)

| 섹션 | 내용 |
|------|------|
| 프로젝트 구조 | 전체 폴더 및 파일 레이아웃 |
| 모듈 설명 | 각 모듈의 1줄 요약 |
| 의존성 | 필요한 라이브러리 |
| 빌드 & 실행 | 기본 컴파일 명령어 |
| 데이터 흐름 | 모듈 간 통신 다이어그램 |
| 코드 품질 | 장점과 개선점 |

### [MODULES.md](MODULES.md) - 모듈별 상세 명세 (★중요)

각 모듈별 심화 가이드:

#### 1. [인증 모듈](/MODULES.md#1-인증-모듈-authh--authcpp)
- 클래스 설계  
- API명세 (connect, authenticate)
- 데이터베이스 스키마
- ⚠️ 보안 고려사항

#### 2. [로깅 모듈](/MODULES.md#2-로깅-모듈-logh--logcpp)
- 5가지 로그 타입
- 비동기 처리 흐름
- LogItem 구조체
- XML 파싱
- DB 테이블 설계

#### 3. [녹화 모듈](/MODULES.md#3-비디오-녹화-모듈-recorderh--recordercpp)
- FFmpeg 설정
- 타임스탬프 보정 로직
- 파일 분할 (60초 단위)
- 메타데이터 처리
- ⚠️ TLS 인증서 설정

#### 4. [정리 모듈](/MODULES.md#4-파일-정리-모듈-cleanh--cleanupcpp)
- 자동 정리 알고리즘
- 파라미터 설정
- 로그 출력
- 권장 설정

#### 5. [RFID 모니터링](/MODULES.md#5-rfid-모니터링-rfid_monitorh--rfid_monitorcpp)
- Unix Domain Socket
- NDJSON 파싱
- 자동 재연결
- JSON 추출 알고리즘
- 트러블슈팅

### [BUILD_GUIDE.md](BUILD_GUIDE.md) - 빌드 & 배포 가이드

| 섹션 | 내용 |
|------|------|
| 의존성 설치 | apt-get 명령어 |
| 빌드 단계 | cmake, make 과정 |
| 빌드 옵션 | Debug, Release, 병렬 빌드 |
| 실행 방법 | 기본, 백그라운드, systemd |
| 테스트 | 포트, DB, 영상, 로드 테스트 |
| 유지보수 | logrotate, DB 최적화 |
| 성능 모니터링 | valgrind, perf, 메모리 프로파일링 |
| 트러블슈팅 | 일반적인 문제와 해결책 |
| 배포 체크리스트 | 배포 전/후 확인사항 |

### [CODE_REVIEW.md](CODE_REVIEW.md) - 코드 품질 & 보안

| 섹션 | 내용 |
|------|------|
| 코드 점수표 | 각 모듈별 A-F 평가 |
| 우수 사항 | 멀티스레드, 모듈화 등 |
| 보안 이슈 | 5개 CRITICAL/HIGH/MEDIUM |
| 에러 처리 | 개선 권장사항 |
| 보안 체크리스트 | 배포 전 확인사항 |
| 정적 분석 | cppcheck 사용법 |
| 개선 우선순위 | P0~P2 로드맵 |

### [API.md](API.md) - API 명세 & 통신 프로토콜

| 섹션 | 내용 | 포트 |
|------|------|------|
| 인증 API | 로그인 요청/응답 형식 | 5555 |
| 음성 API | 오디오 포맷, 수신 흐름 | 5556 |
| 알림 API | 부정승차 알림 포맷 | 5557 |
| DB 스키마 | 모든 테이블 정의 | - |
| RFID 데이터 | JSON 형식, 파싱 | Unix Socket |
| XML 메타데이터 | 카메라 데이터 예시 | - |
| 테스트 명령어 | telnet, nc 사용법 | - |

---

## 🔗 빠른 링크

### 가장 자주 찾는 정보

| 질문 | 답변 위치 |
|------|---------|
| 어떻게 빌드하나요? | [BUILD_GUIDE.md - 빌드](/BUILD_GUIDE.md#📦-빌드-프로세스) |
| 프로그램을 어떻게 실행하나요? | [BUILD_GUIDE.md - 실행](/BUILD_GUIDE.md#🚀-실행) |
| 인증이 어떻게 동작하나요? | [MODULES.md - 인증](/MODULES.md#1-인증-모듈-authh--authcpp) |
| 데이터는 어디에 저장되나요? | [README.md - 데이터 흐름](/README.md#-데이터-흐름-다이어그램) |
| port 5555는 뭐하는 포트? | [API.md - 인증 API](/API.md#-인증-api-포트-5555) |
| 보안 이슈가 뭐가 있나요? | [CODE_REVIEW.md - 보안](/CODE_REVIEW.md#⚠️-보안-이슈) |
| 로그는 어디에 나가나요? | [MODULES.md - 로깅](/MODULES.md#2-로깅-모듈-logh--logcpp) |
| 영상은 어디에 저장되나요? | [MODULES.md - 녹화](/MODULES.md#3-비디오-녹화-모듈-recorderh--recordercpp) |
| RFID 데이터 형식은? | [API.md - RFID 데이터](/API.md#📨-rfid-이벤트-데이터) |
| 디버깅 방법은? | [BUILD_GUIDE.md - 트러블슈팅](/BUILD_GUIDE.md#🐛-문제-해결) |

---

## 🔍 문서 검색 팁

### grep으로 검색

```bash
# "5555" 포트 관련 정보 찾기
grep -n "5555" README.md MODULES.md API.md

# "SQL Injection" 관련 정보
grep -r "SQL Injection" CODE_REVIEW.md

# "reconnect" (재연결) 정보
grep -n "reconnect\|retry" MODULES.md BUILD_GUIDE.md
```

### 키워드별 분류

#### 🔐 보안 정보
- [SQL Injection](/CODE_REVIEW.md#1️⃣-critical-sql-injection-취약점)
- [비밀번호 저장](/CODE_REVIEW.md#2️⃣-high-비밀번호-평문-저장)
- [TLS 검증](/CODE_REVIEW.md#5️⃣-medium-tls-인증서-검증-비활성화)
- [보안 체크리스트](/CODE_REVIEW.md/#-보안-체크리스트)

#### 🚀 배포 정보
- [의존성 설치](/BUILD_GUIDE.md#단계별-빌드)
- [빌드 프로세스](/BUILD_GUIDE.md#📦-빌드-프로세스)
- [systemd 서빙](/BUILD_GUIDE.md#systemd-서비스로-등록)
- [모니터링](/BUILD_GUIDE.md#📊-성능-모니터링)

#### 🐛 디버깅 정보
- [컴파일 에러](/BUILD_GUIDE.md#컴파일-에러)
- [런타임 에러](/BUILD_GUIDE.md#런타임-에러)
- [트러블슈팅](/BUILD_GUIDE.md#🐛-문제-해결)
- [프로파일링](/BUILD_GUIDE.md#성능-모니터링)

#### 📊 API 정보
- [인증 API](/API.md#-인증-api-포트-5555)
- [음성 API](/API.md#-음성-api-포트-5556)
- [알림 API](/API.md#-알림-api-포트-5557)
- [데이터베이스 테이블](/API.md#-데이터베이스-스키마)

---

## 📋 학습 경로 추천

### 👶 완전 초보자

```
1️⃣ README.md (15분)
   ↓
2️⃣ MODULES.md 인증/로깅 (20분)
   ↓
3️⃣ BUILD_GUIDE.md 빌드 (15분)
   ↓
4️⃣ 실제 코드 읽기 (auth.h/cpp)
   ↓
5️⃣ 코드 실행 및 로그 분석
```

### 🎓 중급자

```
1️⃣ README.md (5분, 복습)
   ↓
2️⃣ API.md (20분)
   ↓
3️⃣ MODULES.md 전체 (45분)
   ↓
4️⃣ CODE_REVIEW.md (30분)
   ↓
5️⃣ BUILD_GUIDE.md 트러블슈팅 (20분)
```

### 🚀 고급자

```
1️⃣ CODE_REVIEW.md - 보안 섹션
   ↓
2️⃣ 개선안 구현
   ↓
3️⃣ MODULES.md 상세 토론
   ↓
4️⃣ 성능 최적화
```

---

## 📝 문서 관리 및 업데이트

### 문서 수정 시 주의사항

모든 문서는 **마크다운(.md) 형식**이며, 다음 규칙을 따릅니다:

- **헤더**: `# 프로젝트`, `## 섹션`, `### 소섹션` (최대 3단계)
- **코드 블록**: ` ```cpp ` 또는 ` ```bash `
- **강조**: `**굵게**`, `*이탤릭*`
- **목록**: `- 항목` 또는 `1. 항목`
- **링크**: `[텍스트](문서.md#섹션)`

### 마지막 업데이트 정보

각 문서의 끝에는 다음을 포함합니다:

```markdown
---

마지막 업데이트: YYYY-MM-DD
```

현재 상태:
- ✅ README.md - 2026-02-13
- ✅ MODULES.md - 2026-02-13
- ✅ BUILD_GUIDE.md - 2026-02-13
- ✅ CODE_REVIEW.md - 2026-02-13
- ✅ API.md - 2026-02-13
- ✅ INDEX.md (이 파일) - 2026-02-13

---

## 📞 도움말

### 문서를 찾을 수 없나요?

1. 페이지 내 검색: `Ctrl+F` / `Cmd+F`
2. 이 INDEX.md의 검색 섹션 사용
3. 위 "빠른 링크" 표 확인

### 문서가 최신이 아니라면?

각 문서의 마지막 업데이트 날짜를 확인하세요.
최신 정보는 소스 코드의 주석을 참조하세요.

### 추가 자료

- SFEPS 전체 프로젝트: `/home/iam/finalProject/SFEPS/`
- 카메라 문서: `/home/iam/finalProject/SFEPS/Camera/`
- 클라이언트 코드: `/home/iam/finalProject/SFEPS/client/`
- 하드웨어: `/home/iam/finalProject/SFEPS/hardware/`

---

**Happy coding! 🚀**

마지막 업데이트: 2026-02-13
