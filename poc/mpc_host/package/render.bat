@echo off
rem render.bat <plugin.so> [seconds]: plays a short test melody through the plugin and writes <plugin>_emulated.wav next to this file; prints CPU use per audio block.
setlocal
set "H=%~dp0"
if "%~1"=="" (echo usage: render.bat plugin.so [seconds] & pause & exit /b 2)
if not defined MPC_HOST_ROOT set "MPC_HOST_ROOT=%H%data"
set "SECS=%~2"
if "%SECS%"=="" set SECS=5
"%H%mpc_host.exe" "%~1" - %SECS% "%H%%~n1"
echo.
echo wrote "%H%%~n1_emulated.wav"
pause
