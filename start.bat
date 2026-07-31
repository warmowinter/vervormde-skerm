@echo off
cd /d "%~dp0"
if not exist "node_modules\electron\dist\electron.exe" (
  echo Electron runtime is missing. Run: npm.cmd install
  pause
  exit /b 1
)
start "" "node_modules\electron\dist\electron.exe" "."
