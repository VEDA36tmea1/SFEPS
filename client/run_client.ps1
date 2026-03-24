# SFEPS 클라이언트 실행 스크립트 (Windows PowerShell)
# 이 파일에서 환경변수를 수정하세요.
# 실행 방법: client 폴더 또는 build-mingw 폴더 어디서든 호출 가능
#   cd C:\Users\2-08\Desktop\SFEPS\client
#   .\run_client.ps1

# 스크립트 위치 기준으로 client 폴더를 찾음 (build-mingw 에서 호출해도 동작)
$clientDir = $PSScriptRoot
if ($clientDir -like '*build-mingw*') {
    $clientDir = Split-Path $clientDir -Parent
}

# ── 서버 연결 ──────────────────────────────────────────────────────────────────
$env:RTSP_STREAM_URL         = "rtsp://192.168.0.101:8554/cam1"
$env:FRAUD_SERVER_HOST       = "192.168.0.101"
$env:FRAUD_SERVER_PORT       = "5557"
$env:POS_SERVER_PORT         = "5558"
$env:AUTH_TLS_ENABLE         = "0"          # TLS 사용 시 1

# ── TLS/CA (로그인 채널) ─────────────────────────────────────────────────────
# TLS 사용 시 AUTH_TLS_ENABLE=1 로 변경하고, CA 파일 경로를 확인하세요.
$caPath = Join-Path $clientDir "certs\auth_ca.pem"
$env:AUTH_TLS_PORT                 = "6555"
$env:AUTH_PLAINTEXT_PORT           = "5555"
$env:AUTH_ALLOW_PLAINTEXT_FALLBACK = "0"
$env:AUTH_TLS_CA_FILE              = $caPath

# 선택: 통합 TLS 토글(프로젝트의 다른 경로에서 참조 가능)
$env:SFEPS_CLIENT_TLS_ENABLE = $env:AUTH_TLS_ENABLE
$env:SFEPS_CLIENT_CA_FILE    = $env:AUTH_TLS_CA_FILE

if (-not (Test-Path $caPath)) {
    Write-Warning "TLS CA 파일을 찾을 수 없습니다: $caPath"
    Write-Warning "TLS 로그인 사용 시 certs/auth_ca.pem 파일을 배치하세요."
}

# ── 카메라 CGI 밝기/대조 제어 ──────────────────────────────────────────────────
$env:CAMERA_CGI_USER         = "admin"      # 카메라 로그인 아이디
$env:CAMERA_CGI_PASSWORD     = "CCgbdCCgbd"      # 카메라 로그인 비밀번호

# 기본값 그대로 사용 시 아래 두 줄은 주석 유지 (192.168.0.84 고정)
# $env:CAMERA_BRIGHTNESS_CGI_URL = "https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Brightness={value}"
# $env:CAMERA_CONTRAST_CGI_URL   = "https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Contrast={value}"

# HTTPS 자체서명 인증서 허용 (카메라 기본 설정)
$env:CAMERA_CGI_ALLOW_INSECURE_TLS = "1"

# ── 실행 ───────────────────────────────────────────────────────────────────────
$exePath = Join-Path $clientDir "build-mingw\appHanwhaVisionSFEPS.exe"
if (-not (Test-Path $exePath)) {
    Write-Error "실행파일을 찾을 수 없습니다: $exePath"
    exit 1
}
Write-Host "[run_client.ps1] AUTH_TLS_ENABLE=$($env:AUTH_TLS_ENABLE) AUTH_TLS_CA_FILE=$($env:AUTH_TLS_CA_FILE)"
& $exePath
