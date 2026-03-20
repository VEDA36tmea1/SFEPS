# SFEPS

**SFEPS (Subway Fare Evasion Prevention System)** 는 카메라 영상, 메타데이터, RFID, 관제 UI를 결합해
지하철 부정 승차 및 이상 상황을 빠르게 탐지하고 대응하기 위한 통합 관제 프로젝트입니다.

이 저장소는 단일 애플리케이션이 아니라, 다음 요소를 하나의 파이프라인으로 연결한 **통합 시스템 레포지토리**입니다.

- 실시간 영상/메타데이터 수집
- 서버 측 이벤트 판정 및 브로드캐스트
- 관제 클라이언트 모니터링/알림/추적
- 하드웨어 제어(음성, RFID, 레이저)
- 기능/비기능 테스트 자동화

## 프로젝트 한눈에 보기

SFEPS는 현장 센서 데이터가 운영 관제까지 이어지는 흐름을 목표로 합니다.

```text
Camera/Metadata + RFID + Device Inputs
                |
                v
        SFEPS Server (분석/판정/스트리밍)
                |
                v
        SFEPS Client (관제 UI/알림/추적)
                |
                v
      운영 로그/영상 아카이브/테스트 검증
```

## 저장소 구성

아래는 루트 기준 주요 디렉토리와 역할입니다.

- [`client/`](client): Qt/QML 기반 관제 클라이언트
  - 상세: [client/README.md](client/README.md)
- [`server/`](server): 이벤트 판정, 인증/알림/위치/음성/영상 목록 서비스
  - 상세: [server/README.md](server/README.md)
- [`tests/`](tests): pytest + Squish 기반 기능/비기능 테스트
  - 상세: [tests/README.md](tests/README.md)
- [`Camera/`](Camera): 카메라 메타데이터 수집/분석 및 좌표 보정 도구
  - 상세: [Camera/get_metadata/README.md](Camera/get_metadata/README.md), [Camera/calibration/README.md](Camera/calibration/README.md)
- [`hardware/`](hardware): Raspberry Pi 및 STM32 기반 하드웨어 제어 코드
  - 상세: [hardware/Raspi-driver/README.md](hardware/Raspi-driver/README.md), [hardware/Raspi-laser/README.md](hardware/Raspi-laser/README.md), [hardware/stm32-laser/README.md](hardware/stm32-laser/README.md)
- [`mediamtx/`](mediamtx): RTSP 스트리밍 구성/실행 스크립트
- [`docker/`](docker): 컨테이너 기반 실행/운영 보조 자원
- [`docs/`](docs): 인수인계/운영/DB 흐름 문서

## SFEPS가 포함하는 범위

이 프로젝트는 단순 탐지 데모가 아니라, 운영 관제 관점의 end-to-end 범위를 다룹니다.

- 로그인 인증 및 세션 기반 접근 제어
- 실시간 이벤트 알림과 위치 추적 연동
- 영상/메타데이터 기반 판정 파이프라인
- 음성 통신과 현장 장치 연계
- 테스트 코드 기반 기능/안정성/성능 검증

## Jenkins CI/CD

이 저장소는 Jenkins 파이프라인을 통해 서버 빌드/테스트와 배포 자동화를 함께 관리합니다.

- `Jenkinsfile`: 기본 CI/CD 파이프라인
- `Jenkinsfile.nf`: 비기능(Recoverability/Reliability/Performance) 중심 파이프라인
- `develop` 브랜치: CI 통과 후 test 환경 배포
- `main` 브랜치: 승인 단계 후 prod 환경 배포
- 상세 설정/운영 문서: [docs/jenkins_cd.md](docs/jenkins_cd.md), [docs/jenkins_agent.md](docs/jenkins_agent.md)

## 문서 안내

세부 기능 설명, 빌드 방법, 환경변수, 운영 절차는 각 디렉토리의 `README.md`에 정리되어 있습니다.

## 문서 원칙

루트 README는 **프로젝트 개요와 진입 경로**를 안내하고,
빌드/환경변수/프로토콜/운영 절차 같은 기술 상세는 각 디렉토리 README에 분리해 관리합니다.
