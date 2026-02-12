// C 포트: Python mfrc522(SimpleMFRC522/BasicMFRC522/MFRC522) 핵심 동작
// - wiringPi + wiringPiSPI 기반
// - UID 읽기, 섹터(3블록) 텍스트 읽기/쓰기 지원
//
// 공개 함수:
//   int rc522c_init(int spi_ch, int speed_hz, int rst_bcm);
//   int rc522c_read_id_blocking(uint32_t *out_id);
//   int rc522c_read_text_sector_blocking(int trailer_block,
//                                        uint32_t *out_id,
//                                        char *out_text,
//                                        size_t max_len);
//   int rc522c_write_text_sector_blocking(int trailer_block,
//                                         const char *text,
//                                         uint32_t *out_id);

#include "rc522_full.h"

#include <string.h>
#include <wiringPi.h>
#include <wiringPiSPI.h>

// -------- 전역 설정 --------

static int g_spi_ch = 0;            // 0: spidev0.0 (CE0)
static int g_spi_speed = 1000000;   // 1MHz
static int g_rst_pin = 25;          // BCM25 (물리 22)

static const uint8_t g_default_key[6] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

// -------- MFRC522 레지스터/커맨드 정의 (Python MFRC522 클래스와 동일) --------

// PCD Commands
#define PCD_IDLE        0x00
#define PCD_AUTHENT     0x0E
#define PCD_RECEIVE     0x08
#define PCD_TRANSMIT    0x04
#define PCD_TRANSCEIVE  0x0C
#define PCD_RESETPHASE  0x0F
#define PCD_CALCCRC     0x03

// PICC Commands
#define PICC_REQIDL     0x26
#define PICC_REQALL     0x52
#define PICC_ANTICOLL   0x93
#define PICC_SELECTTAG  0x93
#define PICC_AUTHENT1A  0x60
#define PICC_AUTHENT1B  0x61
#define PICC_READ       0x30
#define PICC_WRITE      0xA0
#define PICC_HALT       0x50

// Status
#define MI_OK       0
#define MI_NOTAGERR 1
#define MI_ERR      2

// MFRC522 Registers
#define CommandReg        0x01
#define CommIEnReg        0x02
#define DivlEnReg         0x03
#define CommIrqReg        0x04
#define DivIrqReg         0x05
#define ErrorReg          0x06
#define Status1Reg        0x07
#define Status2Reg        0x08
#define FIFODataReg       0x09
#define FIFOLevelReg      0x0A
#define WaterLevelReg     0x0B
#define ControlReg        0x0C
#define BitFramingReg     0x0D
#define CollReg           0x0E

#define ModeReg           0x11
#define TxModeReg         0x12
#define RxModeReg         0x13
#define TxControlReg      0x14
#define TxAutoReg         0x15
#define TxSelReg          0x16
#define RxSelReg          0x17
#define RxThresholdReg    0x18
#define DemodReg          0x19

#define TModeReg          0x2A
#define TPrescalerReg     0x2B
#define TReloadRegH       0x2C
#define TReloadRegL       0x2D
#define TCounterValueRegH 0x2E
#define TCounterValueRegL 0x2F

#define CRCResultRegM     0x21
#define CRCResultRegL     0x22

#define RFCfgReg          0x26

// -------- 저수준 SPI / 레지스터 I/O --------

static void rc522c_write_reg(uint8_t addr, uint8_t val)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)((addr << 1) & 0x7E);
    buf[1] = val;
    wiringPiSPIDataRW(g_spi_ch, buf, 2);
}

uint8_t rc522c_read_reg(uint8_t addr)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(((addr << 1) & 0x7E) | 0x80);
    buf[1] = 0x00;
    wiringPiSPIDataRW(g_spi_ch, buf, 2);
    return buf[1];
}

static void rc522c_set_bitmask(uint8_t reg, uint8_t mask)
{
    uint8_t tmp = rc522c_read_reg(reg);
    rc522c_write_reg(reg, tmp | mask);
}

static void rc522c_clear_bitmask(uint8_t reg, uint8_t mask)
{
    uint8_t tmp = rc522c_read_reg(reg);
    rc522c_write_reg(reg, tmp & (uint8_t)(~mask));
}

