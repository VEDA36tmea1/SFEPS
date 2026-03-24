# SFEPS Traceability Matrix (요구사항 추적 매트릭스)
Project: 지하철 부정승차 방지 시스템 (SFEPS)  
Version: 1.2  
Date: 2026-03-21  
Author: 서형철  
Basis:
- SFEPS SRS v1.2
- SFEPS Test Condition Specification v1.2
- SFEPS Test Case Specification v1.2

---

## 1. 문서 개요
본 문서는 SFEPS 요구사항 ID와 해당 요구사항을 검증하는 Test Condition, Test Case 간의 추적 관계를 정의한다.  
추적성은 요구사항 누락 여부 점검, 테스트 커버리지 확인, 변경 영향 분석의 기준으로 사용한다.

---

## 2. Traceability Matrix

| Requirement ID | Requirement Summary | Test Condition ID | Test Case ID |
|---|---|---|---|
| REQ-ARCH-001 | 시스템 구성 요소(Camera, Server, QT Client, STM32) 구성 | TC-SYS-01 | TC-SYS-01 |
| REQ-ARCH-002 | Camera → Server → QT Client 영상/이벤트 데이터 흐름 | TC-FUNC-STREAM-01, TC-FUNC-UI-01, TC-SYS-01 | TC-FUNC-STREAM-01, TC-FUNC-UI-01, TC-SYS-01 |
| REQ-ARCH-003 | QT Client → Server → STM32 Tracking 제어 흐름 | TC-FUNC-TRACK-01, TC-NF-PERF-01, TC-SYS-01 | TC-FUNC-TRACK-01, TC-NF-PERF-01, TC-SYS-01 |
| REQ-FUNC-LOGIN-001 | 유효 ID/PW 로그인 | TC-FUNC-LOGIN-01, TC-SYS-01 | TC-FUNC-LOGIN-01, TC-SYS-01 |
| REQ-FUNC-LOGIN-002 | 존재하지 않는 ID 또는 잘못된 PW 로그인 실패 처리 | TC-FUNC-LOGIN-02, TC-FUNC-LOGIN-03 | TC-FUNC-LOGIN-02, TC-FUNC-LOGIN-03 |
| REQ-FUNC-LOGIN-003 | ID/PW 공백 입력 검증 | TC-FUNC-LOGIN-04 | TC-FUNC-LOGIN-04 |
| REQ-FUNC-STREAM-001 | 영상 수신 및 화면 표시 | TC-FUNC-STREAM-01, TC-SYS-01 | TC-FUNC-STREAM-01, TC-SYS-01 |
| REQ-FUNC-STREAM-002 | 스트리밍 오류 상태 표시 | TC-FUNC-STREAM-02 | TC-FUNC-STREAM-02 |
| REQ-FUNC-STREAM-003 | 스트리밍 자동 재연결/복구 | TC-FUNC-STREAM-03 | TC-FUNC-STREAM-03 |
| REQ-FUNC-EVENT-001 | 무태그 기반 의심 판정 | TC-FUNC-EVENT-01, TC-SYS-01 | TC-FUNC-EVENT-01, TC-SYS-01 |
| REQ-FUNC-EVENT-002 | 우대카드 나이 규칙 판정 | TC-FUNC-EVENT-02, TC-FUNC-EVENT-03 | TC-FUNC-EVENT-02, TC-FUNC-EVENT-03 |
| REQ-FUNC-EVENT-003 | 의심 이벤트 생성 | TC-FUNC-EVENT-04, TC-SYS-01 | TC-FUNC-EVENT-04, TC-SYS-01 |
| REQ-FUNC-EVENT-004 | 정상 시 이벤트 미생성 | TC-FUNC-EVENT-05 | TC-FUNC-EVENT-05 |
| REQ-FUNC-EVENT-005 | 비정상 이벤트 메시지 포맷 처리 | TC-FUNC-EVENT-06 | TC-FUNC-EVENT-06 |
| REQ-FUNC-UI-001 | 의심 이벤트 목록 표시 | TC-FUNC-UI-01, TC-SYS-01 | TC-FUNC-UI-01, TC-SYS-01 |
| REQ-FUNC-UI-002 | 이벤트 상세 팝업 표시 및 상세 정보 제공 | TC-FUNC-UI-02, TC-SYS-01 | TC-FUNC-UI-02, TC-SYS-01 |
| REQ-FUNC-UI-003 | 로그아웃 요청 전송 및 로그인 화면 복귀 | TC-FUNC-UI-03 | TC-FUNC-UI-03 |
| REQ-FUNC-TRACK-001 | 스트리밍 객체 바운딩박스의 Track 버튼 기반 수동 실행 | TC-FUNC-TRACK-01, TC-NF-PERF-01, TC-NF-RELI-01, TC-SYS-01 | TC-FUNC-TRACK-01, TC-NF-PERF-01, TC-NF-RELI-01, TC-SYS-01 |
| REQ-FUNC-TRACK-002 | 레이저 Tracking ON/OFF 제어 | TC-FUNC-TRACK-01, TC-NF-PERF-01, TC-NF-RELI-01, TC-SYS-01 | TC-FUNC-TRACK-01, TC-NF-PERF-01, TC-NF-RELI-01, TC-SYS-01 |
| REQ-FUNC-TRACK-003 | Tracking ON/OFF UI 상태 표시 | TC-FUNC-TRACK-02, TC-NF-RELI-01 | TC-FUNC-TRACK-02, TC-NF-RELI-01 |
| REQ-FUNC-NET-001 | 연결/단절/재연결 상태 표시 | TC-FUNC-NET-01, TC-NF-REC-01 | TC-NF-REC-01 |
| REQ-NF-PERF-001 | Track 버튼 클릭부터 레이저 ON ACK까지 1초 이내 | TC-NF-PERF-01 | TC-NF-PERF-01 |
| REQ-NF-PERF-002 | 1분 내 50건 이상 의심 이벤트 처리 | TC-NF-PERF-02 | TC-NF-PERF-02 |
| REQ-NF-RELI-001 | Tracking 20회 반복 안정성 | TC-NF-RELI-01 | TC-NF-RELI-01 |
| REQ-NF-RELI-002 | 1시간 연속 스트리밍 유지 | TC-NF-RELI-02 | TC-NF-RELI-02 |
| REQ-NF-REC-001 | 네트워크 단절 5초 초과 시 자동 로그아웃 후 재로그인 복구 | TC-FUNC-STREAM-03, TC-FUNC-NET-01, TC-NF-REC-01 | TC-FUNC-STREAM-03, TC-NF-REC-01 |

---

## 3. 커버리지 확인
- 모든 REQ-* 항목은 최소 1개 이상의 Test Condition에 매핑되어야 한다.
- 모든 REQ-* 항목은 최소 1개 이상의 Test Case에 매핑되어야 한다.
- Scenario Test인 TC-SYS-01은 주요 아키텍처/기능 흐름의 통합 검증 항목으로 사용한다.

---

## 4. 변경 관리 원칙
- 요구사항 ID가 변경되면 본 매트릭스를 우선 갱신한다.
- Test Condition 또는 Test Case 추가/삭제 시 해당 REQ 행의 매핑을 함께 수정한다.
- 동일 요구사항을 여러 테스트가 검증하는 경우, 대표 테스트와 보조 테스트를 모두 유지한다.