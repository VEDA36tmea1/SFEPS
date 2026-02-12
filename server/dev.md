문제: RFID 소켓이 연결되었다가 곧바로 끊기거나(EOF), 클라이언트에서 "Connection refused"가 반복 발생함

증상 요약
- `smart_server` 로그: 
  - ">> [RFID] 데몬 연결 성공! 데이터 수신 대기 중..." 이후 곧바로 ">> [RFID] 데몬 연결 끊김. 재접속 시도..." 반복
  - `[RFID DEBUG] read() returned 0 (EOF) from socket...` 출력
- 데몬(`rc522_uds_daemon`) 로그: `UDS: client connected (fd=...)` 출력 후 write 에러 또는 연결 종료 발생

원인
- 구현 초기에는 클라이언트에서 `read()`를 타임아웃 기반으로 폴링하였음. 데몬은 카드 태그가 발생할 때만 한 줄의 NDJSON을 전송하고, 평상시에는 전송이 없음.
- 이로 인해 클라이언트가 소켓에서 즉시 EOF(또는 read 반환 0)를 받는 상황이 발생했고, 경우에 따라 연결이 닫히며 재접속이 반복됨.

해결 전략 (요약)
- 소켓을 활성적으로 계속 `read()` 하지 말고, 이벤트가 발생했을 때만 읽도록 변경함.
- 구현 방식: `poll()`을 사용하여 읽기 가능 이벤트(`POLLIN`)가 발생할 때만 `read()`를 호출.

변경사항 요약
- `server/src/rfid_monitor.cpp`
  - 기존: `read()`에 소켓 recv 타임아웃을 설정하고 루프에서 `read()` 호출
  - 변경: `poll()` 사용 (1초 타임아웃) — `POLLIN` 이벤트가 발생하면 `read()` 수행. `POLLHUP`/`POLLERR` 처리 추가.
  - EOF 발생시( `read()` == 0 ) 디버그 로그 추가
- `hardware/.../rc522_uds_daemon.cpp`
  - `accept()`/`write()` 실패 시 `perror()` 출력 추가, 클라이언트 접속 로그 추가

핵심 코드(요약)
```cpp
// poll 대기 예시
struct pollfd pfd;
pfd.fd = sock_fd;
pfd.events = POLLIN;
int ret = poll(&pfd, 1, 1000); // 1초
if (ret > 0 && (pfd.revents & POLLIN)) {
    ssize_t n = read(sock_fd, buffer, sizeof(buffer)-1);
    // n>0 처리, n==0 EOF, n<0 오류 처리
}
```

빌드 & 테스트
1. 데몬(먼저 실행):
```bash
sudo /home/iam/finalProject/SFEPS/hardware/Raspi-driver/RC522_RFID/Server_examples/rc522_uds_daemon --no-daemon --socket /tmp/rc522_events.sock
```
2. 서버(다른 터미널):
```bash
cd /home/iam/finalProject/SFEPS/server/build
sudo ./smart_server
```
3. 데몬/서버 로그 확인: 데몬에서 `UDS write` 또는 `UDS accept` perror 출력이 있는지, 서버에서 `>>> [RFID Tag]` 로그가 카드 태그 후 한 줄만 찍히는지 확인

추가 권장
- 다중 소켓이나 높은 부하 환경이면 `epoll()`로 교체 권장
- C++ 비동기 라이브러리(`boost::asio`) 도입 시 더 깔끔한 이벤트 루프 구현 가능

결론
- 현재 구현(poll 기반)은 데몬이 데이터가 있을 때만 `smart_server`가 읽도록 하여 불필요한 재접속/EOF 문제를 해결합니다.

작성자: 개발팀
일시: 2026-02-12
