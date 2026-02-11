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

```c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringPiSPI.h>

#define SPI_CH      0
#define SPI_SPEED   1000000
#define RC522_RST   25   // BCM

// RC522 registers (일부)
#define CommandReg      0x01
#define ComIEnReg       0x02
#define DivIEnReg       0x03
#define ComIrqReg       0x04
#define DivIrqReg       0x05
#define ErrorReg        0x06
#define Status1Reg      0x07
#define Status2Reg      0x08
#define FIFODataReg     0x09
#define FIFOLevelReg    0x0A
#define ControlReg      0x0C
#define BitFramingReg   0x0D
#define ModeReg         0x11
#define TxControlReg    0x14
#define TxASKReg        0x15
#define TModeReg        0x2A
#define TPrescalerReg   0x2B
#define TReloadRegH     0x2C
#define TReloadRegL     0x2D
#define VersionReg      0x37

// PCD command
#define PCD_IDLE            0x00
#define PCD_TRANSCEIVE      0x0C
#define PCD_SOFTRESET       0x0F

// PICC command
#define PICC_REQIDL         0x26
#define PICC_ANTICOLL       0x93

static void rc522_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)((reg << 1) & 0x7E);
    buf[1] = val;
    wiringPiSPIDataRW(SPI_CH, buf, 2);
}

static uint8_t rc522_read_reg(uint8_t reg)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(((reg << 1) & 0x7E) | 0x80);
    buf[1] = 0x00;
    wiringPiSPIDataRW(SPI_CH, buf, 2);
    return buf[1];
}

static void rc522_set_bitmask(uint8_t reg, uint8_t mask)
{
    uint8_t tmp = rc522_read_reg(reg);
    rc522_write_reg(reg, tmp | mask);
}

static void rc522_clear_bitmask(uint8_t reg, uint8_t mask)
{
    uint8_t tmp = rc522_read_reg(reg);
    rc522_write_reg(reg, tmp & (uint8_t)(~mask));
}

static void rc522_antenna_on(void)
{
    uint8_t v = rc522_read_reg(TxControlReg);
    if ((v & 0x03) != 0x03) {
        rc522_set_bitmask(TxControlReg, 0x03);
    }
}

static void rc522_reset(void)
{
    digitalWrite(RC522_RST, LOW);
    delay(10);
    digitalWrite(RC522_RST, HIGH);
    delay(50);

    rc522_write_reg(CommandReg, PCD_SOFTRESET);
    delay(50);
}

static void rc522_init(void)
{
    rc522_reset();

    rc522_write_reg(TModeReg, 0x8D);
    rc522_write_reg(TPrescalerReg, 0x3E);
    rc522_write_reg(TReloadRegL, 30);
    rc522_write_reg(TReloadRegH, 0);
    rc522_write_reg(TxASKReg, 0x40);
    rc522_write_reg(ModeReg, 0x3D);

    rc522_antenna_on();
}

// 상태값: 0 success, 1 no tag, -1 error
static int rc522_request(uint8_t req_mode, uint8_t *tag_type)
{
    int i;
    uint8_t irq_en = 0x77;
    uint8_t wait_irq = 0x30;
    uint8_t n;

    rc522_write_reg(ComIEnReg, irq_en | 0x80);
    rc522_clear_bitmask(ComIrqReg, 0x80);
    rc522_set_bitmask(FIFOLevelReg, 0x80);
    rc522_write_reg(CommandReg, PCD_IDLE);
    rc522_write_reg(FIFODataReg, req_mode);
    rc522_write_reg(CommandReg, PCD_TRANSCEIVE);
    rc522_set_bitmask(BitFramingReg, 0x80);

    for (i = 2000; i > 0; i--) {
        n = rc522_read_reg(ComIrqReg);
        if (n & wait_irq) break;
        if (n & 0x01) break; // timer irq
    }

    rc522_clear_bitmask(BitFramingReg, 0x80);

    if (i == 0) return 1;
    if (rc522_read_reg(ErrorReg) & 0x1B) return -1;

    tag_type[0] = rc522_read_reg(FIFODataReg);
    tag_type[1] = rc522_read_reg(FIFODataReg);
    return 0;
}

static int rc522_anticoll(uint8_t *uid)
{
    int i;
    uint8_t n;

    rc522_write_reg(BitFramingReg, 0x00);
    rc522_write_reg(ComIEnReg, 0xF7);
    rc522_clear_bitmask(ComIrqReg, 0x80);
    rc522_set_bitmask(FIFOLevelReg, 0x80);
    rc522_write_reg(CommandReg, PCD_IDLE);
    rc522_write_reg(FIFODataReg, PICC_ANTICOLL);
    rc522_write_reg(FIFODataReg, 0x20);
    rc522_write_reg(CommandReg, PCD_TRANSCEIVE);
    rc522_set_bitmask(BitFramingReg, 0x80);

    for (i = 2000; i > 0; i--) {
        n = rc522_read_reg(ComIrqReg);
        if (n & 0x30) break;
        if (n & 0x01) break;
    }
    rc522_clear_bitmask(BitFramingReg, 0x80);
    if (i == 0) return -1;
    if (rc522_read_reg(ErrorReg) & 0x1B) return -1;

    for (i = 0; i < 5; i++) uid[i] = rc522_read_reg(FIFODataReg);
    return 0;
}

int main(void)
{
    uint8_t version;
    uint8_t tag_type[2];
    uint8_t uid[5];

    if (wiringPiSetupGpio() < 0) {
        perror("wiringPiSetupGpio");
        return 1;
    }

    pinMode(RC522_RST, OUTPUT);
    digitalWrite(RC522_RST, HIGH);

    if (wiringPiSPISetup(SPI_CH, SPI_SPEED) < 0) {
        perror("wiringPiSPISetup");
        return 1;
    }

    rc522_init();
    version = rc522_read_reg(VersionReg);
    printf("RC522 VersionReg = 0x%02X\n", version);
    if (version == 0x00 || version == 0xFF) {
        printf("SPI wiring check needed.\n");
        return 1;
    }

    printf("Waiting tag...\n");
    while (1) {
        if (rc522_request(PICC_REQIDL, tag_type) == 0) {
            if (rc522_anticoll(uid) == 0) {
                printf("UID: %02X %02X %02X %02X\n",
                    uid[0], uid[1], uid[2], uid[3]);
                delay(1000);
            }
        }
        delay(100);
    }

    return 0;
}
```

