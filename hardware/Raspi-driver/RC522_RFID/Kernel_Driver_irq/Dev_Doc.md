
## RFID- Device Driver 개발 문서 정리

해당 드라이버는 MFRC522팁의 ISO/IEC 14443A/MIFARE 프로토콜을 활용해 RFID 태그를 읽고
목적에 따라 text를 변경하여 ID를 지정해(쓰기) 데모 버전에 사용하기 위한 카드 리더기 제작을 목적으로 한다. 

```text
    1. polling -> interrupt 
    -> 효율성 증가 
```
사용자 앱 호출이나 보안성을 강화해보자 . 

데이터시트의 Section 9 (Registers Overview, 페이지 11~45), Section 18 (Commands, 페이지 72~75), Section 10 (SPI Interface, 페이지 46~48), Section 12 (Interrupt Request System, 페이지 68), Section 13 (Timer Unit, 페이지 67), Section 11 (Analog Interface, 페이지 61~64) 등을 기반으로 개발 



### 1. SPI 동작 원리: MFRC522와의 통신 기반 이해

해당 모듈은 SPI를 통해 HOST(Raspi: Linux) 와 통신한다. 

`SPI slave`로 동작하고 최대 10Mbit/s 속도를 지원한다. 

`Full-duplex` ( 동시 송수신 ) 방식으로  MOSI (master- out . Slave-in ) , MISO,SCK,NSS(select slave) pin을 사용한다. 

- SPI 쓰기(Write) 동작: 코드의 rc522_spi_write_reg 함수처럼, 호스트가 주소 바이트와 데이터 바이트를 연속 전송. 주소 바이트는 (addr << 1) & 0x7E 형식으로, MSB가 0으로 쓰기 모드 표시. 데이터시트 페이지 48: "Write data" 섹션에서 "To write data to the MFRC522 using the SPI interface the following byte order has to be used. It is possible to write out up to n-data bytes by only sending one address byte." 예를 들어, TxControlReg(0x14)에 값을 쓰면 안테나 드라이버를 제어해요. 이 과정에서 FIFO 버퍼나 레지스터에 직접 접근.

- SPI 읽기(Read) 동작: rc522_spi_read_reg처럼, 주소 바이트에 0x80 OR (읽기 모드 표시). 호스트가 주소 + 더미 바이트(0x00)를 보내고, 슬레이브가 응답을 반환. 데이터시트 페이지 47: "General" 섹션에서 "The implemented SPI compatible interface is according to a standard SPI interface." 풀 듀플렉스이므로, 쓰기 중에도 읽기 데이터가 올 수 있어요. 코드에서 spi_sync를 사용해 이걸 처리.

- 메모리 접근 원리: MFRC522의 메모리는 레지스터(Section 9, 페이지 11~45)와 FIFO 버퍼(Section 12, 페이지 65)로 나뉘어요. 레지스터는 0x00~0x3F 주소로, 각 비트가 특정 기능(예: CommandReg 0x01에서 명령 실행). FIFO는 64바이트 버퍼로, 데이터 송수신 시 사용(페이지 65: "Besides writing to and reading from the FIFO-buffer, the FIFO-buffer pointers might be reset by setting the bit FlushBuffer"). SPI를 통해 레지스터 접근 시, 호스트가 주소 지정 후 읽기/쓰기. 함수 상호 작용: 읽기/쓰기 함수(dev_read_reg, dev_write_reg)가 모든 상위 함수의 기반이 돼요. 예를 들어, rc522_to_card는 FIFO에 데이터를 쓰고 CommandReg를 업데이트해 명령 실행.

- 인터럽트와 SPI 연계: 데이터시트 Section 12 (Interrupt Request System, 페이지 68)에서 CommIRqReg(0x04), DivIRqReg(0x05) 등 IRQ 비트 설명. 코드의 IRQ 핸들러(rc522_spi_irq_thread)는 spi->irq에서 신호를 받아 irq_event를 set하고 wake_up_interruptible로 블로킹 함수 깨움. SPI 접근과 IRQ는 병렬: IRQ가 발생하면 CommIrqReg를 읽어 이벤트 확인.

- SPI 타이밍: 데이터시트 Section 23.8 (Timing for the SPI compatible interface)에서 SCK 클럭 주기 등 지정. 코드에서 spi->max_speed_hz로 설정(로그: "max_speed: %d Hz").

### 2. 메모리 접근과 함수 상호 작용 개요

레지스터 접근: 모든 함수가 dev_read_reg/dev_write_reg를 통해. 데이터시트 Table 5 (Registers Overview, 페이지 11~12)에서 주소 목록. 예: CommandReg(0x01) 쓰기로 명령 시작(PCD_RESETPHASE=0x0F 등, 페이지 14).
FIFO 접근: FIFODataReg(0x09)로 데이터 입/출력, FIFOLevelReg(0x0A)로 레벨 확인(페이지 21~22). rc522_to_card처럼 FIFO 플러시(FlushBuffer=1) 후 데이터 쓰기.
함수 상호 작용 체인: rc522_init_chip → rc522_reset (CommandReg 쓰기) → rc522_antenna_on (TxControlReg 비트 set). 상위 함수(rc522_read_uid_blocking)는 rc522_request → rc522_anticoll 호출. IRQ 모드: wait_event_interruptible로 대기 → IRQ 핸들러가 wake → no_block 함수 호출.
에러 핸들링: ErrorReg(0x06, 페이지 18) 체크(예: BufferOvfl, CollErr). 코드에서 rc522_to_card가 err & 0x1B == 0 확인.

