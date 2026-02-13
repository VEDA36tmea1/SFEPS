# RFID 데이터 흐름 (End-to-End 상세 설명)

## 시스템 아키텍처

```
┌─────────────────────┐
│   RC522 RFID 센서   │  실물 카드 태깅 이벤트
└──────────┬──────────┘
           │
    /dev/rc522 (kernel driver)
           │
┌──────────▼──────────────────────────────────────────┐
│   rc522_uds_daemon (Hardware Layer)                 │
│   - /dev/rc522 파일 열기 (카드 읽기)                  │
│   - RC522_READ_CARD ioctl 호출                       │
│   - RC522_READ_TEXT_SECTOR ioctl 호출                │
│   - NDJSON 포맷팅 ("{id, text, timestamp}")         │
│   - UDS 서버 (listen_fd) 대기                        │
└──────────┬──────────────────────────────────────────┘
           │
   Unix Domain Socket (UDS)
   /tmp/rc522_events.sock
           │
┌──────────▼──────────────────────────────────────────┐
│   RfidMonitor (Application Layer)                   │
│   - smart_server 내 별도 스레드                      │
│   - UDS 클라이언트 (connect)                         │
│   - poll() 으로 이벤트 대기                          │
│   - read() & JSON 파싱                              │
│   - save_to_db() 호출                               │
└──────────┬──────────────────────────────────────────┘
           │
┌──────────▼──────────────────────────────────────────┐
│   로그 & DB                                         │
│   - cout: ">>> [RFID Tag] UID: ... (...) Time: ..." │
│   - save_to_db() : DB에 저장 대기 (구현 예정)       │
└───────────────────────────────────────────────────────┘
```

## 단계별 흐름 (카드 한 번 태깅 시)

### 1. 카드 태깅
```
사용자: 카드reader에 카드 접근
         ↓
RC522 센서: 태그 감지 → 데이터 읽음
         ↓
Kernel Driver (/dev/rc522): 드라이버 인터럽트 발생
```

### 2. rc522_uds_daemon (데몬) 처리

**파일**: `hardware/Raspi-driver/RC522_RFID/Server_examples/rc522_uds_daemon.cpp`

```cpp
// main 루프 (line 147-193)
while (g_running) {
    int client_fd = accept(...);  // 1. 클라이언트 대기 (smart_server 연결)
    if (client_fd < 0) continue;

    while (g_running) {
        // 2. ioctl 호출: 카드 UID/텍스트 읽기
        if (ioctl(rc522_fd, RC522_READ_CARD, &uid) < 0) {
            usleep(100000);
            continue;  // 아직 카드 없음
        }

        // 3. 카드 데이터가 있을 때만 진행
        time_t ts = time(nullptr);
        struct rc522_read_text text_data {};
        text_data.trailer_block = g_trailer;
        if (ioctl(rc522_fd, RC522_READ_TEXT_SECTOR, &text_data) != 0) {
            // 텍스트 읽기 실패하면 빈 값
            text_data.uid = uid;
            text_data.text[0] = '\0';
        }

        // 4. JSON 포맷팅 (NDJSON)
        // 예: {"device_id":1,"id":"ABCD1234","text":"adult","timestamp":1707833456}\n
        std::string line = format_tag_event(text_data.uid, text_data.text, ts);

        // 5. UDS로 클라이언트에 한 줄 전송
        ssize_t n = write(client_fd, line.data(), line.size());
        if (n <= 0) {
            perror("UDS write");  // 쓰기 실패 → 클라이언트 종료
            close(client_fd);
            client_fd = -1;
            break;
        }
        usleep(500000);  // 0.5초 대기 후 다음 카드 기다림
    }
}
```

**출력 예시 (stderr)**
```
UDS: client connected (fd=5)
```

### 3. RfidMonitor (클라이언트) 수신 & 파싱

**파일**: `server/src/rfid_monitor.cpp`

#### 3-1. UDS 서버(데몬)에 connect

```cpp
// rfid_monitor.cpp run_loop() (line 68-149)

// 1. UDS 서버(데몬)에 connect
if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
    perror("rc522 socket connect");
    // 재접속 시도
    continue;
}

std::cout << ">> [RFID] 데몬 연결 성공! 데이터 수신 대기 중..." << std::endl;
```

**출력 예시**
```
>> [RFID] 데몬 연결 성공! 데이터 수신 대기 중...
```

#### 3-2. poll()로 이벤트 대기

```cpp
// 2. poll()로 데이터 도착 이벤트 대기 (1초 타임아웃)
struct pollfd pfd;
pfd.fd = sock_fd;
pfd.events = POLLIN;  // 읽기 가능 이벤트 감시

while (m_running) {
    int ret = poll(&pfd, 1, 1000);  // 1초 또는 이벤트 발생 시 반환
    if (ret < 0) {
        perror("poll");
        break;
    } else if (ret == 0) {
        // 타임아웃: 데이터 없음, 다시 대기
        continue;
    }

    // ret > 0: 이벤트 발생
    if (pfd.revents & POLLIN) {  // 읽기 가능
        // 다음 단계로
    }
}
```

