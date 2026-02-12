# Python (SimpleMFRC522 + MFRC522) vs C (rc522_full.c) 비교

## 1. 초기화 (Init)

| 항목 | Python MFRC522 | C rc522_full |
|------|-----------------|--------------|
| **GPIO RST** | `__init__`에서 한 번: `GPIO.output(pin_rst, 1)` | `rc522c_init()`에서 OUTPUT + HIGH 한 번만 ✅ |
| **Soft Reset** | `MFRC522_Init()` → `MFRC522_Reset()` → `CommandReg = PCD_RESETPHASE` 만 | `rc522c_reset()` → `CommandReg = PCD_RESETPHASE` + delay(50) 만 ✅ |
| **TModeReg** | 0x8D | 0x8D ✅ |
| **TPrescalerReg** | 0x3E | 0x3E ✅ |
| **TReloadReg L/H** | 30, 0 | 30, 0 ✅ |
| **TxAutoReg** | 0x40 | 0x40 ✅ |
| **ModeReg** | 0x3D | 0x3D ✅ |
| **RFCfgReg (Rx Gain)** | 없음 | 없음 ✅ |
| **Antenna On** | `AntennaOn()` → TxControlReg 하위 2비트 0x03 | `rc522c_antenna_on()` 동일 ✅ |

**요약:** C와 Python 모두 RST 한 번 HIGH, 소프트 리셋만, RFCfgReg 미설정으로 동일함.

---

## 2. AntennaOn 조건

| | Python | C |
|---|--------|---|
| **조건** | `if (~(temp & 0x03)):` → (temp & 0x03) != 0x03 이면 SetBitMask | `if ((temp & 0x03) != 0x03)` 동일 ✅ |

동일 로직.

---

## 3. SPI / 레지스터 I/O

| | Python | C |
|---|--------|---|
| **쓰기** | `spi.xfer2([(addr<<1)&0x7E, val])` | `buf[0]=(addr<<1)&0x7E; buf[1]=val; wiringPiSPIDataRW(..., buf, 2)` ✅ |
| **읽기** | `spi.xfer2([((addr<<1)&0x7E)|0x80, 0])` → `return val[1]` | 동일 주소/비트, `buf[1]` 반환 ✅ |

동일 동작.

---

## 4. Request (REQA)

| | Python MFRC522_Request | C rc522c_request |
|---|------------------------|-------------------|
| BitFramingReg | 0x07 | 0x07 ✅ |
| ToCard(PCD_TRANSCEIVE, [reqMode]) | 동일 | 동일 ✅ |
| 성공 조건 | `status == MI_OK and backBits == 0x10` | `status == MI_OK && back_bits == 0x10` ✅ |

동일.

---

## 5. Anticoll

| | Python MFRC522_Anticoll | C rc522c_anticoll |
|---|--------------------------|-------------------|
| BitFramingReg | 0x00 | 0x00 ✅ |
| 송신 | [PICC_ANTICOLL, 0x20] | 동일 ✅ |
| 응답 검사 | `len(backData) == 5` + BCC XOR 검사 | `back_bits == 40` + BCC 검사 ✅ |

동일 (Python은 바이트 수, C는 비트 수로 검사).

---

## 6. SelectTag / Auth / Read / Write

| | Python | C |
|---|--------|---|
| SelectTag | PICC_SELECTTAG, 0x70, UID 5바이트, CRC 2바이트, backLen==0x18 | 동일 ✅ |
| Auth | PICC_AUTHENT1A, block, KEY, UID 4바이트 + Status2Reg & 0x08 | 동일 ✅ |
| Read 블록 | backData 16바이트면 OK (MAX_LEN=16) | back_bits >= 128, 앞 16바이트 사용 ✅ |
| Write 블록 | backLen==4, (backData[0]&0x0F)==0x0A (4비트 ACK) | back_bits==4, (back_data[0]&0x0F)==0x0A ✅ |

동일.

---

## 7. SimpleMFRC522 vs C 데모 흐름

| | Python read_no_block / read | C rc522c_read_text_sector_blocking |
|---|-----------------------------|-------------------------------------|
| Request → Anticoll | 동일 | 동일 |
| uid_to_num (5바이트→정수) | `n = n*256 + uid[i]` (i=0..5, **5바이트** 사용) | `n = n*256 + uid5[i]` (i=0..**4**만, 상위 4바이트) ✅ C가 올바름 (UID는 4바이트, 5번째는 BCC) |
| 블록 | BLOCK_ADDRS = [8,9,10], trailer 11 | blocks = [8,9,10], trailer 11 ✅ |
| Auth(11, KEY, uid) | 동일 | 동일 ✅ |

**Python uid_to_num 버그:** `for i in range(0, 5)` 이면 uid[0]~uid[4] 5바이트를 쓰는데, UID 값으로는 보통 앞 4바이트만 쓰고 5번째는 BCC이므로, Python이 5바이트 다 넣어서 계산하면 C와 수치가 다를 수 있음. C는 4바이트만 사용해서 ID가 Python과 1바이트 차이 날 수 있음. (실제로 SimpleMFRC522는 uid[0]~[4]까지 써서 40비트 숫자로 만듦. 표준적으로는 UID 4바이트만 쓰는 게 맞음.)

---

## 8. 차이 요약

**초기화(Init) 관련 차이 없음.**  
RST 한 번 HIGH, 소프트 리셋만, 같은 레지스터 값(TMode/TPrescaler/TReload/TxAuto/Mode), RFCfgReg 미설정, AntennaOn 동일. Init 순서도 동일(소프트 리셋 → 타이머 레지스터 → AntennaOn).
