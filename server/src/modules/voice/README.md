# voice

이 폴더는 오디오 수신 서비스 구현을 담고 있습니다. 클라이언트가 전송한 오디오 바이트를 받아 링 버퍼에 쌓고, 재생 스레드가 이를 소비하는 구조입니다.

## 파일

### `audio_service.cpp`
- Audio TCP/TLS 포트 리스너 구현입니다.
- `AudioRingBuffer`와 `AudioPlayback`을 초기화한 뒤, 수신 바이트를 링 버퍼에 밀어 넣습니다.
- allowlist 검증 후 클라이언트를 수락합니다.
- `SFEPS_AUDIO_MAX_BYTES`를 초과하는 payload는 거부합니다.
- 소켓 read timeout은 `SFEPS_SOCKET_READ_TIMEOUT_MS`를 따릅니다.

## 연결 관계

- `main.cpp`는 `run_audio_receiver()`를 별도 스레드로 실행합니다.
- 실제 오디오 재생/버퍼 구현은 이 폴더 밖의 오디오 공용 컴포넌트(`audio_playback`, `audio_ring_buffer`, `audio_common`)를 사용합니다.

## 읽을 때 참고할 점

- 이 파일은 "네트워크 입력" 담당입니다.
- 오디오 포맷, 재생 장치, 내부 버퍼링 세부 동작은 공용 오디오 유틸 파일 쪽을 함께 봐야 전체가 보입니다.
