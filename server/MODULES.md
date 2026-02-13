# SFEPS Server - 모듈별 상세 명세서

## 📑 목차
1. [인증 모듈 (auth)](#1-인증-모듈-authh--authcpp)
2. [로깅 모듈 (log)](#2-로깅-모듈-logh--logcpp)
3. [비디오 녹화 모듈 (recorder)](#3-비디오-녹화-모듈-recorderh--recordercpp)
4. [파일 정리 모듈 (cleanup)](#4-파일-정리-모듈-cleanh--cleanupcpp)
5. [RFID 모니터링 (rfid_monitor)](#5-rfid-모니터링-rfid_monitorh--rfid_monitorcpp)

---

## 1. 인증 모듈 (auth.h / auth.cpp)

### 📌 목적
Qt 클라이언트의 로그인 요청을 받아 MariaDB에서 사용자 정보를 검증

### 🏗️ 클래스 설계

```cpp
class Authenticator {
private:
    MYSQL* conn;         // MariaDB 연결 핸들
    const char* host;    // DB 호스트 (예: "192.168.0.92")
    const char* user;    // DB 사용자명 (예: "pi")
    const char* pass;    // DB 비밀번호 (예: "raspberry")
    const char* db_name; // 데이터베이스명 (예: "Client_db")
    std::mutex dbMutex;  // 멀티스레드 동시 접근 보호

public:
    Authenticator(const char* host, const char* user, 
                  const char* pass, const char* db);
    ~Authenticator();
    
    bool connect();
    bool authenticate(const std::string& id, const std::string& pw);
};
```

### 🔄 API 명세

#### `Authenticator::Authenticator()`
```cpp
Authenticator auth("192.168.0.92", "pi", "raspberry", "Client_db");
```
- 인증 객체 생성 및 DB 설정 초기화
- 실제 연결은 `connect()` 호출 시 수행

#### `bool Authenticator::connect()`
```cpp
if (auth.connect()) {
    std::cout << "DB Connected" << std::endl;
} else {
    std::cerr << "Connection Failed" << std::endl;
}
```
- MariaDB에 TCP 연결 (기본 포트: 3306)
- 반환값: 성공 시 true, 실패 시 false

#### `bool Authenticator::authenticate()`
```cpp
if (auth.authenticate("admin", "password123")) {
    std::cout << "인증 성공" << std::endl;
} else {
    std::cout << "인증 실패" << std::endl;
}
```
- 사용자 ID와 비밀번호 검증
- DB 쿼리: `SELECT id FROM users WHERE id='admin' AND password='password123'`
- 반환값: 결과 행이 1개 이상이면 true

### 💾 데이터베이스 스키마

```sql
CREATE TABLE users (
    id VARCHAR(50) PRIMARY KEY,
    password VARCHAR(100) NOT NULL,
    name VARCHAR(100),
    role VARCHAR(20),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

### ⚠️ 보안 고려사항

#### 현재 구현의 문제점
```cpp
// ❌ SQL Injection 취약
std::string query = "SELECT id FROM users WHERE id = '" + id + "' AND password = '" + pw + "'";
```

#### 개선안: Prepared Statement
```cpp
// ✅ Prepared Statement 사용 권장
MYSQL_STMT* stmt = mysql_stmt_init(conn);
const char* query = "SELECT id FROM users WHERE id = ? AND password = ?";
mysql_stmt_prepare(stmt, query, strlen(query));

// 파라미터 바인딩
MYSQL_BIND bind[2];
memset(bind, 0, sizeof(bind));
bind[0].buffer = (char*)id.c_str();
bind[0].buffer_length = id.length();
mysql_stmt_bind_param(stmt, bind);
```

### 🧵 스레드 안전성

- `std::mutex dbMutex`로 동시 쿼리 보호
- 여러 스레드에서 동시에 인증 요청 가능

```cpp
{
    std::lock_guard<std::mutex> dbLock(dbMutex);
    // 이 블록 내에서만 DB 작업 안전
    mysql_query(conn, query.c_str());
}
```

### 🔍 사용 예제

```cpp
// main.cpp에서 인증 처리
void handle_auth_client(int client_fd) {
    Authenticator auth("192.168.0.92", "pi", "raspberry", "Client_db");
    auth.connect();
    
    // 클라이언트에서 ID/PW 수신
    char buffer[256];
    recv(client_fd, buffer, sizeof(buffer), 0);
    
    // 파싱: "admin|password123"
    std::string id = ...;
    std::string pw = ...;
    
    // 인증
    bool success = auth.authenticate(id, pw);
    
    // 결과 응답
    send(client_fd, success ? "OK" : "FAIL", 4, 0);
}
```

---

## 2. 로깅 모듈 (log.h / log.cpp)

### 📌 목적
5가지 종류의 로그를 멀티스레드 안전 큐에 모아 비동기로 DB 저장

### 🏗️ 클래스 설계

```cpp
struct LogItem {
    LogType type;              // 로그 타입
    std::string str1, str2;    // 공통 텍스트 필드
    std::string time_str;      // 이벤트 발생 시간
    int x, y;                  // 좌표
    std::string event;         // 감지된 이벤트
    int age;                   // 추정 나이
    std::string photo_path;    // 사진 파일 경로
    bool login_success;        // 로그인 성공 여부
};

enum LogType { 
    SYSTEM_LOG,      // 시스템 이벤트
    LOGIN_LOG,       // 로그인 기록
    ANALYTICS_LOG,   // 부정승차 분석
    RECORDING_LOG,   // 비디오 녹화
    CLEANUP_DB_LOG   // DB 청소
};

class DBLogger {
private:
    MYSQL* conn;
    std::queue<LogItem> logQueue;    // 로그 버퍼
    std::thread workerThread;         // 비동기 처리 스레드
    std::mutex queueMutex;            // 큐 보호
    std::condition_variable cv;       // 워커 스레드 깨우기
    std::atomic<bool> isRunning;      // 실행 상태

private:
    void processQueue();              // 워커 함수
    void parseAndLogXML(...);         // XML 파싱

public:
    DBLogger(const char* db = "CCgbd");
    bool connect();
    void enqueue(...);                // 일반 로그
    void enqueueLogin(...);           // 로그인 로그
    void enqueueAnalytics(...);       // 분석 로그
    void enqueueRecording(...);       // 녹화 파일 기록
    void parseAndLogXML(const char* xmlData);
};
```

### 🔄 API 및 사용 예제

#### 1️⃣ 시스템 로그
```cpp
logger.enqueue("SYSTEM", "RTSP Connected via TLS (Secure)");
logger.enqueue("ERROR", "Failed to open output file");

// DB에 저장되는 쿼리:
// INSERT INTO system_logs (type, message, timestamp) 
// VALUES ('SYSTEM', 'RTSP Connected...', NOW());
```

#### 2️⃣ 로그인 로그
```cpp
logger.enqueueLogin("admin", "192.168.a.1", true);      // 성공
logger.enqueueLogin("invalid_user", "192.168.1.100", false);  // 실패

// DB에 저장:
// INSERT INTO login_logs (username, ip, success, login_at) 
// VALUES ('admin', '192.168.1.1', 1, NOW());
```

#### 3️⃣ 분석 로그 (카메라 감지 데이터)
```cpp
logger.enqueueAnalytics(
    "2026-02-13T14:30:22",           // 이벤트 발생 시간
    "person",                        // 객체 타입
    450, 280,                        // X, Y 좌표
    "intrusion",                     // 감지 이벤트
    28,                              // 추정 나이
    "/videos/snapshot_001.jpg"       // 사진 경로
);

// DB에 저장:
// INSERT INTO analytics_logs (obj_type, x, y, event, age, photo_path, event_time)
// VALUES ('person', 450, 280, 'intrusion', 28, '...', '2026-02-13 14:30:22');
```

#### 4️⃣ 녹화 파일 기록
```cpp
logger.enqueueRecording("/home/iam/finalProject/SFEPS/videos/rec_20260213_143022.mp4");

// DB에 저장:
// INSERT INTO recording_logs (filename, saved_at)
// VALUES ('rec_20260213_143022.mp4', NOW());
```

#### 5️⃣ XML 메타데이터 파싱
```cpp
const char* xmlData = R"(<?xml version="1.0"?>
<tt:MetadataStream xmlns:tt="http://www.onvif.org/ver10/schema/onvif">
    <tt:VideoAnalytics>
        <tt:Frame UtcTime="2026-02-13T14:30:22Z">
            <tt:Object ObjectId="1">
                <tt:Appearance>
                    <tt:Position X="450" Y="280"/>
                </tt:Appearance>
            </tt:Object>
        </tt:Frame>
    </tt:VideoAnalytics>
</tt:MetadataStream>)";

logger.parseAndLogXML(xmlData);
```

### 🔄 비동기 처리 흐름

```
┌─────────────────────┐
│ 메인 스레드         │
└──────┬──────────────┘
       │
       ├─► enqueue() 호출
       │   ↓
       ├─► LogItem을 queue에 추가
       │   ↓
       ├─► cv.notify_one()
       │   ↓
       └── 즉시 반환 (논블로킹)

                    ┌──────────────────────┐
                    │ 워커 스레드          │
                    └──────┬───────────────┘
                           │
                           ├─► processQueue() 루프
                           │   ↓
                           ├─► 조건 변수 대기
                           │   ↓
                           ├─► notify 수신 시 깨어남
                           │   ↓
                           ├─► queue에서 LogItem 가져오기
                           │   ↓
                           ├─► DB에 INSERT 실행 (블로킹)
                           │   ↓
                           └── queue가 비워질 때까지 반복
```

### 📊 로깅 타입별 DB 테이블

#### system_logs
```sql
CREATE TABLE system_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    type VARCHAR(50),
    message VARCHAR(500),
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_time (timestamp)
);
```

#### login_logs
```sql
CREATE TABLE login_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(100),
    ip_address VARCHAR(45),
    success BOOLEAN,
    login_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_user (username),
    INDEX idx_time (login_at)
);
```

#### analytics_logs
```sql
CREATE TABLE analytics_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    obj_type VARCHAR(50),
    x INT, y INT,
    event VARCHAR(50),
    age INT,
    photo_path VARCHAR(255),
    event_time DATETIME,
    saved_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_event (event),
    INDEX idx_time (event_time)
);
```

---

## 3. 비디오 녹화 모듈 (recorder.h / recorder.cpp)

### 📌 목적
RTSP 스트림을 수신하여 60초 단위 MP4 파일로 분할 저장 + 메타데이터 추출

### 🏗️ 클래스 설계

```cpp
class RTSPRecorder {
private:
    // FFmpeg 컨텍스트
    AVFormatContext *input_ctx;      // RTSP 입력
    AVFormatContext *output_ctx;     // MP4 출력
    int video_stream_idx;            // 비디오 스트림 인덱스
    int meta_stream_idx;             // 메타데이터 스트림 인덱스
    
    // 시간 관리
    time_t start_time;               // 파일 시작 시간
    std::string current_filename;    // 현재 파일명
    
    // 타임스탬프 보정
    int64_t last_dts;
    int64_t start_dts_offset;
    bool is_first_packet;
    
    // 참조
    DBLogger& logger;
    std::atomic<bool>& running_flag;

private:
    bool connect_and_record();
    bool open_output_file(AVCodecParameters* par);
    void close_current_file();
    void cleanup();

public:
    RTSPRecorder(DBLogger& logger, std::atomic<bool>& running_flag);
    ~RTSPRecorder();
    void run();
};
```

### ⚙️ 설정 상수

```cpp
// 비디오 저장 경로
static const char* VIDEO_SAVE_DIR = "/home/iam/finalProject/SFEPS/videos";

// RTSP 스트림 URL (카메라 IP 및 포트)
static const char* RTSP_URL = "rtsps://192.168.0.92:8332/cam1";

// 파일 분할 주기 (초)
static const int SEGMENT_DURATION = 60;
```

### 🔄 주요 함수

#### `RTSPRecorder::run()`
```cpp
void RTSPRecorder::run() {
    while (running_flag) {
        if (!connect_and_record()) {
            std::cerr << "[System] Connection Retry in 5s..." << std::endl;
        }
        cleanup();
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}
```
- RTSP 연결 시도
- 5초 간격으로 자동 재연결

#### `bool RTSPRecorder::connect_and_record()`

```cpp
// 1. RTSP 옵션 설정
AVDictionary* opts = nullptr;
av_dict_set(&opts, "rtsp_transport", "tcp", 0);
av_dict_set(&opts, "stimeout", "5000000", 0);
av_dict_set(&opts, "tls_verify", "0", 0);  // Self-Signed 인증서 허용

// 2. 스트림 열기
avformat_open_input(&input_ctx, RTSP_URL, nullptr, &opts);

// 3. 스트림 정보 조회
avformat_find_stream_info(input_ctx, nullptr);

// 4. 비디오 스트림 찾기
video_stream_idx = av_find_best_stream(
    input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0
);

// 5. 첫 MP4 파일 생성
open_output_file(input_ctx->streams[video_stream_idx]->codecpar);

// 6. 프레임 읽기 & 쓰기 루프
while (running_flag) {
    av_read_frame(input_ctx, &pkt);
    
    // 60초 경과 + 키프레임 감지 → 파일 분할
    if (std::time(nullptr) - start_time >= SEGMENT_DURATION 
        && (pkt.flags & AV_PKT_FLAG_KEY)) {
        close_current_file();
        open_output_file(...);
    }
    
    // 타임스탬프 보정 후 MP4에 쓰기
    // ...
    av_interleaved_write_frame(output_ctx, &pkt);
}
```

#### `bool RTSPRecorder::open_output_file()`

```cpp
// 현재 시간으로 파일명 생성
current_filename = "/home/iam/.../videos/rec_20260213_143022.mp4";

// MP4 포맷 컨텍스트 생성
avformat_alloc_output_context2(&output_ctx, nullptr, "mp4", current_filename.c_str());

// 비디오 스트림 복사
AVStream* out = avformat_new_stream(output_ctx, nullptr);
avcodec_parameters_copy(out->codecpar, par);
out->codecpar->codec_tag = 0;

// 파일 오픈
avio_open(&output_ctx->pb, current_filename.c_str(), AVIO_FLAG_WRITE);

// MP4 헤더 쓰기
avformat_write_header(output_ctx, nullptr);

// 타임스탬프 상태 초기화
last_dts = AV_NOPTS_VALUE;
start_dts_offset = AV_NOPTS_VALUE;
is_first_packet = true;
```

#### `void RTSPRecorder::close_current_file()`

```cpp
// MP4 트레일러 쓰기 (필수!)
av_write_trailer(output_ctx);

// 파일 종료
avio_closep(&output_ctx->pb);
avformat_free_context(output_ctx);

// DB에 기록
logger.enqueueRecording(current_filename);

std::cout << "[Rec] Saved: " << current_filename << std::endl;
```

### 🔐 보안: Self-Signed 인증서 처리

```cpp
// RTSP 연결 대기 시 "SSL certificate problem" 에러 방지
av_dict_set(&opts, "tls_verify", "0", 0);

// 이 옵션이 없으면:
// [Error] Failed to connect! Check IP, Port(8332), or Cert.
```

### ⏱️ 타임스탬프 보정 로직

문제: RTSP 스트림을 여러 파일로 나누면 각 파일의 시작 시간이 필요
- 파일 1: 0초 ~ 60초
- 파일 2: 60초 ~ 120초 (연속성)

```cpp
// 첫 패킷의 DTS를 기준점(offset)으로 저장
if (is_first_packet) {
    if (pkt.dts != AV_NOPTS_VALUE) {
        start_dts_offset = pkt.dts;
        is_first_packet = false;
    }
}

// 모든 타임스탬프에서 offset 빼기
if (start_dts_offset != AV_NOPTS_VALUE) {
    if (pkt.dts != AV_NOPTS_VALUE) pkt.dts -= start_dts_offset;
    if (pkt.pts != AV_NOPTS_VALUE) pkt.pts -= start_dts_offset;
}

// DTS 단조성 보장 (DTS는 항상 증가해야 함)
if (last_dts != AV_NOPTS_VALUE && pkt.dts <= last_dts) {
    int64_t diff = last_dts + 1 - pkt.dts;
    pkt.dts += diff;
    if (pkt.pts != AV_NOPTS_VALUE) pkt.pts += diff;
}
last_dts = pkt.dts;
```

### 📊 파일 생성 예시

```
/home/iam/finalProject/SFEPS/videos/
├── rec_20260213_143022.mp4  (60초)
├── rec_20260213_143122.mp4  (60초)
├── rec_20260213_143222.mp4  (60초)
└── ...
```

---

## 4. 파일 정리 모듈 (cleanup.h / cleanup.cpp)

### 📌 목적
지정된 보관 기간을 초과한 오래된 MP4 파일을 자동 삭제

### 🔄 함수 명세

```cpp
void run_file_cleanup_worker(
    std::atomic<bool>& running_flag,      // 실행 플래그
    const std::string& save_dir,          // 모니터링 디렉토리
    long retention_sec = 60               // 보관 기간 (초)
);
```

### 📋 파라미터 설명

| 파라미터 | 타입 | 설명 |
|---------|------|------|
| `running_flag` | `std::atomic<bool>&` | false일 때 종료 |
| `save_dir` | `std::string` | 정리할 디렉토리 (예: `/home/.../videos`) |
| `retention_sec` | `long` | 파일 보관 기간 (초 단위, 기본값 60초) |

### 💾 사용 예제

```cpp
// main.cpp에서 정리 스레드 시작
std::atomic<bool> running{true};

// 10분(600초) 보관 후 삭제
std::thread cleanup_t(
    run_file_cleanup_worker, 
    std::ref(running), 
    "/home/iam/finalProject/SFEPS/videos", 
    600
);

// 프로그램 종료 시
running = false;
cleanup_t.join();
```

### 🔄 동작 흐름

```cpp
void run_file_cleanup_worker(...) {
    while (running_flag) {
        try {
            if (fs::exists(save_dir)) {
                // 현재 시간
                auto now = fs::file_time_type::clock::now();
                
                // 디렉토리 모든 파일 순회
                for (const auto& entry : fs::directory_iterator(save_dir)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        
                        // 파일명 패턴 필터링: rec_*.mp4
                        if (filename.rfind("rec_", 0) == 0 &&  // rec_로 시작
                            filename.length() >= 4 &&
                            filename.compare(filename.length() - 4, 4, ".mp4") == 0) {
                            
                            // 파일 나이 계산
                            auto ftime = fs::last_write_time(entry);
                            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                                now - ftime
                            ).count();
                            
                            // 보관 기간 초과 시 삭제
                            if (age >= retention_sec) {
                                std::cout << "[Cleanup] Del: " << filename 
                                          << " (Age: " << age << "s)" << std::endl;
                                fs::remove(entry.path());
                            }
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Cleanup Error] " << e.what() << std::endl;
        }
        
        // 10초마다 검사
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}
```

### 📊 동작 결과 로그

```
[Cleanup] Del: rec_20260213_142022.mp4 (Age: 601s)
[Cleanup] Del: rec_20260213_142122.mp4 (Age: 541s)
[Cleanup] Del: rec_20260213_142222.mp4 (Age: 481s)
```

### ⚙️ 권장 설정

```cpp
// 상황별 retention_sec 값
// - 테스트: 60초
// - 일일 저장: 86400초 (24시간)
// - 주간 저장: 604800초 (1주)
// - 월간 보관: 2592000초 (30일)

std::thread cleanup_t(
    run_file_cleanup_worker,
    std::ref(running),
    VIDEO_SAVE_DIR,
    2592000  // 30일 보관
);
```

---

## 5. RFID 모니터링 (rfid_monitor.h / rfid_monitor.cpp)

### 📌 목적
RPi의 RC522 RFID 커널 드라이버가 제공하는 Unix 소켓에서 UID/나이 정보 실시간 수신

### 🏗️ 클래스 설계

```cpp
class RfidMonitor {
private:
    std::atomic<bool>& m_running;          // 실행 플래그
    std::string m_socket_path;             // Unix 소켓 경로
    std::string m_db_host, m_db_user;
    std::string m_db_pass, m_db_name;

private:
    void run_loop();
    std::string extract_json_value(const std::string& json, 
                                   const std::string& key);
    std::string get_current_datetime();
    void save_to_db(const std::string& uid, 
                    const std::string& age_group, 
                    const std::string& time_str);

public:
    RfidMonitor(std::atomic<bool>& running_flag,
                const std::string& db_host,
                const std::string& db_user,
                const std::string& db_pass,
                const std::string& db_name);
    ~RfidMonitor();
    void start();
};
```

### 🔌 Unix Domain Socket 통신

```cpp
// 소켓 경로
std::string m_socket_path = "/tmp/rc522_events.sock";

// RPi RC522 드라이버가 이 소켓에 UID 데이터를 보냄
// 스마트 서버가 이 소켓에서 수신
```

### 📨 데이터 형식

#### 수신 형식 (NDJSON - Newline Delimited JSON)
```json
{"id": "12345ABCDE", "text": "5_15"}
{"id": "67890FGHIJ", "text": "16_25"}
{"id": "ABCDE12345", "text": "26_35"}
```

- `id`: RFID 태그 UID (10자리)
- `text`: 나이 그룹 (형식: `min_max`, 예: "5_15" = 5~15세)

#### 파싱 예제
```cpp
std::string json_line = "{\"id\": \"12345ABCDE\", \"text\": \"5_15\"}";

std::string uid = extract_json_value(json_line, "id");      // "12345ABCDE"
std::string age_group = extract_json_value(json_line, "text");  // "5_15"

// 실행 결과
std::cout << "UID: " << uid << ", Age: " << age_group << std::endl;
// UID: 12345ABCDE, Age: 5_15
```

### 💾 사용 예제

```cpp
// main.cpp에서 RFID 모니터링 시작
std::atomic<bool> running{true};

RfidMonitor rfid_monitor(
    running,
    "192.168.0.92",      // DB 호스트
    "pi",                // DB 사용자
    "raspberry",         // DB 비밀번호
    "Client_db"          // 데이터베이스명
);

// 스레드에서 실행
std::thread rfid_t([&rfid_monitor]() {
    rfid_monitor.start();
});

// 프로그램 종료 시
running = false;
rfid_t.join();
```

### 🔄 동작 흐름

```
메인 루프 (바깥쪽)
├─ Unix 소켓 생성
├─ /tmp/rc522_events.sock 연결 시도
└─ 연결 실패 시 1초 대기 후 재시도
   
   데이터 수신 루프 (안쪽)
   ├─ 1초 타임아웃으로 read() 호출
   ├─ 데이터 수신 시
   │  ├─ line_buffer에 누적
   │  ├─ \n 단위로 JSON 라인 추출
   │  ├─ extract_json_value()로 파싱
   │  └─ save_to_db() 호출
   ├─ 연결 끊김 (-1 반환)
   │  └─ 안쪽 루프 탈출 → 재연결
   └─ EOF 수신 (0 반환)
      └─ 안쪽 루프 탈출 → 재연결
```

### 🧵 타임아웃 설정

```cpp
// 블로킹 소켓에 1초 타임아웃 설정
struct timeval tv;
tv.tv_sec = 1;       // 1초
tv.tv_usec = 0;      // 0 마이크로초
setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

// 효과:
// - read()가 1초마다 반환
// - Ctrl+C 종료 시 최대 1초 지연
// - CPU 점유율 감소
```

### 🔍 JSON 파싱 구현

```cpp
std::string RfidMonitor::extract_json_value(
    const std::string& json, 
    const std::string& key
) {
    // 키 검색: "id":
    std::string search = "\"" + key + "\":";
    size_t start = json.find(search);
    if (start == std::string::npos) return "";

    start += search.length();
    
    if (json[start] == '"') {
        // 문자열 값: "value"
        start++;
        size_t end = json.find("\"", start);
        return (end == std::string::npos) ? "" : 
               json.substr(start, end - start);
    } else {
        // 숫자 값: 123
        size_t end = json.find_first_of(",}", start);
        return (end == std::string::npos) ? 
               json.substr(start) : 
               json.substr(start, end - start);
    }
}
```

### 🕐 시간 포맷팅

```cpp
std::string RfidMonitor::get_current_datetime() {
    time_t now = time(nullptr);
    struct tm tstruct = *localtime(&now);
    char buf[80];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tstruct);
    return std::string(buf);
}

// 결과: "2026-02-13 14:30:22"
```

### 📊 로그 출력 예시

```
>> [RFID] 데몬 연결 성공! 데이터 수신 대기 중...
>>> [RFID Tag] UID: 12345ABCDE (5_15) Time: 2026-02-13 14:30:22
>>> [RFID Tag] UID: 67890FGHIJ (16_25) Time: 2026-02-13 14:30:45
>> [RFID] 데몬 연결 끊김. 재접속 시도...
```

### ⚠️ 트러블슈팅

#### 문제: "connect: No such file or directory"
```
[RFID] 데몬 연결 끊김. 재접속 시도...
rc522 socket connect: No such file or directory
```

**해결책**:
- RC522 드라이버 상태 확인
- 소켓 파일 존재 확인: `ls -la /tmp/rc522_events.sock`
- RC522 서비스 재시작: `systemctl restart rc522`

#### 문제: "Parse Error"
**수신 데이터가 형식이 이상할 때**

해결책: JSON 데이터 검증
```cpp
try {
    std::string uid = extract_json_value(json_line, "id");
    if (uid.empty()) {
        std::cerr << "[RFID] Empty UID: " << json_line << std::endl;
    }
} catch (...) {
    std::cerr << "[RFID] Parse Error" << std::endl;
}
```

---

## 🔗 모듈 간 통신 다이어그램

```
┌─────────────────────────────────────────────────────────────┐
│                        메인 스레드                           │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ Ctrl+C 시그널 처리 및 플래그 제어                   │  │
│  │ (running = false)                                   │  │
│  └──────────────────────────────────────────────────────┘  │
└────────┬────────────────────┬─────────────────────┬──────────┘
         │                    │                     │
    ┌────▼─────┐        ┌─────▼──────┐      ┌──────▼─────┐
    │ 인증      │        │ 비디오     │      │ RFID       │
    │ 스레드    │        │ 녹화       │      │ 모니터링   │
    │ (5555)    │        │ 스레드     │      │ 스레드     │
    │           │        │            │      │            │
    │ Auth      │        │ Recorder   │      │ RfidMonitor│
    └────┬──────┘        └────┬───────┘      └──────┬─────┘
         │                    │ (VideoPath)         │
         │                    │                     │
         ├────────────────────┼────────────────────┤
         │                    │                    │
         ▼                    ▼                    ▼
    ┌────────────────────────────────────────────────────┐
    │           DBLogger (로깅 큐 + 워커)               │
    │  ┌──────────────────────────────────────────────┐ │
    │  │ enqueue() → logQueue → processQueue()        │ │
    │  │ (비동기 처리, 멀티스레드 안전)              │ │
    │  └──────────────────────────────────────────────┘ │
    └────────┬───────────────────────────────────────────┘
             │ (SQL INSERT)
             ▼
    ┌────────────────────────────┐
    │      MariaDB               │
    │  • login_logs              │
    │  • analytics_logs          │
    │  • recording_logs          │
    │  • system_logs             │
    └────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ 보조 스레드들                                               │
├─────────────────────────────────────────────────────────────┤
│ • 음성 수신 (포트 5556) → aplay로 스피커 출력            │
│ • 파일 정리 (cleanup_worker) → 오래된 MP4 삭제          │
│ • 부정승차 알림 (포트 5557) → Qt 클라이언트로 전송      │
└─────────────────────────────────────────────────────────────┘
```

---

## ✅ 모듈 구현 상태

| 모듈 | 파일 | 상태 | 비고 |
|------|------|------|------|
| 인증 | auth.h/.cpp | ✅ 완성 | SQL Injection 방지 개선 필요 |
| 로깅 | log.h/.cpp | ✅ 완성 | XML 파싱 기능 포함 |
| 녹화 | recorder.h/.cpp | ✅ 완성 | 타임스탬프 보정 구현 |
| 정리 | cleanup.h/.cpp | ✅ 완성 | 성능 최적화 가능 |
| RFID | rfid_monitor.h/.cpp | ✅ 완성 | DB 저장 로직 필요 |

