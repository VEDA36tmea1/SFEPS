# STM32 Laser (Galvo) Controller

Nucleo STM32F401 기반 레이저 갈보 제어 펌웨어. TIM1/TIM2 PWM으로 서보(또는 갈보) 제어.

---

## 폴더 구성

| 폴더 | 설명 |
|------|------|
| **stm32/** | Nucleo 보드 관련 프로젝트 (Core, Drivers, CMake, .ioc, 링커 스크립트 등) |
| **host_cpp/** | 호스트 PC용 IBVS/제어 코드 (STM32와 통신) |
| **tmp_raspi_server/** | 라즈베리 파이 서버 관련 임시/테스트용 |

펌웨어 빌드·플래시는 **stm32/** 디렉터리 기준으로 진행.

---

## 빌드 방법

### 1. 사전 요구사항

- **ARM 툴체인**: `gcc-arm-none-eabi`
- **CMake** 3.18+
- **Ninja** 또는 **Make**

```bash
sudo apt update
sudo apt install gcc-arm-none-eabi cmake ninja-build
# Ninja 없이 빌드할 경우: sudo apt install gcc-arm-none-eabi cmake make
```

### 2. 빌드

Nucleo 펌웨어는 **stm32/** 에서 빌드:

#### Ubuntu (PC) 권장 옵션

- **권장**: Ninja + (가능하면) 최신 CMake
- 장점: 빌드 속도/의존성 처리 안정적

```bash
sudo apt update
sudo apt install gcc-arm-none-eabi cmake ninja-build
```

#### Raspberry Pi (라즈베리 파이) 권장 옵션

- 라즈베리 파이 OS/기본 apt에서는 CMake가 3.18.x 인 경우가 있어, **Preset이 기대대로 동작하지 않거나**(preset 이름이 경로로 해석) 제너레이터가 없는 경우가 있습니다.
- **권장**: `Unix Makefiles` + `-DCMAKE_TOOLCHAIN_FILE=...` 로 크로스 컴파일러를 확실히 지정

```bash
sudo apt update
sudo apt install gcc-arm-none-eabi cmake make
```

**방법 1: Preset 사용 (Ubuntu PC에서 가장 간단한 방법)**  

`stm32/CMakePresets.json` 에서 이미 `CMAKE_TOOLCHAIN_FILE` 이 설정되어 있으므로,
**stm32 디렉터리에서 preset만 호출하면** 됩니다.

```bash
cd hardware/stm32-laser/stm32

# 최초 설정(또는 CMake 설정을 변경하고 싶을 때 1회):
cmake --preset Debug

# 빌드:
cmake --build build/Debug
```

Release 빌드는:

```bash
cd hardware/stm32-laser/stm32
cmake --preset Release
cmake --build build/Release
```

> 참고: `cmake --build build/Debug` 실행 시 `ninja: no work to do.` 라고 나오면  
> 이미 최신 상태로 빌드가 끝난 것이므로 오류가 아닙니다.

**방법 2: Preset 없이 직접 빌드 (라즈베리 파이 등 CMake 3.18 환경)**

ARM 툴체인을 쓰려면 **반드시** `-DCMAKE_TOOLCHAIN_FILE` 로 툴체인 파일을 지정합니다.

```bash
cd hardware/stm32-laser/stm32
mkdir -p build/Debug
cd build/Debug
cmake ../.. -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=../../cmake/gcc-arm-none-eabi.cmake
cd ../..
cmake --build build/Debug
```

Ninja가 없으면 `-G "Unix Makefiles"` 로 Make 사용:

```bash
cd build/Debug
cmake ../.. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=../../cmake/gcc-arm-none-eabi.cmake
cd ../..
cmake --build build/Debug
```

---

## 운영체제별 빌드 옵션 차이 요약

| 항목 | Ubuntu (PC) | Raspberry Pi |
|------|-------------|--------------|
| **CMake 버전** | 비교적 최신(환경 따라 3.22+ 가능) | 기본 apt가 3.18.x 인 경우 많음 |
| **Generator** | `-G Ninja` 권장 | `-G "Unix Makefiles"` 권장 (Ninja 없으면 특히) |
| **Preset 사용** | 가능(환경 따라 `cmake --preset Debug "$(pwd)"`) | 환경에 따라 preset 이름이 경로로 해석될 수 있어 비권장 |
| **Toolchain 지정** | 가능하면 명시(권장) | **필수** (`-DCMAKE_TOOLCHAIN_FILE=../../cmake/gcc-arm-none-eabi.cmake`) |

추가로, STM32CubeMX 서브 타깃(`STM32_Drivers`)에도 `-mthumb/-mcpu` 옵션을 명시적으로 걸어 **ASM/어셈블 단계가 ARM 모드로 떨어지는 문제**를 방지했습니다.

빌드 결과물 (예: Debug 기준):

- `stm32/build/Debug/stm32_laser.elf` — 플래시/디버깅용
- `stm32/build/Debug/stm32_laser.bin` — st-flash 업로드용 (빌드 시 자동 생성)

---

## 플래시 방법 (Nucleo 보드)

Nucleo 보드는 내장 ST-Link가 있으므로 USB만 연결하면 됨.

### 1. stlink 설치

```bash
sudo apt install stlink-tools
```

### 2. 보드에 올리기

현재 사용 중인 st-flash는 **.bin + 주소** 형식만 지원하므로:

```bash
cd hardware/stm32-laser/stm32
st-flash write build/Debug/stm32_laser.bin 0x8000000
```

(Release로 빌드했으면 `build/Release/stm32_laser.bin` 사용)

---

## WiFi(ESP-8266) 연결 및 DMA 수신

ESP-8266를 USART1에 연결하면 **와이파이로 들어온 데이터를 DMA로 수신**해 같은 명령(펄스/모드)을 처리한다.

### 핀 연결 (Nucleo-F401RE)

| ESP-8266 핀 | Nucleo 핀 |
|-------------|------------|
| VBUS (5V)   | 5V         |
| GND         | GND        |
| **TX**      | **D2 (PA10)** USART1_RX |
| **RX**      | **D8 (PA9)**  USART1_TX |

### 동작

- **USART1** (PA9=TX, PA10=RX) 115200 8N1.
- 수신은 **DMA (DMA2 Stream2)** 로 버퍼에 적재하고, **IDLE 라인 인터럽트**로 한 줄 단위로 처리.
- 프로토콜은 기존과 동일: `1500`, `1500 1200`, `mode 0` / `mode 1`. 응답은 USART1(WiFi)로 전송.

---

## 터미널(UART)로 펄스 폭 제어

PWM 주파수 기본 **50 Hz** (주기 20 ms = 20,000 us). `main.c`의 **`PWM_FREQ_HZ`** 를 바꿔서 50 / 100 / 250 Hz 등으로 빌드·테스트 가능. 펄스 폭 **800 us ~ 2,200 us** 범위를 UART로 입력해 CH1(PA8), CH2(PA0)에 적용할 수 있음.

### 연결

- Nucleo 보드 USB로 PC 연결 (ST-Link 가상 시리얼 포트)
- 포트: Linux 예시 `/dev/ttyACM0`, Windows `COMx`
- **115200 8N1**

### 보내는 형식 (한 줄씩, Enter로 전송)

| 입력 예시 | 동작 |
|-----------|------|
| `1500` | CH1(PA8), CH2(PA0) 둘 다 **1500 us** |
| `1500 1200` | CH1 = 1500 us, CH2 = 1200 us |
| `800` | 최소 폭 (800 us) |
| `2200` | 최대 폭 (2200 us) |

- 값은 **800 ~ 2200** 밖이면 자동으로 이 구간으로 잘림.
- 보드가 처리하면 `OK CH1=1500 CH2=1200 us` 형태로 한 줄 응답.

### 터미널에서 사용 예

```bash
# 방법 1: screen (입력한 글자는 안 보일 수 있음, Enter 후 OK만 보임)
screen /dev/ttyACM0 115200

# 방법 2: minicom — 로컬 에코 켜면 입력한 글자도 보임 (추천)
minicom -D /dev/ttyACM0 -b 115200
# 들어간 뒤 Ctrl+A → Z → E 로 "Local Echo" 켜기
```

한 줄에 숫자 하나 또는 두 개 입력 후 Enter.  
(보드에서 입력을 다시 안 보내므로, 입력이 보이게 하려면 minicom 로컬 에코 사용.)

**채널 ↔ 핀:** 첫 번째 숫자 → **PA8** (TIM1), 두 번째 숫자 → **PA0** (TIM2).  
한 개만 보내면 PA8·PA0 둘 다 같은 값.

**확인:** Enter 치면 `OK PA8=1500 PA0=1500 us` 같은 응답이 와야 함. 안 오면 보드와 연결/보드레이트 확인.

**자동 회전 (1200~1800 us):**  
전원 인가 후 **별도 입력 없이** 펄스 폭이 1200↔1800으로 왕복합니다. **UART로 값을 보내면 그 값으로 2초 동안 고정**되고, 2초가 지난 뒤에만 다시 1200↔1800 자동 스윕이 이어집니다. (손 떼자마자 다시 움직이던 동작을 막기 위함.)

---

## Project Configuration (STM32CubeMX 기준)

### 1. Clock Configuration

| 항목 | 값 |
|------|-----|
| HCLK | **84 MHz** (기본 설정 그대로 사용) |

---

### 2. Timers

#### TIM2

| 항목 | 설정 |
|------|------|
| Clock Source | Internal Clock |
| Channel1 | PWM Generation CH1 **(PA0)** |

**Configuration**

| 항목 | 값 |
|------|-----|
| Prescaler | 83 |
| Counter Period (ARR) | **19999** (50 Hz, 20 ms). 런타임에 `PWM_FREQ_HZ`로 ARR 재설정됨 |
| Pulse | 1500 (초기 90°) |
| Mode | PWM mode 1 |
| Polarity | **Low** (반전) |

---

#### TIM1

| 항목 | 설정 |
|------|------|
| Clock Source | Internal Clock |
| Channel1 | PWM Generation CH1 **(PA8)** |

**Configuration**

| 항목 | 값 |
|------|-----|
| Prescaler | 83 |
| Counter Period (ARR) | **19999** (50 Hz, 20 ms). 런타임에 `PWM_FREQ_HZ`로 ARR 재설정됨 |
| Pulse | 1500 (초기 90°) |
| Mode | PWM mode 1 |
| Polarity | **Low** (반전) |

---

### 3. GPIO (자동 설정)

| 핀 | 기능 |
|----|------|
| **PA0** | TIM2_CH1 |
| **PA8** | TIM1_CH1 |

CubeMX에서 위 타이머/채널 설정 시 해당 핀은 자동으로 AF로 설정됨.

---

## 요약

- **폴더**: Nucleo 관련 코드는 `stm32/`, 호스트 코드는 `host_cpp/`, 라즈베리 파이 서버용은 `tmp_raspi_server/`
- **빌드**: `cd stm32` 후 `cmake --preset Debug` → `cmake --build build/Debug`
- **플래시**: `cd stm32` 후 `st-flash write build/Debug/stm32_laser.bin 0x8000000`
- **PWM**: PA0(TIM2_CH1), PA8(TIM1_CH1), **50 Hz** (주기 20 ms, ARR 19999, PSC 83), 펄스 800~2200 us, UART(115200)로 제어

---

## 문제 해결

- **minicom에서 숫자 입력해도 안 움직임**  
  - Enter 후 `OK PA8=... PA0=... us` 가 보이는지 확인. 보이면 명령은 들어간 것 → 서보/갈보 전원·신호선·GND 확인.  
  - 응답이 전혀 없으면: 보드레이트 115200, 포트(/dev/ttyACM0), 케이블/연결 확인.

- **"시그널 꽂으면 가만히 있고, 뽑으면 움직인다"** → **dev.md** 참고.

- **A1015(PNP)로 서보 연결 시**  
  - Emitter → +5V, Collector → 서보 신호선, Base → 1k~4.7kΩ → PA0 (또는 PA8). Collector–5V 10kΩ 풀업. GND 공통.

- **PWM 극성 (Polarity)**  
  현재 **Low**(반전)로 설정됨. NPN/반전 회로 사용 시 이 설정이 맞음.

- **PA0에 선 꽂으면 지지직 소리**  
  - GND를 보드와 드라이버(서보/앰프)가 **한 점에서만** 공통으로 쓰는지 확인 (접지 루프 방지).  
  - PA0·PA8은 3.3V 논리. 5V 입력 드라이버면 레벨 변환 필요.  
  - TIM1(PA8)은 MOE 사용으로 출력 활성화됨. 다시 빌드·플래시 후 테스트.
