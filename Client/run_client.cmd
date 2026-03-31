@echo off
setlocal
cd /d "%~dp0"
if exist ".\run_client_2.ps1" (
  powershell -NoProfile -ExecutionPolicy Bypass -File ".\run_client_2.ps1"
) else if exist ".\run_client.ps2" (
  rem Legacy: .ps2 확장자는 PowerShell에서 -File로 실행 불가
  echo run_client.ps2 detected but cannot be executed by PowerShell. Use run_client_2.ps1 instead.
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File ".\run_client.ps1"
)
endlocal
