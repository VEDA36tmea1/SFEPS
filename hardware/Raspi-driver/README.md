## Raspi-driver

라즈베리 파이 4에서 RFID 모듈과 PAM8403 디지털 앰프 모듈, DIY 스피커 유닛을 제어하기 위한 드라이버/스크립트 모음입니다.

---

## 1. 폴더 동기화 스크립트 사용법 (`sync-to-pi.sh`)

이 폴더만 라즈베리 파이로 빠르게 동기화하기 위한 `rsync` 래퍼 스크립트입니다.

### 1-1. 사전 준비

- 라즈베리 파이 OS (또는 Linux) 설치 완료
- SSH 활성화 (`raspi-config` 혹은 `ssh` 서비스)
- PC에서 라즈베리 파이로 SSH 접속 가능해야 함
  - 예: `ssh physical-100@192.168.0.23`

### 1-2. 한 번만 실행하는 동기화 명령

PC(개발 머신)에서:

```bash
rsync -avz --delete \
  /home/ros2man/Desktop/SFEPS/hardware/Raspi-driver/ \
  physical-100@192.168.0.23:~/SFEPS/hardware/Raspi-driver/
```

- `physical-100@192.168.0.23` 부분을 자신의 라즈베리 파이 계정/주소로 변경
- 처음에는 라즈베리 파이 측에 다음 디렉터리를 한 번 생성해 주는 것이 좋습니다.

```bash
ssh physical-100@192.168.0.23 'mkdir -p ~/SFEPS/hardware/Raspi-driver'
```

### 1-3. `sync-to-pi.sh` 스크립트 사용

`Raspi-driver` 폴더 안에는 다음 스크립트가 있습니다.

- `sync-to-pi.sh`  
  - 인자: `[user@host]` (생략 시 기본값 `pi@raspberrypi`)
  - 동작: 현재 폴더 내용을 `~/SFEPS/hardware/Raspi-driver/` 로 동기화

사용 예시:

```bash
cd /home/ros2man/Desktop/SFEPS/hardware/Raspi-driver
./sync-to-pi.sh physical-100@192.168.0.23
```

---

## 2. 라즈베리 파이 4 + RFID 모듈 연결

여기서는 **SPI 타입 RFID 모듈 (예: MFRC522)** 를 기준으로 합니다. 실제 사용 모듈에 맞게 핀 번호/인터페이스(SPI/UART/I2C)를 조정해야 합니다.

### 2-1. 하드웨어 연결 (SPI 예시)

라즈베리 파이 4 (40핀 GPIO 헤더 기준):

- 3.3V 전원: `Pin 1 (3V3)` → RFID VCC
- GND: `Pin 6 (GND)` → RFID GND
- SPI0 SCK: `Pin 23 (GPIO11, SCLK)` → RFID SCK
- SPI0 MOSI: `Pin 19 (GPIO10, MOSI)` → RFID MOSI
- SPI0 MISO: `Pin 21 (GPIO9, MISO)` → RFID MISO
- SPI0 CE0: `Pin 24 (GPIO8, CE0)` → RFID SDA/NSS
- (선택) RST 핀: `Pin 22 (GPIO25)` → RFID RST

> 주의: RFID 모듈이 5V 전용인지, 3.3V 대응인지 모듈 스펙을 반드시 확인하세요. 5V 전용 모듈을 직결하면 라즈베리 파이가 손상될 수 있습니다.

### 2-2. 라즈베리 파이에서 SPI 활성화

라즈베리 파이에서:

```bash
sudo raspi-config
```

- `Interface Options` → `SPI` → `Enable`
- 재부팅 후 `ls /dev/spi*` 로 `/dev/spidev0.0` 등이 보이는지 확인

### 2-3. IRQ 모드 사용 시 풀업 확인

RC522를 **IRQ 모드**(`Kernel_Driver_irq`, `dtoverlay=rc522-overlay-irq`)로 사용할 경우, IRQ 핀(GPIO24, Pin 18)은 보드에 따라 오픈드레인 형식이라 **풀업이 없으면 엣지가 잘 잡히지 않을 수 있습니다.**

- Device Tree 오버레이(`rc522-overlay-irq.dts`)에서 pinctrl로 GPIO24 풀업을 걸어 두었지만, 카드 태깅 시 반응이 없거나 `spi->irq: 0` 이면 풀업을 확인하세요.
- 수동으로 풀업을 주려면 (라즈베리 파이에서):
  ```bash
  gpio -g mode 24 up
  ```
- 자세한 내용은 `RC522_RFID/README.md` 및 `RC522_RFID/Kernel_Driver_irq/README.md`, `Dev.md` 를 참고하세요.

### 2-4. RFID용 기본 디바이스 드라이버(커널 모듈) 초안 개념

