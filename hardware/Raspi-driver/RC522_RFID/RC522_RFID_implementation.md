# RC522 RFID 구현 가이드 (Raspberry Pi 4)

요청한 핀맵 기준으로, 먼저 `wiringPi`를 이용한 사용자 공간 C 테스트 코드를 만들고, 이후 리눅스 커널 SPI 드라이버로 확장하는 절차를 단계별로 정리했다.

---

## 0) 핀맵 확인 (요청 기준)

| RC522 핀 | RPi 4 Physical | BCM | 역할 |
| --- | --- | --- | --- |
| SDA (SS) | Pin 24 | GPIO8 | SPI CS0 |
| SCK | Pin 23 | GPIO11 | SPI Clock |
| MOSI | Pin 19 | GPIO10 | SPI MOSI |
| MISO | Pin 21 | GPIO9 | SPI MISO |
| GND | Pin 6/9/14 | GND | 공통 접지 |
| RST | Pin 22 | GPIO25 | Reset 출력 |
| 3.3V | Pin 1 | 3.3V | 전원 |
| IRQ | Pin 18 | GPIO24 | 인터럽트 입력 (초기에는 선택) |

주의:
- RC522는 `3.3V` 동작이다. `5V` 연결 금지.
- 초기 bring-up 단계에서는 `IRQ` 없이도 카드 UID 읽기까지 구현 가능하다.

---

## 1) 1단계 목표: 사용자 공간 C 테스트 먼저 성공

핵심 목표:
1. SPI 통신 정상 여부 확인 (`VersionReg` 읽기)
2. RC522 초기화
3. 태그 감지/UID 읽기

이 단계는 커널 드라이버보다 디버깅이 빠르고, 하드웨어 문제를 먼저 분리할 수 있다.

---

## 2) 라즈베리파이 설정

```bash
sudo raspi-config
```

- `Interface Options -> SPI -> Enable`
- 재부팅 후 확인:

```bash
ls -l /dev/spidev0.0
```

정상이라면 `/dev/spidev0.0`가 보여야 한다.

---

## 3) 사용자 공간 C 코드 구조 (wiringPi + SPI)

`wiringPi`의 `wiringPiSPIDataRW()`를 사용하면 빠르게 테스트 가능하다.

파일 예시:
- `src/rc522_user.c`
- `Makefile`

### 3-1. RC522 레지스터 접근 로직

RC522 SPI 프레임:
- 주소 전송 시 `((addr << 1) & 0x7E)` 형식 사용
- 읽기는 MSB(`0x80`) set

샘플 코드는 길이가 길어져서 여기서는 생략합니다.

- 최소 테스트/데모 코드는 저장소의 아래 파일을 참고하세요.
  - `Kernel_Driver/`로 포팅하기 전에 사용자 공간에서 bring-up 확인:  
    `User_Space_test_src/rc522_full.c`, `User_Space_test_src/rc522_full_demo.c`
  - 커널 드라이버 구현(현재):  
    `Kernel_Driver/rc522_spi.c`, `Kernel_Driver/rc522_core.c`, `Kernel_Driver/rc522_chardev.c`

핵심은 “레지스터 접근이 **2바이트 full-duplex** 프레임으로 이뤄진다”는 점입니다(읽기 시 TX+RX 동시).

### 3-2. Makefile 예시

Makefile 샘플은 생략합니다. 현재 저장소에서는 다음을 참고하세요.

- 사용자 공간 데모 빌드: `User_Space_test_src/Makefile`
- 커널 모듈/테스트/DTBO 빌드: `Kernel_Driver/Makefile`

### 3-3. 빌드/실행

```bash
make
sudo ./rc522_user
```

정상 기대:
- `RC522 VersionReg = 0x91` 또는 `0x92` 계열
- 태그를 가까이 대면 UID 출력

### 3-4. Python `SimpleMFRC522` 스타일 C 래퍼 (`rc522_full.c` / `rc522_full_demo.c`)