이제 함수별로 자세히 정리. 각 항목: 코드 구현 설명 → 데이터시트 연관 → 상호 작용 → SPI/메모리 접근 포인트.

### 3. 함수별 상세 정리 (rc522_core.c 중심)
rc522_set_bitmask / rc522_clear_bitmask

구현: 레지스터 읽기(tmp = read_reg) 후 OR/AND 연산으로 비트 수정, write_reg. 예: TxControlReg에 0x03 set.
데이터시트 연관: Section 9.1.1 (Register Bit Behavior, 페이지 13): r/w 비트 설명. Table 6: "Behavior of Register Bits and its Designation". TxControlReg(0x14, 페이지 27): "Controls the logical behavior of the antenna driver pins TX1 and TX2".
상호 작용: rc522_antenna_on에서 호출. 다른 함수(rc522_to_card)가 IRQ 비트 set/clear에 사용. 체인: init_chip → antenna_on → set_bitmask.
SPI/메모리: SPI 읽기/쓰기 2회. 메모리: 레지스터 직접 접근, atomic-like (단일 연산).

rc522_antenna_on

구현: TxControlReg 읽기, 0x03 비트가 아니면 set_bitmask.
데이터시트 연관: Section 11.2 (TX Driver, 페이지 61): "Controls the logical behavior of the antenna driver pins TX1 and TX2". Table 47 (페이지 27): InvTx2RFOn 등 비트. 안테나 활성화로 RF 필드 생성.
상호 작용: rc522_init_chip에서 호출. 태그 통신 전 필수. 후속: rc522_to_card (Transceive 시 안테나 사용).
SPI/메모리: 읽기 1회, 쓰기 1회 (set_bitmask 경유). 메모리: TxControlReg(0x14) 접근.

rc522_reset

구현: CommandReg에 PCD_RESETPHASE(0x0F) 쓰기, msleep(50).
데이터시트 연관: Section 18.3.1.10 (SoftReset Command, 페이지 75): "This command performs a reset of the device. The configuration data of the internal buffer remains unchanged. All registers are set to the reset values." 페이지 72: Command overview, Soft Reset=1111.
상호 작용: rc522_init_chip에서 호출 (soft reset). GPIO reset 후 soft reset. 체인: probe → core_init → reset.
SPI/메모리: 쓰기 1회. 메모리: CommandReg(0x01, 페이지 14) 접근. 리셋 후 SerialSpeedReg(0x1F) 등 초기화 (페이지 75: 9.6 kbps로 reset).

rc522_init_chip

구현: reset 호출 후 TModeReg=0x8D, TPrescalerReg=0x3E 등 쓰기, antenna_on.
데이터시트 연관: Section 13 (Timer Unit, 페이지 67): TModeReg(0x2A, 페이지 37), TPrescalerReg(0x2B, 페이지 37)로 타이머 설정 (fTimer = 6.78 MHz / TPreScaler). TxAutoReg(0x15, 페이지 28): Force100ASK 등. ModeReg(0x11, 페이지 25): MSBFirst, CRCPreset.
상호 작용: probe에서 호출. 타이머는 IRQ/타임아웃에 사용. 체인: reset → antenna_on. 후속: 모든 통신 함수.
SPI/메모리: 여러 쓰기 (타이머 레지스터 0x2A~0x2D, 페이지 37~38). 메모리: 타이머 reload 값(TReloadRegH/L=0/30)으로 카운트다운 설정.

rc522_calculate_crc

구현: DivIrqReg 클리어(0x04), FIFO 플러시(0x80), FIFODataReg에 데이터 쓰기, CommandReg=PCD_CALCCRC(0x03), 루프 대기 후 CRCResultReg 읽기.
데이터시트 연관: Section 11.5 (CRC co-processor, 페이지 64): "The following parameters of the CRC co-processor can be configured. The CRC preset value can either be 0000h, 6363h, A671h or FFFFh". Table 147: 16-bit CRC, ISO/IEC 14443A 알고리즘. CRCResultRegM/L(0x21/0x22, 페이지 33).
상호 작용: rc522_read_block/write_block에서 호출 (CRC 계산). 체인: to_card → calculate_crc. IRQ: DivIrqReg(0x05, 페이지 16)로 CRC 완료 확인.
SPI/메모리: 여러 쓰기(FIFODataReg 0x09, 반복), 읽기(CRCResultReg). 메모리: FIFO 버퍼 사용 (페이지 65: FIFOLevelReg 플러시).

rc522_to_card

