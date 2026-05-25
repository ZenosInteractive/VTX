@echo off
REM ============================================================================
REM named_pipe_vtx.bat -- VTX side of the named-pipe demo.
REM
REM Launches the VTX consumer in named-pipe SERVER mode: it creates the pipe
REM \\.\pipe\<name>, blocks until a producer connects, then records every
REM frame it receives into a .vtx replay.
REM
REM This is one of TWO independent processes.  Run this FIRST, in its own
REM terminal.  Then run named_pipe_producer.bat in a SECOND terminal -- the
REM two processes do not share a shell pipeline; they meet over the named
REM pipe.  This mirrors the real use case: VTX waiting, and an external
REM producer (e.g. a game injector) connecting when it is ready.
REM
REM Usage:
REM   named_pipe_vtx.bat [pipe_name] [output.vtx] [schema.json]
REM
REM   pipe_name    default: vtx   -> the pipe is \\.\pipe\vtx
REM   output.vtx   default: pipe_output.vtx  (in the current directory)
REM   schema.json  default: <samples>/content/writer/arena/arena_schema.json
REM ============================================================================

setlocal

set "SAMPLES_DIR=%~dp0"
set "BIN_DIR=%SAMPLES_DIR%..\build\bin\Release"

set "PIPE_NAME=%~1"
if "%PIPE_NAME%"=="" set "PIPE_NAME=vtx"

set "OUTPUT=%~2"
if "%OUTPUT%"=="" set "OUTPUT=pipe_output.vtx"

set "SCHEMA=%~3"
if "%SCHEMA%"=="" set "SCHEMA=%SAMPLES_DIR%content\writer\arena\arena_schema.json"

set "CONSUMER=%BIN_DIR%\vtx_sample_pipe_consumer.exe"

if not exist "%CONSUMER%" (
    echo ERROR: consumer not found: %CONSUMER%
    echo Build it:  cmake --build build --config Release --target vtx_sample_pipe_consumer
    exit /b 1
)
if not exist "%SCHEMA%" (
    echo ERROR: schema not found: %SCHEMA%
    exit /b 1
)

echo VTX side: opening named pipe \\.\pipe\%PIPE_NAME%
echo VTX side: waiting for a producer to connect (this blocks -- it is normal)...
echo.

REM serve:<path> tells the consumer to CREATE the pipe and wait for a producer.
"%CONSUMER%" "serve:\\.\pipe\%PIPE_NAME%" "%OUTPUT%" "%SCHEMA%"
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo VTX side: done -- replay written to %OUTPUT%
) else (
    echo VTX side: consumer exited with code %RC%
)

endlocal & exit /b %RC%
