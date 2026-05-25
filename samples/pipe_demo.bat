@echo off
REM ============================================================================
REM pipe_demo.bat -- End-to-end demo of the pipe data source on Windows.
REM
REM Runs the frame "server" (vtx_sample_pipe_producer) and streams its frames
REM straight into the VTX side (vtx_sample_pipe_consumer) through an anonymous
REM pipe.  The two processes talk purely via CLI arguments + an stdout/stdin
REM pipe -- no files, no sockets:
REM
REM     vtx_sample_pipe_producer N  |  vtx_sample_pipe_consumer - out.vtx schema
REM
REM The producer emits N length-prefixed JSON frames and a zero-size sentinel;
REM the consumer records them into a .vtx replay and finalises on the sentinel,
REM so the pipeline self-terminates and leaves a valid file.
REM
REM Usage:
REM   pipe_demo.bat [num_frames] [output.vtx] [schema.json]
REM
REM   num_frames   default: 200
REM   output.vtx   default: pipe_output.vtx  (in the current directory)
REM   schema.json  default: <samples>/content/writer/arena/arena_schema.json
REM
REM Build the executables first:
REM   cmake --build build --config Release --target vtx_sample_pipe_producer
REM   cmake --build build --config Release --target vtx_sample_pipe_consumer
REM ============================================================================

setlocal

REM --- resolve paths relative to this script so it runs from any directory ---
set "SAMPLES_DIR=%~dp0"
set "BIN_DIR=%SAMPLES_DIR%..\build\bin\Release"

REM --- arguments with defaults ---
set "FRAMES=%~1"
if "%FRAMES%"=="" set "FRAMES=200"

set "OUTPUT=%~2"
if "%OUTPUT%"=="" set "OUTPUT=pipe_output.vtx"

set "SCHEMA=%~3"
if "%SCHEMA%"=="" set "SCHEMA=%SAMPLES_DIR%content\writer\arena\arena_schema.json"

set "PRODUCER=%BIN_DIR%\vtx_sample_pipe_producer.exe"
set "CONSUMER=%BIN_DIR%\vtx_sample_pipe_consumer.exe"

REM --- sanity checks ---
if not exist "%PRODUCER%" (
    echo ERROR: producer not found: %PRODUCER%
    echo Build it:  cmake --build build --config Release --target vtx_sample_pipe_producer
    exit /b 1
)
if not exist "%CONSUMER%" (
    echo ERROR: consumer not found: %CONSUMER%
    echo Build it:  cmake --build build --config Release --target vtx_sample_pipe_consumer
    exit /b 1
)
if not exist "%SCHEMA%" (
    echo ERROR: schema not found: %SCHEMA%
    exit /b 1
)

echo Streaming %FRAMES% frames through a pipe:
echo   producer %FRAMES%  ^|  consumer -^>  %OUTPUT%
echo.

REM --- the pipeline: producer stdout -> consumer stdin ---
"%PRODUCER%" %FRAMES% | "%CONSUMER%" - "%OUTPUT%" "%SCHEMA%"

if errorlevel 1 (
    echo.
    echo Pipeline failed.
    exit /b 1
)

echo.
echo Done -- replay written to %OUTPUT%
exit /b 0