구현: IRQ 설정(CommIEnReg=irq_en|0x80), FIFO 플러시, 데이터 쓰기, CommandReg=command (AUTHENT=0x0E or TRANSCEIVE=0x0C), BitFramingReg set, 루프 대기(i=2000, usleep), ErrorReg 체크, FIFO 읽기.
데이터시트 연관: Section 18.3 (Commands Overview, 페이지 72): Transceive=1100, Authent=1110. CommIEnReg(0x02, 페이지 14): IRQ enable. CommIrqReg(0x04, 페이지 16): IRQ 상태. ErrorReg(0x06, 페이지 18): WrErr, BufferOvfl 등. BitFramingReg(0x0D, 페이지 23): StartSend=1로 전송 시작.
상호 작용: 거의 모든 상위 함수(request, anticoll, authenticate, read_block 등)에서 호출. 체인: request → to_card (TRANSCEIVE). IRQ: wait_irq (RxIRq/TxIRq) 대기 (페이지 68: Interrupt Sources).
SPI/메모리: 다중 읽기/쓰기 (FIFOLevelReg 0x0A, ControlReg 0x0C). 메모리: FIFO 버퍼 입/출력 (페이지 21: FIFODataReg dy 비트).

rc522_request

구현: BitFramingReg=0x07, buf[0]=req_mode(PICC_REQIDL=0x26), to_card(TRANSCEIVE).
데이터시트 연관: Section 18.3.1 (Command Description, 페이지 73): REQA/WUPA Commands (PICC_REQIDL). BitFramingReg(페이지 23): RxAlign/TxLastBits로 비트 프레이밍.
상호 작용: rc522_read_uid_no_block에서 호출. 체인: request → to_card. 후속: anticoll.
SPI/메모리: to_card 경유. 메모리: BitFramingReg(0x0D).

rc522_anticoll

구현: BitFramingReg=0x00, ser_num=PICC_ANTICOLL(0x93), to_card, UID 체크섬 검증.
데이터시트 연관: Section 18.3.1.2 (Anticollision, 페이지 추정, but from search: Anticollision 루프). CollReg(0x0E, 페이지 23): CollErr.
상호 작용: request 후 호출. 체인: anticoll → to_card. 후속: select_tag.
SPI/메모리: to_card 경유. 메모리: back_data FIFO 읽기.

rc522_select_tag

구현: buf=PICC_SELECTTAG(0x93), calculate_crc, to_card.
데이터시트 연관: Section 18.3.1.3 (Select Command, 페이지 추정).
상호 작용: anticoll 후. 체인: select → to_card → calculate_crc.
SPI/메모리: calculate_crc 경유 FIFO.

rc522_authenticate

구현: buff=auth_mode(PICC_AUTHENT1A=0x60), 섹터 키/UID 쓰기, to_card(AUTHENT), Status2Reg 체크(Crypto1On=0x08).
데이터시트 연관: Section 18.3.1.9 (MFAuthent Command, 페이지 74~75): "Performs the MIFARE® standard authentication". Status2Reg(0x08, 페이지 20): MFCrypto1On.
상호 작용: read_text_sector에서 호출. 체인: authenticate → to_card. 후속: read_block (인증 후).
SPI/메모리: to_card 경유 FIFO 12바이트.

rc522_stop_crypto1

구현: Status2Reg 클리어(0x08).
데이터시트 연관: 페이지 75: Crypto1On=0으로 인증 종료.
상호 작용: authenticate 후. clear_bitmask 경유.
SPI/메모리: 읽기/쓰기.

rc522_read_block / rc522_write_block

구현: buf=PICC_READ/WRITE, calculate_crc, to_card, 데이터 복사/쓰기.
데이터시트 연관: Section 18.3.1.4 (Read/Write Commands, 페이지 추정). CRC 사용 (페이지 64).
상호 작용: authenticate 후. 체인: read_block → to_card → calculate_crc.
SPI/메모리: FIFO 16바이트 블록.

rc522_read_uid_no_block / rc522_read_uid_blocking

구현: request + anticoll, uid_to_num. blocking: while 루프 + wait_event_interruptible (IRQ 대기, timeout 500ms).
데이터시트 연관: Anticollision/Select (페이지 72~74). Section 12 IRQ (페이지 68: TimerIRq 등).
상호 작용: chardev_read/ioctl에서 호출. 체인: no_block → request → anticoll. IRQ: irq_event set (핸들러) → wake.
SPI/메모리: 다중 to_card.

rc522_read_text_sector_blocking / rc522_write_text_sector_blocking

구현: request + anticoll + select + authenticate + 3블록 read/write (default_key FF*6).
데이터시트 연관: MIFARE Classic 섹터 구조 (페이지 74: Authent). 블록 16바이트 (페이지 64).
상호 작용: ioctl에서 호출. 체인: 전체 통신 플로우.
SPI/메모리: 여러 블록 FIFO.

### 4. chardev와 spi 파일 상호 작용

rc522_chardev_ioctl: RC522_RESET → soft_reset, READ_CARD → read_uid_blocking. SPI ops를 통해 core 호출.
rc522_spi_probe: GPIO reset, core_init, IRQ 요청 (IRQF_TRIGGER_FALLING, 페이지 68 IRQ). 데이터시트 페이지 71: Reset Timing.
상호: probe → core_init → chardev_register. IRQ → core의 waitq wake.