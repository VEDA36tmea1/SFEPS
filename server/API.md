# SFEPS Server - API 명세서 & 데이터 포맷

## 📡 통신 프로토콜

### 포트 설정

| 포트 | 프로토콜 | 용도 | 방향 |
|------|---------|------|------|
| 5555 | TCP | 로그인 인증 | 양방향 |
| 5556 | TCP | 음성 데이터 | 수신 |
| 5557 | TCP | 부정승차 알림 | 송신 |

### 네트워크 환경

```
┌─────────────────────────────────────────────┐
│              로컬 네트워크                   │
│         192.168.0.0/24 서브넷               │
├─────────────────────────────────────────────┤
│
├─ RPi 4B (server)        192.168.0.50
│  ├─ Port 5555 (auth)
│  ├─ Port 5556 (voice)
│  ├─ Port 5557 (alert)
│  └─ RFID → /tmp/rc522_events.sock
│
├─ Hanwha Camera          192.168.0.92
│  └─ Port 8332 (RTSP)
│
├─ MariaDB Server         192.168.0.92
│  └─ Port 3306 (MySQL)
│
└─ Qt Client              192.168.x.x
   └─ Port 5555 (auth)
```

---

## 🔐 인증 API (포트 5555)

### 요청 형식

#### 형식 1: 텍스트 기반 (간단)
```
<ID>|<PASSWORD>
```

**예시:**
```
admin|password123
```

#### 형식 2: JSON 기반 (확장성 좋음)
```json
{
    "action": "login",
    "id": "admin",
    "password": "password123",
    "timestamp": "2026-02-13T14:30:22Z"
}
```

### 응답 형식

#### 성공 응답
```
OK
```

또는 JSON:
```json
{
    "status": "success",
    "message": "Authentication successful",
    "user": {
        "id": "admin",
        "name": "Administrator",
        "role": "admin"
    },
    "token": "eyJhbGciOiJIUzI1NiIs..."
}
```

#### 실패 응답
```
FAIL
```

또는 JSON:
```json
{
    "status": "error",
    "code": 401,
    "message": "Invalid credentials"
}
```

### 예제 구현

#### C++ 서버
```cpp
void handle_auth_connection(int client_fd, Authenticator& auth) {
    char buffer[256];
    ssize_t bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);
    
    if (bytes > 0) {
        buffer[bytes] = '\0';
        
        // 파싱: "admin|password123"
        std::string input(buffer);
        size_t delim = input.find('|');
        if (delim != std::string::npos) {
            std::string id = input.substr(0, delim);
            std::string pw = input.substr(delim + 1);
            
            // 공백 제거
            id.erase(id.find_last_not_of(" \n\r\t")+1);
            pw.erase(pw.find_last_not_of(" \n\r\t")+1);
            
            // 인증
            bool success = auth.authenticate(id, pw);
            
            // 응답
            const char* response = success ? "OK" : "FAIL";
            send(client_fd, response, strlen(response), 0);
        }
    }
    
    close(client_fd);
}
```

#### Python 클라이언트
```python
import socket

def login(host, port, username, password):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    
    # 요청 전송
    request = f"{username}|{password}"
    sock.sendall(request.encode())
    
    # 응답 수신
    response = sock.recv(1024).decode()
    sock.close()
    
    if response == "OK":
        print(f"✓ 로그인 성공: {username}")
        return True
    else:
        print(f"✗ 로그인 실패")
        return False

# 사용
login("192.168.0.50", 5555, "admin", "password123")
```

---

## 🎙️ 음성 API (포트 5556)

### 오디오 형식

| 항목 | 값 |
|------|-----|
| 샘플 레이트 | 16000 Hz (16 kHz) |
| 채널 | 1 (Mono) |
| 비트 깊이 | 16-bit (PCM_S16_LE) |
| 프레임당 샘플 | 가변 |

### 데이터 흐름

```
┌─────────────────────────────────┐
│ Qt Client / App                 │
│ (음성 녹음)                     │
└────────────┬────────────────────┘
             │ TCP 스트림 (Port 5556)
             ↓
┌─────────────────────────────────┐
│ Server (run_audio_receiver)     │
│                                 │
│1. 소켓에서 수신                │
│2. 버퍼에 저장                  │
│3. 파일 종료 후 aplay로 재생   │
└─────────────────────────────────┘
             │
             ↓
    [ALSA 스피커 재생]
```