### 3-2. Makefile 예시

```make
CC=gcc
CFLAGS=-O2 -Wall
TARGET=rc522_user
SRC=src/rc522_user.c
LIBS=-lwiringPi

all:
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)
```

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

```c
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/mutex.h>

struct rc522_dev {
    struct spi_device *spi;
    struct mutex lock;
    int rst_gpio;
    int irq_gpio; // optional
};

static int rc522_spi_read_reg(struct rc522_dev *d, u8 reg, u8 *val)
{
    u8 tx[2] = { (u8)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    u8 rx[2] = { 0, };
    struct spi_transfer t[] = {
        { .tx_buf = tx, .rx_buf = rx, .len = 2, },
    };
    int ret;
    struct spi_message m;

    spi_message_init(&m);
    spi_message_add_tail(&t[0], &m);
    ret = spi_sync(d->spi, &m);
    if (ret < 0)
        return ret;
    *val = rx[1];
    return 0;
}

static int rc522_probe(struct spi_device *spi)
{
    struct rc522_dev *d;

    d = devm_kzalloc(&spi->dev, sizeof(*d), GFP_KERNEL);
    if (!d)
        return -ENOMEM;

    d->spi = spi;
    mutex_init(&d->lock);
    spi_set_drvdata(spi, d);

    dev_info(&spi->dev, "rc522 probed\n");
    return 0;
}

static void rc522_remove(struct spi_device *spi)
{
    dev_info(&spi->dev, "rc522 removed\n");
}

static const struct of_device_id rc522_of_match[] = {
    { .compatible = "nxp,mfrc522" },
    { }
};
MODULE_DEVICE_TABLE(of, rc522_of_match);

static struct spi_driver rc522_driver = {
    .driver = {
        .name = "rc522",
        .of_match_table = rc522_of_match,
    },
    .probe = rc522_probe,
    .remove = rc522_remove,
};
module_spi_driver(rc522_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MFRC522 SPI driver");
```

### 5-2. Device Tree Overlay 예시

`CE0 + GPIO25(RST) + GPIO24(IRQ)` 기준:

