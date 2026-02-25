
# SFEPS Test Case Specification (테스트케이스 명세서)
Project: 지하철 부정승차 방지 시스템(SFEPS)  
Version: 1.0  
Author: 서형철
Environment: Windows 11(QT Client), Raspberry Pi(Server), Camera(PNO-A9081R), STM32(Nucleo F401RE, Laser only)

## 공통 전제/표기
- **계정 데이터(유효 계정)**: `admin / 1111`
- **Mock age**: 얼굴 추정 나이 미구현 시, 서버/테스트훅/테스트모드에서 임의값 주입 가능해야 함
- **RFID 태그 주입 방식**: 실제 RFID 태그 또는 서버 모킹 API/시뮬레이터를 사용(프로젝트 환경에 맞춰 선택)
- **측정**: 성능(1초)은 로그 타임스탬프 또는 클라이언트/서버 계측(버튼 클릭 시각, 디바이스 ON/ACK 시각)으로 측정

---

## 1) Functional Test Cases — LOGIN

### TC-FUNC-LOGIN-01 유효한 ID/PW 입력 시 로그인 성공
- **Level/Type/Priority**: System / Functional / High
- **Execution**: Manual
- **Pre-condition**
  - QT Client 실행 가능
  - Server 접속 가능
  - 계정 `admin` 존재
- **Input Data**
  - ID=`admin`, PW=`1111`
- **Steps**
  1. QT Client 로그인 화면 진입
  2. ID에 `admin` 입력
  3. PW에 `1111` 입력
  4. 로그인 버튼 클릭
- **Expected Result**
  - 로그인 성공
  - 메인 화면으로 전환(또는 메인 UI 로딩 완료)

### TC-FUNC-LOGIN-02 존재하지 않는 ID 입력 시 로그인 실패 및 오류 메시지 표시
- **Level/Type/Priority**: System / Functional / High
- **Execution**: Manual
- **Pre-condition**: Server 접속 가능
- **Input Data**: ID=`no_user_999`, PW=`1111`
- **Steps**: 존재하지 않는 ID로 로그인 시도
- **Expected Result**
  - 로그인 실패
  - 오류 메시지 표시(문구는 구현 정책, “존재하지 않는 계정/로그인 정보 오류” 등)

### TC-FUNC-LOGIN-03 잘못된 PW 입력 시 로그인 실패 및 오류 메시지 표시
- **Level/Type/Priority**: System / Functional / High
- **Execution**: Manual
- **Pre-condition**: `admin` 계정 존재
- **Input Data**: ID=`admin`, PW=`WrongPW!`
- **Steps**: 잘못된 비밀번호로 로그인 시도
- **Expected Result**
  - 로그인 실패
  - 오류 메시지 표시

### TC-FUNC-LOGIN-04 ID 또는 PW 공백(미입력 포함) 시 로그인 실패 및 안내
- **Level/Type/Priority**: System / Functional / High
- **Execution**: Manual
- **Pre-condition**: 로그인 화면
- **Input Data(서브케이스)**
  - 4-1: ID=``, PW=`1111`
  - 4-2: ID=`admin`, PW=``
  - 4-3: ID=``, PW=``
- **Steps**
  1. 각 서브케이스 입력 상태로 로그인 버튼 클릭
- **Expected Result**
  - 로그인 실패
  - 필수 입력 안내 메시지 표시
  - (가능하면) 서버 요청 미발생 또는 즉시 실패 처리

### TC-FUNC-LOGIN-05 로그인 성공 후 메인 화면 전환 확인
- **Level/Type/Priority**: System / Functional / Medium
- **Execution**: Manual
- **Pre-condition**: 유효 계정 존재
- **Input Data**: ID=`admin`, PW=`1111`
- **Steps**
  1. TC-FUNC-LOGIN-01 수행
  2. 메인 화면 주요 UI(스트리밍 패널/이벤트 영역 등) 표시 확인
- **Expected Result**
  - 메인 화면 정상 표시
  - 사용자 조작 가능 상태

---

## 2) Functional Test Cases — STREAM

