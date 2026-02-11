# RC522 IRQ 전환 개발 노트 (Kernel_Driver_irq)

## 1. 배경 / 목표

- 기존 `Kernel_Driver`는 **폴링 기반**으로 UID/텍스트를 읽음
  - 장점: 구현이 단순하고 이미 안정 동작 (UID, 텍스트, Ctrl+C 처리까지 완료)
  - 단점: 항상 REQA/anticoll를 주기적으로 돌리므로 전력/CPU 측면에서 비효율
- **IRQ(GPIO24) 기반 모드**로 전환해:
  - 카드가 실제로 이벤트를 만들 때만 커널이 깨어나도록 하는 게 목표
  - 기존 사용자 API(`read()`, `ioctl(RC522_READ_CARD/READ_TEXT_SECTOR`)는 그대로 유지
- 원본 드라이버를 깨지 않기 위해 `Kernel_Driver_irq` 폴더를 만들어 **실험 전용 워크스페이스**로 분리.

---

## 2. 단계별 구현/확인 내용 (여기까지)

### 2-1. IRQ 워크스페이스 스캐폴딩

- `Kernel_Driver` 그대로 유지, 새 폴더 `Kernel_Driver_irq` 생성
- 파일:
  - `Kernel_Driver_irq/README.md`
    - 이 폴더의 목적과 단계 계획(스캐폴딩 → DTO+probe → IRQ 요청 → waitqueue 전환 → 정리)을 요약
  - `Kernel_Driver_irq/Makefile`
    - `build/`를 사용한 out-of-tree 빌드
    - 원본 소스가 이 폴더에 없으면 `../Kernel_Driver`에서 가져와서 **심볼릭 링크**로 사용
    - 절대경로(`abspath`)를 사용해 링크가 깨지지 않도록 수정

#### Makefile 핵심 아이디어

- `MOD_SRCS := rc522_core.c rc522_spi.c rc522_chardev.c`
- `prepare_build`에서:
  - `Kernel_Driver_irq/`에 파일이 있으면 **IRQ 버전 소스**를 `build/`에 링크
  - 없으면 `../Kernel_Driver/`의 원본을 `build/`에 링크
- 이렇게 해서:
  - **공통 부분은 원본을 공유**하고,
  - IRQ로 바꾸고 싶은 파일만 `Kernel_Driver_irq`로 복사해 수정할 수 있게 구조화.

### 2-2. IRQ용 SPI 드라이버 분기 (`rc522_spi.c`)

- `Kernel_Driver_irq/rc522_spi.c`를 새로 생성:
  - 기존 `Kernel_Driver/rc522_spi.c`를 기반으로 하되,
  - `probe()`에서 아래 로그 추가:
    - `rc522_irq/probe: called`
    - `chip_select`, `max_speed_hz`
    - `spi->irq` 값 출력: `dev_info(&spi->dev, "  spi->irq: %d ...\n", spi->irq);`
- 나머지 로직은 **현재까지 검증된 폴링 버전과 동일**:
  - `devm_kzalloc`으로 `struct rc522_spi` 할당
  - `devm_gpiod_get_optional("reset", ...)`로 RST GPIO 토글
  - `rc522_core_init()` 호출
  - `rc522_chardev_register()`로 `/dev/rc522` 생성
- 즉, **현재 단계에서는 동작은 그대로 두고 “IRQ 번호가 어떻게 보이는지”만 확인**하는 상태.

### 2-3. IRQ용 Device Tree Overlay 추가 (`rc522-overlay-irq.dts`)

- `Kernel_Driver_irq/rc522-overlay-irq.dts` 생성:
  - SPI0 CE0 + RST(GPIO25)는 기존과 동일
  - 추가로 GPIO24를 인터럽트로 연결:
    - `interrupt-parent = <&gpio>;`
    - `interrupts = <24 2>;`  (예: falling edge, 보드/커널 설정에 따라 조정 가능)
  - CE0 spidev 비활성화 fragment 포함 (기존 overlay와 동일 컨셉)
- 빌드:
  - `make dtbo` → `rc522-overlay-irq.dtbo` 생성

