# SFEPS 클라이언트 실행 스크립트 (Windows PowerShell)
# NOTE: 이 파일은 기존 run_client.ps2(커스텀) 내용을 그대로 ps1 확장자로 복사한 것입니다.

# 스크립트 위치 기준으로 client_msvc 폴더를 찾음 (build-msvc 에서 호출해도 동작)
$clientDir = $PSScriptRoot
if ($clientDir -like '*build-msvc*') {
    $clientDir = Split-Path $clientDir -Parent
}

function Set-DefaultEnv([string]$name, [string]$value) {
    if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
        [Environment]::SetEnvironmentVariable($name, $value, "Process")
    }
}

# ── 서버 연결 ──────────────────────────────────────────────────────────────────
Set-DefaultEnv "RTSP_STREAM_URL" "rtsp://192.168.0.84/profile2/media.smp"
# metadata 수신 RTSP (객체 박스용): 기본은 영상 URL과 동일
Set-DefaultEnv "METADATA_RTSP_URL" $env:RTSP_STREAM_URL
# 카메라가 trackID=v/m 이 아니면 0/1 등으로 지정
Set-DefaultEnv "METADATA_VIDEO_TRACK_ID" "v"
Set-DefaultEnv "METADATA_META_TRACK_ID" "m"
Set-DefaultEnv "FRAUD_SERVER_HOST" "192.168.0.101"
Set-DefaultEnv "FRAUD_SERVER_PORT" "5557"
# Auth 서버 호스트 (AuthManager 기본값이 192.168.0.82라서 로그가 82로 보일 수 있음)
[Environment]::SetEnvironmentVariable("AUTH_SERVER_HOST", "192.168.0.101", "Process")
Set-DefaultEnv "POS_SERVER_PORT" "5558"
# 비디오 아카이브 카탈로그 서버 (VideoArchiveManager - ArchiveView의 녹화 목록/재생)
Set-DefaultEnv "VIDEO_CATALOG_HOST"       "192.168.0.101"
Set-DefaultEnv "SFEPS_VIDEO_CATALOG_PORT" "5559"
Set-DefaultEnv "AUTH_TLS_ENABLE" "0"          # TLS 사용 시 1

# ── 저지연 스트리밍 옵션(직접 RTSP) ────────────────────────────────────────────
# 1이면 로그인/서버 연결 없이 메인 화면에서 RTSP 직접 재생
Set-DefaultEnv "SFEPS_DIRECT_STREAM_MODE" "0"
# 1이면 ONVIF 메타데이터(XMLParser)로 Human bbox를 직접 파싱해 오버레이
Set-DefaultEnv "SFEPS_USE_ONVIF_METADATA" "1"
# ffmpeg | gstreamer
Set-DefaultEnv "RTSP_BACKEND" "gstreamer"
# 목표 표시 FPS (worker emit 간격)
Set-DefaultEnv "RTSP_TARGET_FPS" "30"
# grab 후 추가로 버릴 프레임 수 (live-edge 유지)
Set-DefaultEnv "RTSP_DROP_GRABS" "3"
# ffmpeg 저지연 옵션 (필요 시 조정)
Set-DefaultEnv "RTSP_FFMPEG_OPTIONS" "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|max_delay;0|probesize;32768|analyzeduration;0|reorder_queue_size;0"
# OpenCV CAP_GSTREAMER 전용 파이프라인 (비어있으면 FFmpeg fallback)
if ([string]::IsNullOrWhiteSpace($env:SFEPS_GSTREAMER_PIPELINE) -and $env:RTSP_BACKEND -eq "gstreamer") {
    $env:SFEPS_GSTREAMER_PIPELINE = "rtspsrc location=$($env:RTSP_STREAM_URL) latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! appsink drop=true max-buffers=1 sync=false"
}

# ── TLS/CA (로그인 채널) ─────────────────────────────────────────────────────
# TLS 사용 시 AUTH_TLS_ENABLE=1 로 변경하고, CA 파일 경로를 확인하세요.
$caPath = Join-Path $clientDir "certs\auth_ca.pem"
Set-DefaultEnv "AUTH_TLS_PORT" "6555"
Set-DefaultEnv "AUTH_PLAINTEXT_PORT" "5555"
[Environment]::SetEnvironmentVariable("AUTH_ALLOW_PLAINTEXT_FALLBACK", "1", "Process")
[Environment]::SetEnvironmentVariable("AUTH_TLS_CA_FILE", $caPath, "Process")

# 선택: 통합 TLS 토글(프로젝트의 다른 경로에서 참조 가능)
[Environment]::SetEnvironmentVariable("SFEPS_CLIENT_TLS_ENABLE", $env:AUTH_TLS_ENABLE, "Process")
[Environment]::SetEnvironmentVariable("SFEPS_CLIENT_CA_FILE", $caPath, "Process")
[Environment]::SetEnvironmentVariable("SFEPS_ALERT_TLS_ENABLE", $env:AUTH_TLS_ENABLE, "Process")

