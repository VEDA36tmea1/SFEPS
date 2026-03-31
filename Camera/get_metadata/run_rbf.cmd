@echo off
rem ============================================================
rem run_rbf.cmd  — camera_RBF.exe 실행 (빌드 없이 바로 실행)
rem
rem 사용법:
rem   run_rbf.cmd                          → GStreamer low-latency 모드
rem   run_rbf.cmd --no-remote-id           → remote ID 없이
rem   run_rbf.cmd --tracker-mode deepsort  → deepsort 모드
rem   run_rbf.cmd ARGS...                  → 기타 인수 그대로 전달
rem
rem   RBF_DUAL_BBOX=0 (아래 set)           → --dual-bbox 끄기 (RAW/EMA 비교 창 없음)
rem   기본 RBF_DUAL_BBOX=1                 → --dual-bbox 자동 추가
rem
rem 경로 커스터마이징 (기본값은 아래 set 줄을 수정):
rem   OPENCV_BIN    : opencv_world*.dll 이 있는 폴더
rem   GST_ROOT      : GStreamer MSVC x86_64 루트
rem ============================================================
setlocal

cd /d "%~dp0"

set "OPENCV_BIN=C:\Users\2-16\Downloads\opencv-gst\install\x64\vc17\bin"
set "GST_ROOT=C:\Program Files\gstreamer\1.0\msvc_x86_64"
set "GST_BIN=%GST_ROOT%\bin"
set "GST_PLUGIN_PATH=%GST_ROOT%\lib\gstreamer-1.0"

rem GStreamer 백엔드 활성화 (USE_GSTREAMER=1 → low-latency 파이프라인 자동 구성)
set "USE_GSTREAMER=1"
set "PATH=%OPENCV_BIN%;%GST_BIN%;%PATH%"

rem RAW vs EMA bbox 동시 표시 (camera_RBF_raw + camera_RBF). 끄려면 0.
set "RBF_DUAL_BBOX=1"

set "EXE=obj-msvc\camera_RBF.exe"

if not exist "%EXE%" (
    echo [error] %EXE% 가 없습니다. 먼저 make_msvc.cmd 로 빌드해주세요.
    exit /b 1
)

if "%RBF_DUAL_BBOX%"=="1" (
    echo [run] %EXE% --tracker-mode native --dual-bbox %*
    "%EXE%" --tracker-mode native --dual-bbox %*
) else (
    echo [run] %EXE% --tracker-mode native %*
    "%EXE%" --tracker-mode native %*
)

endlocal