static void rc522c_antenna_on(void)
{
    uint8_t temp = rc522c_read_reg(TxControlReg);
    if ((temp & 0x03) != 0x03) {
        rc522c_set_bitmask(TxControlReg, 0x03);
    }
}

/* Python MFRC522_Reset()와 동일: 소프트 리셋만. RST 핀은 rc522c_init()에서 한 번 HIGH로 설정 */
static void rc522c_reset(void)
{
    rc522c_write_reg(CommandReg, PCD_RESETPHASE);
    delay(50);
}

static void rc522c_init_chip(void)
{
    rc522c_reset();

    // Python Init()와 동일
    rc522c_write_reg(TModeReg, 0x8D);
    rc522c_write_reg(TPrescalerReg, 0x3E);
    rc522c_write_reg(TReloadRegL, 30);
    rc522c_write_reg(TReloadRegH, 0);

    rc522c_write_reg(TxAutoReg, 0x40);
    rc522c_write_reg(ModeReg, 0x3D);

    rc522c_antenna_on();
}

// -------- 공용 초기화 함수 --------

int rc522c_init(int spi_ch, int speed_hz, int rst_bcm)
{
    g_spi_ch = spi_ch;
    g_spi_speed = speed_hz;
    g_rst_pin = rst_bcm;

    if (wiringPiSetupGpio() < 0) {
        return -1;
    }

    if (g_rst_pin >= 0) {
        pinMode(g_rst_pin, OUTPUT);
        digitalWrite(g_rst_pin, HIGH);
    }

    if (wiringPiSPISetup(g_spi_ch, g_spi_speed) < 0) {
        return -1;
    }

    rc522c_init_chip();
    return 0;
}

// -------- CRC 계산 --------

static void rc522c_calculate_crc(const uint8_t *data, size_t len, uint8_t *out_crc_low, uint8_t *out_crc_high)
{
    size_t i;

    rc522c_clear_bitmask(DivIrqReg, 0x04);
    rc522c_set_bitmask(FIFOLevelReg, 0x80);

    for (i = 0; i < len; i++) {
        rc522c_write_reg(FIFODataReg, data[i]);
    }

    rc522c_write_reg(CommandReg, PCD_CALCCRC);

    i = 0xFF;
    while (1) {
        uint8_t n = rc522c_read_reg(DivIrqReg);
        i--;
        if ((i == 0) || (n & 0x04)) {
            break;
        }
    }

    *out_crc_low  = rc522c_read_reg(CRCResultRegL);
    *out_crc_high = rc522c_read_reg(CRCResultRegM);
}

// -------- 카드와 통신 (Python MFRC522.MFRC522_ToCard) --------

static int rc522c_to_card(uint8_t command,
                          const uint8_t *send_data,
                          size_t send_len,
                          uint8_t *back_data,
                          size_t *back_bits)
{
    uint8_t irq_en = 0x00;
    uint8_t wait_irq = 0x00;
    uint8_t last_bits = 0;
    uint8_t n = 0;
    int status = MI_ERR;
    int i;

    if (command == PCD_AUTHENT) {
        irq_en = 0x12;
        wait_irq = 0x10;
    } else if (command == PCD_TRANSCEIVE) {
        irq_en = 0x77;
        wait_irq = 0x30;
    }

    rc522c_write_reg(CommIEnReg, irq_en | 0x80);
    rc522c_clear_bitmask(CommIrqReg, 0x80);
    rc522c_set_bitmask(FIFOLevelReg, 0x80);

    rc522c_write_reg(CommandReg, PCD_IDLE);

    for (i = 0; i < (int)send_len; i++) {
        rc522c_write_reg(FIFODataReg, send_data[i]);
    }

    rc522c_write_reg(CommandReg, command);

    if (command == PCD_TRANSCEIVE) {
        rc522c_set_bitmask(BitFramingReg, 0x80);
    }

    i = 2000;
    while (1) {
        n = rc522c_read_reg(CommIrqReg);
        i--;
        if ((i == 0) || (n & 0x01) || (n & wait_irq)) {
            break;
        }
        delayMicroseconds(200); // Python 구현의 time.sleep(0.35) 보다 상당히 짧게
    }

    rc522c_clear_bitmask(BitFramingReg, 0x80);

    if (i != 0) {
        uint8_t err = rc522c_read_reg(ErrorReg);
        if ((err & 0x1B) == 0x00) {
            status = MI_OK;
            if (n & irq_en & 0x01) {
                status = MI_NOTAGERR;
            }
            if (command == PCD_TRANSCEIVE) {
                uint8_t fifo_level = rc522c_read_reg(FIFOLevelReg);
                last_bits = rc522c_read_reg(ControlReg) & 0x07;
                if (last_bits != 0) {
                    *back_bits = (size_t)((fifo_level - 1) * 8 + last_bits);
                } else {
                    *back_bits = (size_t)(fifo_level * 8);
                }

                if (fifo_level == 0) {
                    fifo_level = 1;
                }
                if (fifo_level > 16) {
                    fifo_level = 16;
                }

                if (back_data) {
                    for (i = 0; i < fifo_level; i++) {
                        back_data[i] = rc522c_read_reg(FIFODataReg);
                    }
                }
            }
        } else {
            status = MI_ERR;
        }
    }

    return status;
}