### 예제 구현

#### C++ 서버
```cpp
void run_audio_receiver() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {AF_INET, htons(5556), {INADDR_ANY}};
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);
    
    std::cout << "[Audio] Listening on port 5556..." << std::endl;

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        
        // 음성 데이터 수신
        std::vector<char> audio_buffer;
        char buf[4096];
        ssize_t bytes;
        while ((bytes = read(client_fd, buf, sizeof(buf))) > 0) {
            audio_buffer.insert(audio_buffer.end(), buf, buf + bytes);
        }
        close(client_fd);

        if (audio_buffer.empty()) {
            std::cout << "[Audio] Empty data" << std::endl;
            continue;
        }

        std::cout << "[Audio] Received " << audio_buffer.size() << " bytes" << std::endl;

        // aplay로 재생
        FILE* aplay = popen("aplay -f S16_LE -r 16000 -c 1", "w");
        if (aplay) {
            fwrite(audio_buffer.data(), 1, audio_buffer.size(), aplay);
            pclose(aplay);
        }
    }
}
```

#### Python 클라이언트
```python
import socket
import numpy as np
from scipy.io import wavfile

def send_audio(host, port, audio_file):
    """
    음성 파일을 서버에 송신
    
    Args:
        host: 서버 IP (예: "192.168.0.50")
        port: 포트 (5556)
        audio_file: WAV 파일 경로
    """
    # WAV 파일 읽기
    sample_rate, audio_data = wavfile.read(audio_file)
    
    # 16-bit PCM으로 변환
    if audio_data.dtype != np.int16:
        audio_data = (audio_data * 32767).astype(np.int16)
    
    # 연결
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    
    # 전송
    sock.sendall(audio_data.tobytes())
    sock.close()
    
    print(f"✓ 음성 전송 완료: {len(audio_data)} samples")

# 사용
send_audio("192.168.0.50", 5556, "voice_message.wav")
```

---

## 🚨 알림 API (포트 5557)

### 알림 데이터 형식

#### JSON 포맷
```json
{
    "alert_type": "fraud_detected",
    "severity": "HIGH",
    "timestamp": "2026-02-13T14:30:22Z",
    "details": {
        "camera_id": "cam1",
        "person_id": "12345ABCDE",
        "age_group": "5_15",
        "event": "intrusion",
        "location": {
            "x": 450,
            "y": 280
        },
        "photo_url": "/videos/snapshot_001.jpg"
    },
    "action": "ALERT_USER"
}
```

#### 텍스트 포맷
```
FRAUD_ALERT|12345ABCDE|5_15|intrusion|2026-02-13T14:30:22Z
```

### 알림 타입

| 타입 | 심각도 | 설명 |
|------|--------|------|
| `fraud_detected` | HIGH | 부정승차 감지 |
| `suspicious_activity` | MEDIUM | 의심 활동 |
| `system_error` | LOW | 시스템 오류 |
| `auth_failure` | MEDIUM | 인증 실패 |

### 예제 구현

#### C++ 서버 (브로드캐스트)
```cpp
std::vector<int> g_client_sockets;
std::mutex g_sockets_mutex;

void broadcast_alert(const std::string& alert_json) {
    std::lock_guard<std::mutex> lock(g_sockets_mutex);
    
    for (auto it = g_client_sockets.begin(); it != g_client_sockets.end();) {
        if (send(*it, alert_json.c_str(), alert_json.length(), 0) < 0) {
            // 연결 끊김
            close(*it);
            it = g_client_sockets.erase(it);
            std::cerr << "[Alert] Client disconnected" << std::endl;
        } else {
            ++it;
        }
    }
    
    std::cout << "[Alert] Broadcasted to " << g_client_sockets.size() 
              << " clients" << std::endl;
}

// 사용: 부정승차 감지 시
std::string alert = R"({
    "alert_type": "fraud_detected",
    "severity": "HIGH",
    "timestamp": "2026-02-13T14:30:22Z",
    "details": {
        "person_id": "12345ABCDE",
        "age_group": "5_15",
        "event": "intrusion"
    }
})";
broadcast_alert(alert);
```

