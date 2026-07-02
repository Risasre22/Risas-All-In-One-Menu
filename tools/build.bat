@echo off
setlocal

set "SRC_DIR=%~dp0"
set "BUILD_DIR=%SRC_DIR%build"

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

echo [INFO] Clearing previous build cache...
if exist "%BUILD_DIR%\CMakeCache.txt" del /Q "%BUILD_DIR%\CMakeCache.txt"
if exist "%BUILD_DIR%\CMakeFiles" rmdir /S /Q "%BUILD_DIR%\CMakeFiles"

echo [INFO] Running CMake configure...
"C:\Program Files\CMake\bin\cmake.exe" ^
    -S "%SRC_DIR%" ^
    -B "%BUILD_DIR%" ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake configure failed.
    exit /b %ERRORLEVEL%
)

echo.
echo [INFO] Configure succeeded. Building Release...

"C:\Program Files\CMake\bin\cmake.exe" ^
    --build "%BUILD_DIR%" ^
    --config Release ^
    --parallel

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed.
    exit /b %ERRORLEVEL%
)

echo.
echo [SUCCESS] Build complete. DLL written to:
echo   %SRC_DIR%..\SKSE\Plugins\1RisaAllInOneMenu.dll

endlocal