// -------- Request / Anticoll / Select / Auth / Read/Write --------

static int rc522c_request(uint8_t req_mode, uint8_t *tag_type)
{
    uint8_t buf[1];
    size_t back_bits = 0;
    int status;

    // BitFramingReg = 0x07 (Python과 동일)
    rc522c_write_reg(BitFramingReg, 0x07);

    buf[0] = req_mode;
    status = rc522c_to_card(PCD_TRANSCEIVE, buf, 1, tag_type, &back_bits);
    if ((status != MI_OK) || (back_bits != 0x10)) {
        status = MI_ERR;
    }
    return status;
}

static int rc522c_anticoll(uint8_t *uid5)
{
    uint8_t ser_num[2];
    uint8_t back_data[16];
    size_t back_bits = 0;
    int status;
    int i;
    uint8_t ser_num_check = 0;

    rc522c_write_reg(BitFramingReg, 0x00);

    ser_num[0] = PICC_ANTICOLL;
    ser_num[1] = 0x20;

    status = rc522c_to_card(PCD_TRANSCEIVE, ser_num, 2, back_data, &back_bits);
    if (status != MI_OK) {
        return MI_ERR;
    }

    if (back_bits != 40) { // 5바이트 * 8
        return MI_ERR;
    }

    // BCC 체크
    for (i = 0; i < 4; i++) {
        ser_num_check ^= back_data[i];
    }
    if (ser_num_check != back_data[4]) {
        return MI_ERR;
    }

    for (i = 0; i < 5; i++) {
        uid5[i] = back_data[i];
    }
    return MI_OK;
}

static int rc522c_select_tag(const uint8_t *uid5)
{
    uint8_t buf[9];
    uint8_t back_data[3];
    size_t back_bits = 0;
    int status;
    int i;
    uint8_t crc_low, crc_high;

    buf[0] = PICC_SELECTTAG;
    buf[1] = 0x70;
    for (i = 0; i < 5; i++) {
        buf[2 + i] = uid5[i];
    }

    rc522c_calculate_crc(buf, 7, &crc_low, &crc_high);
    buf[7] = crc_low;
    buf[8] = crc_high;

    status = rc522c_to_card(PCD_TRANSCEIVE, buf, 9, back_data, &back_bits);
    if ((status == MI_OK) && (back_bits == 0x18)) {
        return (int)back_data[0]; // SAK
    }
    return 0;
}

static int rc522c_authenticate(uint8_t auth_mode,
                               uint8_t block_addr,
                               const uint8_t *sector_key6,
                               const uint8_t *uid4)
{
    uint8_t buff[12];
    int i;
    int status;
    size_t back_bits = 0;

    buff[0] = auth_mode;
    buff[1] = block_addr;
    for (i = 0; i < 6; i++) {
        buff[2 + i] = sector_key6[i];
    }
    for (i = 0; i < 4; i++) {
        buff[8 + i] = uid4[i];
    }

    status = rc522c_to_card(PCD_AUTHENT, buff, 12, NULL, &back_bits);
    if (status != MI_OK) {
        return MI_ERR;
    }
    if ((rc522c_read_reg(Status2Reg) & 0x08) == 0) {
        return MI_ERR;
    }
    return MI_OK;
}

