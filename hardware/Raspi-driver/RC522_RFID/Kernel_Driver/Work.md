ㅊㅊ## 1. 커널 드라이버 (rc522_full.c/h 기반)
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
- 타깃: `make` → **build/** 에 rc522.ko 등 생성, make clean, make modules_install
- **build/** 폴더: 빌드 결과물이 여기에만 생성됨 (소스 디렉토리 정리 유지)
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
| `RC522_READ_TEXT_SECTOR` | 섹터 텍스트 읽기 → 인자: `struct rc522_read_text` (trailer_block 입력, uid/text 출력) |

정의 위치: `rc522_ioctl.h` (매크로 + `struct rc522_reg_data`, `struct rc522_read_text`). 커널/유저 공간 공용.

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
- **test_rc522_poll** (지속 폴링): 계속 폴링하며 다른 카드가 대면 UID와 섹터 텍스트 출력. `sudo ./test_rc522_poll [--trailer 11]` 실행. Ctrl+C로 종료.
  - 참고: `test_rc522_poll`는 `sigaction()`(SA_RESTART 미사용)으로 Ctrl+C 시 블로킹 `ioctl()`이 `EINTR`로 깨지도록 처리함. (구버전 바이너리면 `make test_rc522_poll`로 재빌드)

### 5-2. 프로세스 종료 및 모듈 제거

**중요**: 커널 모듈을 제거하기 전에 **유저 영역 프로세스를 먼저 종료**해야 합니다.

**올바른 순서:**

1. **유저 영역 프로세스 확인 및 종료**
   ```bash
   # 실행 중인 테스트 프로세스 확인
   ps -ef | grep test_rc522
   
   # 정상 종료 시도 (SIGTERM, 시그널 15)
   sudo kill <PID>
   # 또는 Ctrl+C로 종료 (프로그램이 시그널을 처리하도록 설계됨)
   
   # 그래도 안 되면 강제 종료 (SIGKILL, 시그널 9)
   sudo kill -9 <PID>
   ```

2. **디바이스 사용 확인**
   ```bash
   # /dev/rc522를 사용하는 프로세스 확인
   sudo lsof /dev/rc522
   sudo fuser /dev/rc522
   ```

3. **커널 모듈 제거**
   ```bash
   # 프로세스가 모두 종료된 후 모듈 제거
   sudo rmmod rc522
   ```

**잘못된 방법 (피해야 할 것):**
- ❌ `kill -9`로 바로 강제 종료 → 프로세스가 정리되지 않고 커널 모듈이 잠길 수 있음
- ❌ 유저 프로세스가 살아있는 상태에서 `rmmod` → "Module rc522 is in use" 오류 발생

**권장 순서 요약:**
1. 유저 영역 프로세스 정상 종료 시도 (`kill` 또는 Ctrl+C)
2. 필요 시 강제 종료 (`kill -9`)
3. 디바이스 사용 확인 (`lsof`, `fuser`)
4. 커널 모듈 제거 (`rmmod`)

### 5-1. 지속 폴링 + 텍스트 읽기 요약

1. **IOCTL API 확장 (`rc522_ioctl.h`)**
   - `RC522_READ_TEXT_SECTOR` 추가: 섹터 텍스트 읽기용 IOCTL
   - `struct rc522_read_text` 추가: `trailer_block`(입력), `uid`, `text`(출력)

2. **커널 드라이버 (`rc522_chardev.c`)**
   - `RC522_READ_TEXT_SECTOR` 핸들러 추가
   - 내부에서 `rc522_read_text_sector_blocking()` 호출해 카드 텍스트 읽기

3. **지속 폴링 테스트 프로그램 (`test_rc522_poll.c`)**
   - 무한 루프로 계속 폴링
   - UID가 바뀌면 새 UID 출력
   - 해당 카드의 섹터 텍스트도 함께 읽어서 출력
   - Ctrl+C로 안전하게 종료 가능

4. **사용 방법**
   ```bash
   # 빌드
   make test_rc522_poll

   # 실행 (기본 섹터 11 사용)
   sudo ./test_rc522_poll

   # 다른 섹터 지정 (예: 섹터 3 = 블록 15)
   sudo ./test_rc522_poll --trailer 15
   ```

5. **동작 방식**
   - 약 200ms 간격으로 `/dev/rc522` 를 통해 계속 폴링
   - 카드가 감지되면 UID를 읽어서 이전 UID와 비교
   - UID가 바뀌었을 때만 다음을 출력:
     - 새 카드 UID
     - 섹터 텍스트 (인증 성공 시, 저장된 문자열)
   - 다른 카드를 대면 새로운 값이 계속해서 갱신되어 출력됨

## 6. build/ 폴더 안의 파일들

`make` 실행 시 **build/** 폴더에 생성되는 파일들:

| 파일 | 설명 |
|------|------|
| **rc522.ko** | 최종 커널 모듈 (로드할 파일: `sudo insmod build/rc522.ko`) |
| **rc522.o** | 링크된 오브젝트 (rc522_core.o + rc522_spi.o + rc522_chardev.o 합침) |
| **rc522_core.o**, **rc522_spi.o**, **rc522_chardev.o** | 각 소스 파일의 컴파일된 오브젝트 |
| **rc522.mod.c**, **rc522.mod.o** | 모듈 메타데이터 (MODULE_* 매크로 처리용) |
| **rc522.mod** | 모듈 정보 (depmod용) |
| **Module.symvers** | 심볼 버전 정보 (모듈 간 심볼 참조용) |
| **modules.order** | 모듈 로드 순서 |
| **`.*.cmd`** | 빌드 명령 캐시 (재빌드 시 사용, 숨김 파일) |
| **rc522_*.c → ../rc522_*.c** | 소스 파일 심볼릭 링크 (build/에서 상위 소스 참조) |
| **Makefile** | Kbuild용 최소 Makefile (obj-m, rc522-y) |

**정리:** `make clean` 실행 시 위 파일들 모두 삭제됨. 소스 디렉토리(`rc522_*.c`, `rc522.h` 등)는 그대로 유지.
