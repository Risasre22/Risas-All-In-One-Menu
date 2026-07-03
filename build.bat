@echo off
setlocal

set "SOURCE_DIR=%~dp0"
set "BUILD_DIR=%SOURCE_DIR%build"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake was not found in PATH.
    exit /b 1
)

if not defined VCPKG_ROOT (
    echo [ERROR] VCPKG_ROOT is not set.
    echo Set it to your vcpkg installation directory and run this script again.
    exit /b 1
)

set "VCPKG_TOOLCHAIN=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
if not exist "%VCPKG_TOOLCHAIN%" (
    echo [ERROR] vcpkg toolchain not found at: %VCPKG_TOOLCHAIN%
    exit /b 1
)

echo [INFO] Configuring Release build...
cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" ^
    -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
if errorlevel 1 exit /b %errorlevel%

echo [INFO] Building Release...
cmake --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 exit /b %errorlevel%

echo [SUCCESS] Build complete.
echo Default output: %BUILD_DIR%\Release\1RisaAllInOneMenu.dll
endlocal
