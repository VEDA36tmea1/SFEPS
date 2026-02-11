## RC522 (MFRC522) Linux Kernel Driver (Raspberry Pi)

이 폴더는 Raspberry Pi의 **SPI0 CE0**에 연결된 **RC522 RFID 모듈**을 위한 리눅스 커널 모듈(Out-of-tree)과 Device Tree Overlay, 테스트 프로그램을 포함합니다.

### 하드웨어 연결 (기본)

- **SPI0 CE0 (SDA/SS)**: GPIO8 (물리 24)
- **SCK**: GPIO11 (물리 23)
- **MOSI**: GPIO10 (물리 19)
- **MISO**: GPIO9 (물리 21)
- **RST**: GPIO25 (물리 22)
- **3.3V / GND**: 전원
- **IRQ(GPIO24)**: 미사용(초기 구현에서 제외)

### 디렉토리 구성 / 코드 구조

- **`rc522_spi.c`**
  - `spi_driver` 구현(Probe/Remove)
  - SPI 레지스터 R/W (full-duplex 읽기 포함)
  - RST GPIO 제어(선택)
  - 코어 초기화 + chardev 등록/해제
- **`rc522_core.c`**
  - MFRC522 프로토콜/로직(REQA, anticoll, select, auth, block read/write, UID/텍스트 섹터 read/write)
  - 카드 감지/폴링 루프는 시그널(Ctrl+C)로 깨지도록 처리
- **`rc522_chardev.c`**
  - `/dev/rc522` 문자 디바이스(misc device)
  - `read()` = UID 4바이트 블로킹 읽기
  - `ioctl()` = RESET/UID/레지스터/텍스트 섹터 읽기
- **`rc522.h`**
  - 레지스터/명령 정의, 공용 구조체/프로토타입
- **`rc522_ioctl.h`**
  - 유저/커널 공용 IOCTL 정의 및 데이터 구조체
- **`rc522-overlay.dts` / `rc522-overlay-nospidev.dts`**
  - SPI0 CE0에 `rc522@0` 디바이스를 생성하는 DTO
  - spidev 충돌 방지(CE0를 rc522가 사용하도록 spidev 비활성화 포함)
- **`Makefile`**
  - 커널 모듈 빌드(결과물은 `build/`에 생성)
  - dtbo 빌드, 테스트 앱 빌드 타깃 제공
- **테스트 프로그램**
  - `test_rc522.c`: VersionReg 확인 + UID(read/ioctl) 1회 테스트
  - `test_rc522_poll.c`: 지속 폴링(UID 변경 감지 + 텍스트 읽기), Ctrl+C 종료
  - `run_test.sh`: 로드/기본 점검/테스트 실행 헬퍼

### 빌드

라즈베리 파이(타깃)에서 커널 헤더가 준비되어 있어야 합니다.

```bash
cd /home/physical-100/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
make
```

- 결과 모듈: `build/rc522.ko`
- `make clean`으로 `build/` 결과물 정리

### Device Tree Overlay 빌드 및 적용

```bash
cd /home/physical-100/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
make dtbo
```

#### 방법 A) config.txt에 등록(권장)

```bash
sudo cp rc522-overlay.dtbo /boot/overlays/
sudo nano /boot/config.txt
```

`/boot/config.txt` 끝에 추가:

```text
dtoverlay=rc522-overlay
```

적용:

```bash
sudo reboot
```

#### 방법 B) overlay 실패 시 대안

- 오버레이 적용이 실패하면 `rc522-overlay-nospidev.dtbo`를 사용하거나,
- 원인/대응은 상위 문서 `../Dev.md`를 참고하세요.

### 커널 모듈 로드/언로드

#### 로드

오버레이 적용(재부팅) 후:

```bash
cd /home/physical-100/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
sudo insmod build/rc522.ko
ls -l /dev/rc522
dmesg | grep rc522
```

#### 언로드(중요: 유저 프로세스 먼저 종료)

`/dev/rc522`를 열고 있는 유저 프로그램이 있으면 `rmmod`가 실패할 수 있습니다.

```bash
# 1) 유저 영역 프로세스부터 종료
ps -ef | grep test_rc522
sudo kill <PID>        # 정상 종료 시도 (SIGTERM)
sudo kill -9 <PID>     # 마지막 수단 (SIGKILL)

# 2) 디바이스 사용 여부 확인
sudo lsof /dev/rc522
sudo fuser /dev/rc522

# 3) 모듈 제거
sudo rmmod rc522
```

### 유저 공간 API (/dev/rc522)

#### 1) UID 읽기 (read)

- `read(fd, buf, 4)` → UID 4바이트(빅엔디안으로 묶어서 출력 가능)

#### 2) IOCTL

`rc522_ioctl.h`에 정의:

- `RC522_RESET`: 소프트 리셋
- `RC522_READ_CARD`: UID 블로킹 읽기(`__u32*`)
- `RC522_READ_REG` / `RC522_WRITE_REG`: 레지스터 1바이트 R/W (`struct rc522_reg_data`)
- `RC522_READ_TEXT_SECTOR`: 섹터(3블록) 텍스트 읽기 (`struct rc522_read_text`)

### 테스트

```bash
cd /home/physical-100/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
make test_rc522
make test_rc522_poll

sudo ./test_rc522
sudo ./test_rc522_poll --trailer 11
```

빠르게 로드/테스트를 한번에:

```bash
sudo ./run_test.sh
```

### 트러블슈팅

- `/dev/rc522`가 안 생김, overlay 적용 실패, VersionReg=0x00, spidev 충돌 등은 `../Dev.md`에 정리되어 있습니다.