**흐름**
- **카드 없을 때**: `poll()` 반환 0 → `continue` → 1초 대기 후 다시 `poll()`
- **카드 태깅 시점**: 데몬이 UDS로 한 줄 전송 → `poll()` 반환 > 0 (POLLIN) → `read()` 수행

#### 3-3. read() & 버퍼 처리

```cpp
    ssize_t n = read(sock_fd, buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        line_buffer += buffer;  // 버퍼에 축적

        // 줄바꿈(\n) 기준으로 완전한 JSON 한 줄씩 추출
        size_t pos;
        while ((pos = line_buffer.find('\n')) != std::string::npos) {
            std::string json_line = line_buffer.substr(0, pos);
            line_buffer.erase(0, pos + 1);
            // 다음 단계로
        }
    }
```

**수신 예시**
```
json_line = {"device_id":1,"id":"ABCD1234","text":"adult","timestamp":1707833456}
```

#### 3-4. JSON 파싱 & 데이터 추출

```cpp
try {
    // 3. JSON 문자열에서 각 field 추출 (간단 파서 사용, 라이브러리 없음)
    std::string uid = extract_json_value(json_line, "id");
    // extract_json_value() 함수에서:
    //  "id":"ABCD1234" 패턴 찾아 → "ABCD1234" 반환

    std::string age_group = extract_json_value(json_line, "text");
    // age_group = "adult"

    std::string now = get_current_datetime();
    // now = "2026-02-12 16:47:30"

    // 4. 로그 출력 (모니터링용)
    std::cout << ">>> [RFID Tag] UID: " << uid 
              << " (" << age_group << ") Time: " << now << std::endl;
    
    // 5. DB 저장 함수 호출
    save_to_db(uid, age_group, now);
} catch (...) {
    std::cerr << "[RFID] Parse Error" << std::endl;
}
```

**출력 예시**
```
>>> [RFID Tag] UID: ABCD1234 (adult) Time: 2026-02-12 16:47:30
[DB Save Request] UID: ABCD1234, Group: adult, Time: 2026-02-12 16:47:30
```

#### 3-5. save_to_db() 호출

```cpp
void RfidMonitor::save_to_db(const std::string& uid, 
                             const std::string& age_group, 
                             const std::string& time_str) {
    // 현재: 로그만 출력 (DB 연동 구현 대기)
    std::cout << "[DB Save Request] UID: " << uid 
              << ", Group: " << age_group 
              << ", Time: " << time_str << std::endl;
    
    // TODO: 실제 DB 구현
    // db_logger->insertRfidLog(uid, age_group, time_str);
}
```

## 전체 로그 흐름 예시 (실제 실행)

```
[System] RFID 모니터링 서비스 시작됨.
>> [RFID] 데몬 연결 성공! 데이터 수신 대기 중...

... 카드 없는 상태 (1초마다 poll() 타임아웃) ...

... 카드 태깅 발생 ...

>>> [RFID Tag] UID: ABCD1234 (adult) Time: 2026-02-12 16:47:30
[DB Save Request] UID: ABCD1234, Group: adult, Time: 2026-02-12 16:47:30

>>> [RFID Tag] UID: XYZ9999 (child) Time: 2026-02-12 16:47:35
[DB Save Request] UID: XYZ9999, Group: child, Time: 2026-02-12 16:47:35
```

## 핵심 포인트 정리

| 단계 | 담당 | 동작 | 출력 |
|------|------|------|------|
| 1 | RC522 센서 | 카드 감지 | (없음) |
| 2 | Kernel Driver | 데이터 레지스터 업데이트 | (없음) |
| 3 | rc522_uds_daemon | ioctl → JSON 생성 → write() | `UDS: client connected` (접속시만) |
| 4 | RfidMonitor (poll) | poll() 대기 (1초 타임아웃) | (없음, 카드 없을 때는 침묵) |
| 5 | RfidMonitor (read) | read() on 이벤트 | (없음, 내부 처리) |
| 6 | RfidMonitor (parse) | extract_json_value | (없음, 내부 처리) |
| 7 | RfidMonitor (log) | cout | `>>> [RFID Tag] UID: ... (...) Time: ...` |
| 8 | RfidMonitor (db) | save_to_db | `[DB Save Request] UID: ..., Group: ..., Time: ...` |

## 다음 작업 (예정)

- `save_to_db()` 함수 구현: DBLogger를 사용해 MySQL에 실제 INSERT 수행
- 에러 처리 강화: 데몬 응답 없음, JSON 파싱 오류, DB 연결 실패 등
- 성능 최적화: 다중 클라이언트 지원 (epoll으로 교체 가능)
