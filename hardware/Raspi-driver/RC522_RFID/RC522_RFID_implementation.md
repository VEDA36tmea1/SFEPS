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

