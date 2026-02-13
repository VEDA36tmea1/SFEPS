# SFEPS Server - 종합 가이드

SFEPS(Smart Fraud Examination Platform Server)는 RTSP 비디오 스트림 기반 실시간 모니터링, 부정승차 감지, RFID 데이터 수집을 통합 관리하는 C++ 기반 서버입니다.

## 📋 프로젝트 구조

```
server/
├── include/          # 헤더 파일
│   ├── auth.h        # 사용자 인증
│   ├── cleanup.h     # 파일 정리
│   ├── log.h         # 로깅 시스템
│   ├── recorder.h    # 비디오 녹화
│   └── rfid_monitor.h # RFID 모니터링
├── src/              # 소스 파일
│   ├── auth.cpp
│   ├── cleanup.cpp
│   ├── log.cpp
│   ├── recorder.cpp
│   ├── rfid_monitor.cpp
│   └── main.cpp
├── CMakeLists.txt    # CMake 빌드 설정
├── build/            # 빌드 산출물
└── README.md         # 이 문서
```

---

## 🔧 모듈 상세 설명

### 1️⃣ **인증 모듈 (auth.h / auth.cpp)**

**역할**: Qt 클라이언트의 로그인 요청을 처리하고 MariaDB에서 사용자 인증

#### 주요 기능
- `Authenticator` 클래스: MariaDB 연결 관리
- `connect()`: DB 연결 초기화
- `authenticate(id, pw)`: 사용자 ID/비밀번호 검증

#### 코드 구조
```cpp
class Authenticator {
    MYSQL* conn;           // MariaDB 핸들
    std::mutex dbMutex;    // 멀티스레드 보호
    
    bool authenticate(const std::string& id, const std::string& pw);
};
```

#### 동작 흐름
1. Qt 클라이언트가 ID/PW 전송
2. 쿼리 방식으로 users 테이블 검색
3. 일치하는 행이 있으면 인증 성공 반환

**⚠️ 보안 주의사항**: 
- 현재 단순 쿼리 방식 사용 (SQL Injection 취약)
- 프로덕션 환경에서는 Prepared Statement 권장

---

### 2️⃣ **로깅 시스템 (log.h / log.cpp)**

**역할**: 시스템 로그, 로그인 기록, 분석 데이터를 비동기 큐 방식으로 DB 저장

#### 주요 기능
- `DBLogger` 클래스: 멀티스레드 안전 로깅
- 5가지 로그 타입 지원

| 로그 타입 | 용도 | 저장 데이터 |
|--------|------|---------|
| SYSTEM_LOG | 시스템 이벤트 | type, message |
| LOGIN_LOG | 로그인 기록 | username, ip, success |
| ANALYTICS_LOG | 부정승차 감지 | time, objType, x, y, event, age, photo_path |
| RECORDING_LOG | 녹화 파일 | filename |
| CLEANUP_DB_LOG | DB 청소 요청 | - |

#### 핵심 메서드
```cpp
void enqueue(const std::string& type, const std::string& message);
void enqueueLogin(const std::string& user, const std::string& ip, bool success);
void enqueueAnalytics(const std::string& time, const std::string& objType, 
                      int x, int y, const std::string& event, int age, 
                      const std::string& photoPath);
void parseAndLogXML(const char* xmlData);
```

#### 동작 흐름
```
enqueue() 호출
    ↓
LogItem을 로그 큐에 추가
    ↓
조건 변수로 워커 스레드 깨우기
    ↓
processQueue()가 비동기로 DB에 INSERT
```

#### 특징
- **비동기 처리**: 워커 스레드가 독립적으로 DB 저장
- **XML 파싱**: 카메라 메타데이터의 좌표/이벤트 정보 추출
- **스레드 안전**: 뮤텍스와 조건 변수 사용

**카메라 해상도 설정**: 기본값은 4K(3840x2160)입니다. 코드를 직접 수정하여 해상도를 변경합니다.

