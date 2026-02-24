문제: RFID 소켓이 연결되었다가 곧바로 끊기거나(EOF), 클라이언트에서 "Connection refused"가 반복 발생함

증상 요약
- `smart_server` 로그: 
  - ">> [RFID] 데몬 연결 성공! 데이터 수신 대기 중..." 이후 곧바로 ">> [RFID] 데몬 연결 끊김. 재접속 시도..." 반복
  - `[RFID DEBUG] read() returned 0 (EOF) from socket...` 출력
- 데몬(`rc522_uds_daemon`) 로그: `UDS: client connected (fd=...)` 출력 후 write 에러 또는 연결 종료 발생

원인
- 구현 초기에는 클라이언트에서 `read()`를 타임아웃 기반으로 폴링하였음. 데몬은 카드 태그가 발생할 때만 한 줄의 NDJSON을 전송하고, 평상시에는 전송이 없음.
- 이로 인해 클라이언트가 소켓에서 즉시 EOF(또는 read 반환 0)를 받는 상황이 발생했고, 경우에 따라 연결이 닫히며 재접속이 반복됨.

해결 전략 (요약)
- 소켓을 활성적으로 계속 `read()` 하지 말고, 이벤트가 발생했을 때만 읽도록 변경함.
- 구현 방식: `poll()`을 사용하여 읽기 가능 이벤트(`POLLIN`)가 발생할 때만 `read()`를 호출.

변경사항 요약
- `server/src/rfid_monitor.cpp`
  - 기존: `read()`에 소켓 recv 타임아웃을 설정하고 루프에서 `read()` 호출
  - 변경: `poll()` 사용 (1초 타임아웃) — `POLLIN` 이벤트가 발생하면 `read()` 수행. `POLLHUP`/`POLLERR` 처리 추가.
  - EOF 발생시( `read()` == 0 ) 디버그 로그 추가
- `hardware/.../rc522_uds_daemon.cpp`
  - `accept()`/`write()` 실패 시 `perror()` 출력 추가, 클라이언트 접속 로그 추가

핵심 코드(요약)
```cpp
// poll 대기 예시
struct pollfd pfd;
pfd.fd = sock_fd;
pfd.events = POLLIN;
int ret = poll(&pfd, 1, 1000); // 1초
if (ret > 0 && (pfd.revents & POLLIN)) {
    ssize_t n = read(sock_fd, buffer, sizeof(buffer)-1);
    // n>0 처리, n==0 EOF, n<0 오류 처리
}
```

빌드 & 테스트
1. 데몬(먼저 실행):
```bash
sudo /home/iam/finalProject/SFEPS/hardware/Raspi-driver/RC522_RFID/Server_examples/rc522_uds_daemon --no-daemon --socket /tmp/rc522_events.sock
```
2. 서버(다른 터미널):
```bash
cd /home/iam/finalProject/SFEPS/server/build
sudo ./smart_server
```
3. 데몬/서버 로그 확인: 데몬에서 `UDS write` 또는 `UDS accept` perror 출력이 있는지, 서버에서 `>>> [RFID Tag]` 로그가 카드 태그 후 한 줄만 찍히는지 확인

추가 권장
- 다중 소켓이나 높은 부하 환경이면 `epoll()`로 교체 권장
- C++ 비동기 라이브러리(`boost::asio`) 도입 시 더 깔끔한 이벤트 루프 구현 가능

결론
- 현재 구현(poll 기반)은 데몬이 데이터가 있을 때만 `smart_server`가 읽도록 하여 불필요한 재접속/EOF 문제를 해결합니다.

작성자: 개발팀
일시: 2026-02-12
# 리팩터링 완료: 분리된 아키텍처 및 부정 승차 감지 (2026-02-13)

## 📌 개요
기존 단일 `DBLogger` 중심의 구조를 개선하여 **비디오 저장**, **메타데이터 처리**, **이벤트 매칭**, **Qt 클라이언트 알림**을 각각 분리된 컴포넌트로 구현했습니다. 특히 카메라의 "first/second" 이벤트와 RFID 카드 정보를 비교하여 **부정 승차 사건**을 자동 감지하고 Qt 클라이언트에 알립니다.

## ✅ 분리된 아키텍처 구현

### 1. **Recorder (src/recorder.cpp, include/recorder.h)**
- **역할**: RTSP 스트림에서 비디오를 수신하여 파일로 저장
- **변경사항**:
  - 메타데이터 문자열을 `DBLogger::parseAndLogXML`에 직접 전달하지 않음
  - 대신 `AnalyticsProcessor::publishRaw(metadata_string)` 호출
  - `xml.find("MetadataStream")` 검사 제거 → 모든 메타데이터를 발행
- **책임**: 파일 생성 및 분할만 담당

