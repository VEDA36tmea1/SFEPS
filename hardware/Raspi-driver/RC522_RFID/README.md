# RC522 RFID (Raspberry Pi)

Raspberry Pi SPI0 CE0에 연결된 **RC522(MFRC522) RFID 모듈**용 리눅스 커널 드라이버와 테스트 도구를 포함합니다.

---

## 폴더 구성

| 폴더/파일 | 설명 |
|-----------|------|
| **Kernel_Driver/** | 폴링 기반 커널 드라이버 (기본 버전) |
| **Kernel_Driver_irq/** | IRQ(인터럽트) 기반 커널 드라이버 |
| **Server_examples/** | 서버 연동용 C++ UDS 데몬·예제 (메인 서버와 분리 구조) |
| **User_Space_test_src/** | wiringPi 기반 사용자 공간 데모/테스트 |
| **RC522_RFID_implementation.md** | 구현 가이드·체크리스트 |
| **Dev.md** | 디버깅·트러블슈팅 기록 |

---

## Kernel_Driver vs Kernel_Driver_irq

| 항목 | Kernel_Driver | Kernel_Driver_irq |
|------|----------------|-------------------|
| **카드 감지 방식** | 폴링 (주기적으로 REQA/anticoll 시도) | IRQ(GPIO24) + waitqueue (이벤트 시 깨어남) |
| **IRQ 핀** | 사용 안 함 | GPIO24 (Pin 18) 사용 |
| **Device Tree Overlay** | `rc522-overlay.dts` / `rc522-overlay-nospidev.dts` | `rc522-overlay-irq.dts` (IRQ + pinctrl pull-up 포함) |
| **유저 API** | 동일 (`/dev/rc522`, read, ioctl) | 동일 |
| **빌드 결과물** | `Kernel_Driver/build/rc522.ko` | `Kernel_Driver_irq/build/rc522.ko` |
| **적합한 경우** | IRQ 배선 없이 빠르게 사용 | IRQ 배선 후 전력/반응성 개선 |

**주의:** 두 드라이버 모두 **모듈 이름이 `rc522`** 이므로 동시에 로드할 수 없습니다. 사용할 쪽의 오버레이만 적용하고 해당 쪽의 `.ko`만 `insmod` 하세요.

---

## 사전 준비 (라즈베리 파이)

1. **SPI 활성화**
   ```bash
   sudo raspi-config
   # Interface Options → SPI → Enable
   sudo reboot
   ```
2. **커널 헤더 설치** (모듈 빌드용)
   ```bash
   sudo apt update
   sudo apt install raspberrypi-kernel-headers
   ```

---

## 1. Kernel_Driver (폴링 버전) 탑재 및 확인

### 1-1. 빌드

```bash
cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
make
make test_rc522
make test_rc522_poll
```

- **확인:** `build/rc522.ko`, `test_rc522`, `test_rc522_poll` 파일이 생성되면 성공.

### 1-2. Device Tree Overlay 적용

```bash
cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
make dtbo
sudo cp rc522-overlay.dtbo /boot/overlays/
# 적용 실패 시: sudo cp rc522-overlay-nospidev.dtbo /boot/overlays/
```

`/boot/config.txt` **끝에** 한 줄 추가 (다른 rc522 오버레이가 있으면 주석 처리):

```text
dtoverlay=rc522-overlay
```

재부팅:

```bash
sudo reboot
```

### 1-3. 모듈 로드 (insmod)

```bash
cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
sudo insmod build/rc522.ko
```

### 1-4. 정상 동작 확인

| 확인 항목 | 명령어 | 기대 결과 |
|-----------|--------|-----------|
| 디바이스 노드 | `ls -l /dev/rc522` | `/dev/rc522` 파일 존재 |
| 모듈 로드 | `lsmod | grep rc522` | `rc522` 행이 보임 |
| 커널 로그 | `dmesg | grep rc522` | `rc522 spi0.0: ... probed successfully, /dev/rc522 created` 등 |
| VersionReg | 아래 테스트 실행 | `VersionReg(0x37) = 0x91` 또는 `0x92` |
| UID 읽기 | 카드를 리더에 댄 상태에서 테스트 실행 | UID 8자리 16진수 출력 |

**테스트 실행:**

```bash
cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
sudo ./test_rc522
# 또는 지속 폴링: sudo ./test_rc522_poll
```

- `test_rc522`: VersionReg 출력 후 UID 한 번 읽기 (카드 대기).
- `test_rc522_poll`: 카드 바꿀 때마다 UID·텍스트 출력 (종료: Ctrl+C).

---

## 2. Kernel_Driver_irq (IRQ 버전) 탑재 및 확인

### 2-1. 빌드

```bash
cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver_irq
make
```

- **확인:** `build/rc522.ko` 생성. 테스트 프로그램은 `Kernel_Driver`의 `test_rc522` / `test_rc522_poll` 를 그대로 사용 가능.

### 2-2. Device Tree Overlay 적용

**Kernel_Driver 쪽 오버레이를 끄고** IRQ 전용 오버레이만 사용해야 합니다.

```bash
cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver_irq
make dtbo
sudo cp rc522-overlay-irq.dtbo /boot/overlays/
```

`/boot/config.txt` 에서:
- `dtoverlay=rc522-overlay` 등 **기존 rc522 오버레이 라인은 주석 처리** (`#` 붙임).
- 다음 한 줄만 추가:

```text
dtoverlay=rc522-overlay-irq
```

재부팅:

```bash
sudo reboot
```

### 2-3. IRQ 모드 사용 시 풀업 확인

IRQ 모드에서는 RC522의 **IRQ 핀(GPIO24, Pin 18)** 이 사용됩니다. 보드에 따라 이 핀이 오픈드레인이라 **풀업이 없으면 엣지가 잘 잡히지 않을 수 있습니다.**

- `rc522-overlay-irq.dts` 에서 pinctrl로 GPIO24 풀업을 걸어 두었지만, 카드 태깅 시 반응이 없거나 `dmesg` 에 `spi->irq: 0` 이면 풀업을 확인하세요.
- 수동으로 풀업을 주려면 (라즈베리 파이에서):
  ```bash
  gpio -g mode 24 up
  ```
- 자세한 내용은 `Kernel_Driver_irq/README.md`, `Kernel_Driver_irq/Dev.md` 참고.

### 2-4. 모듈 로드 (insmod)

```bash
cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver_irq
sudo insmod build/rc522.ko
```

### 2-5. 정상 동작 확인

| 확인 항목 | 명령어 | 기대 결과 |
|-----------|--------|-----------|
| 디바이스 노드 | `ls -l /dev/rc522` | `/dev/rc522` 존재 |
| IRQ 번호 | `dmesg | grep rc522` | `spi->irq: 57` (0이면 오버레이/설정 점검) |
| IRQ 요청 | `dmesg | grep rc522` | `rc522_irq: requested irq 57 successfully` |
| 카드 태깅 시 | 카드 댄 뒤 `dmesg | grep rc522` | `rc522_irq: interrupt received (irq=57)` (한 번씩) |
| UID 읽기 | 아래 테스트 실행 | 카드 대면 UID 출력 |

**테스트 실행:** (같은 실행 파일 사용)

```bash
cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
sudo ./test_rc522
# 또는: sudo ./test_rc522_poll
```

- IRQ가 정상이면 카드 댈 때 **폴링보다 빠르게** 반응합니다.
- `spi->irq: 0` 이면 IRQ 미연결 상태 → 500ms 타임아웃 폴링으로만 동작.

---

## Server_examples 활용 (서버 연동)

드라이버 탑재 후, **메인 서버에 태깅 이벤트를 넘기려면** `Server_examples/` 를 사용합니다.

**추천 구조**: **Hardware Driver (C++) ↔ Unix Domain Socket ↔ Main Server**

1. **IRQ 드라이버**를 탑재하고 `/dev/rc522` 가 동작하는지 확인 (위 2. Kernel_Driver_irq 절차).
2. **C++ UDS 데몬** 빌드·실행:
   ```bash
   cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Server_examples
   g++ -o rc522_uds_daemon rc522_uds_daemon.cpp -I../Kernel_Driver -std=c++17
   sudo ./rc522_uds_daemon   # 데몬으로 동작 (또는 --no-daemon 으로 포그라운드)
   ```
3. **메인 서버**는 UDS 클라이언트로 기본 경로 `/tmp/rc522_events.sock` 에 connect 한 뒤, 소켓에서 **한 줄씩** 읽으면 NDJSON 이벤트(`device_id`, `id`, `text`, `timestamp`) 수신.

상세(옵션, 데이터 형식, 대안 예제)는 **`Server_examples/README.md`**, 아키텍처·IPC 선택 이유는 **`Server_examples/document.md`** 참고.

---

## 모듈 제거 (rmmod)

**먼저 `/dev/rc522`를 사용 중인 프로세스를 종료**한 뒤 제거하세요.

```bash
# 테스트 프로그램 종료 (해당 터미널에서 Ctrl+C)
# 또는: sudo pkill -f test_rc522

sudo rmmod rc522
```

- `Module rc522 is in use` 나오면: `sudo lsof /dev/rc522`, `sudo fuser -k /dev/rc522` 등으로 사용 프로세스 확인 후 종료하고 다시 `rmmod`.

---

## 요약 치트시트

| 단계 | Kernel_Driver (폴링) | Kernel_Driver_irq (IRQ) |
|------|---------------------|--------------------------|
| **빌드** | `cd Kernel_Driver && make && make test_rc522` | `cd Kernel_Driver_irq && make` |
| **dtbo** | `make dtbo` → `rc522-overlay.dtbo` | `make dtbo` → `rc522-overlay-irq.dtbo` |
| **config.txt** | `dtoverlay=rc522-overlay` | `dtoverlay=rc522-overlay-irq` |
| **복사** | `sudo cp rc522-overlay.dtbo /boot/overlays/` | `sudo cp rc522-overlay-irq.dtbo /boot/overlays/` |
| **재부팅** | `sudo reboot` | `sudo reboot` |
| **로드** | `sudo insmod build/rc522.ko` | `sudo insmod build/rc522.ko` |
| **확인** | `ls /dev/rc522` + `dmesg | grep rc522` | 위와 동일 + `spi->irq: 57`, `requested irq 57` |
| **테스트** | `sudo ./test_rc522` 또는 `sudo ./test_rc522_poll` | 동일 (Kernel_Driver 디렉터리의 실행 파일 사용) |

---

## 트러블슈팅

- **오버레이 적용 실패:** `Dev.md` 참고. `rc522-overlay-nospidev.dtbo`(폴링용) 시도 또는 IRQ용은 config.txt에 오버레이 한 가지만 등록했는지 확인.
- **VersionReg = 0x00:** SPI 배선·전원 점검. 풀듀플렉스 수정 이력은 `Dev.md` 참고.
- **IRQ가 0으로 나옴:** `dtoverlay=rc522-overlay-irq` 만 켜져 있는지, 재부팅 후 `insmod` 했는지 확인.
- **카드 태깅해도 IRQ 로그 없음:** GPIO24(IRQ) 배선 및 `Kernel_Driver_irq/Dev.md` 의 GPIO 풀업 설명 참고.

상세 내용은 각 하위 폴더의 **README.md**, **Work.md**, **Dev.md** 와 **RC522_RFID_implementation.md** 를 참고하세요.
