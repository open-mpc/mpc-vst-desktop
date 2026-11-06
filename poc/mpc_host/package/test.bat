@echo off
rem Runs the bundled test plugins (armv7 and aarch64 guests) and checks that parameters and state, threads, files and C++ exceptions work on this machine.
setlocal
set "H=%~dp0"
set "MPC_HOST_ROOT=%TEMP%\mpc_host_test_root"
if exist "%MPC_HOST_ROOT%" rmdir /s /q "%MPC_HOST_ROOT%"
mkdir "%MPC_HOST_ROOT%"
set FAIL=0
"%H%mpc_host.exe" --version
for %%A in (armv7 aarch64) do for %%T in (statetest threadtest filetest cxxtest) do call :one %%A %%T
rmdir /s /q "%MPC_HOST_ROOT%" 2>nul
echo.
if %FAIL%==0 (echo ALL 8 TESTS PASSED) else (echo SOME TESTS FAILED - send this window's text to the developer)
pause
exit /b %FAIL%
:one
"%H%mpc_host.exe" "%H%fixtures\%1\%2.so" - 1 "%TEMP%\mpc_host_x" > "%TEMP%\mpc_host_out.txt" 2>&1
findstr /C:"parameters restored" "%TEMP%\mpc_host_out.txt" | findstr /C:"(ok)" >nul
if errorlevel 1 goto bad
"%H%mpc_host.exe" --check "%H%fixtures\%1\%2.so" 120 > "%TEMP%\mpc_host_chk.txt" 2>&1
findstr /R /C:"verdict.:.ok" "%TEMP%\mpc_host_chk.txt" >nul
if errorlevel 1 goto bad
echo   PASS  %1  %2
exit /b 0
:bad
echo   FAIL  %1  %2
type "%TEMP%\mpc_host_out.txt"
type "%TEMP%\mpc_host_chk.txt" 2>nul
set FAIL=1
exit /b 1
