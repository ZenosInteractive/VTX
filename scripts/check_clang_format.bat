@echo off
REM Forwards to the Python implementation.
REM Usage:
REM   scripts\check_clang_format.bat                 -- check vs origin/main
REM   scripts\check_clang_format.bat --fix           -- apply formatting fix
REM   scripts\check_clang_format.bat --base HEAD~1   -- check vs a different ref
setlocal
set "SCRIPT_DIR=%~dp0"
where python >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    python "%SCRIPT_DIR%check_clang_format.py" %*
    exit /b %ERRORLEVEL%
)
where python3 >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    python3 "%SCRIPT_DIR%check_clang_format.py" %*
    exit /b %ERRORLEVEL%
)
echo [ERROR] Python not found in PATH.
exit /b 2