실제 커널 드라이버를 작성하기 전에, 구조를 먼저 정의합니다.

#### 목표

- 커널 모듈로 `/dev/rfid0` 와 같은 문자 디바이스를 제공
- `open/read/ioctl` 을 통해 사용자 공간에서 UID, 카드 타입 등을 읽을 수 있게 함

#### 커널 모듈 구조 (C, 개념 초안)

```c
// drivers/rfid/rfid_mfrc522.c (예시 경로)

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define DRIVER_NAME "rfid_mfrc522"

static int rfid_open(struct inode *inode, struct file *file)
{
    // SPI 디바이스 핸들 준비 등
    return 0;
}

static ssize_t rfid_read(struct file *file, char __user *buf, size_t len, loff_t *off)
{
    // 태그 UID를 읽어서 user space 로 전달
    char uid[16] = {0};
    size_t uid_len = 4; // 예시

    // TODO: SPI 통신으로 실제 UID 읽기

    if (len < uid_len)
        return -EINVAL;

    if (copy_to_user(buf, uid, uid_len))
        return -EFAULT;

    return uid_len;
}

static long rfid_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    // 태그 타입 조회, 안티콜리전 설정 등 다양한 제어용
    return 0;
}

static const struct file_operations rfid_fops = {
    .owner          = THIS_MODULE,
    .open           = rfid_open,
    .read           = rfid_read,
    .unlocked_ioctl = rfid_ioctl,
};

static int rfid_probe(struct spi_device *spi)
{
    // 문자 디바이스 등록, SPI 설정 등
    pr_info(DRIVER_NAME ": probed\n");
    return 0;
}

static int rfid_remove(struct spi_device *spi)
{
    // 리소스 해제
    pr_info(DRIVER_NAME ": removed\n");
    return 0;
}

static const struct of_device_id rfid_of_match[] = {
    { .compatible = "sfeps,rfid-mfrc522" },
    {},
};
MODULE_DEVICE_TABLE(of, rfid_of_match);

static struct spi_driver rfid_driver = {
    .driver = {
        .name           = DRIVER_NAME,
        .of_match_table = rfid_of_match,
    },
    .probe  = rfid_probe,
    .remove = rfid_remove,
};

module_spi_driver(rfid_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SFEPS");
MODULE_DESCRIPTION("RFID MFRC522 driver for Raspberry Pi 4 (draft)");
```

> 위 코드는 **구조 초안**일 뿐 실제 동작하지 않습니다. 추후 `SPI read/write`, 하드웨어 초기화, 태그 감지 로직을 채워 넣어야 합니다.

---

## 3. PAM8403 디지털 앰프 + DIY 스피커 유닛 연결

PAM8403 모듈은 **Class-D 3W+3W 스테레오 앰프**로, 일반적으로 **오디오 아날로그 입력(L/R)** 과 **스피커 출력(L+/L-, R+/R-)**, **전원(5V)** 단자가 있습니다.

### 3-1. 전원 및 스피커 연결

- 전원:
  - PAM8403 VCC → 라즈베리 파이 5V (예: `Pin 2` 또는 `Pin 4`)
  - PAM8403 GND → 라즈베리 파이 GND (예: `Pin 6` 등)
- 스피커:
  - L+ / L- → 좌측 스피커 단자
  - R+ / R- → 우측 스피커 단자

> 스피커 임피던스(예: 4Ω, 8Ω)와 정격 전력을 PAM8403 스펙에 맞춰 사용하세요.

### 3-2. 오디오 입력(라즈베리 파이 → PAM8403)

라즈베리 파이 4는 기본적으로 HDMI 오디오/3.5mm 콤보 잭을 통해 아날로그 오디오를 출력합니다. 일반적인 연결 방식:

- 라즈베리 파이 3.5mm 잭 (또는 DAC HAT 등) → PAM8403 INL/INR (입력 L/R)
  - 3.5mm TRS(스테레오) 케이블을 잘라서 L/R/GND 선을 분리
  - L → INL, R → INR, GND → GND

**GPIO 직접 PWM로 오디오를 만드는 방법**도 있지만, 노이즈/품질 이슈 때문에 가능하면 **전용 DAC HAT 또는 3.5mm 잭**을 사용합니다.

---

## 4. PAM8403 제어용 디바이스 드라이버/제어 로직 초안

PAM8403 자체는 디지털 제어 인터페이스(I2C/SPI 등)가 없는 경우가 많아, 실제로는 **볼륨/뮤트/전원 제어를 GPIO로 구현**하게 됩니다.

예:  
- `AMP_ENABLE` 핀: 앰프 on/off (GPIO → 모듈의 SHDN/EN 핀)  
- `AMP_MUTE` 핀: 뮤트 제어  

