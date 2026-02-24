## IBVS 기반 레이저 제어 IMPLEMENTATION_PLAN

### 1. 목표 정의

- **최종 목표**  
  카메라 이미지에서  
  - (1) 타겟 객체의 중심 좌표  
  - (2) 레이저 스폿 중심 좌표  
  를 검출하고, 두 좌표의 **픽셀 오차**를 이용해 pan/tilt 서보(2축)를 제어하여  
  **레이저 스폿이 타겟 객체 중심에 수렴**하도록 하는 IBVS(Image-Based Visual Servoing) 기반 레이저 포인팅 시스템을 구현한다.

- **역할 분리**  
  - **Host(PC, 라즈베리 등)**  
    - 카메라 입력, 객체·레이저 검출, IBVS 제어 계산  
    - STM 보드로 보낼 **서보 목표값(us)** 생성
  - **STM 보드 (`hardware/stm32-laser`)**  
    - 이미 작성된 **서보 PWM 제어 로직**을 활용  
    - Host가 보내는 **두 축 서보 목표 펄스(us)** 를 그대로 적용하는 **액츄에이터 보드** 역할

---

### 2. 전체 시스템 아키텍처

- **구성 요소**
  - **Camera + Vision Node (Host)**  
    - 입력: 카메라 프레임  
    - 출력:
      - `target_center = (u_t, v_t)`  (타겟 객체 중심 픽셀 좌표)
      - `laser_center  = (u_l, v_l)`  (레이저 스폿 중심 픽셀 좌표)
      - 이미지 해상도 `(W, H)`

  - **IBVS Controller (Host)**  
    - 입력: `target_center`, `laser_center`  
    - 출력: 각 축의 제어 명령 (예: `servo_pan_us`, `servo_tilt_us`)  
    - 방식: 이미지 평면 상의 픽셀 오차를 이용한 단순 P/PD 제어

  - **Communication Layer (Host ↔ STM)**  
    - 프로토콜: 현재 `main.c`가 지원하는 UART 문자열 형식 재사용  
      - `"1500"` 또는 `"1500 1200"` → 두 채널 PWM us 설정  
      - `"mode 0"` / `"mode 1"` → manual / auto 모드 전환  
    - 주기: 카메라 FPS(예: 30Hz) 수준 또는 그 이하(예: 20Hz)로 서보 목표 전송

  - **STM Laser Servo Module (`stm32-laser`)**  
    - 입력(UART): `servo_ch1_us`, `servo_ch2_us` (pan, tilt)  
    - 처리: `__HAL_TIM_SET_COMPARE` 로 해당 us를 PWM으로 출력  
    - 출력: 두 서보 모터(pan/tilt) 구동 → 레이저 방향 제어

---

### 3. 좌표·프레임 정의 및 에러 계산

- **이미지 좌표계 정의**
  - 원점: 이미지 중심
    - \( u_c = W / 2,\; v_c = H / 2 \)
  - 검출 좌표:
    - 타겟 중심: `target_center = (u_t, v_t)`
    - 레이저 중심: `laser_center  = (u_l, v_l)`

- **에러 벡터 정의 (레이저를 타겟으로 정렬)**
  - 수평 오차:
    - \( e_u = u_t - u_l \)
  - 수직 오차:
    - \( e_v = v_t - v_l \)

- **축 매핑**
  - **Pan(수평 서보)** ← \( e_u \) 사용
  - **Tilt(수직 서보)** ← \( e_v \) 사용  
  - 실제 기구에서 움직임 방향과 화면 상 레이저 이동 방향을 비교해 **부호(±)** 는 실험으로 보정

---

### 4. IBVS 제어 법칙 (Host)

- **기본 아이디어**
  - 픽셀 오차를 직접 써서 서보 목표 각도 또는 펄스를 갱신한다.
  - 단순 PD 제어 예:
    - \( \Delta \theta_{pan}  = K_{p,u} \cdot e_u + K_{d,u} \cdot \frac{\Delta e_u}{\Delta t} \)
    - \( \Delta \theta_{tilt} = K_{p,v} \cdot e_v + K_{d,v} \cdot \frac{\Delta e_v}{\Delta t} \)