### TC-FUNC-STREAM-01 Server를 통해 Camera 영상 스트림 수신 및 화면 표시
- **Level/Type/Priority**: Integration/System / Functional / High
- **Execution**: Manual
- **Pre-condition**
  - Camera(PNO-A9081R) 전원 ON 및 네트워크 연결
  - Server에서 Camera 스트림 수신 설정 완료
  - QT Client 메인 화면 진입
- **Input Data**: 없음
- **Steps**
  1. QT Client에서 스트리밍 표시 영역 확인
  2. (필요 시) “Streaming Start/Connect” 기능 수행
  3. 10초 이상 프레임 갱신 확인
- **Expected Result**
  - 영상이 끊김 없이 표시
  - 프레임이 지속적으로 갱신

### TC-FUNC-STREAM-02 네트워크 단절/지연 등으로 스트리밍 불가 시 오류 상태 표시
- **Level/Type/Priority**: Integration/System / Functional / High
- **Execution**: Manual
- **Pre-condition**: 스트리밍이 정상 표시 중(TC-FUNC-STREAM-01 통과 상태)
- **Input Data**: 네트워크 단절(스위치/케이블/방화벽 룰) 또는 지연/차단 시뮬레이션
- **Steps**
  1. Camera↔Server 또는 Server↔Client 구간 네트워크 단절 유도
  2. QT Client 스트리밍 영역 상태 관찰
- **Expected Result**
  - 스트리밍 중단 감지
  - 오류/경고 상태가 UI에 표시

### TC-FUNC-STREAM-03 네트워크 복구 후 스트리밍 자동 재연결/복구 확인
- **Level/Type/Priority**: Integration/System / Functional / High
- **Execution**: Manual
- **Pre-condition**: TC-FUNC-STREAM-02로 스트리밍 장애 상태
- **Input Data**: 네트워크 복구
- **Steps**
  1. 네트워크를 정상 상태로 복구
  2. 자동 재연결 발생 여부 확인(관찰 시간 예: 30~60초)
- **Expected Result**
  - 자동 재연결 성공
  - 영상 표시가 정상으로 복구

---

## 3) Functional Test Cases — RFID

### TC-FUNC-RFID-01 RFID 태그 데이터가 Raspberry Pi(Server)로 정상 수집됨
- **Level/Type/Priority**: Integration / Functional / High
- **Execution**: Manual
- **Pre-condition**
  - RFID 리더/장치가 Raspberry Pi에 연결되어 동작
  - Server가 RFID 수집 서비스 실행 중
- **Input Data**
  - 정상 RFID 태그 1회 태깅(또는 모킹 API로 태그 이벤트 1회 주입)
- **Steps**
  1. RFID 태그를 리더에 태깅
  2. Server 로그 또는 수집 엔드포인트에서 태그 수신 확인
- **Expected Result**
  - 태그 데이터(카드 ID 등)가 Server에 수신/기록됨

### TC-FUNC-RFID-02 RFID 태그 데이터 수집 시각(타임스탬프)이 함께 기록됨
- **Level/Type/Priority**: Integration / Functional / Medium
- **Execution**: Auto
- **Pre-condition**: TC-FUNC-RFID-01 수행 가능
- **Input Data**: RFID 태그 1회 이상(또는 모킹 주입)
- **Steps**
  1. RFID 태그 데이터 주입
  2. 기록된 태그 로그/DB 필드에서 타임스탬프 확인
- **Expected Result**
  - 각 RFID 태그 데이터에 수신 시각이 기록됨(누락 없음)

---

## 4) Functional Test Cases — EVENT(의심 판정/이벤트 생성)
### TC-FUNC-EVENT-01 개찰구 2개 가상선 통과 시각이 정상 기록됨
- **Level/Type/Priority**: Integration(또는 Unit 가능) / Functional / High
- **Execution**: Manual
- **Pre-condition**
  - 가상선 통과 이벤트를 생성할 수 있는 환경
- **Input Data**
  - Line1 통과 시각 t1, Line2 통과 시각 t2(t2>t1)
- **Steps**
  1. 객체가 Line1을 통과하도록 이벤트/시뮬레이션 발생
  2. 객체가 Line2를 통과하도록 이벤트/시뮬레이션 발생
  3. 서버 로그/판정 모듈에서 t1, t2 기록 확인