static void rc522c_stop_crypto1(void)
{
    rc522c_clear_bitmask(Status2Reg, 0x08);
}

static int rc522c_read_block(uint8_t block_addr, uint8_t *out_16bytes)
{
    uint8_t buf[4];
    uint8_t back_data[18];
    size_t back_bits = 0;
    int status;

    buf[0] = PICC_READ;
    buf[1] = block_addr;
    rc522c_calculate_crc(buf, 2, &buf[2], &buf[3]);

    status = rc522c_to_card(PCD_TRANSCEIVE, buf, 4, back_data, &back_bits);
    if (status != MI_OK) {
        return MI_ERR;
    }
    // READ 응답은 16바이트 데이터 + 2바이트 CRC(총 18바이트)로 오는 경우가 흔함.
    // Python 라이브러리는 MAX_LEN=16으로 잘라서 CRC는 무시하고 데이터 16바이트만 사용한다.
    if (back_bits < 16 * 8) {
        return MI_ERR;
    }

    memcpy(out_16bytes, back_data, 16);
    return MI_OK;
}

static int rc522c_write_block(uint8_t block_addr, const uint8_t *in_16bytes)
{
    uint8_t buf[4];
    uint8_t back_data[18];
    size_t back_bits = 0;
    int status;

    // 1단계: WRITE 명령 전송
    buf[0] = PICC_WRITE;
    buf[1] = block_addr;
    rc522c_calculate_crc(buf, 2, &buf[2], &buf[3]);

    status = rc522c_to_card(PCD_TRANSCEIVE, buf, 4, back_data, &back_bits);
    // MFRC522 ACK는 4비트(0x0A) 응답
    if ((status != MI_OK) || (back_bits != 4) || ((back_data[0] & 0x0F) != 0x0A)) {
        return MI_ERR;
    }

    // 2단계: 실제 16바이트 데이터 전송
    {
        uint8_t data_buf[16 + 2];
        memcpy(data_buf, in_16bytes, 16);
        rc522c_calculate_crc(in_16bytes, 16, &data_buf[16], &data_buf[17]);

        status = rc522c_to_card(PCD_TRANSCEIVE, data_buf, 18, back_data, &back_bits);
        if ((status != MI_OK) || (back_bits != 4) || ((back_data[0] & 0x0F) != 0x0A)) {
            return MI_ERR;
        }
    }

    return MI_OK;
}

// -------- 고수준: UID/텍스트 읽기/쓰기 (BasicMFRC522/ SimpleMFRC522 대응) --------

// 트레일러 블록 번호가 유효한지 검사 ((trailer+1)%4==0 이어야 함)
static int rc522c_is_valid_trailer(int trailer_block)
{
    return ((trailer_block + 1) % 4) == 0;
}

// UID 5바이트(마지막은 BCC) -> 32비트 정수 (상위 4바이트만 사용)
static uint32_t rc522c_uid_to_num(const uint8_t uid5[5])
{
    uint32_t n = 0;
    int i;
    for (i = 0; i < 4; i++) {
        n = n * 256U + uid5[i];
    }
    return n;
}

// 태그 ID를 폴링 없이 한 번만 시도 (성공: 0, 실패: -1)
int rc522c_read_id_no_block(uint32_t *out_id)
{
    uint8_t tag_type[2];
    uint8_t uid5[5];
    int status;

    status = rc522c_request(PICC_REQIDL, tag_type);
    if (status != MI_OK) {
        return -1;
    }

    status = rc522c_anticoll(uid5);
    if (status != MI_OK) {
        return -1;
    }

    *out_id = rc522c_uid_to_num(uid5);
    return 0;
}

// 태그가 나올 때까지 blocking (성공: 0, 실패하지 않음)
int rc522c_read_id_blocking(uint32_t *out_id)
{
    while (1) {
        if (rc522c_read_id_no_block(out_id) == 0) {
            return 0;
        }
        delay(50);
    }
}