### 4-1. 간단한 커널 모듈 개념 (GPIO 기반)

```c
// drivers/audio/pam8403_ctrl.c (예시)

#include <linux/module.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>

struct pam8403_data {
    struct gpio_desc *enable_gpio;
    struct gpio_desc *mute_gpio;
};

static int pam8403_probe(struct platform_device *pdev)
{
    struct pam8403_data *data;

    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->enable_gpio = devm_gpiod_get(&pdev->dev, "enable", GPIOD_OUT_LOW);
    data->mute_gpio   = devm_gpiod_get(&pdev->dev, "mute", GPIOD_OUT_HIGH);

    if (IS_ERR(data->enable_gpio) || IS_ERR(data->mute_gpio))
        return -EINVAL;

    // 기본 상태: 앰프 켜기, 뮤트 해제
    gpiod_set_value(data->enable_gpio, 1);
    gpiod_set_value(data->mute_gpio, 0);

    dev_info(&pdev->dev, "PAM8403 amp control initialized\n");
    platform_set_drvdata(pdev, data);
    return 0;
}

static int pam8403_remove(struct platform_device *pdev)
{
    struct pam8403_data *data = platform_get_drvdata(pdev);

    // 종료 시 앰프 끄기
    gpiod_set_value(data->enable_gpio, 0);
    return 0;
}

static const struct of_device_id pam8403_of_match[] = {
    { .compatible = "sfeps,pam8403-ctrl" },
    {},
};
MODULE_DEVICE_TABLE(of, pam8403_of_match);

static struct platform_driver pam8403_driver = {
    .driver = {
        .name           = "pam8403-ctrl",
        .of_match_table = pam8403_of_match,
    },
    .probe  = pam8403_probe,
    .remove = pam8403_remove,
};

module_platform_driver(pam8403_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SFEPS");
MODULE_DESCRIPTION("PAM8403 amp GPIO control driver (draft)");
```

디바이스 트리 예시 (개념):

```dts
amp0: pam8403-ctrl {
    compatible = "sfeps,pam8403-ctrl";
    enable-gpios = <&gpio 17 GPIO_ACTIVE_HIGH>; // AMP_ENABLE
    mute-gpios   = <&gpio 27 GPIO_ACTIVE_HIGH>; // AMP_MUTE
};
```

### 4-2. 사용자 공간 스크립트(간단 버전) 예시

초기에는 커널 모듈까지 가지 않고, `/sys/class/gpio` 또는 `libgpiod` 를 사용한 사용자 공간 스크립트로도 제어할 수 있습니다.

```bash
#!/usr/bin/env bash
# amp-ctrl.sh (개념 예시)

ENABLE_GPIO=17
MUTE_GPIO=27

gpio_export() {
  local gpio=$1
  if [ ! -d "/sys/class/gpio/gpio$gpio" ]; then
    echo "$gpio" | sudo tee /sys/class/gpio/export
    echo "out" | sudo tee "/sys/class/gpio/gpio$gpio/direction"
  fi
}

gpio_export "$ENABLE_GPIO"
gpio_export "$MUTE_GPIO"

case "$1" in
  on)
    echo 1 | sudo tee "/sys/class/gpio/gpio$ENABLE_GPIO/value"
    echo 0 | sudo tee "/sys/class/gpio/gpio$MUTE_GPIO/value"
    ;;
  off)
    echo 0 | sudo tee "/sys/class/gpio/gpio$ENABLE_GPIO/value"
    ;;
  mute)
    echo 1 | sudo tee "/sys/class/gpio/gpio$MUTE_GPIO/value"
    ;;
  unmute)
    echo 0 | sudo tee "/sys/class/gpio/gpio$MUTE_GPIO/value"
    ;;
  *)
    echo "Usage: $0 {on|off|mute|unmute}"
    ;;
esac
```

---

## 5. 다음 단계

- RFID:
  - 실제 사용하는 칩셋(MFRC522 등)에 맞게 **데이터시트 기반 레지스터 설정**, 안티콜리전, UID 읽기 로직 구현
  - 초기에는 Python/C 라이브러리(예: `MFRC522-python`)를 사용하여 프로토타입을 만든 후, 필요시 커널 드라이버로 이관
- PAM8403:
  - 현재는 **전원/뮤트만 GPIO로 제어**하는 수준이며, 오디오 스트림은 ALSA/파이프라인(예: `aplay`)을 통해 출력
  - 필요하다면 특정 이벤트(예: RFID 태그 인식) 시 자동으로 음성 재생/볼륨 조정 로직 추가

이 README는 **초안**이며, 실제 하드웨어 연결/테스트를 진행하면서 구체적인 핀 번호, 드라이버 경로, 빌드/로드 방법을 보완해 나갈 예정입니다.

