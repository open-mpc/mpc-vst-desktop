@echo off
rem Removes "OpenMPC Host.vst3" from the system VST3 folder (asks for administrator rights). Your installed plugins in %APPDATA%\OpenMPC Host are kept.
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
rmdir /S /Q "%CommonProgramFiles%\VST3\OpenMPC Host.vst3"
echo Removed. (Delete %APPDATA%\OpenMPC Host too to remove the plugins you installed.)
pause
