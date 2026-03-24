# SFEPS Test Condition Specification (테스트 조건 명세서)
Project: 지하철 부정승차 방지 시스템 (SFEPS)  
Version: 1.2  
Date: 2026-03-04  
Author: 서형철  
Basis:
- SFEPS SRS v1.2
- SFEPS Test Plan v1.2

---

## 1. 문서 개요
본 문서는 SFEPS 요구사항(SRS)을 기반으로 도출된 Test Condition을 정의한다.  
각 Test Condition은 기능/비기능 요구사항을 검증하기 위한 테스트 목적 단위이며, 상세 Test Case 설계의 기준이 된다.

---

## 2. Test Condition 분류 체계
| 구분 | 설명 |
|---|---|
| Test Level | Unit / Integration / System |
| Test Type | Functional / Non-Functional(Performance, Reliability, Recoverability) |
| Priority | High / Medium / Low |
| Design Technique | EP, BVA, Decision Table, State Transition, Error Guessing, Scenario 등 |
| Trace | 연계 요구사항 ID(REQ-*) |

---

## 3. Functional Test Conditions

### 3.1 TC-FUNC-LOGIN (로그인 기능)
- **Test Level**: System  
- **Test Type**: Functional  
- **Design Technique**: EP  
- **Priority**: High  

| TC-ID | Test Condition Description | Technique | Priority | Trace(REQ) |
|---|---|---:|---:|---|
| TC-FUNC-LOGIN-01 | 유효한 ID/PW 입력 시 로그인 성공 | EP | High | REQ-FUNC-LOGIN-001 |
| TC-FUNC-LOGIN-02 | 존재하지 않는 ID 입력 시 로그인 실패 및 오류 메시지 표시 | EP | Medium | REQ-FUNC-LOGIN-002 |
| TC-FUNC-LOGIN-03 | 잘못된 PW 입력 시 로그인 실패 및 오류 메시지 표시 | EP | Medium | REQ-FUNC-LOGIN-002 |
| TC-FUNC-LOGIN-04 | ID 또는 PW 공백(미입력 포함) 입력 시 로그인 실패 및 안내 | EP | Medium | REQ-FUNC-LOGIN-003 |

---

### 3.2 TC-FUNC-STREAM (영상 스트리밍)
- **Test Level**: Integration  
- **Test Type**: Functional  
- **Design Technique**: State Transition, Error Guessing  
- **Priority**: High  

| TC-ID | Test Condition Description | Technique | Priority | Trace(REQ) |
|---|---|---:|---:|---|
| TC-FUNC-STREAM-01 | Server를 통해 Camera 영상 스트림 수신 및 화면 표시 | State Transition | High | REQ-FUNC-STREAM-001, REQ-ARCH-002 |
| TC-FUNC-STREAM-02 | 네트워크 단절/지연 등으로 스트리밍 불가 시 오류 상태 표시 | Error Guessing | Medium | REQ-FUNC-STREAM-002 |
| TC-FUNC-STREAM-03 | 네트워크 복구 후 스트리밍 자동 재연결/복구 확인 | State Transition | Medium | REQ-FUNC-STREAM-003, REQ-NF-REC-001 |

---

### 3.3 TC-FUNC-EVENT (부정승차 의심 판정 및 이벤트 생성/예외 처리)
- **Test Level**: Unit / Integration(일부)  
- **Test Type**: Functional  
- **Design Technique**: Decision Table, BVA, Error Guessing  
- **Priority**: High  

| TC-ID | Test Condition Description | Technique | Priority | Trace(REQ) |
|---|---|---:|---:|---|
| TC-FUNC-EVENT-01 | 무태그 입력 시 서버 판정로직이 의심으로 판정됨 | Decision Table | High | REQ-FUNC-EVENT-001 |
| TC-FUNC-EVENT-02 | 청소년 우대카드 경계값(19/20) 판정 일관성 검증 | BVA | Medium | REQ-FUNC-EVENT-002 |
| TC-FUNC-EVENT-03 | 노인 우대카드 경계값(59/60) 판정 일관성 검증 | BVA | Medium | REQ-FUNC-EVENT-002 |
| TC-FUNC-EVENT-04 | 의심 판정 시 의심 이벤트가 생성됨 | Decision Table | High | REQ-FUNC-EVENT-003 |
| TC-FUNC-EVENT-05 | 정상 판정 시 의심 이벤트가 생성되지 않음 | Decision Table | High | REQ-FUNC-EVENT-004 |
| TC-FUNC-EVENT-06 | 비정상 이벤트 메시지 포맷 수신 시 크래시/프리징 없이 무시 또는 실패 처리되며 이후 정상 메시지 처리가 가능함 | Error Guessing | Medium | REQ-FUNC-EVENT-005 |

---