### 2. **AnalyticsProcessor (src/analytics.cpp, include/analytics.h)** [新규 컴포넌트]
- **역할**: 메타데이터 파싱, 정규화, DB 저장, 이벤트 감지
- **주요 기능**:
  - `publishRaw(std::string)`: 메타데이터 원문을 큐에 추가
  - `processLine(std::string)`: 각 라인을 파싱하여:
    - 좌표 정규화 (카메라 해상도 3840×2160 기반)
    - `[TAG]` 형식 및 " | " 구분자 파싱
    - ID, Type, Pos(X,Y), Time, Event, Age 추출
  - `workerLoop()`: 별도 스레드에서 큐 처리
- **데이터 흐름**:
  ```
  Recorder -> publishRaw(metadata) -> AnalyticsProcessor queue -> processLine() 
    -> analytics_logs 테이블 INSERT
    -> EventMatcher 호출 (first/second 감지)
  ```

### 3. **EventMatcher (src/event_matcher.cpp, include/event_matcher.h)** [逐次 확장]
- **역할**: first/second 이벤트 대응 및 RFID 카드 비교
- **상태 관리**:
  - `Pending` 구조체 필드:
    - event_id: 카메라에서 받은 객체 ID
    - assigned_age: 랜덤 할당된 연령(Adult/Senior/Youth)
    - card_read: RFID 카드 읽었는지 여부
    - card_text: 카드의 나이 정보
    - card_id: RFID 카드 UID
    - card_age_group: 카드에서 읽은 연령 정보
    - gate_id: 게이트 ID (Gate3, Gate1 등)
    - est_age: 추정 나이(정수)
  - `pending` 맵: 게이트별로 대기 중인 first 이벤트 저장
- **주요 함수**:
  - `register_first(gate, event_id, assigned_age, gate_id, est_age)`:
    - 카메라 "first" 이벤트 감지 → Adult/Senior/Youth 중 무작위로 연령 할당
    - 게이트별 대기 상태 생성
  - `on_rfid_read(uid, card_text, card_id)`:
    - RFID 카드 읽음 → 가장 오래된 대기 이벤트와 페어링
    - 카드의 나이 정보(card_text) 저장
  - `on_second(gate, out_message)`:
    - 카메라 "second" 이벤트 감지 → assigned_age와 card_age_group 비교
    - **불일치 감지** → 메시지 생성: `FRAUD|cardId|ageGroup|gateId|estAge`
    - `send_alert_to_clients()` 호출하여 Qt에 전송

### 4. **alert.cpp (src/alert.cpp, include/alert.h)** [新규 파일]
- **역할**: Qt 클라이언트(포트 5557)에 부정 승차 알림 전송
- **함수**: `send_alert_to_clients(std::string msg)`
  - `g_client_sockets` 접근하여 모든 연결된 클라이언트에 메시지 전송
  - 연결 실패 시 소켓 제거

## ✅ 부정 승차 감지 동작 흐름

```
[1] 카메라 메타데이터 수신
    "[EVENT] Gate3first Active | ID: 48911 | Type: Human | Pos: (0.5, 0.5) | Time: 2026-02-13 11:37:21"
           ↓

[2] AnalyticsProcessor::processLine() 파싱
    - gate = "Gate3"
    - assigned_age = "Adult" (무작위: Adult/Senior/Youth 중 선택)
    - est_age = 30 (카메라에서 제공된 추정 나이)
           ↓

[3] EventMatcher에 등록
    EventMatcher::register_first("Gate3", "48911", "Adult", "Gate3", 30)
    Pending["Gate3"] 상태 생성 → 카드 대기 중
           ↓

[4] RFID 태그 읽음 (사용자 카드 태깅)
    RfidMonitor.save_to_db(uid="ABCD1234", age_group="Senior", time_str="...")
           ↓

[5] EventMatcher 페어링
    EventMatcher::on_rfid_read("ABCD1234", "Senior", "ABCD1234")
    - Pending["Gate3"].card_id = "ABCD1234"
    - Pending["Gate3"].card_age_group = "Senior"
    - Pending["Gate3"].card_read = true
           ↓

[6] 카메라 "second" 이벤트 감지
    "[EVENT] Gate3second Active | ID: 48986 | ... | Time: 2026-02-13 11:37:23"
           ↓

[7] 비교 및 감지
    EventMatcher::on_second("Gate3", msg)
    - assigned_age = "Adult" vs card_age_group = "Senior"
    - 불일치 감지! (Adult ≠ Senior)
           ↓

[8] 메시지 생성
    msg = "FRAUD|ABCD1234|Senior|Gate3|30"
    send_alert_to_clients(msg) 호출
           ↓

[9] Qt 클라이언트 수신 (포트 5557)
    "FRAUD|ABCD1234|Senior|Gate3|30"
    파싱:
      - cardId = "ABCD1234"
      - ageGroup = "Senior"
      - gateId = "Gate3"
      - estAge = 30
    → UI에 부정 승차 경고 표시
```

## ✅ Qt 클라이언트 통신 인터페이스

| 항목 | 값 |
|------|-----|
| 포트 | **5557** |
| 프로토콜 | TCP |
| 메시지 포맷 | `FRAUD\|cardId\|ageGroup\|gateId\|estAge` |
| 예시 | `FRAUD\|ABCD1234\|Senior\|Gate3\|30` |