### 2-4. 빌드 문제 및 해결

#### 증상

- 처음 `Kernel_Driver_irq`에서 `make` 시:
  - `No rule to make target '.../build/rc522_core.c'` 에러 발생
  - 원인: `build/` 안에 `rc522_core.c` 등에 대한 **심볼릭 링크가 없거나 깨져 있음**

#### 원인 분석

- 초기 `Makefile`은:
  - `ln -sf ../Kernel_Driver/rc522_core.c build/rc522_core.c` 같은 상대경로 링크를 생성
- 하지만 `build/` 관점에서 `../Kernel_Driver/...` 경로가 올바르지 않거나,
  이전에 수동으로 만든 잘못된 링크 때문에 Kbuild가 타깃 소스를 찾지 못함.

#### 수정 내용

- `Makefile`의 `prepare_build`를 **절대경로 링크**로 수정:

```make
prepare_build: $(BUILD_DIR)/Makefile
	@for f in $(MOD_SRCS) $(MOD_HDRS); do \
		if [ -f "$(CURDIR)/$$f" ]; then \
			ln -sf "$(CURDIR)/$$f" "$(BUILD_DIR)/$$f"; \
		else \
			ln -sf "$(abspath $(SRC_BASE))/$$f" "$(BUILD_DIR)/$$f"; \
		fi; \
	done
```

- 이후 절차:

```bash
cd Kernel_Driver_irq
make clean        # build 안의 이전 산출물/캐시 삭제
make prepare_build
make              # 성공: build/rc522.ko 생성
```

- `stat build/rc522_core.c` 등으로 링크가 다음처럼 보이는 것을 확인:
  - `build/rc522_core.c -> /home/.../Kernel_Driver/rc522_core.c`
  - `build/rc522_spi.c  -> /home/.../Kernel_Driver_irq/rc522_spi.c`

결과적으로 `Kernel_Driver_irq/build/rc522.ko`가 정상 빌드되는 상태까지 확보됨.

---

## 3. IRQ DTO 적용 및 spi->irq 확인 (완료)

- 빌드:
  - `cd Kernel_Driver_irq`
  - `make dtbo` → `rc522-overlay-irq.dtbo` 생성
- 적용:
  - `sudo cp rc522-overlay-irq.dtbo /boot/overlays/`
  - `/boot/config.txt` 에 `dtoverlay=rc522-overlay-irq` 추가 (기존 rc522 overlay 라인 주석 처리)
  - `sudo reboot`
- 모듈 로드 후 확인:

```bash
sudo insmod Kernel_Driver_irq/build/rc522.ko
dmesg | grep rc522
```

- 실제 로그 예:

```text
[  125.186143] rc522: loading out-of-tree module taints kernel.
[  125.186950] rc522 spi0.0: rc522_irq/probe: called
[  125.186966] rc522 spi0.0:   chip_select: 0, max_speed: 1000000 Hz
[  125.186971] rc522 spi0.0:   spi->irq: 57 (IRQ 단계에서 사용 예정)
[  125.324695] rc522 spi0.0: rc522_irq/probed successfully, /dev/rc522 created
```

- 결론:
  - DTO에서 GPIO24 → IRQ 연결이 정상적으로 설정됨
  - 커널에서 `spi->irq = 57`로 보이고, 향후 `devm_request_threaded_irq()`에 사용할 수 있는 상태

---

## 4. 다음 단계 계획 (요약)

여기까지는 **폴링 로직은 유지한 채 IRQ용 틀만 만든 상태**이고, 다음 단계는:

1. **IRQ 요청 + waitqueue 도입**
   - `rc522_spi.c`에서 `devm_request_threaded_irq()`로 IRQ 요청
   - IRQ 핸들러에서 이벤트 플래그 set + `wake_up_interruptible()`
   - `rc522_core.c`의 블로킹 루프(`rc522_read_uid_blocking` 등)를 `wait_event_interruptible()` 기반으로 전환

2. **폴백/정리**
   - IRQ가 없는 환경이면 기존 폴링 루틴 사용
   - `Dev.md` / `Work.md` / `RC522_RFID_implementation.md`에 최종 구조/동작 기록

