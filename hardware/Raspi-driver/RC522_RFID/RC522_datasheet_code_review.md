# RC522 코드 구현 리뷰 (데이터시트 근거 매핑)

## 1. 리뷰 목적

`src/rc522_full_demo.c`와 `src/rc522_full.c`가 MFRC522 데이터시트의 어떤 동작/규칙을 근거로 구현되었는지 정리한다.

참고:
- 워크스페이스에서 `SZH-EK0404.pdf` 파일은 현재 확인되지 않았다.
- 따라서 본 문서는 **MFRC522 표준 데이터시트 규격(명령/레지스터/프레임 규칙)** 기준으로 매핑했다.
- PDF가 동기화되면 마지막 섹션의 "페이지 매핑 표"에 정확한 페이지/섹션 번호를 채우면 된다.

---

## 2. 대상 파일

- `src/rc522_full.c` (핵심 프로토콜/레지스터 구현)
- `src/rc522_full_demo.c` (CLI 데모/사용자 인터페이스)
- `src/rc522_full.h` (공개 API)

---

## 3. 데이터시트 근거와 코드 매핑

### 3.1 SPI 레지스터 Read/Write 프레임

데이터시트 근거:
- 주소 프레임에서 `addr`는 `(addr << 1) & 0x7E` 형태
- Read는 MSB(0x80) set

코드 매핑:
- `rc522c_write_reg()`에서 `(addr << 1) & 0x7E`
- `rc522c_read_reg()`에서 `((addr << 1) & 0x7E) | 0x80`

검토 결과:
- 데이터시트 SPI 접근 규칙과 일치.

### 3.2 리셋 및 초기화 시퀀스

데이터시트 근거:
- `CommandReg`에 `PCD_RESETPHASE(0x0F)`로 소프트리셋
- 타이머/모드/ASK/안테나 관련 레지스터 초기 설정 필요

코드 매핑:
- `rc522c_reset()`:
  - GPIO RST 토글(옵션)
  - `CommandReg <- PCD_RESETPHASE`
- `rc522c_init_chip()`:
  - `TModeReg=0x8D`
  - `TPrescalerReg=0x3E`
  - `TReloadRegL/H=30/0`
  - `TxAutoReg=0x40`
  - `ModeReg=0x3D`
  - `TxControlReg` bit0/1 set로 안테나 ON

검토 결과:
- RC522 bring-up 시퀀스로 타당하며 Python `mfrc522` 계열 구현과도 정렬됨.

### 3.3 PCD_TRANSCEIVE 공통 루틴 (`ToCard`)

데이터시트 근거:
- IRQ enable 설정, FIFO clear, command 실행, `BitFramingReg` StartSend bit 제어
- `CommIrqReg`/`ErrorReg`로 완료/에러 판정
- `FIFOLevelReg`와 `ControlReg(lastBits)`로 수신 비트 길이 계산

코드 매핑:
- `rc522c_to_card()`:
  - `CommIEnReg`, `CommIrqReg`, `FIFOLevelReg`, `CommandReg` 사용
  - `PCD_TRANSCEIVE` 시 `BitFramingReg` bit7 set/clear
  - `ErrorReg & 0x1B` 검사
  - `back_bits = (fifo-1)*8 + lastBits` 계산

검토 결과:
- 데이터시트 권장 처리 흐름과 부합.

### 3.4 REQA/WUPA (카드 탐지)

데이터시트 근거:
- `PICC_REQIDL(0x26)` 전송
- 7-bit framing 요구 (`BitFramingReg=0x07`)
- ATQA 응답 2바이트(16비트) 기대

코드 매핑:
- `rc522c_request()`:
  - `BitFramingReg=0x07`
  - `req_mode` 1바이트 전송
  - `back_bits == 0x10` 조건 확인

검토 결과:
- REQA 처리 규칙을 정확히 반영.

### 3.5 Anti-collision (UID CL1)

데이터시트 근거:
- `[0x93, 0x20]` 전송
- UID 4바이트 + BCC 1바이트 = 총 40비트
- BCC 검증 필요

코드 매핑:
- `rc522c_anticoll()`:
  - `PICC_ANTICOLL`, `0x20` 전송
  - `back_bits == 40` 확인
  - `uid[0..3] XOR == uid[4]` BCC 검증

검토 결과:
- 데이터시트 anti-collision 규칙을 잘 반영.

### 3.6 Select Tag (SAK 획득)

데이터시트 근거:
- Select frame: `0x93 0x70 + UID(5) + CRC_A(2)`
- 응답으로 SAK 수신

코드 매핑:
- `rc522c_select_tag()`:
  - 위 프레임 구성
  - `rc522c_calculate_crc()`로 CRC 생성
  - 응답 비트 길이 `0x18` 확인 후 SAK 반환

검토 결과:
- Select 프레임 구성 및 CRC 처리 타당.

### 3.7 MIFARE Classic 인증

데이터시트 근거:
- `PICC_AUTHENT1A(0x60)` 또는 `1B(0x61)`
- 블록 주소 + 키6 + UID4 전송
- 성공 후 `Status2Reg.MFCrypto1On` 확인

코드 매핑:
- `rc522c_authenticate()`:
  - 12바이트 인증 프레임 구성
  - `PCD_AUTHENT` 실행
  - `Status2Reg & 0x08` 체크

