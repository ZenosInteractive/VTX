@echo off
setlocal

echo ==========================================
echo      VTX SDK: Build
echo ==========================================

REM 0. Check for CMake
cmake --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [WARN] CMake not found. Attempting to install via winget...

    REM Try to install CMake using Windows Package Manager
    winget install kitware.cmake --silent --accept-source-agreements --accept-package-agreements

    if %errorlevel% neq 0 (
        echo [INFO] You may need to RESTART this script
        echo        to refresh the PATH environment variables.
        echo        If it does not work, please install CMake
        echo        manually from https://cmake.org/download/
        pause
        exit /b 1
    )

    echo [INFO] Installation started. You may need to RESTART this script
    echo        to refresh the PATH environment variables.
    pause
    exit /b 0
)

REM 1. Clean previous build
if exist "build" rmdir /s /q "build"
if exist "dist" rmdir /s /q "dist"

REM 2. Configure
echo [INFO] Configuring...
cmake -S . -B build -A x64 -DCMAKE_INSTALL_PREFIX="dist" -DBUILD_VTX_TOOL=ON

if %errorlevel% neq 0 pause && exit /b 1

REM 3. Build
echo [INFO] Compiling SDK...
cmake --build build --config Release --parallel

if %errorlevel% neq 0 pause && exit /b 1

REM 4. Install
echo [INFO] Generating dist...
cmake --install build --config Release

echo.
echo [SUCCESS] Build completed successfully.
echo   Libraries: dist\lib\
echo   Binaries:  dist\bin\
echo   Headers:   dist\include\
pause
