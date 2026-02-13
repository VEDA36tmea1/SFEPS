# SFEPS Server - 빌드 & 배포 가이드

## 🛠️ 빌드 환경 설정

### 1️⃣ 의존성 설치 (Ubuntu/Debian)

```bash
# 시스템 패키지 업데이트
sudo apt-get update
sudo apt-get upgrade -y

# 필수 도구 설치
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config

# MariaDB 클라이언트 개발 라이브러리
sudo apt-get install -y libmariadb-dev

# FFmpeg 개발 라이브러리
sudo apt-get install -y \
    libavformat-dev \
    libavutil-dev \
    libavcodec-dev

# TinyXML2 라이브러리
sudo apt-get install -y libtinyxml2-dev

# 출력 장치 (음성 재생)
sudo apt-get install -y alsa-utils

# 선택사항: 디버깅 도구
sudo apt-get install -y gdb valgrind
```

### 2️⃣ CMake 버전 확인

```bash
cmake --version
# CMake 3.10.0 이상 권장
```

### 2.1️⃣ 코드 내 설정 (카메라 해상도)

`DBLogger`는 기본적으로 코드에 정의된 해상도를 사용합니다 (기본 4K: 3840x2160). 해상도는 `include/log.h`의 `DBLogger` 멤버 `cam_width`와 `cam_height`로 정의되어 있습니다. 변경하려면 해당 파일의 값을 수정한 후 재빌드합니다.

예: `include/log.h`

```cpp
  // Camera resolution (can be edited in code)
  int cam_width = 3840;   // default 4K width
  int cam_height = 2160;  // default 4K height
```

재빌드 및 재시작 방법:

```bash
cd /home/iam/finalProject/SFEPS/server/build
make -j4
./smart_server
```

---

## 📦 빌드 프로세스

### 단계별 빌드

```bash
# 1. 서버 디렉토리로 이동
cd /home/iam/finalProject/SFEPS/server

# 2. 빌드 디렉토리 생성
mkdir -p build
cd build

# 3. CMake 설정
cmake ..

# 실행 결과 예시:
# -- The C compiler identification is GNU 9.3.0
# -- The CXX compiler identification is GNU 9.3.0
# -- Found MariaDB: /usr/include/mysql (found version "3.1.5")
# -- Configuring done
# -- Generating done
# -- Build files have been written to: /home/iam/finalProject/SFEPS/server/build
```

```bash
# 4. 컴파일
make -j4

# 실행 결과:
# [ 20%] Building CXX object CMakeFiles/smart_server.dir/src/auth.cpp.o
# [ 40%] Building CXX object CMakeFiles/smart_server.dir/src/log.cpp.o
# [ 60%] Building CXX object CMakeFiles/smart_server.dir/src/recorder.cpp.o
# [ 80%] Building CXX object CMakeFiles/smart_server.dir/src/cleanup.cpp.o
# [100%] Building CXX object CMakeFiles/smart_server.dir/src/main.cpp.o
# [100%] Linking CXX executable smart_server
# [100%] Built target smart_server
```

```bash
# 5. 실행 파일 확인
ls -lh smart_server
# -rwxr-xr-x 1 user user 2.3M Feb 13 14:30 smart_server
```

---

## 🔧 빌드 옵션

### 디버그 빌드
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make

# 디버거로 실행
gdb ./smart_server
```

### 릴리즈 빌드 (최적화)
```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4
```

### 병렬 빌드 (속도 향상)
```bash
make -j8  # 8개 코어 사용
```

---

## 🚀 실행

### 기본 실행

```bash
cd /home/iam/finalProject/SFEPS/server/build
./smart_server
```

### 실행 로그 예시

```
========================================
     SFEPS Server Starting Up...
========================================
[System] Initializing modules...
[System] DB Connected. Worker Thread Started.
[Auth] Server listening on port 5555...
[System] Connecting to rtsps://192.168.0.92:8332/cam1 (Secure Mode)...
[System] Connected! Video Stream Index: 0
[Rec] Start: /home/iam/finalProject/SFEPS/videos/rec_20260213_143022.mp4
>> [RFID] 데몬 연결 성공! 데이터 수신 대기 중...
[Audio] Audio server listening on port 5556...
[Alert] Alert server listening on port 5557...

프로그램 실행 중... (Ctrl+C로 종료)
```

### 백그라운드 실행

```bash
# nohup으로 백그라운드 실행
nohup ./smart_server > smart_server.log 2>&1 &

# 프로세스 ID 확인
echo $!

