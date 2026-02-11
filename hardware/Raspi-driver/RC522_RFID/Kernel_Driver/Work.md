## 1. 커널 드라이버 (rc522_full.c/h 기반)
### 추가된 파일
- rc522.h – MFRC522 레지스터/명령 정의, `structrc522_ops`, `struct rc522_dev`, `코어/chardev API` 선언

- rc522_core.c – 레지스터 I/O(ops), 리셋/초기화, Request/Anticoll/Select/Auth, 블록 읽기·쓰기, UID/텍스트 섹터 읽기·쓰기 (유저 공간 `rc522_full.c` 로직 포팅)

- rc522_spi.c – SPI 드라이버: probe/remove, `reset-gpio`s 제어, SPI 레지스터 읽기/쓰기, `rc522_core_init` 및 chardev 등록

- rc522_chardev.c – 문자 디바이스: /dev/rc522, read() 블로킹 UID 4바이트 + unlocked_ioctl (RC522_RESET, RC522_READ_CARD, RC522_READ_REG, RC522_WRITE_REG)
- rc522_ioctl.h – 유저/커널 공용 IOCTL 매크로 및 struct rc522_reg_data

### 동작 요약

- 디바이스 트리에서 reset-gpios(GPIO25) 사용, 없으면 RST 없이 동작

- SPI는 spi_write_then_read로 레지스터 접근

- /dev/rc522에서 read(fd, buf, 4) 시 태그가 감지될 때까지 대기 후 UID 4바이트 반환

## 2. Makefile
- 아웃오브트리 빌드: KERNEL_SRC 기본값 `/lib/modules/$(shell uname -r)/build`
- 타깃: `make` → rc522.ko, make clean, make modules_install
- DTB: `make dtbo` → rc522-overlay.dts를 컴파일해 rc522-overlay.dtbo 생성
- 라즈베리 파이에서:
    ```bash
    make
    sudo insmod rc522.ko
    ```
    (또는 아래 오버레이 적용 후 부팅 시 자동 로드)

## 3. 디바이스 트리 오버레이 (rc522-overlay.dts)
- 대상: `&spi0`
- 노드: `rc522@0` (CE0), `compatible = "nxp,rc522"`, `reg = <0>` , `spi-max-frequency = <1000000>` , `reset-gpios = <&gpio 25 0>`

- 배선: SDA→CE0(GPIO8), SCK→GPIO11, MOSI→GPIO10, MISO→GPIO9, RST→GPIO25, 3.3V/GND

| 문서 핀맵 (implementation.md) | 오버레이 설정 |
| --- | --- |
| SDA (SS) → Pin 24, GPIO8, SPI CS0 | `reg = <0>` → SPI0 CE0 = GPIO8 (Pin 24) |
| SCK → Pin 23, GPIO11 | spi0 기본 핀 = GPIO11 (Pin 23) |
| MOSI → Pin 19, GPIO10 | spi0 기본 핀 = GPIO10 (Pin 19) |
| MISO → Pin 21, GPIO9 | spi0 기본 핀 = GPIO9 (Pin 21) |
| RST → Pin 22, GPIO25 | `reset-gpios = <&gpio 25 0>` = GPIO25 (Pin 22) |
| 3.3V / GND | 배선만 하면 됨 (디바이스 트리 항목 없음) |
| IRQ → Pin 18, GPIO24 | 오버레이에 미포함 (문서상 "초기에는 선택") |

### 적용 방법
```bash
cd ~/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver
make dtbo
sudo cp rc522-overlay.dtbo /boot/overlays/
# /boot/config.txt 끝에 추가:
# dtoverlay=rc522-overlay
sudo reboot
```

**오버레이 적용 실패 시**: 상세 원인·대응은 **Dev.md** 참고.
- 요약: dtbo 재빌드(`make dtbo`) 후 복사·재부팅, 또는 `rc522-overlay-nospidev.dtbo` 사용.

## 4. IOCTL API 및 유저 공간 사용법

유저 공간에서는 **read()** 또는 **ioctl()** 로 리셋/UID 읽기/레지스터 접근을 할 수 있다.

### 4-1. IOCTL API 요약

| 항목 | 내용 |
| --- | --- |
| Magic Number | `'R'` (`RC522_IOC_MAGIC`) |
| `RC522_RESET` | 리더 소프트 리셋 (인자 없음) |
| `RC522_READ_CARD` | 블로킹 UID 읽기 → 인자: `__u32` 포인터에 UID 저장 |
| `RC522_READ_REG` | 레지스터 1바이트 읽기 (입력: `reg`, 출력: `val`) |
| `RC522_WRITE_REG` | 레지스터 1바이트 쓰기 (입력: `reg`, `val`) |

정의 위치: `rc522_ioctl.h` (매크로 + `struct rc522_reg_data`). 커널/유저 공간 공용.

관련 구현:
- **rc522.h / rc522_core.c**: `rc522_soft_reset()`, `rc522_read_reg()`, `rc522_write_reg()` 선언·구현
- **rc522_chardev.c**: `unlocked_ioctl`에서 `RC522_RESET`, `RC522_READ_CARD`, `RC522_READ_REG`, `RC522_WRITE_REG` 처리

### 4-2. 유저 공간에서 쓰는 방법

**방법 1: read() (기존)**  
- `read(fd, buf, 4)` → UID 4바이트 블로킹 읽기

**방법 2: IOCTL**  
- `rc522_ioctl.h`를 include 하고 아래처럼 사용:

```c
#include <fcntl.h>
#include <sys/ioctl.h>
#include "rc522_ioctl.h"

int fd = open("/dev/rc522", O_RDWR);
uint32_t uid;
ioctl(fd, RC522_READ_CARD, &uid);   /* 태그 올 때까지 대기 후 UID 저장 */
printf("UID: %08X\n", (unsigned)uid);

ioctl(fd, RC522_RESET);             /* 리더 소프트 리셋 */

struct rc522_reg_data r = { .reg = 0x37 };  /* VersionReg */
ioctl(fd, RC522_READ_REG, &r);
printf("VersionReg = 0x%02X\n", r.val);
```

유저 앱 빌드 시 라즈베리 파이에서는 커널 헤더 경로 지정 (예: `-I/usr/include`, `raspberrypi-kernel-headers` 설치 시 `linux/ioctl.h`, `linux/types.h` 등 제공).

## 5. 드라이버 테스트

- **빌드**: `make` (모듈), `make test_rc522` (테스트 프로그램)
- **테스트 실행** (디바이스 트리 오버레이 적용·재부팅 후):
  ```bash
  sudo ./run_test.sh
  ```
  또는 수동:
  ```bash
  sudo insmod rc522.ko
  dmesg | grep rc522    # probe 확인
  sudo ./test_rc522     # VersionReg 읽기, read()/ioctl() UID 읽기
  ```
- **test_rc522** 동작: `/dev/rc522` 열기 → VersionReg(0x37) 읽기 → read()로 UID 대기 → ioctl(RC522_READ_CARD)로 UID 대기. 태그를 리더에 갖다 대면 UID가 출력됨.