### Qt 수신 처리 예시:
```cpp
QStringList parts = receivedData.split('|');
if (parts.size() == 5 && parts[0] == "FRAUD") {
    QString cardId = parts[1];       // "ABCD1234"
    QString ageGroup = parts[2];     // "Senior"
    int gateId = parts[3].toInt();   // 3
    int estAge = parts[4].toInt();   // 30
    
    // UI 업데이트
    fraudDetectedSignal(cardId, ageGroup, gateId, estAge);
}
```

## 🔧 구현 파일 및 변경 사항

### 新규 파일:
| 파일 | 용도 |
|------|------|
| `include/analytics.h` | AnalyticsProcessor 헤더 |
| `src/analytics.cpp` | AnalyticsProcessor 구현 |
| `include/event_matcher.h` | EventMatcher 헤더 (확장) |
| `src/event_matcher.cpp` | EventMatcher 구현 (확장) |
| `include/alert.h` | 알림 함수 헤더 |
| `src/alert.cpp` | 알림 함수 구현 |

### 수정 파일:
| 파일 | 변경 내용 |
|------|----------|
| `include/recorder.h` | AnalyticsProcessor 참조 추가, 생성자 시그니처 변경 |
| `src/recorder.cpp` | 메타데이터를 AnalyticsProcessor::publishRaw()로 전달 |
| `src/main.cpp` | AnalyticsProcessor 생성 및 start(), Recorder에 참조 전달 |
| `src/rfid_monitor.cpp` | on_rfid_read() 호출 시 card_id 전달 |
| `include/event_matcher.h` | register_first(), on_rfid_read() 시그니처 확장 |
| `src/event_matcher.cpp` | 메시지 포맷 변경 및 send_alert_to_clients 호출 |
| `src/analytics.cpp` | Config.h 포함, gate_id/est_age 전달, EventMatcher 호출 |
| `CMakeLists.txt` | 새 파일들(analytics.cpp, alert.cpp, event_matcher.cpp) 빌드 대상 추가 |

## 📊 데이터베이스

### analytics_logs 테이블 (기존):
```sql
CREATE TABLE analytics_logs (
  frame_time VARCHAR(100),           -- 메타데이터 타임스탬프
  object_type VARCHAR(50),           -- 객체 타입 (Human, Vehicle)
  x FLOAT,                           -- 픽셀 X 좌표
  y FLOAT,                           -- 픽셀 Y 좌표
  event VARCHAR(100),                -- 이벤트 설명 (first, second)
  estimated_age INT,                 -- 추정 나이
  photo_path VARCHAR(255),           -- 사진 경로
  created_at TIMESTAMP DEFAULT NOW() -- DB 삽입 시간
);
```

## 🛠️ 빌드 및 실행

```bash
cd /home/iam/finalProject/SFEPS/server/build
cmake ..
make -j2

# 서버 실행
./smart_server
```

**로그 예시:**
```
[DBLogger] Camera resolution (from code) set to 3840x2160
[System] DB Connected. Worker Thread Started.
[Analytics] Started.
[System] RFID 모니터링 서비스 시작됨.
[System] 녹화 시작...
[Matcher] Registered first for gate=Gate3 id=48911 age=Adult gate_id=Gate3 est_age=30
[Matcher] Paired RFID uid=ABCD1234 card_id=ABCD1234 age_group=Senior to gate=Gate3
[Matcher] FRAUD DETECTED: FRAUD|ABCD1234|Senior|Gate3|30
[Alert] Sent to clients: FRAUD|ABCD1234|Senior|Gate3|30
```

## 🎯 설계 장점

| 항목 | 설명 |
|------|------|
| 책임 분리 | 비디오 저장, 메타데이터 처리, 이벤트 매칭 각각 독립 |
| 확장성 | 외부 메시지 브로커(Kafka 등) 도입 시 AnalyticsProcessor 인터페이스만 변경 |
| 안정성 | 각 컴포넌트가 독립 스레드 → 한 쪽 부하가 다른 쪽에 영향 최소화 |
| 실시간 성능 | 부정 승차가 발생하는 즉시(second 이벤트 시) Qt에 알림 |
| 테스트 용이성 | 각 컴포넌트를 단위 테스트하기 용이 |

## 📝 향후 개선 항목

- [ ] Prepared Statements로 SQL injection 방지 (현재 `mysql_real_escape_string` 사용)
- [ ] analytics_logs 인덱스 추가 (frame_time, object_type 기준)
- [ ] 카드-이벤트 매칭 기록 로그 추가 (감사 추적용)
- [ ] 시스템 로그 파일 저장 및 로테이션
- [ ] 배치 INSERT로 DB 성능 개선
- [ ] 타임아웃 처리 (first 이벤트 후 N초 내 second 없으면 자동 제거)

작성자: 개발팀
일시: 2026-02-13
