@echo off

REM Run from repo root regardless of where the script was invoked from.
cd /d "%~dp0.."

echo [INFO] Cleaning build artifacts...

if exist "build" (
    echo Deleting build folder...
    rmdir /s /q "build"
)

if exist "build-shared" (
    echo Deleting build-shared folder...
    rmdir /s /q "build-shared"
)

if exist "dist" (
    echo Deleting dist folder...
    rmdir /s /q "dist"
)

echo [SUCCESS] Clean complete.
pause