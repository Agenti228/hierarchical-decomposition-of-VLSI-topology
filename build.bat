@echo off
setlocal enabledelayedexpansion

:: Переходим в папку со скриптом
pushd "%~dp0" || exit /b 1

set "BUILD_DIR=build"

:: 1. Очистка
if exist "%BUILD_DIR%" (
    echo [1/3] Cleaning build directory...
    rd /s /q "%BUILD_DIR%" 2>nul
    if exist "%BUILD_DIR%" (
        timeout /t 2 /nobreak >nul
        rd /s /q "%BUILD_DIR%" 2>nul
    )
)

:: Конфигурация
echo [2/3] Configuring CMake...
cmake -S . -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :error

:: Сборка
echo [3/3] Building...
cmake --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 goto :error

echo Build successful!
echo Executable: %BUILD_DIR%\Release\vlsi_topology.exe
pause
popd
exit /b 0

:error
echo Build failed! Check CMake output above.
pause
popd
exit /b 1