위의 예제는 레지스터/프레임을 직접 다루는 최소 예제이고, 실제로는 Python의 `mfrc522.SimpleMFRC522`처럼
**UID 읽기 / 섹터 단위 텍스트 읽기/쓰기를 한 번에 제공하는 C API**를 쓰면 훨씬 편하다.

- 파일 구조:
  - `src/rc522_full.h` : 공개 API 헤더
  - `src/rc522_full.c` : Python `MFRC522`/`BasicMFRC522`를 C로 포팅한 구현
  - `src/rc522_full_demo.c` : Python `SimpleMFRC522`에 해당하는 데모 프로그램

헤더에서 노출되는 주요 함수:

```c
// SPI 및 RC522 초기화
//  spi_ch : 0 -> /dev/spidev0.0(CE0), 1 -> /dev/spidev0.1(CE1)
//  speed_hz : 예) 1000000
//  rst_bcm  : RST에 연결된 BCM GPIO (예: 25, RST를 3.3V 고정이면 -1)
int rc522c_init(int spi_ch, int speed_hz, int rst_bcm);

// UID 한 번만 시도 (태그 없으면 -1)
int rc522c_read_id_no_block(uint32_t *out_id);

// 태그가 나올 때까지 blocking
int rc522c_read_id_blocking(uint32_t *out_id);

// 섹터(트레일러 블록 기준, 예: 11)의 3개 데이터 블록(총 48바이트)을 텍스트로 읽기
int rc522c_read_text_sector_blocking(int trailer_block,
                                     uint32_t *out_id,
                                     char *out_text,
                                     size_t max_len);

// 섹터(3블록)에 텍스트 쓰기 (최대 48바이트, 부족분 0 패딩)
int rc522c_write_text_sector_blocking(int trailer_block,
                                      const char *text,
                                      uint32_t *out_id);
```

데모 프로그램(`rc522_full_demo.c`)은 위 API를 감싸서 Python의 `SimpleMFRC522`처럼 동작한다:

```bash
# 기본: trailer=11(섹터 2)의 3개 데이터 블록을 읽어서 ID/TEXT 출력
./rc522_full_demo

# UID만 읽기 (SimpleMFRC522.read_id()와 유사)
./rc522_full_demo --id

# 다른 섹터 trailer 지정 (예: 섹터 3의 trailer=15)
./rc522_full_demo --trailer 15

# 텍스트 쓰기 (섹터 trailer=11 기준)
./rc522_full_demo --write "hello rc522"

# SPI 채널/속도/RST 핀 변경
./rc522_full_demo --ch 0 --speed 1000000 --rst 25
./rc522_full_demo --ch 1 --speed 500000 --rst -1   # RST를 3.3V 고정한 경우
```

이 구조를 기반으로 애플리케이션에서는

- UID만 필요한 경우: `rc522c_read_id_blocking()` 호출
- 태그에 사용자 데이터를 저장/조회해야 할 경우:
  - `rc522c_write_text_sector_blocking()`으로 문자열 쓰기
  - `rc522c_read_text_sector_blocking()`으로 문자열 읽기

를 사용하면, Python 코드(`SimpleMFRC522`, `BasicMFRC522`, `MFRC522`)와 거의 동일한 추상 수준으로 C에서 RC522를 다룰 수 있다.

---

## 4) 1단계 디버깅 체크리스트

1. `VersionReg`가 `0x00` / `0xFF`만 나오면:
   - SPI enable 상태 재확인
   - CE0(GPIO8), SCLK(GPIO11), MOSI(GPIO10), MISO(GPIO9) 배선 재확인
   - GND 공통 확인
2. RST 배선 문제:
   - GPIO25가 HIGH 유지되는지 확인
3. 전원 문제:
   - `3.3V` 전원만 사용
4. SPI 속도 낮춰 재시도:
   - `SPI_SPEED`를 `500000` 또는 `250000`으로 변경

---

