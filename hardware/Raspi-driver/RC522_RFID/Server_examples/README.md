# RC522 서버 연동 가이드

라즈베리 파이에서 RC522 태깅 이벤트를 메인 서버에 전달하는 방법을 정리한 문서입니다.  
디바이스 1대 기준이며, 다중 기기 시 `device_id` 로 구분하면 됩니다.

**추천 구조**: **Hardware Driver (C++) ↔ Unix Domain Socket ↔ Main Server**  
→ 아키텍처·IPC 선택 이유는 **`document.md`** 참고.

---

## 1. C++ UDS 데몬 (권장)

메인 서버와 하드웨어를 분리할 때 사용하는 데몬입니다.

| 항목 | 내용 |
|------|------|
| **파일** | `rc522_uds_daemon.cpp` |
| **역할** | `/dev/rc522`를 열고 UDS **서버**로 listen. 메인 서버가 connect 하면 태깅 시 NDJSON 한 줄씩 전송. |
| **빌드** | `g++ -o rc522_uds_daemon rc522_uds_daemon.cpp -I../Kernel_Driver -std=c++17` |
| **실행** | `sudo ./rc522_uds_daemon [--no-daemon] [--socket PATH] [--trailer N]` |

**옵션**

- `--no-daemon`: 포그라운드 실행(디버깅용)
- `--socket PATH`: UDS 경로 (기본 `/tmp/rc522_events.sock`)
- `--trailer N`: 섹터 트레일러 블록 (기본 11)

**메인 서버**: 위 소켓 경로에 **connect** 한 뒤, 소켓에서 한 줄씩 읽으면 NDJSON 이벤트 수신.

---

## 2. 이벤트 데이터 형식 (NDJSON)

태깅 시 한 줄씩 전달되는 JSON 예:

```json
{"device_id":1,"id":"A1B2C3D4","text":"홍길동","timestamp":1707654321}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `device_id` | number | 리더 번호 (현재 1 고정) |
| `id` | string | 카드 UID 8자리 16진수 |
| `text` | string | 섹터 텍스트 (최대 48바이트, 실패 시 `""`) |
| `timestamp` | number | Unix 시각(초) |

REST API·WebSocket·로그 등 외부로 보낼 때 그대로 사용하면 됩니다.

---

## 3. 대안: 서버 내부 스레드에서 디바이스 직접 읽기

데몬을 쓰지 않고 **서버 프로세스 안에서** `/dev/rc522`를 읽고 싶을 때 참고용 예제입니다.

- **파일**: `rc522_device_thread_example.c`
- **패턴**: 스레드 하나가 `open("/dev/rc522")` 후 `ioctl(RC522_READ_CARD)` → `ioctl(RC522_READ_TEXT_SECTOR)` 루프, 콜백으로 구조체 전달.
- **빌드/실행**:  
  `gcc -o rc522_device_thread_example rc522_device_thread_example.c -I../Kernel_Driver -lpthread`  
  `sudo ./rc522_device_thread_example [--trailer N]`

내부는 구조체로 다루고, JSON은 클라이언트/API 응답으로 보낼 때만 직렬화하면 됩니다.

---

## 4. device_id (현재 1, 다중 기기 시)

- **현재**: 디바이스 1대 → `device_id` = **1** 고정.
- **다중 기기**: 기기/리더마다 `device_id` 를 1, 2, 3... 으로 두고, 서버에서 이 값으로 구분.

---

## 5. 정리

| 방식 | 용도 |
|------|------|
| **C++ UDS 데몬** | 메인 서버와 분리 구조. 데몬이 UDS 서버, 서버가 클라이언트로 connect 후 NDJSON 수신. |
| **디바이스 스레드 예제** | 서버 단일 프로세스 안에서 디바이스 직접 읽기 패턴 참고. |

드라이버(IRQ 버전) 빌드·오버레이·insmod는 상위 **`../README.md`** 참고.
