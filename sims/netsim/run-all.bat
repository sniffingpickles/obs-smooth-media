@echo off
REM  run-all.bat - run every netsim scenario in sequence against one destination.
REM    run-all.bat <rtmp|srt|rist|full-push-url> [seconds-per-scenario]
setlocal
set "DEST=%~1"
set "DUR=%~2"
if "%DUR%"=="" set "DUR=40"
if "%DEST%"=="" ( echo usage: %~nx0 ^<rtmp^|srt^|rist^|push-url^> [secs] & exit /b 1 )
set "HERE=%~dp0"

echo ############################################################
echo # netsim run-all -^> %DEST%   (%DUR%s each)
echo # turn ON Verbose Debug Logging in the source first.
echo ############################################################

for %%s in (clean slow veryslow fast burst stall) do (
	echo.
	echo [%TIME%] ===== SCENARIO: %%s (%DUR%s) =====
	start "" /b cmd /c ""%HERE%netsim.bat" %%s "%DEST%""
	timeout /t %DUR% /nobreak >nul
	taskkill /im ffmpeg.exe /f >nul 2>nul
	timeout /t 2 /nobreak >nul
)
echo [%TIME%] ===== all scenarios complete =====
endlocal