- **서보 펄스(us)로 변환**
  - PWM 범위 (STM 펌웨어와 일치시킴):
    - `PWM_US_MIN` ~ `PWM_US_MAX` (예: 800 ~ 2200 us)
    - 중립값: `1500 us`
  - Host 측 상태:
    - `current_pan_us`, `current_tilt_us` 를 내부에서 유지
  - 갱신 로직(단순 P 제어 기준):
    - `current_pan_us  += K_u * e_u`
    - `current_tilt_us += K_v * e_v`
    - 범위 제한:
      - `current_pan_us  = clamp(current_pan_us,  PWM_US_MIN, PWM_US_MAX)`
      - `current_tilt_us = clamp(current_tilt_us, PWM_US_MIN, PWM_US_MAX)`

- **게인/스케일 설정**
  - `K_u`, `K_v` 단위: **[us / pixel]**
  - 초기 설정 감각:
    - "오차 100픽셀 → 펄스 변경 ±100~200us" 수준에서 시작
  - 튜닝:
    - 진동/오버슈트 심함 → K 감소 또는 D항 추가
    - 응답이 너무 느림 → K 증가

---

### 5. Host ↔ STM 통신 프로토콜 설계

- **전제**  
  현재 `main.c`는 이미 다음 명령을 지원:
  - `"1500"` 혹은 `"1500 1200"`: 두 채널 PWM us 설정
  - `"mode 0"` / `"mode 1"`: manual / auto sweep 모드

- **IBVS 운용 정책**
  - IBVS 제어 시:
    - 항상 **`MODE_MANUAL`(0)** 에 두고, Host가 계산한 us를 직접 전송
    - auto sweep(`MODE_AUTO`)은 테스트용 기능으로 유지

- **명령 포맷**
  - 문자열:
    - `"PAN_US TILT_US\r\n"` (예: `"1520 1470\r\n"`)
  - 전송 타이밍:
    - 카메라 프레임마다(예: 30Hz) 또는 그보다 낮은 주기(20Hz 등)로 전송
  - 신뢰성:
    - ACK 없이 **주기적으로 최신 값만 덮어쓰는 방식**으로 단순화

---

### 6. STM 레이저 제어 모듈 설계 (펌웨어)

- **역할**
  - Host에서 전송되는 `"u1 u2"` 명령을 받아 두 서보 채널에 PWM 출력
  - 현재 `main.c`에서 이미 구현된 로직 재사용:
    - `__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, (uint32_t)u1);`
    - `__HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, (uint32_t)u2);`

- **모드 관리**
  - 전원 인가 시:
    - `control_mode = MODE_MANUAL` (기본값 유지)
  - IBVS 사용 시:
    - 필요하면 초기 1회 `"mode 0"` 명령으로 manual 모드 명시
    - 그 이후에는 `"u1 u2"` 명령만 반복 전송

- **PA5(D13, LD2) 상태**
  - 현재 구현:  
    - `MODE_AUTO`일 때: PA5 HIGH  
    - `MODE_MANUAL`일 때: PA5 LOW  
  - IBVS 운용 시에는 항상 manual 이므로 D13은 기본적으로 LOW  
  - 추후 필요하면, **IBVS 활성 상태 표시용 별도 GPIO** 추가 가능

---

### 7. 비전 모듈 (Host) 구현 계획

- **입력 처리**
  - 카메라 프레임 캡처 (OpenCV 등 사용)

- **타겟 객체 검출**
  - 단순 버전:
    - 색 기반 threshold + contour → 중심 좌표 계산
  - 고급 버전:
    - 딥러닝 detector(YOLO 등)로 bounding box 추출 → 중심
  - 출력:
    - `target_center = (u_t, v_t)`  
    - 검출 성공 여부: `target_found = true/false`