해상도 상수는 `include/log.h`에 정의되어 있습니다:

```cpp
int cam_width = 3840;   // default 4K width
int cam_height = 2160;  // default 4K height
```

수정 후 재빌드 및 재시작하면 `parseAndLogXML`이 변경된 해상도로 정규화 좌표를 픽셀 좌표로 변환합니다.

---

### 3️⃣ **비디오 녹화 모듈 (recorder.h / recorder.cpp)**

**역할**: RTSP 스트림을 연속으로 받아 60초 단위 MP4 파일로 분할 저장

#### 주요 기능
- FFmpeg 기반 RTSP 수신 및 MP4 인코딩
- 자가 서명 인증서(Self-Signed) 지원 (RTSPS)
- 타임스탬프 보정으로 매끄러운 영상 이어붙기
- 메타데이터 스트림 처리

#### 설정 상수
```cpp
static const char* VIDEO_SAVE_DIR = "/home/iam/finalProject/SFEPS/videos";
static const char* RTSP_URL = "rtsps://192.168.0.92:8332/cam1";
static const int SEGMENT_DURATION = 60; // 초
```

#### 핵심 메서드
```cpp
class RTSPRecorder {
    bool connect_and_record();      // 연결 & 녹화 루프
    bool open_output_file(...);     // 새 MP4 파일 오픈
    void close_current_file();      // 파일 종료 & 저장
};
```

#### 동작 흐름
```
연결 시도 (재시도)
    ↓
RTSP 스트림 열기
    ↓
영상 프레임 읽기
    ↓ (60초 경과 & 키프레임)
현재 파일 종료
    ↓
새 MP4 파일 생성
    ↓
프레임 쓰기
```

#### 특징
- **보안**: `tls_verify=0` 옵션으로 자가 인증서 허용
- **타임스탐프 관리**: DTS/PTS 오프셋으로 파일 이음새 제거
- **메타데이터**: XML 메타데이터를 로거로 전달

---

### 4️⃣ **파일 정리 모듈 (cleanup.h / cleanup.cpp)**

**역할**: 지정된 보관 기간을 초과한 오래된 MP4 녹화 파일 자동 삭제

#### 주요 기능
- 백그라운드 워커 스레드에서 주기적 정리
- 10초마다 검사
- 파일명 패턴 필터링 (`rec_*.mp4`)

#### 함수 시그니처
```cpp
void run_file_cleanup_worker(
    std::atomic<bool>& running_flag,      // 실행 플래그
    const std::string& save_dir,          // 대상 디렉토리
    long retention_sec = 60               // 보관 기간 (초)
);
```

#### 동작 흐름
```
while (running_flag) {
    디렉토리 스캔
        ↓
    rec_*.mp4 파일 발견
        ↓
    파일 생성 시간 확인
        ↓
    retention_sec 초과 → 삭제
        ↓
    10초 대기
}
```

#### 설정
```cpp
// main.cpp에서 스레드 시작
std::thread cleanup_t(run_file_cleanup_worker, 
                      std::ref(running), 
                      "/home/iam/finalProject/SFEPS/videos", 
                      600);  // 600초(10분) 보관
```

---

### 5️⃣ **RFID 모니터링 (rfid_monitor.h / rfid_monitor.cpp)**

**역할**: RPi의 RC522 RFID 드라이버가 제공하는 Unix 소켓에서 UID/나이 정보 수신

#### 주요 기능
- Unix Domain Socket (`/tmp/rc522_events.sock`) 연결
- NDJSON 형식 파싱
- 자동 재연결 메커니즘
- 소켓 타임아웃으로 신속한 종료

#### 클래스 구조
```cpp
class RfidMonitor {
    std::atomic<bool>& m_running;
    std::string m_socket_path;
    
    void run_loop();
    std::string extract_json_value(...);
    void save_to_db(const std::string& uid, 
                    const std::string& age_group, 
                    const std::string& time_str);
};
```

