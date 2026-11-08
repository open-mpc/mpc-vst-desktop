@echo off
rem Copies "OpenMPC Host.vst3" to the system VST3 folder (asks for administrator rights).
net session >nul 2>&1
if errorlevel 1 (
  echo Asking for administrator rights to write to "%CommonProgramFiles%\VST3" ...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "DST=%CommonProgramFiles%\VST3\OpenMPC Host.vst3"
echo Installing to "%DST%"
xcopy /E /I /Y /Q "%~dp0OpenMPC Host.vst3" "%DST%" >nul
if errorlevel 1 ( echo FAILED to copy. & pause & exit /b 1 )
echo Done. Rescan plugins in your DAW (Ableton Live: Preferences ^> Plug-ins ^> Rescan).
pause
