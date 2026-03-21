# SFEPS Test Plan (테스트 계획서)
Project: 지하철 부정승차 방지 시스템 (SFEPS)  
Version: 1.2  
Date: 2026-03-03  
Author: 서형철  
Basis:
- SFEPS SRS v1.2

---

## 1. 문서 개요 (Document Overview)
### 1.1 목적 (Purpose)
본 문서는 SFEPS의 기능 및 비기능 요구사항을 검증하기 위한 테스트 전략, 범위, 방법, 자원, 일정 및 종료 기준을 정의한다.

---

## 2. 테스트 대상 (Test Item)
- **QT Client (관리자 인터페이스)**
  - 로그인
  - 의심 이벤트 목록/상세 팝업
  - Tracking 버튼 제어 및 상태 표시
  - Server 연결 상태 표시
- **Video Streaming Module**
  - Camera: **PNO-A9081R**
  - Camera → Server → QT Client 스트리밍
- **Fare Evasion Detection Logic (부정승차 의심 판정/이벤트 생성)**
  - 무태그/우대카드 규칙 기반 의심 판정
  - 의심 이벤트 생성
  - 비정상 이벤트 메시지 포맷 처리
- **Tracking Module (Laser Tracking)**
  - **Laser 모듈 제어: STM32 Nucleo F401RE**
  - *관리자가 스트리밍 객체 바운딩박스의 Track 버튼 클릭 시에만* 레이저 제어 동작
- **Server**
  - **Raspberry Pi** 기반 API/중계 서버  
  - 스트리밍 중계, 이벤트 전달, STM32 레이저 제어 명령 중계
- **Network Communication**
  - Server-Client(로그인/명령/이벤트/연결상태)
  - Camera-Server-Client(영상)

---

## 3. 테스트 범위 (Scope of Testing)
### 3.1 포함 범위 (In Scope)

#### 3.1.1 기능 테스트 (Functional Testing)
1) **로그인(QT Client)**
- 유효 ID/PW 로그인 성공
- 존재하지 않는 ID 로그인 실패 및 오류 메시지
- 비밀번호 오류 로그인 실패 및 오류 메시지
- ID/PW 공백 입력 시 실패(서버 요청 미발생 또는 즉시 실패 처리)

2) **영상 스트리밍(Camera-Server-Client)**
- 프레임 수신/표시 시작 및 유지
- 네트워크 단절 시 에러 표시
- 네트워크 복구 시 자동 재연결 및 스트리밍 복구

3) **부정승차 의심 판정 및 이벤트(1차 자동)**
- 무태그 입력 → 의심 판정
- 우대카드 규칙(경계값) 판정 일관성
  - 청소년: 19/20 (19 정상)
  - 노인: 59/60 (60 정상)
- 의심 판정 시 의심 이벤트 생성
- 정상 판정 시 의심 이벤트 미생성
- 잘못된 이벤트 메시지 포맷 처리(크래시 없이 무시/실패 처리, 이후 정상 메시지 처리 지속)

4) **이벤트 UI(2차 확인)**
- 생성된 의심 이벤트가 QT Client에 전달되어 목록에 표시
- 이벤트 선택 시 상세 팝업 표시 및 상세 정보 확인(스크린샷/카드정보/추정나이/발생시각/게이트ID)
- 로그아웃 버튼 클릭 시 로그아웃 요청 전송 및 로그인 화면 복귀

5) **Tracking(레이저) — 관리자 수동 트리거**
- 관리자가 스트리밍 객체 바운딩박스의 Track 버튼 클릭 시 레이저 Tracking 수행(ON/OFF)
- Tracking ON/OFF 상태가 UI에 표시

6) **네트워크(연결 상태 표시)**
- QT Client에서 Server 연결 상태(연결/단절/재연결) 표시

#### 3.1.2 비기능 테스트 (Non-Functional Testing)
**A. 성능(Performance)**
- **Tracking 제어 응답 성능(레이저)**
  - 시작: QT Client 스트리밍 객체 바운딩박스의 Track 버튼 클릭 시각
  - 종료: 레이저 ON ACK 시각
  - 기준: **1초 이내**
- 1분 내 50건 이상 의심 이벤트 처리(처리량)

**B. 신뢰성(Reliability)**
- 의심 이벤트가 존재하는 상태에서 Tracking(ON→OFF/종료)을 **20회 반복 수행**해도
  - 오류/예외/비정상 종료 없음
  - 상태 불일치 없음(UI 표시 vs 레이저 실제 동작)
- 1시간 연속 스트리밍 유지(중단 시 자동 복구 포함 가능)

**C. 회복성(Recoverability)**
- 네트워크 단절이 5초를 초과하면 자동 로그아웃되어 로그인 화면으로 전환됨
- 네트워크 복구 후 재로그인 시 정상 동작 상태로 복귀(연결상태 UI 포함)

