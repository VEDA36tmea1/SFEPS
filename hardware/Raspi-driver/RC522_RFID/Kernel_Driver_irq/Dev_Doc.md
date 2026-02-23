
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