## 5) 2단계: 커널 드라이버로 확장하는 흐름

사용자 공간 코드로 동작이 검증되면, 커널 드라이버에서는 계층을 분리한다.

권장 구성:
- `drivers/misc/rc522/rc522_core.c` (레지스터 제어, anti-collision)
- `drivers/misc/rc522/rc522_spi.c` (SPI probe/remove)
- `drivers/misc/rc522/rc522_chardev.c` (`/dev/rc5220`, read/ioctl)
- `include/uapi/linux/rc522_ioctl.h` (유저 API)

### 5-1. 최소 커널 드라이버 골격

커널 드라이버 골격 예시 코드는 생략합니다. 현재 저장소의 실제 구현은 아래를 기준으로 보시면 됩니다.

- SPI 버스/디바이스 등록: `Kernel_Driver/rc522_spi.c`
- MFRC522 코어 로직: `Kernel_Driver/rc522_core.c`
- `/dev/rc522` chardev + ioctl: `Kernel_Driver/rc522_chardev.c`

### 5-2. Device Tree Overlay 예시

`CE0 + GPIO25(RST) + GPIO24(IRQ)` 기준:

DTO 샘플 코드는 생략합니다. 현재 저장소의 오버레이는 아래를 사용합니다.

- 기본 오버레이: `Kernel_Driver/rc522-overlay.dts` / `Kernel_Driver/rc522-overlay.dtbo`
- 대안 오버레이: `Kernel_Driver/rc522-overlay-nospidev.dts` / `Kernel_Driver/rc522-overlay-nospidev.dtbo`

