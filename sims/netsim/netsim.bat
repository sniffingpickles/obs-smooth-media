@echo off
REM ==========================================================================
REM  netsim.bat - bad-network test streamer for the Smooth Media Source plugin
REM
REM    netsim.bat <scenario> <destination>
REM
REM      scenario   : clean | slow | veryslow | fast | burst | stall | latency
REM      destination: rtmp | srt | rist        -> local listener (self-contained)
REM                   <full push URL>          -> push to a remote server
REM
REM  Examples:
REM    netsim.bat slow  rtmp://172.238.7.67:1935/publish/test
REM    netsim.bat burst "srt://1.2.3.4:8282?streamid=publish/live/test&mode=caller"
REM    netsim.bat stall rist://1.2.3.4:9001
REM    netsim.bat slow  srt
REM  (QUOTE any push URL that contains '&', or cmd will mangle it.)
REM
REM  Override defaults with env vars, e.g.:  set SIZE=1280x720&& set FPS=30
REM ==========================================================================
setlocal enabledelayedexpansion

set "SCENARIO=%~1"
set "DEST=%~2"

if not defined BIND_IP    set "BIND_IP=0.0.0.0"
if not defined RTMP_PORT  set "RTMP_PORT=1936"
if not defined RTMP_APP   set "RTMP_APP=live"
if not defined RTMP_KEY   set "RTMP_KEY=test"
if not defined SRT_PORT   set "SRT_PORT=9000"
if not defined RIST_PORT  set "RIST_PORT=9001"
if not defined SIZE       set "SIZE=1920x1080"
if not defined FPS        set "FPS=60"
if not defined VB         set "VB=4000k"
if not defined AB         set "AB=160k"
if not defined TONE_HZ    set "TONE_HZ=440"
if not defined STALL_ON   set "STALL_ON=25"
if not defined STALL_OFF  set "STALL_OFF=6"
if not defined SRT_LATENCY set "SRT_LATENCY=120"

where ffmpeg >nul 2>nul || (echo error: ffmpeg not found in PATH. & exit /b 1)

if "%SCENARIO%"=="" goto usage
if "%DEST%"=="" goto usage

REM ── push (URL contains '://') vs listen (bare protocol) ───────────────────
set "MODE=listen"
set "PROTO=%DEST%"
if not "%DEST%"=="%DEST:://=%" (
	set "MODE=push"
	for /f "tokens=1 delims=:" %%a in ("%DEST%") do set "PROTO=%%a"
)

if /i "%PROTO%"=="rtmp" goto okproto
if /i "%PROTO%"=="srt"  goto okproto
if /i "%PROTO%"=="rist" goto okproto
echo unsupported protocol/destination: %DEST%
goto usage
:okproto

REM ── scenario -> input pacing ──────────────────────────────────────────────
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
if not defined KNOWN ( echo unknown scenario: %SCENARIO% & goto usage )

set /a GOP=%FPS%*2

REM ── mux + target url ──────────────────────────────────────────────────────
set "MUX=mpegts"
if /i "%PROTO%"=="rtmp" set "MUX=flv"

if "%MODE%"=="push" (
	set "MUXFLAGS=-f %MUX%"
	set "TARGET_URL=%DEST%"
	set "PLAYHINT=point the source at your server's PLAYBACK url for this stream"
) else (
	if /i "%PROTO%"=="rtmp" (
		set "MUXFLAGS=-f flv -listen 1"
		set "TARGET_URL=rtmp://%BIND_IP%:%RTMP_PORT%/%RTMP_APP%/%RTMP_KEY%"
		set "PLAYHINT=rtmp://<host>:%RTMP_PORT%/%RTMP_APP%/%RTMP_KEY%"
	)
	if /i "%PROTO%"=="srt" (
		set "MUXFLAGS=-f mpegts"
		set "TARGET_URL=srt://%BIND_IP%:%SRT_PORT%?mode=listener&latency=%SRT_LATENCY%"
		set "PLAYHINT=srt://<host>:%SRT_PORT%"
	)
	if /i "%PROTO%"=="rist" (
		set "MUXFLAGS=-f mpegts"
		set "TARGET_URL=rist://@%BIND_IP%:%RIST_PORT%"
		set "PLAYHINT=rist://<host>:%RIST_PORT%"
	)
)

echo ============================================================
echo  netsim: %SCENARIO% / %PROTO% / %MODE%
echo  video : %SIZE%@%FPS%  %VB%   audio: %TONE_HZ%Hz tone %AB%
if "%MODE%"=="push" (
	echo  pushing to: %DEST%
	echo  !PLAYHINT!
) else (
	echo  point the Smooth Media Source at:
	echo      !PLAYHINT!
	echo      ^(host = 127.0.0.1 if same box, else this machine's IP^)
	if /i "%PROTO%"=="srt" echo  SRT receive latency: %SRT_LATENCY% ms
)
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
ffmpeg -hide_banner -loglevel warning %RATE_ARGS% -f lavfi -i "testsrc2=size=%SIZE%:rate=%FPS%" %RATE_ARGS% -f lavfi -i "sine=frequency=%TONE_HZ%:sample_rate=44100" -c:v libx264 -preset veryfast -tune zerolatency -profile:v high -b:v %VB% -maxrate %VB% -bufsize %VB% -g %GOP% -pix_fmt yuv420p -c:a aac -b:a %AB% -ar 44100 -ac 2 %DURATION_ARGS% !MUXFLAGS! "!TARGET_URL!"
exit /b

:usage
echo usage: %~nx0 ^<clean^|slow^|veryslow^|fast^|burst^|stall^|latency^> ^<rtmp^|srt^|rist ^| full-push-url^>
exit /b 1

:end
endlocal