### 3.2 제외 범위 (Out of Scope)
- 얼굴 추정 나이 알고리즘 정확도 검증(미구현 → Mock 데이터 활용)
- 하드웨어 물리적 정밀도/스펙 인증(레이저 조준 정밀도 등)
- RFID 센서/리더 품질 인증 및 RFID 데이터 수집/타임스탬프 검증

---

## 4. 테스트 레벨 (Test Levels)
- **Unit Test**
  - 의심 판정 로직(무태그/우대카드 규칙) 단위 검증
  - 이벤트 생성/미생성 로직 단위 검증(테스트 훅/드라이버 기반)
- **Integration Test**
  - Camera-Server-Client 스트리밍
  - Client-Server 통신(로그인/이벤트/연결상태)
  - Server-STM32(레이저 제어 인터페이스)
  - 비정상 이벤트 메시지 포맷 수신/처리
- **System Test**
  - End-to-End 시나리오(로그인 → 스트리밍 → 의심 이벤트 표시 → 상세 확인 → Tracking(레이저)) 검증

---

## 5. 테스트 유형 (Test Types)
### 5.1 기능 테스트 (Functional Testing)
- 로그인 검증(성공/실패/공백)
- 스트리밍 수신/오류표시/재연결 검증
- 의심 판정 규칙 검증(경계값 포함)
- 의심 이벤트 생성/미생성 검증
- 이벤트 UI(목록/상세) 검증
- Tracking(레이저) 제어 및 UI 상태표시 검증
- 네트워크 예외 처리(단절/복구, 포맷 오류) 검증

### 5.2 비기능 테스트 (Non-Functional Testing)
- 성능(Tracking 응답시간, 이벤트 처리량)
- 신뢰성(Tracking 반복 안정성, 장시간 스트리밍)
- 회복성(네트워크 단절 후 자동 로그아웃 및 재로그인 복구)

---

## 6. 테스트 설계 기법 (Test Design Techniques)
- 동등 분할(EP): 로그인 입력
- 경계값(BVA): age 경계(19/20, 59/60)
- 결정 테이블: 우대카드/무태그 판정
- 상태 전이: Streaming/Tracking 상태 및 UI 상태 표시
- 오류 추정: 네트워크 단절, 잘못된 이벤트 메시지 포맷
- 시나리오 테스트: End-to-End 운영 흐름

---

## 7. 테스트 환경 (Test Environment)
- OS: Windows 11
- Client: QT Client(관리자 인터페이스)
- Server: Raspberry Pi (API/중계 서버)
- Camera: PNO-A9081R
- Laser 제어 MCU: STM32 Nucleo F401RE
- Network: Local LAN(단절/지연/손실 시뮬레이션 구성 권장)

---

## 8. 테스트 툴 / 자동화 (Test Tools)
- **pytest**: 로직/통신 기반 단위·통합 테스트 자동화(프로젝트 정책에 따름)
- **Jenkins**: 리그레션 테스트(CI) 파이프라인 구성 및 주기 실행
- **Docker**: 서버 스텁/시뮬레이터를 CI에서 구동하기 위한 도구

---

## 9. 테스트 산출물 (Test Deliverables)
- Test Plan
- Test Condition Specification
- Test Case Specification
- Test Execution Report
- Defect Report
- Traceability Matrix(Requirement ↔ Test Case)

---

## 10. 결함 관리 (Defect Management)
- 결함 등급: Critical / Major / Minor
- Critical 결함 0건 시 릴리즈 가능
- Major 결함은 수정 또는 우회 방안 확보 후 종료

---

## 11. 종료 기준 (Exit Criteria)
- 전체 Test Case 95% 이상 수행
- Critical Defect 0건
- 비기능 요구사항 충족(Tracking 1초, 처리량, 1시간 스트리밍 안정성)
- 주요 시나리오 테스트(TC-SYS-01) 100% 통과

---

## 12. 위험 요소 및 대응 방안 (Risk & Mitigation)
- 판정 로직/시뮬레이터 불완전  
  → 테스트 훅 기반으로 Unit 테스트 우선 구축 후, 통합 테스트로 확장
- 얼굴 추정 나이 알고리즘 미구현  
  → Mock age로 경계값 중심 검증
- 장치 의존성(카메라/STM32) 및 네트워크 불안정  
  → 장치 인터페이스 모킹/시뮬레이션 병행, 테스트베드/야간 실행 분리, 네트워크 단절/복구 시나리오 포함
- UI 자동화의 플래키성(Windows GUI 환경)  
  → 핵심 로직/통신은 Unit/Integration으로 최대한 커버하고, UI는 스모크 중심 + 수동/반자동 병행

---

## 13. 테스트 일정 (High-Level Schedule)
- 계획: Test Plan 수립/업데이트
- 설계: Test Case 업데이트 및 Traceability 정리
- 구현: pytest 자동화 및 Jenkins 리그레션 구성
- 실행: 기능 및 비기능 테스트 수행(테스트베드 포함)
- 종료: Test Report 작성
