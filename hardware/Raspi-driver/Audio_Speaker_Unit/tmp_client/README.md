## tmp_client – 오디오 파일(TCP) 전송용 클라이언트

이 디렉터리의 `send_audio` 프로그램은 **PC → 라즈베리 파이** 로  
임의의 바이너리(예: `RAW PCM`, `MP3`)를 **TCP로 그대로 전송**하는 간단한 클라이언트입니다.

- 전송 포맷 변환(디코딩)은 하지 않고, **파일 내용을 그대로** 서버로 보냅니다.
- 서버 쪽(라즈베리 파이)은 이 바이트 스트림을 받아서 **재생(또는 저장/디코딩)** 을 담당해야 합니다.

---

### 1. 빌드 방법

#### 1-1. 공통 (소스 구조)

- 소스: `send_audio.cpp`
- Makefile: `tmp_client/Makefile`
- 출력 바이너리: `send_audio` (Windows에서는 `send_audio.exe`)

#### 1-2. 라즈베리 파이 / 리눅스에서 빌드

필수 패키지:

```bash
sudo apt update
sudo apt install build-essential
```

빌드:

```bash
cd ~/SFEPS/hardware/Raspi-driver/Audio_Speaker_Unit/tmp_client
make
```

성공 시:

- 현재 디렉터리에 `send_audio` 바이너리가 생성됩니다.

#### 1-3. Windows (PC)에서 빌드

전제:

- MinGW-w64 / MSYS2 / Git Bash 등 **g++ 사용 가능한 환경**
- Winsock 라이브러리(`ws2_32`) 링크 가능

빌드:

```bash
cd C:\Users\2-16\Desktop\SFEPS\hardware\Raspi-driver\Audio_Speaker_Unit\tmp_client
make
```

참고:

- `#pragma comment(lib, "ws2_32.lib")` 관련 **warning은 무시해도 됩니다.**
- Makefile 에서 `OS=Windows_NT` 인 경우 자동으로 `-lws2_32` 를 링크합니다.

---

### 2. 프로그램 사용법

기본 형태:

```bash
./send_audio <server_ip> <port> <audio_file>
```

예시:

- 리눅스 / 라즈베리 파이:

```bash
./send_audio 192.168.0.50 6000 test.raw
./send_audio 192.168.0.50 6000 music.mp3
```

- Windows (PowerShell):

```powershell
.\send_audio.exe 192.168.0.50 6000 "Beenzino - Train (Feat. C JAMM).mp3"
```

주의:

- 파일 이름에 공백이 있을 경우 **따옴표(" ")로 감싸야** 합니다.
- 이 프로그램은 파일을 **그대로** 전송하므로, 서버가 해당 포맷(RAW/MP3 등)을 이해해야 합니다.

---

### 3. 서버(라즈베리 파이)에서 테스트하는 방법

#### 3-1. 단순 수신 + 파일 저장 테스트

서버에서 **바이트가 제대로 오는지** 먼저 확인합니다.

라즈베리 파이(서버) 터미널:

```bash
cd ~/SFEPS/hardware/Raspi-driver/Audio_Speaker_Unit/tmp_client
nc -l 6000 > received.bin
```

PC(클라이언트) 터미널:

```bash
cd C:\Users\2-16\Desktop\SFEPS\hardware\Raspi-driver\Audio_Speaker_Unit\tmp_client
.\send_audio.exe 192.168.0.50 6000 "Beenzino - Train (Feat. C JAMM).mp3"
```

전송이 끝나면:

- 라즈베리 파이에 `received.bin` 파일이 생성됩니다.
- `received.bin` 크기가 원본 mp3와 거의 같은지 확인합니다.

#### 3-2. RAW PCM(음성) 즉시 재생 테스트

`Audio_Speaker_Unit` 폴더의 `audio_speaker_test` 를 이용하면  
**RAW PCM(S16_LE, 16kHz, mono)** 를 바로 오디오 잭으로 재생할 수 있습니다.

1) 먼저 `Audio_Speaker_Unit`에서 재생 프로그램 빌드:

```bash
cd ~/SFEPS/hardware/Raspi-driver/Audio_Speaker_Unit
make        # audio_speaker_test 생성 (상위 Makefile 기준)
```

2) 서버에서 **네트워크 수신 → 바로 재생** 파이프라인 구성:

```bash
cd ~/SFEPS/hardware/Raspi-driver/Audio_Speaker_Unit
nc -l 6000 | ./audio_speaker_test
```

3) 클라이언트에서 RAW PCM 파일 전송:

```bash
cd C:\Users\2-16\Desktop\SFEPS\hardware\Raspi-driver\Audio_Speaker_Unit\tmp_client
.\send_audio.exe 192.168.0.50 6000 test.raw
```

→ 라즈베리 파이 오디오 잭(스피커/헤드셋)에서 `test.raw` 내용이 바로 재생됩니다.

#### 3-3. MP3 실시간 재생 테스트 (외부 플레이어 사용)

MP3는 **디코딩이 필요**하므로, 서버에서 `mpg123` 같은 플레이어를 이용합니다.

1) 라즈베리 파이에 `mpg123` 설치:

```bash
sudo apt update
sudo apt install mpg123
```

2) 서버에서 **네트워크 수신 → mpg123로 디코딩/재생**:

```bash
cd ~/SFEPS/hardware/Raspi-driver/Audio_Speaker_Unit/tmp_client
nc -l 6000 | mpg123 -
```

3) 클라이언트에서 mp3 전송:

```powershell
cd C:\Users\2-16\Desktop\SFEPS\hardware\Raspi-driver\Audio_Speaker_Unit\tmp_client
.\send_audio.exe 192.168.0.50 6000 "Beenzino - Train (Feat. C JAMM).mp3"
```

→ 라즈베리 파이 오디오 잭으로 mp3 음악이 재생됩니다.

---

### 4. 트러블슈팅

- **`make` 가 안 될 때 (Windows)**  
  - Git Bash / MSYS2 / WSL 등 g++ 가 있는 쉘에서 실행했는지 확인.
  - `make` 명령이 없으면 MinGW/MSYS2 설치 또는 `choco install make` 등으로 설치.

- **라즈베리 파이에서 소리가 안 날 때**
  - `aplay /usr/share/sounds/alsa/Front_Center.wav` 로 기본 오디오 출력이 되는지 먼저 확인.
  - 볼륨: `alsamixer` 로 확인.

- **네트워크 연결 문제**
  - 서버 IP, 포트(예: 6000)가 일치하는지 확인.
  - 방화벽/라우터 설정으로 포트가 막혀 있지 않은지 확인.