### 3.4 TC-FUNC-UI (이벤트 목록/상세 팝업)
- **Test Level**: System  
- **Test Type**: Functional  
- **Design Technique**: Scenario  
- **Priority**: High  

| TC-ID | Test Condition Description | Technique | Priority | Trace(REQ) |
|---|---|---:|---:|---|
| TC-FUNC-UI-01 | 생성된 의심 이벤트가 QT Client에 전달되어 목록에 표시됨 | Scenario | High | REQ-FUNC-UI-001, REQ-ARCH-002 |
| TC-FUNC-UI-02 | 이벤트 선택 시 상세 팝업 표시 및 상세 정보 확인 | Scenario | High | REQ-FUNC-UI-002 |
| TC-FUNC-UI-03 | 로그아웃 버튼 클릭 시 로그아웃 요청 전송 및 로그인 화면 복귀 | Scenario | Medium | REQ-FUNC-UI-003 |

---

### 3.5 TC-FUNC-TRACK (Tracking: 레이저)
- **Test Level**: System  
- **Test Type**: Functional  
- **Design Technique**: State Transition, Scenario  
- **Priority**: High  

| TC-ID | Test Condition Description | Technique | Priority | Trace(REQ) |
|---|---|---:|---:|---|
| TC-FUNC-TRACK-01 | 관리자가 스트리밍 객체 바운딩박스의 Track 버튼 클릭 시 Laser Tracking 수행(ON/OFF) | State Transition | High | REQ-FUNC-TRACK-001, REQ-FUNC-TRACK-002, REQ-ARCH-003 |
| TC-FUNC-TRACK-02 | QT Client에서 Tracking ON/OFF 상태가 UI에 표시됨 | State Transition | Medium | REQ-FUNC-TRACK-003 |

---

### 3.6 TC-FUNC-NET (연결 상태 표시)
- **Test Level**: System / Integration  
- **Test Type**: Functional  
- **Design Technique**: State Transition, Error Guessing  
- **Priority**: Low  

| TC-ID | Test Condition Description | Technique | Priority | Trace(REQ) |
|---|---|---:|---:|---|
| TC-FUNC-NET-01 | 네트워크 단절/복구 과정에서 QT Client가 연결/단절/재연결 상태를 표시함 | State Transition | Low | REQ-FUNC-NET-001, REQ-NF-REC-001 |

---

## 4. Non-Functional Test Conditions

### 4.1 TC-NF-PERF (성능)
- **Test Level**: System / Integration  
- **Test Type**: Non-Functional (Performance)  
- **Priority**: High  

| TC-ID | Test Condition Description | Priority | Trace(REQ) |
|---|---|---:|---|
| TC-NF-PERF-01 | 스트리밍 객체 바운딩박스의 Track 버튼 클릭 → 레이저 ON ACK(또는 처리완료) 확인까지 1초 이내 | High | REQ-NF-PERF-001 |
| TC-NF-PERF-02 | 1분 내 50건 이상 의심 이벤트 처리(생성/전달/표시) | High | REQ-NF-PERF-002 |

---

### 4.2 TC-NF-RELI (신뢰성)
- **Test Level**: System / Integration  
- **Test Type**: Non-Functional (Reliability)  
- **Priority**: High  

| TC-ID | Test Condition Description | Priority | Trace(REQ) |
|---|---|---:|---|
| TC-NF-RELI-01 | Tracking(ON→OFF/종료) 20회 반복 시 오류/상태불일치/비정상 종료 없음 | High | REQ-NF-RELI-001 |
| TC-NF-RELI-02 | 1시간 연속 스트리밍 유지(중단 시 자동 복구 포함 가능) | Low | REQ-NF-RELI-002 |

---

### 4.3 TC-NF-REC (회복성)
- **Test Level**: Integration / System  
- **Test Type**: Non-Functional (Recoverability)  
- **Priority**: Low  

| TC-ID | Test Condition Description | Priority | Trace(REQ) |
|---|---|---:|---|
| TC-NF-REC-01 | 네트워크 단절(5초 초과) 시 자동 로그아웃 후 재로그인으로 정상 복구 | Low | REQ-NF-REC-001, REQ-FUNC-NET-001 |

---

## 5. Scenario Test Conditions (System Level, End-to-End)
- **Test Level**: System  
- **Test Type**: Functional (Scenario)  
- **Priority**: High  

| TC-ID | Test Condition Description | Priority | Trace(REQ) |
|---|---|---:|---|
| TC-SYS-01 | 로그인 → 스트리밍 → 의심 이벤트 수신/표시 → 상세 팝업 확인 → Tracking 수행(레이저) | High | REQ-ARCH-001~003, REQ-FUNC-LOGIN-001~003, REQ-FUNC-STREAM-001~003, REQ-FUNC-EVENT-001~005, REQ-FUNC-UI-001~003, REQ-FUNC-TRACK-001~003, REQ-FUNC-NET-001, REQ-NF-PERF-001 |

