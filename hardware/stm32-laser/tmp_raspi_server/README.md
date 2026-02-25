# tmp_raspi_server

라즈베리 파이에서 STM32/레이저 제어와 연동하는 서버 관련 코드를 둘 임시·테스트용 폴더입니다.

- **[AP_SETUP.md](AP_SETUP.md)** — 라즈베리 파이 AP 모드 설정 (ESP8266 ↔ Nucleo-F401RE 연결, Pi를 Wi‑Fi AP로 쓰기)
- **[Dev.md](Dev.md)** — 날짜별 개발/적용 내용 정리

---

## tmp_raspi_server 에서만 메타데이터 + 중심 좌표 테스트

DB 없이, **이 폴더만으로** 카메라 메타데이터와 Human 중심 좌표를 테스트할 수 있는 작은 프로그램을 만들었습니다.

### 1. 구성 파일

- `camera_config.h`  
  - 카메라 IP/포트/RTSP URL 설정
  - 필요 시 `CAMERA_IP`, `RTSP_URL` 을 실제 환경에 맞게 수정
- `rtsp_client_simple.h/.cpp`  
  - RTSP(TCP interleaved)로 메타데이터 채널(2)을 구독하는 최소 클라이언트
- `bbox_to_esp.h/.cpp`  
  - XML 문자열에서 첫 번째 Human 객체의 정규화 중심 좌표를 추출 (`extractFirstHumanCenter`)
  - (필요 시 `sendCenterToEsp` 로 ESP8266 TCP 엔드포인트로 전송 가능)
- `camera_meta_test.cpp`  
  - RTSP로 메타데이터를 받아서, 프레임이 바뀔 때마다 `extractFirstHumanCenter()` 를 호출해
    Human 중심 좌표를 콘솔에 출력하는 테스트용 메인
- `Makefile`  
  - 위 소스들을 빌드해 `camera_meta_test` 바이너리 생성

### 2. 빌드 & 실행 (카메라 메타데이터 테스트)

```bash
cd /home/physical-100/SFEPS/hardware/stm32-laser/tmp_raspi_server

# (필요 시) camera_config.h 안 CAMERA_IP / RTSP_URL 수정

make            # camera_meta_test 빌드
./camera_meta_test
```

정상적으로 카메라와 연결되면, 콘솔에 예를 들어 다음과 같은 로그가 출력됩니다.

```text
✅ Connected to Camera (192.168.0.30:554)
✅ Session ID: ...
🚀 Metadata streaming started (TCP interleaved 2-3)
[META] Human center = (0.5123, 0.4312) at RTP=...
```

Human 객체가 없으면:

```text
[META] No Human bbox found at RTP=...
```

이 프로그램으로 **카메라 → 메타데이터(XML) → Human 바운딩 박스 중심 좌표** 까지가 잘 나오는지 독립적으로 확인할 수 있습니다.

---

## C. ESP8266과 TCP 통신 테스트용 Raspi 서버

ESP8266이 **클라이언트**로서 `192.168.4.1:5555` 로 접속하고,  
라즈베리 파이에서 좌표/문자열을 보내는 테스트용 TCP 서버입니다.

### 1. 빌드

```bash
cd /home/physical-100/SFEPS/hardware/stm32-laser/tmp_raspi_server
make raspi_tcp_server
```

### 2. 실행

```bash
cd /home/physical-100/SFEPS/hardware/stm32-laser/tmp_raspi_server
./raspi_tcp_server          # 기본 포트 5555
# 또는 ./raspi_tcp_server 5555
```

- Pi 입장: `0.0.0.0:5555` 에서 클라이언트(ESP)를 기다립니다.
- ESP8266 쪽에서는 **AP에 붙은 뒤** `192.168.4.1:5555` 로 TCP 접속을 시도하면 됩니다.

터미널에 이렇게 나오면 정상:

```text
[TCP] Raspi TCP 서버 시작
[TCP] Listening on 0.0.0.0:5555
[TCP] ESP8266은 192.168.4.1:5555 로 접속하면 됨.
[TCP] 클라이언트 연결 대기 중...
```

ESP가 접속하면:

```text
[TCP] Client connected from 192.168.4.x:포트
[TCP] 좌표 입력 예시: "0.5 0.3" (x y)
[TCP] 일반 문자열도 전송 가능, "quit" 입력 시 연결 종료
> 
```

이때 라즈베리 파이 터미널에:

- `0.5 0.3` 처럼 **x y** 를 입력하면  
  → ESP 쪽으로 `CX=0.500000,CY=0.300000\n` 이 전송됩니다.
- 그냥 문자열을 입력하면  
  → 해당 문자열 + `\n` 이 그대로 ESP로 전송됩니다.

ESP8266 쪽에서는 이 문자열을 수신해서 **좌표 파싱 또는 디버그 출력**만 해 보면 통신이 잘 되는지 바로 확인할 수 있습니다.

---

## D. ESP8266으로 중심 좌표 넘기는 코드 (개요)

`tmp_raspi_server` 폴더에는 카메라에서 나온 XML을 이용해 **첫 번째 Human 객체의 중심 좌표**를 구하고,  
이를 ESP8266 TCP 엔드포인트로 전달하는 헬퍼 코드가 있습니다:

- `bbox_to_esp.h / bbox_to_esp.cpp`
  - `extractFirstHumanCenter(xml, center)`  
    → `Camera/get_metadata/src/XMLParser.cpp` 와 동일한 방식으로 `<tt:Object>` 블록에서 Human 타입의 좌표를 정규화해 `center.x`, `center.y` (0.0~1.0) 로 추출.
  - `sendCenterToEsp(center, esp_ip, esp_port)`  
    → ESP8266 쪽 TCP 서버/클라이언트로 `"CX=...,CY=...\\n"` 형식의 문자열을 전송.

실제 서비스에서는:

1. `Camera/get_metadata` 쪽에서 XML 메타데이터를 누적/파싱하고,  
2. 원하는 시점에 누적된 XML 문자열을 `extractFirstHumanCenter()` 에 넘겨 중심 좌표를 얻은 뒤,  
3. `sendCenterToEsp()` 로 ESP8266(예: `192.168.4.x:포트`)에 전달하는 흐름으로 통합하면 됩니다.

