@echo off
REM ==========================================================================
REM  netsim.bat - bad-network test streamer for the Smooth Media Source plugin
REM
REM    netsim.bat <rtmp|srt> <clean|slow|veryslow|fast|burst|stall|latency>
REM
REM  Publishes a test pattern (on-screen timer + steady audio tone) as an SRT
REM  or RTMP listener with IRL-style impairments. Point the source at the URL
REM  it prints, then capture the OBS log.
REM
REM  Override defaults with env vars, e.g.:  set SIZE=1280x720&& set FPS=30
REM ==========================================================================
setlocal enabledelayedexpansion

set "PROTO=%~1"
set "SCENARIO=%~2"
if "%SCENARIO%"=="" set "SCENARIO=clean"

if not defined BIND_IP    set "BIND_IP=0.0.0.0"
if not defined RTMP_PORT  set "RTMP_PORT=1936"
if not defined RTMP_APP   set "RTMP_APP=live"
if not defined RTMP_KEY   set "RTMP_KEY=test"
if not defined SRT_PORT   set "SRT_PORT=9000"
if not defined SIZE       set "SIZE=1920x1080"
if not defined FPS        set "FPS=60"
if not defined VB         set "VB=4000k"
if not defined AB         set "AB=160k"
if not defined TONE_HZ    set "TONE_HZ=440"
if not defined STALL_ON   set "STALL_ON=25"
if not defined STALL_OFF  set "STALL_OFF=6"
if not defined SRT_LATENCY set "SRT_LATENCY=120"

where ffmpeg >nul 2>nul || (echo error: ffmpeg not found in PATH. & exit /b 1)

if /i "%PROTO%"=="rtmp" goto okproto
if /i "%PROTO%"=="srt"  goto okproto
echo usage: %~nx0 ^<rtmp^|srt^> ^<clean^|slow^|veryslow^|fast^|burst^|stall^|latency^>
exit /b 1
:okproto

set "RATE_ARGS=-re"
set "DURATION_ARGS="
set "KNOWN="
if /i "%SCENARIO%"=="clean"    ( set "RATE_ARGS=-re" & set "KNOWN=1" )
if /i "%SCENARIO%"=="slow"     ( set "RATE_ARGS=-readrate 0.97" & set "KNOWN=1" )
if /i "%SCENARIO%"=="veryslow" ( set "RATE_ARGS=-readrate 0.93" & set "KNOWN=1" )
if /i "%SCENARIO%"=="fast"     ( set "RATE_ARGS=-readrate 1.03" & set "KNOWN=1" )
if /i "%SCENARIO%"=="burst"    ( set "RATE_ARGS=-re -readrate_initial_burst 4" & set "KNOWN=1" )
if /i "%SCENARIO%"=="stall"    ( set "RATE_ARGS=-re" & set "DURATION_ARGS=-t %STALL_ON%" & set "KNOWN=1" )
if /i "%SCENARIO%"=="latency"  ( set "RATE_ARGS=-re" & set "SRT_LATENCY=2000" & set "KNOWN=1" )
if not defined KNOWN ( echo unknown scenario: %SCENARIO% & exit /b 1 )

set /a GOP=%FPS%*2
set "SRT_URL=srt://%BIND_IP%:%SRT_PORT%?mode=listener&latency=%SRT_LATENCY%"

echo ============================================================
echo  netsim: %PROTO% / %SCENARIO%
echo  video : %SIZE%@%FPS%  %VB%   audio: %TONE_HZ%Hz tone %AB%
echo  point the Smooth Media Source at:
if /i "%PROTO%"=="rtmp" (
	echo      rtmp://^<host^>:%RTMP_PORT%/%RTMP_APP%/%RTMP_KEY%
) else (
	echo      srt://^<host^>:%SRT_PORT%
	echo  SRT receive latency: %SRT_LATENCY% ms
)
echo      ^(host = 127.0.0.1 if same box, else this machine's IP^)
echo  turn ON 'Verbose Debug Logging' in the source's Advanced settings.
echo  Ctrl-C to stop.
echo ============================================================

if /i "%SCENARIO%"=="stall" goto stallloop
call :run
goto :end

:stallloop
call :run
echo --- simulated outage for %STALL_OFF%s ---
timeout /t %STALL_OFF% /nobreak >nul
goto stallloop

:run
if /i "%PROTO%"=="rtmp" (
	ffmpeg -hide_banner -loglevel warning %RATE_ARGS% -f lavfi -i "testsrc2=size=%SIZE%:rate=%FPS%" %RATE_ARGS% -f lavfi -i "sine=frequency=%TONE_HZ%:sample_rate=44100" -c:v libx264 -preset veryfast -tune zerolatency -profile:v high -b:v %VB% -maxrate %VB% -bufsize %VB% -g %GOP% -pix_fmt yuv420p -c:a aac -b:a %AB% -ar 44100 -ac 2 %DURATION_ARGS% -f flv -listen 1 "rtmp://%BIND_IP%:%RTMP_PORT%/%RTMP_APP%/%RTMP_KEY%"
) else (
	ffmpeg -hide_banner -loglevel warning %RATE_ARGS% -f lavfi -i "testsrc2=size=%SIZE%:rate=%FPS%" %RATE_ARGS% -f lavfi -i "sine=frequency=%TONE_HZ%:sample_rate=44100" -c:v libx264 -preset veryfast -tune zerolatency -profile:v high -b:v %VB% -maxrate %VB% -bufsize %VB% -g %GOP% -pix_fmt yuv420p -c:a aac -b:a %AB% -ar 44100 -ac 2 %DURATION_ARGS% -f mpegts "%SRT_URL%"
)
exit /b

:end
endlocal