- **레이저 스폿 검출**
  - 광도 기반:
    - 프레임을 HSV/GRAY 등으로 변환
    - 특정 색/밝기 threshold 후 blob 검출
    - 가장 밝은 blob의 무게중심을 `laser_center = (u_l, v_l)` 로 사용
  - 검출 성공 여부: `laser_found = true/false`

- **검출 실패 처리**
  - 둘 중 하나라도 실패하면:
    - 제어 루프 일시 정지 또는
    - 마지막 명령 유지 + 에러 카운트 증가
    - 일정 시간 이상 실패 시:
      - 레이저 OFF 또는
      - 서보를 안전한 중립 위치(1500, 1500)로 이동

---

### 8. 제어 루프 & 상태 머신 (Host)

- **상태 정의**
  - `IDLE`:
    - IBVS 비활성, STM에는 중립 위치 명령(1500,1500) 유지
  - `TRACKING`:
    - 타겟·레이저 모두 검출 성공 → IBVS 동작
  - `LOST_TARGET` / `LOST_LASER`:
    - 각각 타겟/레이저 검출 실패 상태

- **루프 흐름**
  1. 카메라 프레임 획득
  2. `target_center`, `laser_center` 및 `found` 플래그 계산
  3. 둘 다 `found == true` 이면:
     - \( e_u, e_v \) 계산
     - IBVS 제어기로 `current_pan_us`, `current_tilt_us` 갱신
     - STM으로 `"PAN_US TILT_US\r\n"` 전송
  4. 하나라도 실패하면:
     - 상태에 따라:
       - 중립 위치로 서서히 복귀
       - 또는 마지막 명령 유지
       - 필요 시 레이저 OFF

- **주기**
  - 카메라 FPS(예: 30Hz)에 맞춰 루프 동작
  - 너무 빠른 명령 전송은 서보 떨림 유발 → 필요 시 명령 rate 제한 또는 저역통과 필터 적용

---

### 9. 캘리브레이션 및 튜닝 계획

- **부호(방향) 캘리브레이션**
  - pan 축:
    - 화면에서 타겟이 오른쪽에 있을 때(e_u > 0), 서보 명령을 증가/감소시켰을 때 레이저가 실제로 오른쪽으로 이동하는지 확인
  - tilt 축:
    - 위/아래 방향도 동일하게 확인 후, 필요 시 e_v에 -1을 곱해서 보정

- **게인(스케일) 튜닝**
  - 초기:
    - 예: `K_u = 0.5 us/pixel`, `K_v = 0.5 us/pixel`
  - 조정:
    - 오버슈트·진동 많음 → K 감소 또는 D항 추가
    - 반응 느림 → K 증가

- **물리 한계 반영**
  - 실제 서보 최대/최소 각도 측정
  - 그에 맞춰 `PWM_US_MIN`, `PWM_US_MAX`를 실기 기준으로 재조정

---

### 10. 테스트 및 검증 플랜

- **단위 테스트 (STM 서보 모듈)**
  - PC/라즈베리에서 수동으로:
    - `"1500 1500\r\n"`, `"1600 1400\r\n"` 등 전송
  - 두 서보가 의도한 방향/각도로 움직이는지 확인

- **단위 테스트 (비전 모듈)**
  - 저장된 이미지 또는 라이브 카메라 프레임 위에:
    - 검출된 타겟/레이저 중심 좌표를 overlay 해서 시각적으로 검증

- **통합 테스트 (저게인)**
  - gain을 작게 설정해 천천히 움직이게 한 뒤:
    - 레이저를 타겟 근처에 두고 IBVS ON
    - 시간 경과에 따라 픽셀 오차 \(|e_u|, |e_v|\)가 줄어드는지 확인

- **스트레스 테스트**
  - 조명/배경/반사 등 다양한 조건에서:
    - 검출 실패/오검출 상황에서 시스템이 안전하게 동작하는지 확인
  - 필요 시:
    - 검출 신뢰도 threshold
    - 명령 rate 제한
    - 오차 상한선 등 보호 로직 추가

---

