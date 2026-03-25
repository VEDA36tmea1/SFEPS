# SFEPS 클라이언트 실행 스크립트 (Windows PowerShell)
# 이 파일에서 환경변수를 수정하세요.
# 실행 방법: client_msvc 폴더 또는 build-msvc 폴더 어디서든 호출 가능
#   cd C:\Users\2-16\Desktop\SFEPS\client_msvc
#   .\run_client.ps1

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
Set-DefaultEnv "POS_SERVER_PORT" "5558"
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
Set-DefaultEnv "AUTH_ALLOW_PLAINTEXT_FALLBACK" "0"
Set-DefaultEnv "AUTH_TLS_CA_FILE" $caPath

# 선택: 통합 TLS 토글(프로젝트의 다른 경로에서 참조 가능)
Set-DefaultEnv "SFEPS_CLIENT_TLS_ENABLE" $env:AUTH_TLS_ENABLE
Set-DefaultEnv "SFEPS_CLIENT_CA_FILE" $env:AUTH_TLS_CA_FILE

if (-not (Test-Path $caPath)) {
    Write-Warning "TLS CA 파일을 찾을 수 없습니다: $caPath"
    Write-Warning "TLS 로그인 사용 시 certs/auth_ca.pem 파일을 배치하세요."
}

# ── 카메라 CGI 밝기/대조 제어 ──────────────────────────────────────────────────
Set-DefaultEnv "CAMERA_CGI_USER" "admin"      # 카메라 로그인 아이디
Set-DefaultEnv "CAMERA_CGI_PASSWORD" "CCgbdCCgbd"      # 카메라 로그인 비밀번호

# 기본값 그대로 사용 시 아래 두 줄은 주석 유지 (192.168.0.84 고정)
# $env:CAMERA_BRIGHTNESS_CGI_URL = "https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Brightness={value}"
# $env:CAMERA_CONTRAST_CGI_URL   = "https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Contrast={value}"

# HTTPS 자체서명 인증서 허용 (카메라 기본 설정)
Set-DefaultEnv "CAMERA_CGI_ALLOW_INSECURE_TLS" "1"

# ── 실행 ───────────────────────────────────────────────────────────────────────
# OpenCV 통합 빌드(build-opencv-on)를 우선 사용, 없으면 기존 build-msvc fallback
$exeCandidates = @(
    (Join-Path $clientDir "build-opencv-on\Release\appHanwhaVisionSFEPS.exe"),
    (Join-Path $clientDir "build-msvc\Release\appHanwhaVisionSFEPS.exe")
)
$exePath = $null
foreach ($cand in $exeCandidates) {
    if (Test-Path $cand) { $exePath = $cand; break }
}

if (-not $exePath) {
    Write-Error "실행파일을 찾을 수 없습니다 (build-opencv-on 또는 build-msvc)."
    Write-Host "OpenCV 빌드 예시:"
    Write-Host "  cmake -S . -B build-opencv-on -G ""Visual Studio 17 2022"" -A x64 -DOpenCV_DIR=""C:/Users/2-16/Desktop/SFEPS/opencv-gst/install"""
    Write-Host "  cmake --build build-opencv-on --config Release"
    Write-Host ""
    Write-Host "기존 빌드 예시:"
    Write-Host "  cmake -S . -B build-msvc -G ""Visual Studio 17 2022"" -A x64 -DSFEPS_WITH_OPENCV=OFF"
    Write-Host "  cmake --build build-msvc --config Release"
    exit 1
}

# OpenCV/GStreamer/Qt 런타임 DLL 경로를 우선 추가
$opencvBinCandidates = @(
    "C:\Users\2-16\Desktop\SFEPS\opencv-gst\install\x64\vc17\bin",
    "C:\Users\2-16\Downloads\opencv-gst\install\x64\vc17\bin"
)
$opencvBin = $null
foreach ($cand in $opencvBinCandidates) {
    if (Test-Path $cand) { $opencvBin = $cand; break }
}
$gstreamerBin = "C:\Program Files\gstreamer\1.0\msvc_x86_64\bin"
$gstreamerPluginDir = "C:\Program Files\gstreamer\1.0\msvc_x86_64\lib\gstreamer-1.0"
$qtBin = "C:\Qt\6.10.0\msvc2022_64\bin"
if ($opencvBin) { $env:Path = "$opencvBin;$env:Path" }
if (Test-Path $gstreamerBin) { $env:Path = "$gstreamerBin;$env:Path" }
if (Test-Path $gstreamerPluginDir) { Set-DefaultEnv "GST_PLUGIN_PATH" $gstreamerPluginDir }
if (Test-Path $qtBin) { $env:Path = "$qtBin;$env:Path" }

Write-Host "[run_client.ps1] exe=$exePath opencvBin=$opencvBin backend=$($env:RTSP_BACKEND) AUTH_TLS_ENABLE=$($env:AUTH_TLS_ENABLE)"
& $exePath
