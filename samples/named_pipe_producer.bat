@echo off
REM ============================================================================
REM named_pipe_producer.bat -- Producer side of the named-pipe demo.
REM
REM This is the SECOND of two independent processes.  It connects to the
REM named pipe \\.\pipe\<name> and streams frames into it.
REM
REM By default it streams CONTINUOUSLY: frames keep flowing until you press
REM Enter in this window.  Pressing Enter stops the producer cleanly, which
REM closes the pipe -- the VTX side then finalises the .vtx replay.
REM
REM Handshake: the producer simply retries the connection until it succeeds.
REM A redirect to a pipe that does not exist yet fails, so the retry loop IS
REM the "wait until VTX is ready" handshake.  This is how a real named-pipe
REM client (e.g. a game injector) waits for its server.
REM
REM The producer does NOT share a shell pipeline with VTX: it opens
REM \\.\pipe\<name> as a client, on its own.
REM
REM Run named_pipe_vtx.bat FIRST (other terminal), then run this one.
REM
REM Usage:
REM   named_pipe_producer.bat [pipe_name] [num_frames]
REM
REM   pipe_name    default: vtx   -> the pipe is \\.\pipe\vtx
REM   num_frames   default: 0     -> stream continuously until you press Enter.
REM                A value > 0 sends exactly that many frames, then stops.
REM ============================================================================

setlocal EnableDelayedExpansion

set "SAMPLES_DIR=%~dp0"
set "BIN_DIR=%SAMPLES_DIR%..\build\bin\Release"

set "PIPE_NAME=%~1"
if "%PIPE_NAME%"=="" set "PIPE_NAME=vtx"

set "FRAMES=%~2"
if "%FRAMES%"=="" set "FRAMES=0"

set "PRODUCER=%BIN_DIR%\vtx_sample_pipe_producer.exe"
set "PIPE=\\.\pipe\%PIPE_NAME%"

if not exist "%PRODUCER%" (
    echo ERROR: producer not found: %PRODUCER%
    echo Build it:  cmake --build build --config Release --target vtx_sample_pipe_producer
    exit /b 1
)

REM --- handshake: retry the connection until the VTX side has opened the pipe.
REM     A redirect to a not-yet-existing pipe fails (errorlevel 1); once VTX
REM     has created it the redirect succeeds and the producer streams.
echo Producer: connecting to %PIPE% -- retrying until VTX is up...
if "%FRAMES%"=="0" (
    echo Producer: once connected, frames stream until you press Enter here.
) else (
    echo Producer: will send %FRAMES% frames once connected.
)
set /a tries=0

:try_connect
"%PRODUCER%" %FRAMES% > %PIPE%
if not errorlevel 1 goto sent

set /a tries+=1
if !tries! geq 30 (
    echo.
    echo ERROR: could not connect to %PIPE% after 30 attempts.
    echo Is named_pipe_vtx.bat running in another terminal?
    exit /b 1
)
<nul set /p "=."
REM portable ~1s sleep (works even without an interactive console)
ping -n 2 127.0.0.1 >nul
goto try_connect

:sent
echo.
echo Producer: done -- stream closed, VTX is finalising the replay.
endlocal & exit /b 0
