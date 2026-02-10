# RC522 개발 노트 (디버깅 기록)

## 1. 배경

- Raspberry Pi 4 + RC522 모듈 조합에서
  - Python 라이브러리(`mfrc522.SimpleMFRC522`)로는 UID/데이터 읽기·쓰기가 잘 동작하는데,
  - 직접 작성한 C 코드에서는
    - UID가 카드 없이도 `20 20 20 20` 등 이상한 값으로 찍히거나,
    - `read failed`, `write failed`가 반복 발생하는 문제가 있었다.
- 이 문서는 **Python 구현과 C 포팅 사이의 차이(특히 비트/바이트 길이 처리)** 때문에 생긴 문제를 정리하고,
  최종적으로 어떻게 수정했는지 기록한다.

## 2. 초기 증상

- C 테스트 코드(`rc522_test.c`)에서:
  - 카드가 없는데도 `UID: 20 20 20 20` 같은 값이 계속 출력됨.
  - 카드 UID가 실제 값과 다르게 보이거나, 노이즈가 UID로 찍히는 현상.
- `rc522_full_demo`로 섹터 텍스트 읽기/쓰기를 시도하면:
  - `read failed`
  - `write failed`
  가 자주 발생.

요약하면:

- **UID 쪽**: “태그가 없는데도 UID가 찍힘”.
- **블록 READ/WRITE 쪽**: “Python은 잘 되는데 C는 항상 실패 처리”.

## 3. UID 문제: BCC(체크섬) 검증 누락

### 3.1 Python 구현

Python `MFRC522.Anticoll()` 코드는 다음과 같이 동작한다:

- `MFRC522_ToCard(PCD_TRANSCEIVE, [PICC_ANTICOLL, 0x20])` 호출
- 응답 `backData`를 받으면:
  - 길이가 5바이트인지 확인
  - `backData[0..3]` XOR 한 값이 `backData[4]`와 같은지(BCC) 검증
  - 조건을 만족하지 않으면 `MI_ERR`

즉,

- **UID 4바이트 + BCC 1바이트 = 총 5바이트**가 와야 하고,
- **XOR 체크가 맞을 때만 “정상 UID”로 인정**한다.

### 3.2 C 코드의 초기 상태

- 초기 C 구현(`rc522_anticoll`)에서는:
  - FIFO에서 5바이트를 읽어서 그대로 UID 버퍼에 복사만 하고,
  - **BCC(마지막 바이트) 체크를 하지 않았다.**
- 그 결과:
  - 노이즈나 미완성 응답이 들어와도 “일단 5바이트 있으면 UID”로 취급되었고,
  - 이것이 `20 20 20 20` 같은 **가짜 UID**로 반복 출력되는 원인이었다.

### 3.3 수정 내용

- `rc522_test.c` / `rc522_full.c`의 anticollision 로직을 Python과 동일하게 수정:
  - FIFO에서 UID(4) + BCC(1) 총 5바이트를 읽고,
  - `uid[0] ^ uid[1] ^ uid[2] ^ uid[3] ^ bcc == 0` 인지 검사.
  - 조건이 맞지 않으면 `MI_ERR` 반환 → 이 경우 태그 없음/에러로 처리하고 UID를 출력하지 않음.

이 수정 이후:

- 카드를 대지 않으면 UID가 출력되지 않고,
- 실제 카드가 근처에 있을 때만 정상 UID가 찍히도록 동작이 안정됐다.

## 4. READ/WRITE 실패 원인: 비트 길이(ACK/CRC) 처리 차이

이슈의 핵심은 **“Python은 몇 비트로 응답을 보고, C에서는 몇 비트로 가정했는가”** 이다.

### 4.1 Python `MFRC522_ToCard`의 길이 처리

Python 구현에서:

- FIFO에서 읽어온 바이트 수 = `n`
- ControlReg의 `lastBits`(마지막 바이트의 유효 비트 수)를 조합해서:

```python
if lastBits != 0:
    backLen = (n - 1) * 8 + lastBits
else:
    backLen = n * 8
```

- 그리고 중요한 포인트:
  - **`MAX_LEN = 16`** 으로 정의되어 있어서,
  - `backData`는 최대 16바이트까지만 저장한다.

즉,

- 실제 카드 응답이 18바이트(데이터 16 + CRC 2)를 보내도,
  - Python 쪽에서는 **앞 16바이트만 보관**하고 CRC는 버린다.

### 4.2 READ(블록 읽기) 길이 체크 차이

Python `ReadTag()`:

- `backData` 길이가 **16바이트이면 성공**으로 본다.
- CRC 2바이트는 애초에 `MAX_LEN=16` 때문에 잘려서 들어오므로 신경 쓰지 않는다.

초기 C 구현(`rc522c_read_block()`):

```c
status = rc522c_to_card(..., back_data, &back_bits);
if (status != MI_OK) return MI_ERR;
if (back_bits != 16 * 8) return MI_ERR;
```

