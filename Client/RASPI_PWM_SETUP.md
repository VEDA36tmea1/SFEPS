# Raspberry Pi PWM 연결 가이드

## 네트워크 구조

```
Windows PC (192.168.0.96)
    ↕ 이더넷 (192.168.0.x 서브넷)
라우터 (192.168.0.87) ← SSH 포트포워딩: 2222→22, (5566 미설정)
    ↕ 내부 네트워크 (192.168.50.x 서브넷)
Raspberry Pi (192.168.50.12)
    └─ set_pwm_server.py  :5566 리슨
```

Windows와 Raspberry Pi는 **서로 다른 서브넷**이라 직접 통신 불가.
SSH 터널링으로 우회 접속합니다.

---

## 실행 순서

### 1단계: Raspberry Pi에서 PWM 서버 실행

SSH 접속:
```bash
ssh -p 2222 physical-100@192.168.0.87
```

서버 실행 (백그라운드):
```bash
cd ~/SFEPS/hardware/Raspi-laser
python3 set_pwm_server.py --port 5566 -v &
```

서버 떠있는지 확인:
```bash
ss -tlnp | grep 5566
# 결과: LISTEN 0 ... 0.0.0.0:5566 ... python3
```

### 2단계: Windows에서 SSH 터널 열기

**PowerShell 창 1** (터널 전용, 종료하면 안 됨):
```powershell
ssh -p 2222 -L 15566:localhost:5566 -N physical-100@192.168.0.87
```

> `-L 15566:localhost:5566` = Windows 로컬 15566 포트 → Raspberry Pi 5566 포트로 터널
> `-N` = 명령 실행 없이 포트포워딩만 유지
> 비밀번호 입력 후 커서가 멈추면 정상 (터널 유지 중)

터널 연결 확인 (다른 PowerShell에서):
```powershell
Test-NetConnection -ComputerName 127.0.0.1 -Port 15566
# TcpTestSucceeded : True 이어야 정상
```

### 3단계: Qt 클라이언트 실행

**PowerShell 창 2**:
```powershell
cd C:\Users\2-16\Desktop\SFEPS\client_msvc
.\run_client.ps1
```

`run_client.ps1` 현재 PWM 설정:
```
SFEPS_PWM_HOST = 127.0.0.1   ← SSH 터널 로컬 엔드포인트
SFEPS_PWM_PORT = 15566        ← 터널 로컬 포트
```

---

## 정상 동작 확인

**Qt 로그에서 확인:**
```
[Main] PwmTransmitter mode= "raspi"  host= "127.0.0.1"  port= 15566
[PwmTransmitter] Connecting to Raspberry Pi 127.0.0.1 : 15566
[PwmTransmitter] Connected to Raspberry Pi 127.0.0.1 : 15566
```

**Raspberry Pi 터미널에서 확인:**
```
[set_pwm_server] Qt client connected: ('127.0.0.1', ...)
[set_pwm_server] PAN=1290 TILT=1390
```

---

## 자동화: 터널 백그라운드 유지 (선택)

매번 터널 창을 열기 번거로울 경우 아래 스크립트를 사용합니다.

`start_tunnel.ps1` (client_msvc 폴더에 저장):
```powershell
# SSH 터널을 백그라운드 Job으로 실행
$job = Start-Job -ScriptBlock {
    ssh -p 2222 -L 15566:localhost:5566 -N physical-100@192.168.0.87
}
Write-Host "터널 Job ID: $($job.Id) — 종료하려면: Stop-Job $($job.Id)"
```

또는 **ssh-keygen으로 비밀번호 없이 접속** 설정 후 Task Scheduler에 등록 가능.

---

## 영구 해결: 라우터 포트포워딩 (선택)

라우터 관리 페이지(보통 `http://192.168.0.1`)에서:

| 외부 포트 | 내부 IP | 내부 포트 | 프로토콜 |
|----------|---------|---------|---------|
| 5566 | 192.168.50.12 | 5566 | TCP |

설정 후 `run_client.ps1`을 아래와 같이 변경:
```
SFEPS_PWM_HOST = 192.168.0.87   ← 라우터 IP
SFEPS_PWM_PORT = 5566
```

SSH 터널 불필요.

---

## 포트 충돌 주의

`5566` 포트는 **Cursor IDE가 점유** 중이므로 로컬 터널 포트로 사용 불가.
→ 로컬 터널 포트 `15566` 사용 (Raspberry Pi 서버는 여전히 `5566`).

| 위치 | 포트 | 역할 |
|------|------|------|
| Windows 로컬 | 15566 | SSH 터널 엔드포인트 (Qt 접속 대상) |
| Raspberry Pi | 5566 | set_pwm_server.py 리슨 포트 |
| Cursor IDE | 5566 | 점유 중 (충돌 원인) |