# 로그 모니터링
tail -f smart_server.log

# 프로세스 중단
kill %1
```

### systemd 서비스로 등록

#### 1. 서비스 파일 생성
```bash
sudo nano /etc/systemd/system/sfeps-server.service
```

#### 2. 파일 내용
```ini
[Unit]
Description=SFEPS Smart Fraud Examination Platform Server
After=network.target mysql.service

[Service]
Type=simple
User=pi
WorkingDirectory=/home/iam/finalProject/SFEPS/server/build
ExecStart=/home/iam/finalProject/SFEPS/server/build/smart_server
Restart=always
RestartSec=5
StandardOutput=append:/home/iam/finalProject/SFEPS/server/build/smart_server.log
StandardError=append:/home/iam/finalProject/SFEPS/server/build/smart_server.log

[Install]
WantedBy=multi-user.target
```

#### 3. 서비스 활성화
```bash
sudo systemctl daemon-reload
sudo systemctl enable sfeps-server.service
sudo systemctl start sfeps-server.service

# 상태 확인
sudo systemctl status sfeps-server.service

# 로그 확인
sudo journalctl -u sfeps-server.service -f
```

---

## 🧪 테스트

### 1. 포트 확인

**인증 포트 (5555)**
```bash
netstat -tlnp | grep 5555
# tcp    0    0 0.0.0.0:5555    0.0.0.0:*    LISTEN    12345/smart_server
```

**음성 포트 (5556)**
```bash
netstat -tlnp | grep 5556
```

**알림 포트 (5557)**
```bash
netstat -tlnp | grep 5557
```

### 2. DB 연결 테스트

```bash
# MariaDB 연결 확인
mysql -h 192.168.0.92 -u pi -p -D Client_db

# 테이블 확인
SHOW TABLES;

# 로그 확인
SELECT * FROM login_logs ORDER BY login_at DESC LIMIT 10;
SELECT * FROM system_logs ORDER BY timestamp DESC LIMIT 10;
SELECT * FROM analytics_logs ORDER BY event_time DESC LIMIT 10;
```

### 3. 영상 파일 생성 테스트

```bash
# 영상 저장 디렉토리 확인
ls -lh /home/iam/finalProject/SFEPS/videos/

# 최근 파일 확인
ls -lhrt /home/iam/finalProject/SFEPS/videos/ | tail -5

# 파일 재생 (ffplay)
ffplay /home/iam/finalProject/SFEPS/videos/rec_20260213_143022.mp4
```

### 4. 로드 테스트

```bash
# CPU 모니터링
watch -n 1 'top -bn1 | head -15'

# 메모리 사용량
free -h -s 1

# 디스크 I/O
iostat -x 1

