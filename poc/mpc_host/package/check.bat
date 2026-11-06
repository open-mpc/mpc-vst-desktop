@echo off
rem check.bat <plugin.so>   (or drag a plugin onto this file): asks whether the host should use the plugin, and why not if it should not.
setlocal
set "H=%~dp0"
if "%~1"=="" (echo usage: check.bat plugin.so & pause & exit /b 2)
if not defined MPC_HOST_ROOT set "MPC_HOST_ROOT=%H%data"
"%H%mpc_host.exe" --check "%~1" 180
echo.
echo exit code %ERRORLEVEL%   (0 = ok, 3 = degraded: usable with a warning, 4 = blocked: do not use)
pause
