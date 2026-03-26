@echo off
setlocal

cd /d "%~dp0"

rem VS 2022 Developer 환경 설정 (x64)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64
if errorlevel 1 (
    echo [error] VsDevCmd.bat 실패. VS 2022 Community 설치 여부를 확인해줘.
    exit /b 1
)

rem 사용법:
rem   make_msvc.cmd              -> all (app, camera_client, camera_RBF)
rem   make_msvc.cmd camera_RBF   -> camera_RBF.exe 만 빌드
rem   make_msvc.cmd run-rbf-native            -> 빌드 + 실행
rem   make_msvc.cmd run-rbf-native ARGS="..." -> 빌드 + 인수 전달 실행
make %*

endlocal