#### Python 클라이언트 (수신)
```python
import socket
import json

def receive_alerts(host, port):
    """알림 서버에 연결하고 실시간 알림 수신"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    
    print(f"✓ 알림 서버 연결: {host}:{port}")
    
    try:
        while True:
            # 수신
            data = sock.recv(4096).decode('utf-8')
            if not data:
                break
            
            # JSON 파싱
            alert = json.loads(data)
            
            # 처리
            if alert['alert_type'] == 'fraud_detected':
                person_id = alert['details']['person_id']
                age = alert['details']['age_group']
                event = alert['details']['event']
                
                print(f"""
╔════════ 부정승차 감지 ════════╗
║ UID: {person_id}
║ 나이: {age}
║ 이벤트: {event}
║ 시간: {alert['timestamp']}
╚══════════════════════════╝
                """)
    finally:
        sock.close()

# 백그라운드에서 실행
import threading
thread = threading.Thread(target=receive_alerts, args=("192.168.0.50", 5557))
thread.daemon = True
thread.start()
```

---

## 📊 데이터베이스 스키마

### login_logs 테이블

```sql
CREATE TABLE login_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(100) NOT NULL,
    ip_address VARCHAR(45),
    success BOOLEAN DEFAULT FALSE,
    login_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_user (username),
    INDEX idx_time (login_at),
    INDEX idx_success (success)
);
```

**쿼리 예시:**
```sql
-- 최근 10건 로그인 기록
SELECT username, ip_address, success, login_at 
FROM login_logs 
ORDER BY login_at DESC 
LIMIT 10;

-- 오늘 실패한 로그인 시도
SELECT COUNT(*) as failed_attempts, username
FROM login_logs
WHERE success = 0 
  AND DATE(login_at) = CURDATE()
GROUP BY username
ORDER BY failed_attempts DESC;
```

### system_logs 테이블

```sql
CREATE TABLE system_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    type VARCHAR(50),
    message VARCHAR(500),
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_type (type),
    INDEX idx_time (timestamp)
);
```

**예시 데이터:**
```
| type | message | timestamp |
|------|---------|-----------|
| SYSTEM | RTSP Connected via TLS (Secure) | 2026-02-13 14:30:00 |
| ERROR | Failed to open output file | 2026-02-13 14:30:01 |
| SYSTEM | Video segment saved | 2026-02-13 14:31:00 |
```

### analytics_logs 테이블

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
    INDEX idx_time (event_time),
    INDEX idx_age (age)
);
```

**예시 데이터:**
```sql
INSERT INTO analytics_logs 
(obj_type, x, y, event, age, photo_path, event_time)
VALUES
('person', 450, 280, 'intrusion', 28, '/videos/snap_001.jpg', '2026-02-13 14:30:22');
```

### recording_logs 테이블

```sql
CREATE TABLE recording_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    filename VARCHAR(255),
    saved_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    file_size BIGINT,
    duration INT,  -- 초 단위
    INDEX idx_time (saved_at),
    INDEX idx_filename (filename)
);
```

---

## 📨 RFID 이벤트 데이터

### 소켓 경로
```
/tmp/rc522_events.sock  (Unix Domain Socket)
```

### 데이터 형식 (NDJSON)

```json
{"id": "12345ABCDE", "text": "5_15"}
```

### 필드 설명

| 필드 | 타입 | 크기 | 설명 | 예시 |
|------|------|------|------|------|
| `id` | String | 10 | RFID 태그 UID | "12345ABCDE" |
| `text` | String | 고정 | 나이 그룹 | "5_15", "16_25" |

### 나이 그룹 매핑

```
"1_5"    → 1~5세
"6_10"   → 6~10세
"11_15"  → 11~15세 (미성년자)
"16_25"  → 16~25세 (청년)
"26_35"  → 26~35세 (성인)
"36_50"  → 36~50세 (중년)
"51_65"  → 51~65세 (노인)
"65_"    → 65세 이상 (고령)
```

### 수신 예제

```cpp
// RC522 데이터 수신 및 파싱
std::string json_line = "{\"id\": \"12345ABCDE\", \"text\": \"5_15\"}";