- **Expected Result**
  - t1, t2가 정상 저장/참조 가능

### TC-FUNC-EVENT-02 두 타임스탬프 사이 RFID 태그가 없으면 의심 판정됨
- **Level/Type/Priority**: Unit/Integration / Functional / High
- **Execution**: Auto
- **Pre-condition**
  - t1, t2 생성 가능
  - t1~t2 사이 RFID 입력이 “없음”으로 처리되도록 구성
- **Input Data**
  - t1~t2 구간 RFID 태그 0회
- **Steps**
  1. t1 발생
  2. RFID 태그 없이 t2 발생
  3. 판정 결과 확인
- **Expected Result**
  - 판정=의심
  - 의심 이벤트 생성 조건 충족

### TC-FUNC-EVENT-03 청소년 우대카드 + age<20 → 정상 판정
- **Level/Type/Priority**: Unit/Integration / Functional / High
- **Execution**: Auto
- **Pre-condition**: 우대카드(청소년) 태그 데이터 주입 가능, Mock age 주입 가능
- **Input Data**
  - CardType=청소년, age=19, t1~t2 사이 태그 존재
- **Steps**
  1. t1 발생
  2. t1~t2 사이 청소년 카드 태그 주입
  3. Mock age=19 입력/생성
  4. t2 발생 후 판정 확인
- **Expected Result**
  - 판정=정상
  - 의심 이벤트 미생성

### TC-FUNC-EVENT-04 청소년 우대카드 + age≥20 → 의심 판정
- **Level/Type/Priority**: Unit/Integration / Functional / High
- **Execution**: Auto
- **Input Data**: CardType=청소년, age=20
- **Steps**: TC-FUNC-EVENT-03과 동일 흐름
- **Expected Result**
  - 판정=의심
  - 의심 이벤트 생성 조건 충족

### TC-FUNC-EVENT-05 노인 우대카드 + age>60 → 정상 판정
- **Level/Type/Priority**: Unit/Integration / Functional / High
- **Execution**: Auto
- **Input Data**: CardType=노인, age=61
- **Expected Result**
  - 판정=정상
  - 의심 이벤트 미생성

### TC-FUNC-EVENT-06 노인 우대카드 + age≤60 → 의심 판정
- **Level/Type/Priority**: Unit/Integration / Functional / High
- **Execution**: Auto
- **Input Data**: CardType=노인, age=60
- **Expected Result**
  - 판정=의심
  - 의심 이벤트 생성 조건 충족

### TC-FUNC-EVENT-07 청소년 우대카드 경계값(19/20) 판정 일관성 검증
- **Level/Type/Priority**: Unit/Integration / Functional / Medium
- **Execution**: Auto
- **Input Data(서브케이스)**
  - A: CardType=청소년, age=19 → 정상
  - B: CardType=청소년, age=20 → 의심
- **Steps**
  1. A 수행 후 판정 확인
  2. B 수행 후 판정 확인
- **Expected Result**
  - 경계값에서 규칙대로 일관된 판정

### TC-FUNC-EVENT-08 노인 우대카드 경계값(60/61) 판정 일관성 검증
- **Level/Type/Priority**: Unit/Integration / Functional / Medium
- **Execution**: Auto
- **Input Data(서브케이스)**
  - A: CardType=노인, age=60 → 의심
  - B: CardType=노인, age=61 → 정상
- **Expected Result**
  - 경계값에서 규칙대로 일관된 판정

### TC-FUNC-EVENT-09 의심 판정 시 의심 이벤트 로그가 생성됨
- **Level/Type/Priority**: Unit/Integration / Functional / High
- **Execution**: Auto
- **Pre-condition**: 의심 판정 유도 가능(TC-FUNC-EVENT-02/04/06 중 하나)
- **Input Data**: 의심 판정 케이스 1개
- **Steps**
  1. 의심 판정 발생
  2. 이벤트 생성 여부 확인(서버/DB/이벤트 큐)
- **Expected Result**
  - 이벤트 ID 포함 의심 이벤트 생성됨

### TC-FUNC-EVENT-10 생성된 의심 이벤트가 QT Client에 전달되어 목록에 표시됨
- **Level/Type/Priority**: System / Functional / High
- **Execution**: Manual
- **Pre-condition**
  - QT Client 로그인 후 메인 화면
  - 의심 이벤트 생성 가능