### 11. 코드 구조 제안 (Host 측)

- **모듈 구성 예시**
  - `vision_detector.*`
    - `detect_target(frame) -> (u_t, v_t, target_found)`
    - `detect_laser(frame)  -> (u_l, v_l, laser_found)`
  - `ibvs_controller.*`
    - 내부 상태: `current_pan_us`, `current_tilt_us`, 이전 에러 등
    - `update(e_u, e_v, dt) -> (pan_us, tilt_us)`
  - `stm_interface.*`
    - UART 초기화 및 `"u1 u2\r\n"` 명령 전송 함수
  - `main_ibvs.*`
    - 메인 루프: 카메라 프레임 → detector → controller → stm_interface

- **STM 측 (`stm32-laser`)**
  - 현재 `main.c` 구조 유지
  - Host에서 계산된 두 채널 us 값을 받아 PWM으로 출력하는 **단순 액츄에이터 역할**에 집중


---

### 12. 체크리스트 (구현 단계용)

- [ ] Host 환경 준비
  - [ ] C++17 이상 컴파일러 및 OpenCV 설치
  - [ ] STM 보드와 Host 간 UART 연결 확인 (포트 이름, 보레이트 등)

- [ ] 비전 모듈 (`vision_detector.*`)
  - [ ] 카메라에서 프레임 캡처 코드 작성
  - [ ] 타겟 객체 검출(간단 버전: 색/threshold + contour) 구현
  - [ ] 레이저 스폿 검출(밝기/색 기반 blob) 구현
  - [ ] `detect_target(frame) -> (u_t, v_t, target_found)` 인터페이스 제공
  - [ ] `detect_laser(frame)  -> (u_l, v_l, laser_found)` 인터페이스 제공

- [ ] IBVS 제어 모듈 (`ibvs_controller.*`)
  - [ ] 이미지 좌표계 및 에러 계산 로직 정리 (`e_u`, `e_v`)
  - [ ] 내부 상태: `current_pan_us`, `current_tilt_us`, 이전 에러 저장
  - [ ] P 제어 또는 PD 제어 공식 구현
  - [ ] PWM 범위 (`PWM_US_MIN`, `PWM_US_MAX`) 및 중립값(1500us) 반영
  - [ ] `update(e_u, e_v, dt) -> (pan_us, tilt_us)` 함수 구현

- [ ] STM 인터페이스 모듈 (`stm_interface.*`)
  - [ ] UART(또는 시리얼 포트) 초기화 코드 작성
  - [ ] `"PAN_US TILT_US\r\n"` 형식 문자열 생성 함수 구현
  - [ ] 명령 전송 함수 구현 (주기적으로 최신 값만 전송)
  - [ ] 필요 시, 초기 `"mode 0\r\n"` 전송 로직 추가

- [ ] 메인 루프 (`main_ibvs.*`)
  - [ ] 시스템 상태 정의 (`IDLE`, `TRACKING`, `LOST_TARGET`, `LOST_LASER`)
  - [ ] 루프 내에서 프레임 획득 → 검출 → 에러 계산 → 제어 → STM 전송 순서 구현
  - [ ] 타겟/레이저 미검출 시 안전 동작(중립 이동 또는 유지) 처리
  - [ ] 루프 주기(예: 20~30Hz) 조절

- [ ] 캘리브레이션 및 튜닝
  - [ ] pan/tilt 방향 부호(±) 실험으로 확인
  - [ ] 초기 게인 `K_u`, `K_v` 설정 및 조정
  - [ ] 서보 물리 한계에 맞게 `PWM_US_MIN`, `PWM_US_MAX` 최종 확정

- [ ] 테스트
  - [ ] STM 서보 모듈 단독 테스트 (수동 `"1500 1500\r\n"` 등 전송)
  - [ ] 비전 모듈 단독 테스트 (검출 결과 overlay)
  - [ ] 저게인 통합 테스트 (오차 감소 여부 확인)
  - [ ] 조명/배경 변화 등 스트레스 테스트 및 보호 로직 보완

