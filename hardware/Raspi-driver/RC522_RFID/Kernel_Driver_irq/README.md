# RC522 IRQ Driver Workspace

이 폴더는 기존 `Kernel_Driver`를 보존한 상태에서, **IRQ(인터럽트) 기반 전환**을 단계별로 진행하기 위한 작업 공간입니다.

## 진행 원칙

- 원본(`Kernel_Driver`)은 수정하지 않음
- 이 폴더에서 단계별로 변경
- 각 단계마다 빌드/동작 확인 후 다음 단계 진행

## 단계 계획

1. **스캐폴딩(현재 단계)**  
   - `Kernel_Driver_irq` 폴더 생성
   - 독립 Makefile 준비
   - IRQ용 오버레이 파일 추가

2. **DTO + probe 확인 단계**  
   - IRQ 핀(GPIO24) DT 연결
   - probe에서 `spi->irq` 값 확인 로그

3. **IRQ 요청 단계**  
   - `devm_request_threaded_irq()` 등록
   - IRQ 핸들러에서 이벤트 플래그 set + wakeup

4. **블로킹 경로 전환 단계**  
   - `wait_event_interruptible()` 기반으로 `read()/ioctl` 대기 전환
   - IRQ 미지원 시 폴백(기존 폴링)

5. **정리/문서화 단계**  
   - Dev/Work 문서 갱신
   - 테스트 절차 정리

## 빌드

```bash
cd /home/physical-100/SFEPS/hardware/Raspi-driver/RC522_RFID/Kernel_Driver_irq
make
```