- 여기서 **실제 하드웨어는 18바이트(16바이트 데이터 + 2바이트 CRC)를 보낼 수 있는데**,  
  `back_bits`가 16*8(=128)으로 딱 떨어지지 않으면 무조건 실패로 처리했다.
- Python은 “바이트 수 16개만 맞으면 OK”인데,
  C에서는 “비트 수가 정확히 128인지”를 보고 있었던 것.

#### 수정 후 C 코드

```c
status = rc522c_to_card(..., back_data, &back_bits);
if (status != MI_OK) return MI_ERR;

// 최소 16바이트(=128비트) 이상이면 성공으로 간주, 앞 16바이트만 사용
if (back_bits < 16 * 8) return MI_ERR;

memcpy(out_16bytes, back_data, 16);
```

- 이렇게 바꾼 뒤에는,
  - 실제 응답이 18바이트(16 + CRC2)여도 **앞 16바이트만 데이터로 사용**해서,
  - Python 라이브러리와 동일한 동작을 하게 된다.

### 4.3 WRITE(블록 쓰기) ACK 길이 차이

Python `WriteTag()`에서:

- `PICC_WRITE` 명령을 보내고 나면,
  - **4비트 ACK** (`0x0A`)를 기대한다.
- Python은 다음 조건으로 검사한다:

```python
if not (status == MI_OK) or not (backLen == 4) or not ((backData[0] & 0x0F) == 0x0A):
    status = self.MI_ERR
```

초기 C 구현(`rc522c_write_block()`):

```c
status = rc522c_to_card(..., back_data, &back_bits);
if ((status != MI_OK) || (back_bits != 4 * 8) || ((back_data[0] & 0x0F) != 0x0A)) {
    return MI_ERR;
}
```

- 여기서 **ACK를 4비트가 아니라 “4바이트(32비트)”로 잘못 가정**해서
  - 실제로는 4비트(= back_bits == 4)가 정상인데,
  - 코드에서는 `back_bits == 4*8`(=32)만 허용 → 항상 실패로 처리됐다.

#### 수정 후 C 코드

```c
// 1단계: WRITE 명령 전송
status = rc522c_to_card(..., back_data, &back_bits);
if ((status != MI_OK) || (back_bits != 4) || ((back_data[0] & 0x0F) != 0x0A)) {
    return MI_ERR;
}

// 2단계: 16바이트 데이터 + CRC 2바이트 전송
status = rc522c_to_card(..., back_data, &back_bits);
if ((status != MI_OK) || (back_bits != 4) || ((back_data[0] & 0x0F) != 0x0A)) {
    return MI_ERR;
}
```

- 이제 Python이 기대하는 것과 동일하게:
  - **ACK 길이 = 4비트(back_bits == 4)**,
  - 하위 4비트가 `0x0A`인지 확인.

### 4.4 정리: Python vs C 길이 처리 비교

- **Python READ**
  - FIFO 응답: 16바이트 데이터 + 2바이트 CRC (총 18바이트 가능)
  - `MAX_LEN=16` → 앞 16바이트만 저장, CRC는 버림
  - 성공 조건: `len(backData) == 16`

- **초기 C READ (버그)**
  - 성공 조건: `back_bits == 16*8` (CRC까지 포함한 비트 수가 정확히 128이어야 함)
  - 실제 응답이 18바이트일 경우 조건 불일치 → 항상 실패

- **수정 C READ**
  - 성공 조건: `back_bits >= 16*8`
  - 앞 16바이트만 데이터로 사용 → Python과 동일

---

- **Python WRITE ACK**
  - 기대: 4비트 ACK (`backLen == 4`, `backData[0] & 0x0F == 0x0A`)

- **초기 C WRITE (버그)**
  - 기대: `back_bits == 4*8` (32비트) → 실제 4비트 ACK와 불일치

- **수정 C WRITE**
  - 기대: `back_bits == 4` (4비트), `backData[0] & 0x0F == 0x0A`

이 차이를 맞춰준 뒤에는 C 코드에서도 Python과 동일하게 블록 READ/WRITE가 안정적으로 동작하게 되었다.

## 5. 마무리 및 교훈

- **RFID/저수준 프로토콜 포팅 시, “몇 바이트냐”뿐 아니라 “몇 비트냐”까지 정확히 맞춰봐야 한다.**
- 특히:
  - 응답 끝의 **CRC 바이트를 라이브러리가 어떻게 처리하는지**,
  - ACK 응답이 **4비트인지 1바이트인지**,
  - Python 쪽이 `MAX_LEN` 같은 상수로 응답을 잘라버리고 있지는 않은지
  를 그대로 따라가지 않으면 C 포팅이 애매하게 실패할 수 있다.
- 이번 수정으로:
  - UID BCC 체크 추가,
  - READ 시 최소 16바이트 이상이면 앞 16바이트 사용,
  - WRITE ACK를 4비트로 인식
  등 Python과 동일한 규칙을 C에 반영했고,
  **Python 코드와 C 코드가 동일한 카드/리더 환경에서 같은 결과를 내도록 정렬**했다.