# 네트워크
iftop
```

### 5. 동시 연결 테스트

```bash
# 여러 클라이언트 동시 인증 요청
for i in {1..10}; do
    (python3 -c "
import socket
s = socket.socket()
s.connect(('localhost', 5555))
s.sendall(b'test|pass')
print(s.recv(1024))
s.close()
    ") &
done
wait
```

---

## 🔧 유지보수

### 로그 로테이션

```bash
# logrotate 설정
sudo nano /etc/logrotate.d/sfeps-server
```

```ini
/home/iam/finalProject/SFEPS/server/build/smart_server.log {
    size 100M
    rotate 10
    compress
    missingok
    notifempty
    create 0640 pi pi
}
```

### 정기 정리

```bash
# 일주일 이전 영상 자동 삭제
# (main.cpp의 cleanup_worker에서 관리)

# 수동 정리
find /home/iam/finalProject/SFEPS/videos/ -mtime +7 -name "rec_*.mp4" -delete
```

### DB 유지보수

```sql
-- 정기적 인덱스 최적화
OPTIMIZE TABLE login_logs;
OPTIMIZE TABLE system_logs;
OPTIMIZE TABLE analytics_logs;
OPTIMIZE TABLE recording_logs;

-- 오래된 로그 정기 삭제 (90일 이상)
DELETE FROM login_logs WHERE login_at < DATE_SUB(NOW(), INTERVAL 90 DAY);
DELETE FROM system_logs WHERE timestamp < DATE_SUB(NOW(), INTERVAL 90 DAY);
DELETE FROM analytics_logs WHERE event_time < DATE_SUB(NOW(), INTERVAL 90 DAY);
```

---

## 📊 성능 모니터링

### 1. 메모리 프로파일링

```bash
# valgrind로 메모리 누수 검사
valgrind --leak-check=full --show-leak-kinds=all \
    ./smart_server
```

### 2. CPU 프로파일링

```bash
# perf로 CPU 사용률 분석
perf record -g ./smart_server
perf report
```

### 3. 스레드 모니터링

```bash
# 스레드 개수 모니터링
watch -n 1 "ps -eLf | grep smart_server | wc -l"

# 상세 정보
ps -eLf | grep smart_server
```

---

## 🐛 문제 해결

### 컴파일 에러

#### 에러: "libmariadb.so not found"
```
/usr/bin/ld: cannot find -lmariadb
```

**해결책:**
```bash
sudo apt-get install --reinstall libmariadb-dev
# 또는
pkg-config --cflags --libs mariadb
```

#### 에러: "FFmpeg headers not found"
```
fatal error: libavformat/avformat.h: No such file
```

**해결책:**
```bash
sudo apt-get install --reinstall libavformat-dev libavutil-dev libavcodec-dev
```

### 런타임 에러

#### 에러: "RTSP connection timeout"
```
[Error] Failed to connect! Check IP, Port(8332), or Cert.
```

**진단:**
```bash
# 네트워크 연결 확인
ping 192.168.0.92
telnet 192.168.0.92 8332

# 방화벽 확인
sudo ufw status
sudo ufw allow 8332
```

#### 에러: "Database connection refused"
```
[DB Error] Access denied for user 'pi'@'192.168.0.92'
```

**진단:**
```bash
# DB 연결 테스트
mysql -h 192.168.0.92 -u pi -p

# DB 권한 확인 (MariaDB에서)
SHOW GRANTS FOR 'pi'@'%';
```

#### 에러: "Audio playback failed"
```
[Audio] Failed to start aplay
```

**진단:**
```bash
# ALSA 상태 확인
alsamixer
aplay -l

# 권한 확인
groups pi  # 'audio' 그룹 포함된지 확인
```

---

## 📋 체크리스트

### 배포 전 확인사항

- [ ] 모든 의존성 설치 완료
- [ ] CMake 빌드 성공
- [ ] 포트 설정 (5555, 5556, 5557)
- [ ] DB 연결 설정 확인
  - [ ] Host: 192.168.0.92
  - [ ] User: pi
  - [ ] Password: 올바른지 확인
  - [ ] Database: Client_db 존재
- [ ] RTSP 카메라 연결 테스트
  - [ ] IP: 192.168.0.92
  - [ ] Port: 8332
  - [ ] URL: rtsps://192.168.0.92:8332/cam1
- [ ] 영상 저장 디렉토리 생성
  - [ ] `/home/iam/finalProject/SFEPS/videos` 생성
  - [ ] 읽기/쓰기 권한 확인
- [ ] RFID 소켓 확인
  - [ ] `/tmp/rc522_events.sock` 생성
  - [ ] RC522 드라이버 실행 중
- [ ] 로그 디렉토리 생성
  - [ ] 읽기/쓰기 권한 확인
- [ ] 방화벽 설정
  - [ ] 포트 5555, 5556, 5557 오픈
- [ ] systemd 서비스 등록 (선택)

### 실행 후 확인사항

- [ ] 포트 리스닝 확인
- [ ] 로그 제대로 출력되는지 확인
- [ ] DB에 로그 기록되는지 확인
- [ ] 영상 파일 생성되는지 확인
- [ ] RFID 데이터 수신되는지 확인
- [ ] 메모리 누수 없는지 확인
- [ ] CPU 사용률 정상 범위인지 확인

---

## 📈 성능 지표

### 권장 사양

| 항목 | 요구사항 |
|------|---------|
| CPU | 2코어 이상 (ARMv7 또는 x86_64) |
| 메모리 | 512MB 이상 |
| 디스크 | 640MB/시간 (1080p 60fps 기준) |
| 네트워크 | 10Mbps 이상 (RTSP 스트림) |
| OS | Ubuntu 16.04 LTS 이상, Raspbian 10+ |

### 예상 성능

| 메트릭 | 값 |
|--------|-----|
| 메모리 사용 | ~80MB |
| CPU 사용률 | 15~25% |
| 디스크 I/O | 2~3 MB/s (1080p 기준) |
| DB 삽입 속도 | ~1000 rows/sec |
| 동시 연결 수 | 50+ |

---

## 📞 지원 및 연락처

**문제 발생 시:**

1. 로그 파일 확인: `smart_server.log`
2. 체크리스트 검토
3. 트러블슈팅 섹션 참조
4. 기술 지원팀 연락

---

마지막 업데이트: 2026-02-13