검토 결과:
- 인증 시퀀스가 데이터시트와 정합.

### 3.8 블록 READ

데이터시트 근거:
- `[PICC_READ(0x30), block] + CRC_A`
- 응답은 16바이트 데이터(+CRC_A 2바이트)

코드 매핑:
- `rc522c_read_block()`:
  - READ 프레임 + CRC 전송
  - `back_bits >= 128` 최소 조건 확인
  - 앞 16바이트를 데이터로 사용

검토 결과:
- 실장 관점에서 합리적(데이터 16바이트 중심 처리).

### 3.9 블록 WRITE

데이터시트 근거:
- 1단계: `[PICC_WRITE(0xA0), block] + CRC_A`
- 태그 ACK는 4-bit `0x0A`
- 2단계: `16-byte data + CRC_A`
- 재차 ACK(4-bit `0x0A`) 확인

코드 매핑:
- `rc522c_write_block()`:
  - 2단계 전송 구현
  - 각 단계마다 `back_bits == 4` 및 `(ack & 0x0F) == 0x0A` 검사

검토 결과:
- MIFARE write handshake를 정확히 구현.

### 3.10 Crypto 종료

데이터시트 근거:
- 인증 종료 후 `MFCrypto1On` clear 필요

코드 매핑:
- `rc522c_stop_crypto1()`에서 `Status2Reg` bit3 clear

검토 결과:
- 인증 세션 정리 동작 정상.

---

## 4. `rc522_full_demo.c` 리뷰

구현 개요:
- `--id`: UID만 출력 (`rc522c_read_id_blocking`)
- 기본: 섹터 텍스트 읽기 (`rc522c_read_text_sector_blocking`)
- `--write`: 섹터 텍스트 쓰기 (`rc522c_write_text_sector_blocking`)
- `--ch`, `--speed`, `--rst`로 환경 파라미터 조정 가능

장점:
- 데이터시트 레벨 저수준을 API로 캡슐화해서 사용성이 좋다.
- bring-up/검증에 필요한 옵션이 CLI로 열려 있어 실험하기 편하다.

주의:
- blocking 방식이라 태그가 없으면 계속 대기한다.
- `--trailer` 값 유효성은 라이브러리에서 추가 검증한다.

---

## 5. 코드 품질/리스크 리뷰 (개선 제안)

1) **Select 결과 미검증**
- 위치: `rc522c_read_text_sector_blocking()`, `rc522c_write_text_sector_blocking()`
- 현재 `rc522c_select_tag(uid5)` 반환값(SAK)을 검사하지 않음
- 제안: `<=0`이면 즉시 실패 처리

2) **무한 블로킹 정책**
- 위치: `rc522c_read_id_blocking()`, read/write blocking 루프
- 장시간 태그 미접근 시 복귀 없음
- 제안: timeout 버전 API 추가 (`*_timeout_ms`)

3) **기본 키(A=FF..FF) 고정**
- 보안상 취약하며 실카드에서 실패 빈도 증가 가능
- 제안: API로 Key A/Key B 주입 가능하도록 확장

4) **문자열 변환 단순 복사**
- 위치: `rc522c_read_text_sector_blocking()`
- 바이너리 데이터/중간 NUL 포함 시 문자열 해석이 깨질 수 있음
- 제안: raw 48-byte API 추가 + 상위에서 인코딩 처리

5) **응답 CRC 검증 생략**
- 현재는 RC522 내부 CRC 계산 송신에는 사용하나, 수신 데이터 CRC 검증은 미실시
- 제안: 필요 시 수신 CRC 검증 옵션 추가

---

## 6. PDF 페이지 매핑 템플릿 (추후 채우기)

`SZH-EK0404.pdf` 동기화 후 아래 표에 페이지/섹션 번호를 넣으면 문서 완성도가 높아진다.

| 코드 항목 | 데이터시트 키워드 | PDF 페이지/섹션 |
| --- | --- | --- |
| SPI read/write 프레임 | SPI interface, address byte format | TODO |
| `CommandReg` reset/init | PCD_RESETPHASE, initialization | TODO |
| `rc522c_request` | REQA/WUPA, 7-bit frame | TODO |
| `rc522c_anticoll` | Anti-collision CL1, BCC | TODO |
| `rc522c_select_tag` | Select command, SAK | TODO |
| `rc522c_authenticate` | MIFARE auth A/B, Crypto1 | TODO |
| `rc522c_read_block` | MIFARE Read command | TODO |
| `rc522c_write_block` | MIFARE Write + 4-bit ACK | TODO |
| `rc522c_stop_crypto1` | MFCrypto1On bit clear | TODO |

---

## 7. 결론

- 현재 `rc522_full.c` 구현은 MFRC522/MIFARE Classic 핵심 프로토콜(REQA, anticoll, select, auth, read/write, ACK 규칙)에 대체로 잘 맞는다.
- 특히 ACK를 4-bit로 처리하고 BCC 검증을 수행하는 점은 데이터시트 정합성 측면에서 중요하고 올바른 구현이다.
- 제품화 단계에서는 timeout/키 관리/SAK 검증/CRC 검증을 보강하면 안정성과 재현성이 더 좋아진다.