- **Input Data**: 의심 이벤트 1건
- **Steps**
  1. 의심 이벤트 생성
  2. QT Client 이벤트 목록 갱신/수신 확인
- **Expected Result**
  - 이벤트 목록에 신규 이벤트가 표시됨(이벤트 ID/시간 등)

### TC-FUNC-EVENT-11 의심 이벤트가 DB에 저장됨(필수 필드 포함)
- **Level/Type/Priority**: Integration / Functional / High
- **Execution**: Auto
- **Pre-condition**: DB 연결 정상
- **Input Data**: 의심 이벤트 1건
- **Steps**
  1. 의심 이벤트 생성
  2. DB에서 해당 이벤트 레코드 조회
  3. 필수 필드 존재 확인(이벤트 ID, 발생 시간, 게이트 ID, 카드 정보, 추정 나이, 스크린샷/참조 경로)
- **Expected Result**
  - 레코드 저장됨
  - 필수 필드 누락 없음

### TC-FUNC-EVENT-12 정상 판정 시 의심 이벤트가 생성되지 않음
- **Level/Type/Priority**: Unit/Integration / Functional / High
- **Execution**: Auto
- **Input Data**: 정상 판정 케이스(예: 청소년 age=19 또는 노인 age=61)
- **Steps**
  1. 정상 판정 입력 수행
  2. 이벤트 생성 여부 확인
- **Expected Result**
  - 의심 이벤트 생성되지 않음

### TC-FUNC-EVENT-13 잘못된 이벤트 메시지 포맷 처리(크래시 없이 무시/실패 처리)
- **Level/Type/Priority**: Integration / Functional / Medium
- **Execution**: Auto
- **Pre-condition**: Server 또는 테스트 도구로 이벤트 메시지 전송 가능
- **Input Data(예)**
  - 필수 필드 누락 JSON
  - 타입 불일치
  - 깨진 JSON
- **Steps**
  1. Client 또는 Server에 비정상 메시지 3종 이상 주입
  2. 앱/서버 동작 상태 확인
  3. 마지막으로 정상 이벤트 메시지 1건을 주입하여 정상 처리되는지 확인
- **Expected Result**
  - 비정상 이벤트 메시지 수신 시 크래시/프리징 없음
  - 비정상 메시지는 무시 또는 실패 처리되며 시스템 전체 동작에 영향 없음
  - 정상 메시지 처리는 계속 가능

---

## 5) Functional Test Cases — UI(이벤트 목록/상세 팝업)

### TC-FUNC-UI-01 이벤트 로그 목록 조회/표시 가능
- **Level/Type/Priority**: System / Functional / High
- **Execution**: Manual
- **Pre-condition**: QT Client 로그인 완료, 의심 이벤트 1건 이상 존재
- **Steps**
  1. 이벤트 로그 목록 영역 확인
  2. 목록 새로고침(있다면) 수행
- **Expected Result**
  - 이벤트 목록이 표시되고 항목 식별 가능

### TC-FUNC-UI-02 이벤트 선택 시 상세 팝업 표시
- **Level/Type/Priority**: System / Functional / High
- **Execution**: Manual
- **Pre-condition**: 의심 이벤트 목록 표시 상태
- **Steps**
  1. 이벤트 1건 선택/클릭
- **Expected Result**
  - 상세 팝업(Detail View)이 표시됨

### TC-FUNC-UI-03 상세 팝업 정보 표시(스크린샷/카드정보/추정나이/발생시각/게이트ID)
- **Level/Type/Priority**: System / Functional / High
- **Execution**: Manual
- **Pre-condition**: 상세 팝업 표시 상태(TC-FUNC-UI-02 선행)
- **Steps**
  1. 팝업 내 각 필드 존재 및 값 표시 확인
- **Expected Result**
  - 얼굴 스크린샷(또는 캡처 프레임) 표시
  - 카드 정보 표시
  - 추정 나이 표시(Mock 포함)
  - 발생 시각/게이트ID 표시

---

## 6) Functional Test Cases — TRACK(사이렌+레이저)

