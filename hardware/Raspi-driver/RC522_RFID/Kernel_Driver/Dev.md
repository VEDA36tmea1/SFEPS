# RC522 커널 드라이버 — 개발/트러블슈팅 (Dev.md)

문제점과 대응 방법을 정리한 문서입니다.

---

## 0. `/dev/rc522` 가 안 생길 때 (가장 흔한 경우)

### 현상
```text
ls -l /dev/rc522
ls: cannot access '/dev/rc522': No such file or directory
```
`lsmod | grep rc522` 도 비어 있거나, 모듈은 있는데 디바이스만 없음.

### 이유
`/dev/rc522` 는 **다음 순서**로만 생성됩니다.

1. **디바이스 트리 오버레이 적용** → SPI 버스에 `compatible = "nxp,rc522"` 인 자식 노드(rc522@0)가 붙음.
2. **rc522.ko 로드** (수동 `insmod` 또는 부팅 시 자동).
3. **SPI 드라이버 probe** → 오버레이로 생긴 장치와 매칭되면 `rc522_spi_probe()` 실행 → misc_register → **`/dev/rc522` 생성**.

오버레이가 적용되지 않으면 SPI 쪽에 “nxp,rc522” 장치가 없어서 probe가 안 되고, **모듈만 로드해도 `/dev/rc522` 는 생기지 않습니다.**

### 해결 절차 (순서대로)

**1단계: 오버레이 적용**

- 이전에 `sudo dtoverlay rc522-overlay` 가 **실패했다면** → **nospidev 버전** 사용.

```bash
cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
make dtbo
sudo cp rc522-overlay-nospidev.dtbo /boot/overlays/
sudo dtoverlay rc522-overlay-nospidev
```

- 위가 성공하면(에러 없이 끝나면) 2단계로.
- **재부팅으로 쓰고 싶다면**: `/boot/config.txt` 맨 아래에 한 줄 추가 후 재부팅.
  ```text
  dtoverlay=rc522-overlay-nospidev
  ```

**2단계: 모듈 로드**

```bash
sudo insmod rc522.ko
```

**3단계: 확인**

```bash
lsmod | grep rc522
ls -l /dev/rc522
dmesg | tail -5
```

- `dmesg` 에 rc522 probe 관련 메시지가 있고 `/dev/rc522` 가 보이면 정상.

**정리**: “디바이스가 안 생긴다” → **먼저 오버레이 적용(1단계)** 이 되었는지 확인하고, 그 다음 모듈 로드(2단계) 순서를 지키면 됩니다.

---

## 1. 디바이스 트리 오버레이 적용 실패

### 현상
```text
sudo dtoverlay rc522-overlay
* Failed to apply overlay '0_rc522-overlay' (kernel)
```

### 원인
- **호환 문자열**: Pi 4/5 기본 DTB는 `brcm,bcm2711`인데, 오버레이가 `brcm,bcm2835`만 있으면 매칭 실패할 수 있음.
- **spidev 노드**: CE0에 붙는 기본 `&spidev0`를 비활성화하는 fragment에서, 해당 노드가 없거나 이름/구조가 다르면 적용 실패.

### 대응

| 순서 | 조치 |
|------|------|
| 1 | **dtbo 재빌드 후 재적용** — `dtc -@`로 심볼 포함 빌드된 dtbo 사용. `make dtbo && sudo cp rc522-overlay.dtbo /boot/overlays/` 후 재부팅 또는 `sudo dtoverlay rc522-overlay` 재시도. |
| 2 | **spidev 비활성화 없는 오버레이 사용** — `rc522-overlay-nospidev.dts`로 빌드한 `rc522-overlay-nospidev.dtbo` 사용. `make dtbo` 후 `sudo cp rc522-overlay-nospidev.dtbo /boot/overlays/` 하고 `sudo dtoverlay rc522-overlay-nospidev` 또는 config.txt에 `dtoverlay=rc522-overlay-nospidev` 추가 후 재부팅. |
| 3 | **적용 여부 확인** — `dmesg | grep rc522`, `ls -l /dev/rc522`. `/dev/rc522` 생성 여부로 드라이버 probe 확인. |

### 오버레이 파일 구분
- **rc522-overlay.dts** — CE0 사용을 위해 기본 spidev 비활성화 fragment 포함. `dtoverlay` 적용 실패 시 아래 사용.
- **rc522-overlay-nospidev.dts** — spidev 비활성화 없이 rc522 노드만 추가. **이 버전이 “맞는” 경우**: Pi에서 `&spidev0` 노드가 없거나 이름이 달라서 메인 오버레이가 실패할 때. 적용만 되면 드라이버는 동일하게 동작함. (CE0에 spidev가 원래 없다면 nospidev로 추가한 rc522@0만 있어서 충돌 없음.)

**메인 오버레이 실패 여부 확인:** (1) `sudo dtoverlay -r rc522-overlay-nospidev` 후 `sudo rmmod rc522` (2) `sudo cp rc522-overlay.dtbo /boot/overlays/` 후 `sudo dtoverlay rc522-overlay` 실행 → 에러 없으면 메인 성공, `* Failed to apply overlay*` 나오면 메인 실패. (3) 복구: `sudo dtoverlay rc522-overlay-nospidev` 후 `sudo insmod rc522.ko`

---

## 2. VersionReg가 0x00으로 나올 때

### 현상
- `test_rc522` 에서 `VersionReg(0x37) = 0x00` (정상은 0x91 또는 0x92).

### 의미
- RC522와 SPI 통신이 되지 않거나 응답이 없음. 배선·전원·CE 충돌 가능성.