#### JSON 데이터 형식
```json
{"id": "12345ABCDE", "text": "5_15"}
```

#### 동작 흐름
```
1. Unix 소켓 생성/연결
   ↓
2. 데이터 수신 (타임아웃: 1초)
   ↓
3. NDJSON 파싱
   - "id" 추출 (UID)
   - "text" 추출 (나이 그룹)
   ↓
4. save_to_db() 호출 (DB 저장)
   ↓
5. 연결 종료 시 재시도
```

#### 특징
- **논블로킹**: 타임아웃으로 빠른 종료 가능
- **자동 재연결**: 데몬 재시작 시 자동 대응
- **간단한 JSON 파서**: 외부 라이브러리 무의존

---

### 6️⃣ **메인 서버 (main.cpp)**

**역할**: 전체 모듈 조율 및 클라이언트 통신 담당

#### 주요 스레드
```
메인 스레드
├── 인증 수신 스레드 (포트 5555)
├── 녹화 스레드
├── 파일 정리 스레드
├── 음성 수신 스레드 (포트 5556)
├── RFID 모니터링 스레드
└── 부정승차 알림 서버 (포트 5557)
```

#### 포트 설정
| 포트 | 용도 | 방향 |
|------|------|------|
| 5555 | 로그인 인증 | 양방향 |
| 5556 | 음성 데이터 | 수신 |
| 5557 | 부정승차 알림 | 송신 |

#### 신호 처리
```cpp
std::signal(SIGINT, signal_handler);   // Ctrl+C 처리
std::signal(SIGTERM, signal_handler);  // 종료 신호 처리
```

---

## 🔌 의존성

### 외부 라이브러리
```
MariaDB C API (libmysqlclient)
FFmpeg (libavformat, libavutil)
TinyXML2 (libtinyxml2)
POSIX (pthread, socket)
```

### 설치 (Ubuntu/Debian)
```bash
sudo apt-get install libmariadb-dev libavformat-dev libavutil-dev libtinyxml2-dev
```

---

## 🚀 빌드 & 실행

### 빌드
```bash
cd /home/iam/finalProject/SFEPS/server
mkdir -p build
cd build
cmake ..
make
```

### 실행
```bash
./smart_server
```

### 실행 로그 예시
```
[System] DB Connected. Worker Thread Started.
[System] Connecting to rtsps://192.168.0.92:8332/cam1 (Secure Mode)...
[System] Connected! Video Stream Index: 0
[Rec] Start: /home/iam/finalProject/SFEPS/videos/rec_20260213_143022.mp4
>> [RFID] 데몬 연결 성공! 데이터 수신 대기 중...
[Audio] Received 8192 bytes, playing...
```

---

## 🔍 데이터 흐름 다이어그램

```
┌─────────────┐
│ RTSP Camera │
└──────┬──────┘
       │ (비디오 스트림)
       ↓
┌──────────────────┐
│ RTSPRecorder     │
│ (FFmpeg)         │
└──────┬───────────┘
       │ (MP4 파일 저장)
       ↓
┌──────────────────┐      ┌──────────────┐
│ /home/.../videos │      │ DBLogger     │
└──────────────────┘      │ (스레드 큐)  │
                          └──────┬───────┘
                                 │ (비동기 저장)
                                 ↓
                          ┌──────────────┐
                          │ MariaDB      │
                          │ (로깅 테이블)│
                          └──────────────┘

┌──────────────────────┐
│ RPi RC522 RFID       │
│ (Kernel Driver)      │
└──────────┬───────────┘
           │ (Unix Socket)
           ↓
┌──────────────────────┐
│ RfidMonitor          │
│ (데이터 수신 & 파싱) │
└──────────┬───────────┘
           │ (DB 저장)
           ↓
        [DB]

┌──────────────────────┐
│ Qt 클라이언트        │
│ (포트 5555)          │
└──────────┬───────────┘
           │ (인증 요청)
           ↓
┌──────────────────────┐
│ Authenticator        │
│ (users 테이블 검증)  │
└──────────────────────┘

┌──────────────────────┐
│ 음성/고객센터        │
│ (포트 5556)          │
└──────────┬───────────┘
           │ (PCM 음성 데이터)
           ↓
┌──────────────────────┐
│ Audio Receiver       │
│ (aplay로 재생)       │
└──────────────────────┘
```