### TC-FUNC-TRACK-01 관리자가 상세 팝업에서 Tracking 버튼 클릭 시에만 Tracking 수행
- **Level/Type/Priority**: System / Functional / High
- **Execution**: Manual
- **Pre-condition**
  - 의심 이벤트 1건 존재 및 상세 팝업 표시
- **Input Data**: 없음
- **Steps**
  1. 상세 팝업에서 Tracking 버튼 클릭
- **Expected Result**
  - Tracking 절차가 시작됨(서버로 제어 요청)
  - (관찰/로그 기준) 오디오 ON 및 레이저 ON 제어가 트리거됨

### TC-FUNC-TRACK-02 Tracking 시작 시 Server가 오디오(사이렌) ON, 종료 시 OFF 제어
- **Level/Type/Priority**: Integration/System / Functional / High
- **Execution**: Manual
- **Pre-condition**
  - 오디오(사이렌) 디바이스가 Raspberry Pi에 연결되어 제어 가능
  - 상세 팝업에서 Tracking 수행 가능
- **Steps**
  1. Tracking 버튼 클릭하여 시작
  2. 오디오 ON 여부 확인(실제 소리 또는 서버 로그/상태)
  3. Tracking 종료(팝업 내 종료 버튼/토글 정책에 따라)
  4. 오디오 OFF 여부 확인
- **Expected Result**
  - 시작 시 오디오 ON
  - 종료 시 오디오 OFF
  - 상태/로그가 일치

### TC-FUNC-TRACK-03 Tracking 시작 시 Server→STM32로 레이저 트래킹 ON, 종료 시 OFF 제어
- **Level/Type/Priority**: Integration/System / Functional / High
- **Execution**: Manual
- **Pre-condition**
  - STM32가 Server와 연결되어 레이저 제어 가능
- **Steps**
  1. Tracking 시작
  2. 레이저 ON/트래킹 동작 확인(실물 관찰 또는 STM32 ACK/로그)
  3. Tracking 종료
  4. 레이저 OFF 확인
- **Expected Result**
  - 시작 시 레이저 ON
  - 종료 시 레이저 OFF
  - ACK/로그가 일치

### TC-FUNC-TRACK-04 QT Client에서 Tracking ON/OFF 상태가 UI에 표시됨
- **Level/Type/Priority**: System / Functional / Medium
- **Execution**: Manual
- **Pre-condition**: Tracking 제어 가능 상태
- **Steps**
  1. Tracking 시작 후 UI 상태 표시 확인(ON)
  2. Tracking 종료 후 UI 상태 표시 확인(OFF)
- **Expected Result**
  - UI에 상태가 정확히 반영됨

---

## 7) Non-Functional Test Cases — Performance

### TC-NF-PERF-01 Tracking 버튼 클릭 → (오디오 ON + 레이저 ON) 시작/ACK 확인까지 1초 이내
- **Level/Type/Priority**: System/Integration / Non-Functional(Performance) / High
- **Execution**: Manual
- **Pre-condition**
  - 의심 이벤트 존재 및 상세 팝업 표시
  - 오디오 제어 가능(Server)
  - 레이저 제어 가능(STM32)
  - 시간 측정용 로그/계측 가능
- **Measurement**
  - Start: QT Client Tracking 버튼 클릭 시각
  - End: 오디오 ON 처리완료 확인 시각과 레이저 ON ACK(또는 처리완료) 시각 중 더 늦은 시각
- **Steps**
  1. 계측/로그 수집 시작
  2. Tracking 버튼 클릭
  3. End 시각 확보(서버/STM32 로그 또는 응답)
  4. 경과 시간 계산
- **Expected Result**
  - 경과 시간 ≤ 1.0초

### TC-NF-PERF-02 1분 내 20건 이상 의심 이벤트 처리(생성/전달/표시)
- **Level/Type/Priority**: System / Non-Functional(Performance) / High
- **Execution**: Auto
- **Pre-condition**
  - 의심 이벤트를 자동/시뮬레이션으로 20건 이상 생성 가능
- **Steps**
  1. 60초 동안 의심 이벤트 20건 이상 발생시키기(시뮬레이터/모킹)
  2. 서버 처리 성공 수/실패 수 확인
  3. QT Client 이벤트 목록 표시 수 확인