// 섹터(3블록) 텍스트 읽기 (BasicMFRC522.read_no_block와 유사)
// trailer_block: 섹터 트레일러 블록 (예: 11)
// out_text: 널 종료 문자열 버퍼
// max_len: out_text 크기
// 성공: 0, 실패: -1
int rc522c_read_text_sector_blocking(int trailer_block,
                                     uint32_t *out_id,
                                     char *out_text,
                                     size_t max_len)
{
    uint8_t uid5[5];
    uint8_t uid4[4];
    uint8_t key[6];
    uint8_t data[16 * 3];
    uint8_t tag_type[2];
    int blocks[3];
    int i;
    int status;

    if (!rc522c_is_valid_trailer(trailer_block) || max_len == 0) {
        return -1;
    }

    blocks[0] = trailer_block - 3;
    blocks[1] = trailer_block - 2;
    blocks[2] = trailer_block - 1;

    memcpy(key, g_default_key, sizeof(key));

    // 태그 나올 때까지 blocking
    while (1) {
        status = rc522c_request(PICC_REQIDL, tag_type);
        if (status != MI_OK) {
            delay(50);
            continue;
        }

        status = rc522c_anticoll(uid5);
        if (status != MI_OK) {
            delay(50);
            continue;
        }
        break;
    }

    for (i = 0; i < 4; i++) {
        uid4[i] = uid5[i];
    }
    if (out_id) {
        *out_id = rc522c_uid_to_num(uid5);
    }

    // Select + Authenticate
    rc522c_select_tag(uid5);
    status = rc522c_authenticate(PICC_AUTHENT1A, (uint8_t)trailer_block, key, uid4);
    if (status != MI_OK) {
        rc522c_stop_crypto1();
        return -1;
    }

    // 3개 데이터 블록 읽기
    memset(data, 0, sizeof(data));
    for (i = 0; i < 3; i++) {
        if (rc522c_read_block((uint8_t)blocks[i], &data[i * 16]) != MI_OK) {
            rc522c_stop_crypto1();
            return -1;
        }
    }

    rc522c_stop_crypto1();

    // ASCII 문자열로 변환 (널 종료, 최대 max_len-1)
    {
        size_t copy_len = sizeof(data);
        size_t j;
        if (copy_len >= max_len) copy_len = max_len - 1;
        for (j = 0; j < copy_len; j++) {
            out_text[j] = (char)data[j];
        }
        out_text[copy_len] = '\0';
    }

    return 0;
}

// 섹터(3블록)에 텍스트 쓰기 (BasicMFRC522.write_no_block 유사)
// text 길이가 48바이트보다 짧으면 pad, 길면 잘림
int rc522c_write_text_sector_blocking(int trailer_block,
                                      const char *text,
                                      uint32_t *out_id)
{
    uint8_t uid5[5];
    uint8_t uid4[4];
    uint8_t key[6];
    uint8_t data[16 * 3];
    uint8_t tag_type[2];
    int blocks[3];
    int i;
    int status;
    size_t text_len;

    if (!rc522c_is_valid_trailer(trailer_block)) {
        return -1;
    }

    blocks[0] = trailer_block - 3;
    blocks[1] = trailer_block - 2;
    blocks[2] = trailer_block - 1;

    memcpy(key, g_default_key, sizeof(key));
    memset(data, 0, sizeof(data));

    text_len = strlen(text);
    if (text_len > sizeof(data)) {
        text_len = sizeof(data);
    }
    memcpy(data, text, text_len);

    // 태그 나올 때까지 blocking
    while (1) {
        status = rc522c_request(PICC_REQIDL, tag_type);
        if (status != MI_OK) {
            delay(50);
            continue;
        }

        status = rc522c_anticoll(uid5);
        if (status != MI_OK) {
            delay(50);
            continue;
        }
        break;
    }

    for (i = 0; i < 4; i++) {
        uid4[i] = uid5[i];
    }
    if (out_id) {
        *out_id = rc522c_uid_to_num(uid5);
    }

    // Select + Authenticate
    rc522c_select_tag(uid5);
    status = rc522c_authenticate(PICC_AUTHENT1A, (uint8_t)trailer_block, key, uid4);
    if (status != MI_OK) {
        rc522c_stop_crypto1();
        return -1;
    }

    // 3개 데이터 블록 쓰기
    for (i = 0; i < 3; i++) {
        if (rc522c_write_block((uint8_t)blocks[i], &data[i * 16]) != MI_OK) {
            rc522c_stop_crypto1();
            return -1;
        }
    }

    rc522c_stop_crypto1();
    return 0;
}