---

## ✅ 코드 품질 검토

### 장점
- ✅ **멀티스레드 안전**: 뮤텍스와 조건 변수 올바른 사용
- ✅ **자동 재연결**: 카메라/RFID 연결 실패 시 자동 복구
- ✅ **비동기 처리**: UI 블로킹 없는 로깅 및 녹화
- ✅ **모듈화**: 각 기능이 독립적 클래스로 분리
- ✅ **시그널 처리**: Ctrl+C 종료 시 안전한 정리

### 개선 권장사항
- ⚠️ **SQL Injection 방지**: Prepared Statement 사용 필요
- ⚠️ **에러 핸들링 강화**: 네트워크 에러 재시도 전략
- ⚠️ **설정 파일화**: 하드코딩된 IP/포트 → config.txt
- ⚠️ **로깅 레벨**: DEBUG/INFO/ERROR 구분
- ⚠️ **유닛 테스트**: 각 모듈에 대한 테스트 케이스 추가

---

## 📊 성능 최적화

### 1. DB 인덱싱
```sql
-- videos 테이블에 timestamp 인덱스
CREATE INDEX idx_video_time ON videos(recorded_at);

-- analytics 테이블에 이벤트 타입 인덱싱
CREATE INDEX idx_analytics_event ON analytics(event_type);
```

### 2. 비디오 품질 튜닝
```cpp
// recorder.h에서 비트레이트 설정 가능
// FFmpeg 옵션으로 인코딩 품질 조절
av_dict_set(&opts, "preset", "medium", 0);  // slow, medium, fast
```

### 3. 정리 주기 조정
```cpp
// 더 자주 정리 필요 시 run_file_cleanup_worker 호출 시
std::thread cleanup_t(run_file_cleanup_worker, 
                      std::ref(running), 
                      "/home/iam/finalProject/SFEPS/videos", 
                      300);  // 5분으로 단축
```

---

## 🐛 트러블슈팅

### 문제 1: RTSP 연결 실패
```
[Error] Failed to connect! Check IP, Port(8332), or Cert.
```
**해결책**:
- IP/포트 확인: `ping 192.168.0.92`
- RTSP URL 유효성: mediamtx 설정 확인
- 방화벽: `ufw allow 8332`

### 문제 2: DB 연결 실패
```
[DB Error] Access denied for user 'pi'@'192.168.0.92'
```
**해결책**:
- MariaDB 권한 설정: `GRANT ALL ON *.* TO 'pi'@'%'`
- DB_HOST IP 확인

### 문제 3: 오디오 재생 안 됨
```
[Audio] Failed to start aplay
```
**해결책**:
```bash
# ALSA 설정 확인
aplay -l

# 테스트 음성 재생
speaker-test -t wav -c 2 -l 1
```

### 문제 4: RFID 데이터 수신 안 됨
```
[RFID] 데몬 연결 끊김. 재접속 시도...
```
**해결책**:
- RC522 커널 드라이버 상태 확인
- Unix 소켓 존재 확인: `ls -la /tmp/rc522_events.sock`
- SPI 연결 확인: `dmesg | grep rc522`

---

## 📝 라이선스

본 프로젝트는 내부 용도로 제한됩니다.

---

## 👥 개발팀

**SFEPS Project Team** - Smart Fraud Examination Platform Server

마지막 업데이트: 2026-02-13