- **Expected Result**
  - 60초 내 20건 이상 처리 완료
  - 누락/중복이 허용 범위를 벗어나지 않음(정량 기준은 팀 정책에 따름)

### TC-NF-PERF-03 1시간 연속 스트리밍 유지(중단 시 자동 복구 포함 가능)
- **Level/Type/Priority**: System / Non-Functional(Performance) / High
- **Execution**: Manual
- **Pre-condition**: 스트리밍 정상 상태
- **Steps**
  1. 스트리밍을 1시간 연속 실행
  2. 중간 끊김/오류 발생 여부 기록
  3. 끊김 발생 시 자동 복구 수행 여부 기록(해당 정책이 있는 경우)
- **Expected Result**
  - 1시간 동안 스트리밍 유지
  - 치명적 장애(앱 다운/서버 다운) 없이 동작

---

## 8) Non-Functional Test Cases — Reliability

### TC-NF-RELI-01 Tracking(ON→OFF/종료) 20회 반복 시 오류/상태불일치/비정상 종료 없음
- **Level/Type/Priority**: System/Integration / Non-Functional(Reliability) / High
- **Execution**: Manual
- **Pre-condition**
  - 의심 이벤트 존재(또는 반복 생성 가능)
  - 오디오/레이저 제어 정상
- **Steps**
  1. 이벤트 상세 팝업 진입
  2. 아래를 20회 반복
     - Tracking 시작
     - 2~3초 유지
     - Tracking 종료
  3. 반복 중 오류/예외/크래시 여부 확인
  4. 각 반복에서 UI 상태와 실제 장치 동작 일치 여부 확인
- **Expected Result**
  - 20회 수행 동안 비정상 종료/오류 없음
  - UI 표시와 오디오/레이저 동작이 일치

---

## 9) Non-Functional Test Cases — Recoverability

### TC-NF-REC-01 네트워크 단절 후 복구 시 시스템 정상 동작 상태로 복귀
- **Level/Type/Priority**: Integration/System / Non-Functional(Recoverability) / High
- **Execution**: Manual
- **Pre-condition**
  - QT Client와 Server 정상 연결
  - (가능하면) 스트리밍 및 이벤트 수신 가능한 상태
- **Steps**
  1. 정상 동작 상태 확인(연결 상태/스트리밍 등)
  2. 네트워크 단절 유도(10초)
  3. 단절 중 상태 표시/오류 처리 확인
  4. 네트워크 복구
  5. 시스템 기능 정상 복귀 확인(연결 상태, 스트리밍 재연결, 이벤트 수신 가능 여부)
- **Expected Result**
  - 복구 후 정상 상태로 돌아옴(재연결 및 기능 재개)
  - 연결/단절/재연결 상태가 UI에 표시됨

---

## 10) Scenario Test Cases (End-to-End)

### TC-SYS-01 로그인 → 스트리밍 → 의심 이벤트 수신/표시 → 상세 팝업 확인 → Tracking 수행(오디오+레이저)
- **Level/Type/Priority**: System / Functional(Scenario) / High
- **Execution**: Manual
- **Pre-condition**
  - 유효 계정 존재(`admin / 1111`)
  - 스트리밍 가능
  - 의심 이벤트 생성 가능(시뮬레이션/모킹 포함)
  - 오디오/레이저 제어 가능
- **Steps**
  1. 로그인 성공
  2. 스트리밍 영상 표시 확인
  3. 의심 이벤트 1건 발생(무태그 또는 우대카드 규칙 위반 등)
  4. QT Client 이벤트 목록에 표시되는지 확인
  5. 해당 이벤트 클릭 → 상세 팝업 표시
  6. 팝업에서 스크린샷/카드정보/추정나이/시간/게이트ID 확인
  7. Tracking 버튼 클릭
  8. 오디오 ON 및 레이저 ON 확인
  9. Tracking 종료 후 오디오 OFF 및 레이저 OFF 확인
- **Expected Result**
  - 전체 흐름이 끊김 없이 수행됨
  - 이벤트 정보 표시가 정확함
  - Tracking 동작(오디오/레이저)이 정상 수행됨