IRQ(GPIO24)를 사용하는 **인터럽트 기반 모드**는 “다음 단계(8장)”에서 적용할 예정입니다(아래 “## 8) IRQ 기반 인터럽트 모드로 전환” 참고).

---

## 6) 단계별 로드맵 (실무 순서)

1. **배선 + SPI 활성화**
2. **사용자 공간 C(wiringPi)에서 `VersionReg` 읽기 성공**
3. **REQA + Anticollision로 UID 출력 성공**
4. **에러/타임아웃/재시도 로직 안정화**
5. **커널 SPI 드라이버 probe/remove 구현**
6. **커널 내부 레지스터 read/write 함수 이식**
7. **char device + ioctl API 제공**
8. **IRQ 기반 이벤트 처리(선택)**
9. **udev 규칙/서비스화(systemd)**

### 6-1. 📝 RC522 리눅스 커널 드라이버 구현 체크리스트

이 체크리스트는 제안한 3개의 모듈 파일과 헤더 파일을 중심으로, 개발 환경 설정부터 최종 테스트까지의 흐름을 다룹니다.

**현재 구현 상태 요약** (Kernel_Driver 기준):  
- ✅ 완료(코드/테스트 포함): 빌드/DTO/드라이버(SPI+코어+chardev)/IOCTL+read()/UID+텍스트 읽기/지속 폴링 테스트/시그널(EINTR) 처리  
- 🧩 다음 단계(계획): **IRQ(GPIO24) 기반 인터럽트 모드**로 전환(폴링 최소화)

2026-02-11 기준.

---

#### 1. 개발 환경 및 빌드 설정 (Environment Setup)

라즈베리 파이 커널 헤더 설치 및 빌드 시스템을 구축합니다.

- [x] **커널 헤더 설치**: 현재 실행 중인 커널 버전에 맞는 헤더 파일 설치 (`sudo apt install raspberrypi-kernel-headers`)
- [x] **Makefile 작성**:
  - [x] `obj-m` 변수에 모듈 오브젝트 지정 (`rc522.o` — 문서의 `rc522_driver.o`와 동일 역할)
  - [x] 멀티 파일 컴파일 설정 (`rc522-y := rc522_core.o rc522_spi.o rc522_chardev.o`)
  - [x] 커널 소스 경로 지정 (`KERNEL_SRC`, 문서의 `KDIR`에 해당)

#### 2. 디바이스 트리 오버레이 작성 (Device Tree Overlay)

라즈베리 파이에게 "SPI 버스에 RC522라는 장치가 연결되었다"고 알려주기 위해 필요합니다. SPI 드라이버의 probe 함수를 호출하는 트리거가 됩니다.

- [x] **DTS 파일 작성** (`rc522-overlay.dts`):
  - [x] SPI0 (또는 사용 중인 SPI 버스) 노드 타겟팅
  - [x] `compatible` 속성 정의 (`"nxp,rc522"`) → 드라이버의 `of_match_table`과 일치
  - [x] SPI 속성 설정 (`spi-max-frequency`, `reset-gpios`)
- [x] **DTS 컴파일**: `make dtbo`로 `.dtbo` 생성
- [ ] **오버레이 적용(보드에서 수동)**: `/boot/overlays/` 복사 + `/boot/config.txt` 등록 + 재부팅

#### 3. 유저 API 정의 (`rc522_ioctl.h`)

커널과 유저 애플리케이션이 공통으로 사용할 명령어를 정의합니다.

- [x] **Magic Number 정의**: `RC522_IOC_MAGIC` = `'R'`
- [x] **IOCTL 매크로 정의**:
  - [x] `RC522_RESET`: 리더기 소프트 리셋
  - [x] `RC522_READ_CARD`: 카드 감지 및 UID 읽기 (블로킹), 인자 `__u32` 포인터
  - [x] `RC522_WRITE_REG` / `RC522_READ_REG`: 디버깅용 레지스터 직접 접근 (`struct rc522_reg_data`)
  - [x] `RC522_READ_TEXT_SECTOR`: 섹터 텍스트 읽기 (`struct rc522_read_text`)

**참고**: `Kernel_Driver/rc522_ioctl.h`에 정의. 유저 앱은 이 헤더를 포함한 뒤 `ioctl(fd, RC522_READ_CARD, &uid)` 등으로 사용합니다. **`read(fd, buf, 4)`** 로도 UID 블로킹 읽기 가능합니다.

#### 4. SPI 서브시스템 구현 (`drivers/misc/rc522/rc522_spi.c`)

하드웨어 버스와 드라이버를 연결합니다.

- [x] **SPI Driver 구조체 선언**: `struct spi_driver`
- [x] **Device ID Table**: 디바이스 트리 `compatible`과 매칭 (`of_match_table`, `spi_device_id`)
- [x] **Probe 함수 구현** (`rc522_spi_probe`):
  - [x] SPI 레지스터 R/W 구현. 읽기는 **2바이트 full-duplex(spi_sync + transfer)** 방식으로 안정화
  - [x] 디바이스 메모리 할당 (`devm_kzalloc`)
  - [x] RST GPIO 제어 후 `rc522_core_init()` 호출
- [x] **Remove 함수 구현**: `rc522_chardev_unregister`, `rc522_core_cleanup`

#### 5. 코어 로직 구현 (`drivers/misc/rc522/rc522_core.c`)

실제 RC522 칩을 제어하는 로직입니다. (기존 C++ 코드를 이식)

- [x] **SPI 전송 래퍼 함수**: `rc522_spi.c`에서 `spi_write_then_read`로 레지스터 읽기/쓰기 구현, 코어는 `rc522_ops`로 호출
- [x] **SPI full-duplex 읽기 이슈 해결**: 커널에서 0x00만 읽히던 문제를 2바이트 동시 전송으로 수정(VersionReg 정상)
- [x] **초기화 루틴**: SoftReset, Timer 설정, Antenna On (`rc522_init_chip`)
- [x] **RFID 프로토콜 함수**:
  - [x] `rc522_request` (PCD_Request / 태그 요청)
  - [x] `rc522_anticoll` (PCD_Anticoll / UID 획득)
  - [x] CRC 계산 (`rc522_calculate_crc`), Select/Auth/Read/Write 블록

#### 6. 캐릭터 디바이스 구현 (`drivers/misc/rc522/rc522_chardev.c`)

유저 공간(`/dev/rc522`)과의 다리 역할을 합니다.

- [x] **Chardev 등록**:
  - [x] `misc_register` 사용 (`/dev/rc522` 자동 생성, 문서의 cdev/class 방식과 동등)
  - [ ] `alloc_chrdev_region` / `cdev_init` / `cdev_add` / `class_create` / `device_create` — 현재 미사용
- [x] **File Operations (fops) 구현**:
  - [x] `open`: Mutex로 보호, `private_data`에 `rc522_dev` 설정
  - [x] `release`: 정리
  - [x] `unlocked_ioctl`: `RC522_RESET`, `RC522_READ_CARD`, `RC522_READ_REG`, `RC522_WRITE_REG`, `RC522_READ_TEXT_SECTOR` 처리. **`read()`** 로도 UID 4바이트 블로킹 읽기 가능

##### 6-A. `misc_register` vs 수동 `cdev/class` 방식 정리

- **현재 방식(`misc_register`)**
  - 장점: 구현이 단순하고(`/dev/rc522` 1개 기준) 커널에서 많은 보일러플레이트를 대신 처리해줌
  - 의미: `miscdevice`는 내부적으로 문자 디바이스 등록 흐름(major/minor 할당, cdev 등록 등)을 묶어 제공하는 “간편 등록 API”에 가깝다.
  - 결론: **단일 디바이스(예: `/dev/rc522`)면 충분**하며, “안 써도 되나?” → **안 써도 됨(현재 방식 유지 가능)**.

- **수동 방식(`alloc_chrdev_region` + `cdev_init/add` + `class_create` + `device_create`)**
  - 장점: major/minor 및 sysfs(class) 노출을 직접 제어 가능
  - 필요해지는 경우:
    - **RC522 여러 개**를 붙여서 `/dev/rc5220`, `/dev/rc5221`처럼 “다중 인스턴스”를 만들고 싶을 때
    - 특정 class 기반 udev 규칙/권한/네이밍 정책이 필요할 때
    - sysfs에 별도 속성(디바이스별 정보)을 더 노출하고 싶을 때

##### 6-B. 추후 RFID 모듈 추가 시(다중 디바이스) 수동 방식 전환 단계(요약)

- [ ] **minor 관리 전략 수립**: `ida`/`idr`로 minor 할당(디바이스 여러 개 대응)
- [ ] **chrdev 영역 확보**: `alloc_chrdev_region()`로 major/minor 범위 확보
- [ ] **cdev 등록**: 디바이스별 `cdev_init()` + `cdev_add()`
- [ ] **class 생성**: `class_create()`로 `/sys/class/rc522/` 생성
- [ ] **device 노드 생성**: 디바이스마다 `device_create()`로 `/dev/rc522<N>` 생성
- [ ] **fops private_data 구조 변경**: 전역 1개 포인터가 아니라 “디바이스 인스턴스별 private”로 연결
- [ ] **정리 경로**: remove 시 `device_destroy()` → `cdev_del()` → `unregister_chrdev_region()` 순서로 해제

#### 7. 통합 및 테스트

- [x] **모듈 빌드**: `make` → `rc522.ko` 생성
- [x] **모듈 로드**: `sudo insmod build/rc522.ko` (DTO 적용 후)
- [x] **커널 로그 확인**: `dmesg | grep rc522` (Probe 성공 여부 확인)
- [x] **테스트 앱 작성/검증** (C언어):
  - [x] `/dev/rc522` open
  - [x] `read(fd, buf, 4)` 로 UID 4바이트 읽기 (`test_rc522`)
  - [x] `ioctl(fd, RC522_READ_CARD, &uid)` 로 UID 읽기 (`test_rc522`, `test_rc522_poll`)
  - [x] `ioctl(fd, RC522_READ_TEXT_SECTOR, ...)` 로 섹터 텍스트 읽기 (`test_rc522_poll`)
  - [x] Ctrl+C 시 블로킹 동작이 `EINTR`로 깨지도록 처리(커널 루프 + 유저 앱 `sigaction`)

---

## 7) 테스트 전략

- 단위 테스트:
  - 레지스터 read/write 함수 입력/출력 검증
- 통합 테스트:
  - 실제 태그 2종 이상 UID 반복 인식
- 스트레스 테스트:
  - 10,000회 polling에서 timeout/error율 측정
- 성능:
  - 태그 인식 지연(ms) 로그화

---

## 8) IRQ 기반 인터럽트 모드로 전환 (다음 단계)

현재 드라이버는 UID/텍스트 읽기에서 **폴링 방식**(주기적으로 REQA/anticoll 시도)으로 동작합니다.  
다음 단계에서는 RC522의 **IRQ 핀(GPIO24)**을 사용해 “이벤트가 있을 때만 깨는” 구조로 전환합니다.

### 8-1. 목표

- 유저 공간 API는 그대로 유지:
  - `read()` / `ioctl(RC522_READ_CARD)`는 여전히 “카드가 올 때까지 블로킹”
- 내부 구현을 변경:
  - “while+sleep 폴링” → “IRQ 발생 시 wakeup + 처리”

### 8-2. Device Tree (DTO) 변경 포인트

- 현재 오버레이는 RST만 포함(IRQ 미사용). IRQ 모드에서는 아래 중 하나를 추가해야 합니다.
  - **방법 A(권장)**: 표준 DT 방식으로 GPIO interrupt 연결
    - `interrupt-parent = <&gpio>;`
    - `interrupts = <24 2>;`  (보드/커널에 따라 Edge Falling 등 값이 달라질 수 있음)
  - **방법 B**: 커스텀 프로퍼티(예: `irq-gpios`)로 GPIO를 받아서 드라이버에서 `gpiod_to_irq()` 처리

### 8-3. 커널 드라이버 변경 포인트(설계)

- **irq 요청**
  - `spi->irq` 또는 DT로부터 얻은 IRQ 번호로 `devm_request_threaded_irq()` 호출
- **대기/깨우기**
  - `wait_queue_head_t` + `wait_event_interruptible()`로 블로킹 read/ioctl을 “sleep” 상태로 두고,
  - IRQ 핸들러(또는 threaded handler)에서 “이벤트 발생” 플래그 설정 후 `wake_up_interruptible()`
- **RC522 내부 IRQ 설정**
  - `ComIEnReg`(CommIEnReg) 등의 인터럽트 enable 레지스터를 설정하고,
  - 인터럽트 원인은 `CommIrqReg`/`DivIrqReg` 등을 읽어서 확인 및 클리어
- **폴백**
  - IRQ가 DT에 없거나 request 실패 시에는 **기존 폴링 모드로 폴백**(bring-up/호환성 유지)

### 8-4. 체크리스트(IRQ 전환 작업)

- [ ] DTO에 GPIO24 인터럽트 연결 추가(신규 오버레이 또는 기존 오버레이 확장)
- [ ] `rc522_spi.c`에서 IRQ 획득/요청(`devm_request_threaded_irq`)
- [ ] `rc522_core.c`의 블로킹 루프를 `wait_event_interruptible()` 기반으로 전환
- [ ] 인터럽트 enable/원인 처리(레지스터 설정/ACK) 추가
- [ ] IRQ 미지원 환경 폴백(폴링 유지) + 문서 업데이트

---

## 9) 참고 및 권장사항

- `wiringPi`는 유지보수가 제한적이므로, 최종 제품은 `spidev + libgpiod` 또는 커널 드라이버 방식 권장.
- 초기 성공 기준은 단순하다:
  - `VersionReg` 정상값
  - 태그 UID 안정 출력
- 이 두 가지가 되면 커널 드라이버로 넘어가도 된다.