// 파싱
std::string uid = extract_json_value(json_line, "id");
std::string age_group = extract_json_value(json_line, "text");

// 출력
std::cout << "UID: " << uid << ", Age: " << age_group << std::endl;
// 출력: UID: 12345ABCDE, Age: 5_15
```

---

## 🎥 XML 메타데이터 (카메라)

### 형식

```xml
<?xml version="1.0"?>
<tt:MetadataStream xmlns:tt="http://www.onvif.org/ver10/schema/onvif">
    <tt:VideoAnalytics>
        <tt:Frame UtcTime="2026-02-13T14:30:22Z">
            <tt:Object ObjectId="1">
                <tt:Appearance>
                    <tt:Position X="450" Y="280"/>
                </tt:Appearance>
            </tt:Object>
            <tt:Object ObjectId="2">
                <tt:Appearance>
                    <tt:Position X="620" Y="350"/>
                </tt:Appearance>
            </tt:Object>
        </tt:Frame>
    </tt:VideoAnalytics>
</tt:MetadataStream>
```

### 파싱 예제

```cpp
void DBLogger::parseAndLogXML(const char* xmlData) {
    XMLDocument doc;
    if (doc.Parse(xmlData) != XML_SUCCESS) return;

    XMLElement* root = doc.FirstChildElement("tt:MetadataStream");
    if (!root) return;

    XMLElement* frame = root->FirstChildElement("tt:VideoAnalytics")
                           ->FirstChildElement("tt:Frame");
    if (!frame) return;

    // Frame 속성
    const char* utcTime = frame->Attribute("UtcTime");

    // 각 Object (감지된 물체) 처리
    XMLElement* obj = frame->FirstChildElement("tt:Object");
    while (obj) {
        int obj_id = std::atoi(obj->Attribute("ObjectId"));
        
        XMLElement* pos = obj->FirstChildElement("tt:Appearance")
                            ->FirstChildElement("tt:Position");
        int x = std::atoi(pos->Attribute("X"));
        int y = std::atoi(pos->Attribute("Y"));

        std::cout << "Object " << obj_id << ": (" << x << ", " << y << ")" << std::endl;
        
        // DB에 저장
        enqueueAnalytics(utcTime, "person", x, y, "detected", 0, "");

        obj = obj->NextSiblingElement("tt:Object");
    }
}
```

---

## 📋 통신 프로토콜 요약

### TCP 3-Way Handshake + 데이터 전송

```
┌─────────────────────────────────────────────┐
│ Phase 1: Connection Establishment           │
├─────────────────────────────────────────────┤
│ Client → Server: SYN                        │
│ Server → Client: SYN-ACK                    │
│ Client → Server: ACK                        │
└─────────────────────────────────────────────┘
                      ↓
┌─────────────────────────────────────────────┐
│ Phase 2: Data Transfer                      │
├─────────────────────────────────────────────┤
│ Client → Server: [REQUEST]                  │
│ Server → Client: [RESPONSE]                 │
└─────────────────────────────────────────────┘
                      ↓
┌─────────────────────────────────────────────┐
│ Phase 3: Connection Termination             │
├─────────────────────────────────────────────┤
│ Client → Server: FIN                        │
│ Server → Client: ACK                        │
│ Server → Client: FIN                        │
│ Client → Server: ACK                        │
└─────────────────────────────────────────────┘
```

---

## 🔧 API 테스트 명령어

### 포트 5555 (인증)
```bash
# Telnet으로 테스트
telnet 192.168.0.50 5555
> admin|password123
< OK

# nc로 테스트
echo -n "admin|password123" | nc 192.168.0.50 5555

# curl 사용 불가 (HTTP 아님)
```

### 포트 5556 (음성)
```bash
# WAV 파일 전송
cat voice.wav | nc -w 1 192.168.0.50 5556

# arecord로 실시간 스트림
arecord -f S16_LE -r 16000 -c 1 | nc 192.168.0.50 5556
```

### 포트 5557 (알림)
```bash
# 수신만 가능
nc -l 192.168.0.50 5557

# 별도의 nc로 테스트
echo '{"alert_type":"test"}' | nc 192.168.0.50 5557
```

---

마지막 업데이트: 2026-02-13