### 대응
- **배선**: SDA(SS)→CE0(Pin24), SCK→Pin23, MOSI→Pin19, MISO→Pin21, RST→Pin22(GPIO25), 3.3V/GND. MISO 끊김 시 읽기값이 0이 됨.
- **전원**: RC522는 3.3V만 사용. 5V 연결 금지.
- **SPI 활성화**: `raspi-config` → Interface Options → SPI → Enable.
- nospidev 사용 중이면 같은 버스에 spidev가 있으면 충돌할 수 있음. 이 경우 메인 오버레이(spidev 비활성화)가 적용되도록 1번 대응 다시 시도.

---

## 3. 모듈은 로드되는데 /dev/rc522 가 없음

### 현상
- `lsmod | grep rc522` 에서 모듈은 보임.
- `ls /dev/rc522` 시 디바이스 없음.

### 원인
- SPI 디바이스가 probe되지 않음. 디바이스 트리 오버레이가 적용되지 않아 `compatible = "nxp,rc522"` 인 SPI 자식 노드가 없음.

### 대응
- **0. `/dev/rc522` 가 안 생길 때** 의 1단계(오버레이) 먼저 적용 후, `sudo rmmod rc522` → `sudo insmod rc522.ko` 다시 시도.
- `dmesg | grep -E "spi|rc522"` 로 SPI 버스·rc522 probe 메시지 확인.

---

## 4. 커널 모듈 빌드 오류 (implicit declaration)

### 현상
```text
error: implicit declaration of function 'msleep'
error: implicit declaration of function 'usleep_range'
```

### 원인
- 해당 함수 선언이 있는 헤더 미포함.

### 대응
- `rc522_spi.c` 상단에 `#include <linux/delay.h>` 추가 (이미 반영됨).

---

---

## 5. 테스트 프로세스 강제 종료 (이미 떠 있는 test_rc522 끄기)

- **한 개만 끌 때**: `sudo kill 2713` (PID는 `ps -ef | grep test_rc522` 로 확인)
- **test_rc522 전부 끌 때**: `sudo pkill -f test_rc522` 또는 `sudo killall test_rc522`

---

## 6. 테스트 중 Ctrl+C가 안 먹을 때

### 현상
- `sudo ./test_rc522` 실행 후 `read()` 또는 `ioctl(RC522_READ_CARD)` 대기 중 Ctrl+C를 눌러도 종료되지 않음.

### 원인
- 프로세스가 커널 드라이버의 블로킹 루프 안에 있어서, 시그널이 사용자 공간에 전달되지 않음.

### 대응
- 드라이버에서 `signal_pending(current)` 검사 후 `-ERESTARTSYS` 반환하도록 수정됨. **모듈 재빌드 후 재로드** 필요:
  ```bash
  make
  sudo rmmod rc522
  sudo insmod rc522.ko
  make test_rc522
  sudo ./test_rc522
  ```
  대기 중 **Ctrl+C** 누르면 곧바로(또는 최대 약 50ms 내) “중단됨 (Ctrl+C)” 후 종료됨.

---

---

## 7. 유저 공간 데모(rc522_full_demo) 실행 시 "Unable to open SPI device"

### 현상
- `sudo ./rc522_full_demo` → `Unable to open SPI device: No such file or directory`

### 원인
- **CE0(SPI0)를 커널 RC522 드라이버가 사용 중**이면 `/dev/spidev0.0` 이 생성되지 않음. 유저 공간 데모는 wiringPi → **spidev** 로 SPI를 열기 때문에, 지금 상태에선 열 수 없음.

### 대응 (둘 중 하나만 사용)
| 사용할 쪽 | 조치 |
|-----------|------|
| **커널 드라이버** (`/dev/rc522`, `test_rc522`) | 오버레이 + `insmod rc522.ko` 유지. `sudo ./test_rc522` 로 테스트. |
| **유저 공간 데모** (`rc522_full_demo`) | 커널 드라이버와 오버레이 해제 → spidev 복구 후 데모 실행. 아래 명령 참고. |

**유저 공간 데모만 쓰려면:** (순서 지키기)
```bash
# 1) /dev/rc522 쓰는 프로세스 모두 종료 (안 하면 rmmod 실패)
sudo pkill -f test_rc522

# 2) 모듈 내리기
sudo rmmod rc522

# 3) 오버레이 제거
sudo dtoverlay -r rc522-overlay-nospidev   # 적용한 오버레이 이름에 맞게

# 4) spidev 확인 후 데모 실행
ls -l /dev/spidev0.0   # 있으면 데모 실행 가능
sudo ./rc522_full_demo
```
다시 커널 드라이버 쓰려면: `sudo dtoverlay rc522-overlay-nospidev` 후 `sudo insmod rc522.ko`

**"Module rc522 is in use" 나올 때:** `/dev/rc522`를 연 프로세스가 있다는 뜻.
1. **열어둔 프로세스 강제 종료:** `sudo fuser -k /dev/rc522`
2. **1~2초 대기 후** `sudo rmmod rc522` (바로 하면 프로세스가 완전히 정리되기 전에 실패할 수 있음).
3. 여전히 실패하면 `lsmod | grep rc522` 로 Used by 확인. `sudo lsof /dev/rc522` 로 또 다른 프로세스 있는지 확인.
4. **그래도 안 되면 재부팅** 후 오버레이 없이 부팅해 spidev 쓰기: `sudo reboot`, 부팅 후 `ls /dev/spidev0.0` 확인하고 `sudo ./rc522_full_demo`.

**SPI 자체가 꺼져 있는 경우:** `raspi-config` → Interface Options → SPI → Enable 후 재부팅. 그 다음에도 `/dev/spidev0.0` 이 없으면 위처럼 오버레이/모듈을 내려야 함.

---

추가로 겪은 문제가 있으면 **현상 / 원인 / 대응** 형식으로 이 파일에 항목을 붙여 나가면 됩니다.
