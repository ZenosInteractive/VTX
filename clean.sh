#!/bin/bash

echo "[INFO] Cleaning build artifacts..."

if [ -d "build" ]; then
    echo "Deleting build folder..."
    rm -rf "build"
fi

if [ -d "build-shared" ]; then
    echo "Deleting build-shared folder..."
    rm -rf "build-shared"
fi

if [ -d "dist" ]; then
    echo "Deleting dist folder..."
    rm -rf "dist"
fi

echo "[SUCCESS] Clean complete."
read -p "Press [Enter] to continue..."
