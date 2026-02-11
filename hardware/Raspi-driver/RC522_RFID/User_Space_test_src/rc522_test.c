#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringPiSPI.h>

// 기본값(실행 인자로 변경 가능)
static int g_spi_ch = 0;          // 0: /dev/spidev0.0 (CE0), 1: /dev/spidev0.1 (CE1)
static int g_spi_speed = 1000000; // Hz
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
    wiringPiSPIDataRW(g_spi_ch, buf, 2);
}

static uint8_t rc522_read_reg(uint8_t reg)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(((reg << 1) & 0x7E) | 0x80);
    buf[1] = 0x00;
    wiringPiSPIDataRW(g_spi_ch, buf, 2);
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

static uint8_t rc522_read_version_stable(void)
{
    // 배선/신호가 애매할 때 한 번만 읽으면 0x00/0xFF가 튀는 경우가 있어 여러 번 읽어봄
    uint8_t v = 0;
    for (int i = 0; i < 5; i++) {
        v = rc522_read_reg(VersionReg);
        delay(2);
    }
    return v;
}

static int is_plausible_version(uint8_t v)
{
    // 일반적으로 0x91/0x92가 많이 보이지만, 최소한 0x00/0xFF면 통신 불가로 판단
    return (v != 0x00 && v != 0xFF);
}

static void print_usage(const char *argv0)
{
    printf("Usage: %s [--ch 0|1] [--speed HZ]\n", argv0);
    printf("  --ch     SPI chip-select: 0(/dev/spidev0.0) or 1(/dev/spidev0.1)\n");
    printf("  --speed  SPI speed in Hz (e.g. 100000, 500000, 1000000)\n");
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
    uint8_t buf[5]; // UID 4바이트 + BCC 1바이트

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

    // FIFO에서 UID(4바이트) + BCC(1바이트) 읽기
    for (i = 0; i < 5; i++) buf[i] = rc522_read_reg(FIFODataReg);

    // BCC 체크: UID[0]^UID[1]^UID[2]^UID[3]^BCC == 0 이어야 정상
    if ((buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4]) != 0) {
        return -1; // 유효하지 않은 데이터이므로 태그 없음/에러로 처리
    }

    // UID만 호출자 버퍼에 복사
    for (i = 0; i < 4; i++) uid[i] = buf[i];
    uid[4] = 0; // 마지막 바이트는 0으로 정리(사용하지 않음)
    return 0;
}

// 최근에 읽은 UID를 기억해서, 일정 시간 후 초기화/변경 여부를 확인하기 위한 상태
static uint8_t g_last_uid[5] = {0, };
static int g_last_uid_valid = 0;
static unsigned int g_last_uid_time_ms = 0;

int main(int argc, char **argv)
{
    uint8_t version = 0;
    uint8_t tag_type[2];
    uint8_t uid[5] = {0, };   // 시작 시 UID 버퍼를 0으로 초기화
    int user_ch = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "--ch")) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 2;
            }
            user_ch = (int)strtol(argv[++i], NULL, 10);
            if (user_ch != 0 && user_ch != 1) {
                fprintf(stderr, "Invalid --ch: %d (use 0 or 1)\n", user_ch);
                return 2;
            }
            continue;
        }
        if (!strcmp(argv[i], "--speed")) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 2;
            }
            g_spi_speed = (int)strtol(argv[++i], NULL, 10);
            if (g_spi_speed <= 0) {
                fprintf(stderr, "Invalid --speed: %d\n", g_spi_speed);
                return 2;
            }
            continue;
        }

        fprintf(stderr, "Unknown arg: %s\n", argv[i]);
        print_usage(argv[0]);
        return 2;
    }

    if (wiringPiSetupGpio() < 0) {
        perror("wiringPiSetupGpio");
        return 1;
    }

    pinMode(RC522_RST, OUTPUT);
    digitalWrite(RC522_RST, HIGH);

    int channels_to_try[2];
    int ntry = 0;
    if (user_ch == 0 || user_ch == 1) {
        channels_to_try[ntry++] = user_ch;
    } else {
        channels_to_try[ntry++] = 0;
        channels_to_try[ntry++] = 1;
    }

    int ok = 0;
    for (int ti = 0; ti < ntry; ti++) {
        g_spi_ch = channels_to_try[ti];
        if (wiringPiSPISetup(g_spi_ch, g_spi_speed) < 0) {
            perror("wiringPiSPISetup");
            continue;
        }

        rc522_init();
        version = rc522_read_version_stable();
        printf("SPI ch=%d speed=%dHz -> RC522 VersionReg = 0x%02X\n",
               g_spi_ch, g_spi_speed, version);

        if (is_plausible_version(version)) {
            ok = 1;
            break;
        }
    }

    if (!ok) {
        printf("SPI wiring check needed.\n");
        printf("Hints:\n");
        printf(" - RC522 전원은 3.3V/GND (5V 금지)\n");
        printf(" - RC522의 SDA 핀은 SS(NSS) 입니다: CE0(GPIO8) 또는 CE1(GPIO7)에 연결\n");
        printf(" - MISO/MOSI/SCK 배선 확인 (GPIO9/10/11)\n");
        printf(" - RST를 3.3V에 확실히 올리거나, 코드의 RST 핀(BCM %d)과 실제 연결 일치 확인\n", RC522_RST);
        printf(" - 속도를 낮춰 재시도: %s --speed 100000\n", argv[0]);
        return 1;
    }

    printf("Waiting tag...\n");
    while (1) {
        unsigned int now_ms = millis();

        // 1초(1000ms) 지나면 마지막 UID 상태를 초기화
        if (g_last_uid_valid && (now_ms - g_last_uid_time_ms >= 1000)) {
            memset(g_last_uid, 0, sizeof(g_last_uid));
            g_last_uid_valid = 0;
        }

        if (rc522_request(PICC_REQIDL, tag_type) == 0) {
            if (rc522_anticoll(uid) == 0) {
                // 이전에 읽은 UID와 다르거나, 초기화된 상태라면 새로 출력
                if (!g_last_uid_valid || memcmp(g_last_uid, uid, 5) != 0) {
                    printf("UID: %02X %02X %02X %02X\n",
                           uid[0], uid[1], uid[2], uid[3]);
                    memcpy(g_last_uid, uid, 5);
                    g_last_uid_valid = 1;
                    g_last_uid_time_ms = now_ms;
                }
            }
        }

        delay(100);
    }
}