# RC522 IRQ Driver Workspace

이 폴더는 기존 `Kernel_Driver`를 보존한 상태에서, **IRQ(인터럽트) 기반 전환**을 단계별로 진행하기 위한 작업 공간입니다.

## 진행 원칙

- 원본(`Kernel_Driver`)은 수정하지 않음
- 이 폴더에서 단계별로 변경
- 각 단계마다 빌드/동작 확인 후 다음 단계 진행

## 단계 계획 / 현재 상태

1. **스캐폴딩**  ✅  
   - `Kernel_Driver_irq` 폴더 생성
   - 독립 Makefile + `build/` 아웃오브트리 빌드
   - 원본 `Kernel_Driver` 소스를 심볼릭 링크로 재사용

2. **DTO + probe 확인**  ✅  
   - `rc522-overlay-irq.dts`에서 GPIO24를 IRQ로 연결 (`interrupts = <24 2>`)
   - pinctrl로 GPIO24를 INPUT+pull-up 으로 설정
   - probe 로그에서 `spi->irq: 57` 확인

3. **IRQ 요청 + 핸들러**  ✅  
   - `devm_request_threaded_irq()`로 IRQ 등록
   - threaded IRQ 핸들러에서 `irq_event` set + `wake_up_interruptible()`

4. **블로킹 경로 전환**  ✅  
   - `rc522_core.c` 의 `rc522_read_uid_blocking()`을 `wait_event_interruptible_timeout()` 기반으로 변경  
   - IRQ가 들어오면 즉시 깨고, 없으면 500ms 타임아웃 후 폴링 재시도

5. **폴백/정리**  ✅  
   - IRQ가 없거나(`spi->irq == 0`) 요청 실패 시 기존 폴링 방식 유지
   - 자세한 구현/이슈는 `Kernel_Driver_irq/Dev.md` 및 상위 구현 문서에 정리

## 빌드

```bash
cd /home/physical-100/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver_irq
make
```

### IRQ 테스트 시 GPIO24 풀업

- RC522 IRQ 핀(GPIO24)은 보드에 따라 오픈드레인 형식일 수 있어서, 풀업이 없으면 엣지가 잘 안 잡힐 수 있음.
- `rc522-overlay-irq.dts`에서 pinctrl로 pull-up을 걸어두었지만, 문제가 의심되면 다음 명령으로 수동으로도 풀업을 줄 수 있음:

```bash
gpio -g mode 24 up
```