# 기타 서비스(Voice/Alert/Position/VideoCatalog) TLS 기본 토글 동기화
[Environment]::SetEnvironmentVariable("SFEPS_POS_TLS_ENABLE", "$($env:AUTH_TLS_ENABLE)", "Process")
[Environment]::SetEnvironmentVariable("SFEPS_VIDEO_CATALOG_TLS_ENABLE", $env:AUTH_TLS_ENABLE, "Process")
Set-DefaultEnv "QT_FFMPEG_PROTOCOL_WHITELIST" "file,crypto,data,http,https,tcp,tls,rtp,rtsp,udp"

if (-not (Test-Path $caPath)) {
    Write-Warning "TLS CA 파일을 찾을 수 없습니다: $caPath"
    Write-Warning "TLS 로그인 사용 시 certs/auth_ca.pem 파일을 배치하세요."
}

# ── PWM 전송 모드 (camera_RBF --qt-mode 연동) ──────────────────────────────────
# PWM 설정은 항상 강제 적용 (Set-DefaultEnv는 이미 설정된 값을 덮어쓰지 않으므로 직접 설정)
[Environment]::SetEnvironmentVariable("SFEPS_PWM_MODE", "raspi",        "Process")  # raspi | stm
[Environment]::SetEnvironmentVariable("SFEPS_PWM_HOST", "127.0.0.1",    "Process")  # SSH 터널: ssh -p 2222 -L 15566:localhost:5566 -N physical-100@192.168.0.87
[Environment]::SetEnvironmentVariable("SFEPS_PWM_PORT", "15566",        "Process")  # 로컬 터널 포트 (Cursor가 5566 점유 중)

# ── 카메라 CGI 밝기/대조 제어 ──────────────────────────────────────────────────
# 기존 프로세스/시스템 환경변수 값이 남아 있어도 항상 의도한 계정으로 덮어씀
[Environment]::SetEnvironmentVariable("CAMERA_CGI_USER", "admin", "Process")          # 카메라 로그인 아이디
[Environment]::SetEnvironmentVariable("CAMERA_CGI_PASSWORD", "CCgbdCCgbd", "Process") # 카메라 로그인 비밀번호
[Environment]::SetEnvironmentVariable("CAMERA_CGI_ALLOW_INSECURE_TLS", "1", "Process")

# ── 실행 ───────────────────────────────────────────────────────────────────────
$exeCandidates = @(
    (Join-Path $clientDir "build-msvc\Release\appHanwhaVisionSFEPS.exe"),
    (Join-Path $clientDir "build-opencv-on-msvc\Release\appHanwhaVisionSFEPS.exe"),
    (Join-Path $clientDir "build-opencv-on\Release\appHanwhaVisionSFEPS.exe")
)
$exePath = $null
foreach ($cand in $exeCandidates) {
    if (Test-Path $cand) { $exePath = $cand; break }
}

if (-not $exePath) {
    Write-Error "실행파일을 찾을 수 없습니다 (build-opencv-on 또는 build-msvc)."
    exit 1
}

# --- 런타임 DLL 경로 직접 지정 (2-08 사용자 환경) ---
$qtBin = "C:\\Qt\\6.10.2\\msvc2022_64\\bin"
$opencvBin = "C:\\Users\\2-08\\Desktop\\SFEPS\\opencv-gst\\opencv-gst\\install\\x64\\vc17\\bin"
$gstreamerBin = "C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64\\bin"
$gstreamerPluginDir = "C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64\\lib\\gstreamer-1.0"

# Qt를 최우선으로 두어 Qt Multimedia가 Qt 번들 FFmpeg DLL을 먼저 로드하도록 보장
# (OpenCV 번들 FFmpeg가 먼저 잡히면 HTTP 프로토콜 미지원 이슈가 발생할 수 있음)
$runtimePrefix = @()
if (Test-Path $qtBin) { $runtimePrefix += $qtBin }
if (Test-Path $gstreamerBin) { $runtimePrefix += $gstreamerBin }
if (Test-Path $opencvBin) { $runtimePrefix += $opencvBin }
if ($runtimePrefix.Count -gt 0) {
    $env:Path = (($runtimePrefix -join ";") + ";" + $env:Path)
}
if (Test-Path $gstreamerPluginDir) { Set-DefaultEnv "GST_PLUGIN_PATH" $gstreamerPluginDir }

Write-Host "[run_client_2.ps1] exe=$exePath opencvBin=$opencvBin backend=$($env:RTSP_BACKEND) AUTH_TLS_ENABLE=$($env:AUTH_TLS_ENABLE) SFEPS_CLIENT_TLS_ENABLE=$($env:SFEPS_CLIENT_TLS_ENABLE) SFEPS_POS_TLS_ENABLE=$($env:SFEPS_POS_TLS_ENABLE) SFEPS_ALERT_TLS_ENABLE=$($env:SFEPS_ALERT_TLS_ENABLE)"
& $exePath