```dts
/dts-v1/;
/plugin/;

/ {
    compatible = "brcm,bcm2711";

    fragment@0 {
        target = <&spi0>;
        __overlay__ {
            status = "okay";
            rc522@0 {
                compatible = "nxp,mfrc522";
                reg = <0>; /* CE0 */
                spi-max-frequency = <10000000>;
                rst-gpios = <&gpio 25 0>;
                irq-gpios = <&gpio 24 0>;
                status = "okay";
            };
        };
    };
};
```

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
- ✅ 완료: 1(빌드), 2(DTS), 4(SPI), 5(코어), 6(Chardev) 대부분  
 
- ⚠️ 미구현/다른 방식: 3(IOCTL API), 6(ioctl 대신 `read()`로 UID), 7(실기 테스트·테스트 앱)
    
2026-02-11 기준 .

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
- [ ] **DTS 컴파일 및 적용**: `make dtbo`로 `.dtbo` 생성 가능. `/boot/overlays/` 복사 및 `/boot/config.txt` 등록은 수동 수행

#### 3. 유저 API 정의 (`rc522_ioctl.h`)

커널과 유저 애플리케이션이 공통으로 사용할 명령어를 정의합니다.

- [x] **Magic Number 정의**: `RC522_IOC_MAGIC` = `'R'`
- [x] **IOCTL 매크로 정의**:
  - [x] `RC522_RESET`: 리더기 소프트 리셋
  - [x] `RC522_READ_CARD`: 카드 감지 및 UID 읽기 (블로킹), 인자 `__u32` 포인터
  - [x] `RC522_WRITE_REG` / `RC522_READ_REG`: 디버깅용 레지스터 직접 접근 (`struct rc522_reg_data`)

**참고**: `Kernel_Driver/rc522_ioctl.h`에 정의. 유저 앱은 이 헤더를 포함한 뒤 `ioctl(fd, RC522_READ_CARD, &uid)` 등으로 사용합니다. **`read(fd, buf, 4)`** 로도 UID 블로킹 읽기 가능합니다.

#### 4. SPI 서브시스템 구현 (`drivers/misc/rc522/rc522_spi.c`)

하드웨어 버스와 드라이버를 연결합니다.

- [x] **SPI Driver 구조체 선언**: `struct spi_driver`
- [x] **Device ID Table**: 디바이스 트리 `compatible`과 매칭 (`of_match_table`, `spi_device_id`)
- [x] **Probe 함수 구현** (`rc522_spi_probe`):
  - [x] SPI 전송은 `spi_write_then_read` 사용 (기본 `spi_setup()`으로 동작)
  - [x] 디바이스 메모리 할당 (`devm_kzalloc`)
  - [x] RST GPIO 제어 후 `rc522_core_init()` 호출
- [x] **Remove 함수 구현**: `rc522_chardev_unregister`, `rc522_core_cleanup`

#### 5. 코어 로직 구현 (`drivers/misc/rc522/rc522_core.c`)

실제 RC522 칩을 제어하는 로직입니다. (기존 C++ 코드를 이식)

- [x] **SPI 전송 래퍼 함수**: `rc522_spi.c`에서 `spi_write_then_read`로 레지스터 읽기/쓰기 구현, 코어는 `rc522_ops`로 호출
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
  - [x] `unlocked_ioctl`: `RC522_RESET`, `RC522_READ_CARD`, `RC522_READ_REG`, `RC522_WRITE_REG` 처리. **`read()`** 로도 UID 4바이트 블로킹 읽기 가능

#### 7. 통합 및 테스트

- [x] **모듈 빌드**: `make` → `rc522.ko` 생성
- [ ] **모듈 로드**: `sudo insmod rc522.ko` (또는 DTO 적용 후 부팅 시 자동)
- [ ] **커널 로그 확인**: `dmesg | grep rc522` (Probe 성공 여부 확인)
- [ ] **테스트 앱 작성** (C언어):
  - [ ] `/dev/rc522` open
  - [ ] `read(fd, buf, 4)` 로 UID 4바이트 읽기 (현재 구현은 ioctl 대신 read 사용)
  - [ ] 결과 출력

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

## 8) 참고 및 권장사항

- `wiringPi`는 유지보수가 제한적이므로, 최종 제품은 `spidev + libgpiod` 또는 커널 드라이버 방식 권장.
- 초기 성공 기준은 단순하다:
  - `VersionReg` 정상값
  - 태그 UID 안정 출력
- 이 두 가지가 되면 커널 드라이버로 넘어가도 된다.

