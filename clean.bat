@echo off
echo [INFO] Cleaning build artifacts...

if exist "build" (
    echo Deleting build folder...
    rmdir /s /q "build"
)

if exist "dist" (
    echo Deleting dist folder...
    rmdir /s /q "dist"
)

echo [SUCCESS] Clean complete.
pause