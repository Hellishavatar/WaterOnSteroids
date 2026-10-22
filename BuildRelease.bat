@echo off
setlocal enabledelayedexpansion
rem Water On Steroids release build and package script.

cd /d "%~dp0"

set "BUILD_DIR=build"
set "CONFIG=Release"
set "EXPECTED_GENERATOR=Visual Studio 17 2022"

set "PACKAGE_DIR=dist\WaterOnSteroids"
set "PACKAGE_FILE="

if /I "%~1"=="clean" (
    echo [WOS] Cleaning %BUILD_DIR% ...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    shift
)

rem Prefer the CMake bundled with Visual Studio.
set "CMAKE_EXE="
set "CMAKE_VERSION="
set "VS_INSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "!VSWHERE!" (
    for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath 2^>nul`) do (
        if not defined VS_INSTALL set "VS_INSTALL=%%i"
    )

    rem Fall back to the latest Visual Studio installation.
    if not defined VS_INSTALL (
        for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -property installationPath 2^>nul`) do (
            if not defined VS_INSTALL set "VS_INSTALL=%%i"
        )
    )
)

if defined VS_INSTALL (
    set "VS_CMAKE=!VS_INSTALL!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if exist "!VS_CMAKE!" set "CMAKE_EXE=!VS_CMAKE!"
)

rem Check standard Visual Studio installation paths.
for %%y in (2022 18) do (
    for %%e in (Community Professional Enterprise BuildTools) do (
        if not defined CMAKE_EXE (
            set "VS_CANDIDATE=%ProgramFiles%\Microsoft Visual Studio\%%y\%%e"
            if exist "!VS_CANDIDATE!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
                set "VS_INSTALL=!VS_CANDIDATE!"
                set "CMAKE_EXE=!VS_CANDIDATE!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            )
        )
    )
)

rem Last fallback: first CMake found in PATH.
if not defined CMAKE_EXE (
    for /f "delims=" %%i in ('where cmake 2^>nul') do (
        if not defined CMAKE_EXE set "CMAKE_EXE=%%i"
    )
)

if not defined CMAKE_EXE (
    echo [ERROR] CMake was not found in Visual Studio or PATH.
    exit /b 1
)

rem Read the quoted CMake path through a temporary file to avoid FOR /F parsing issues.
set "CMAKE_VERSION_FILE=%TEMP%\WOS-cmake-version-!RANDOM!-!RANDOM!.txt"
"!CMAKE_EXE!" --version > "!CMAKE_VERSION_FILE!" 2>nul
if errorlevel 1 (
    if exist "!CMAKE_VERSION_FILE!" del /q "!CMAKE_VERSION_FILE!" >nul 2>nul
    echo [ERROR] Unable to run: !CMAKE_EXE!
    exit /b 1
)

for /f "usebackq tokens=1,2,3" %%a in ("!CMAKE_VERSION_FILE!") do (
    if /I "%%a %%b"=="cmake version" if not defined CMAKE_VERSION set "CMAKE_VERSION=%%c"
)
del /q "!CMAKE_VERSION_FILE!" >nul 2>nul

if not defined CMAKE_VERSION (
    echo [ERROR] Unable to determine the version of: !CMAKE_EXE!
    exit /b 1
)

set "CMAKE_MAJOR="
set "CMAKE_MINOR="
for /f "tokens=1,2 delims=." %%a in ("!CMAKE_VERSION!") do (
    set /a CMAKE_MAJOR=%%a
    set /a CMAKE_MINOR=%%b
)

set "CMAKE_TOO_OLD="
if !CMAKE_MAJOR! LSS 3 set "CMAKE_TOO_OLD=1"
if !CMAKE_MAJOR! EQU 3 if !CMAKE_MINOR! LSS 21 set "CMAKE_TOO_OLD=1"
if defined CMAKE_TOO_OLD (
    echo [ERROR] CMake !CMAKE_VERSION! is too old. CMake 3.21 or newer is required.
    echo          Selected executable: !CMAKE_EXE!
    exit /b 1
)

echo [WOS] CMake = !CMAKE_EXE!
echo [WOS] CMake version = !CMAKE_VERSION!

where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Git was not found in PATH. It is required to populate extern\.
    exit /b 1
)

rem Initialize the Visual Studio x64 developer environment.
if defined VS_INSTALL (
    set "VSDEVCMD=!VS_INSTALL!\Common7\Tools\VsDevCmd.bat"
    if exist "!VSDEVCMD!" (
        call "!VSDEVCMD!" -no_logo -arch=x64 -host_arch=x64 >nul
        if errorlevel 1 (
            echo [ERROR] Failed to initialize the Visual Studio environment:
            echo          !VSDEVCMD!
            exit /b 1
        )
    )
)

rem Prefer the shared CommonLib vcpkg, then Visual Studio, then PATH.
set "SELECTED_VCPKG_ROOT="
if exist "C:\ClibDT\vcpkg\scripts\buildsystems\vcpkg.cmake" (
    set "SELECTED_VCPKG_ROOT=C:\ClibDT\vcpkg"
)
if not defined SELECTED_VCPKG_ROOT if defined VS_INSTALL (
    if exist "!VS_INSTALL!\VC\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "SELECTED_VCPKG_ROOT=!VS_INSTALL!\VC\vcpkg"
    )
)

if not defined SELECTED_VCPKG_ROOT (
    set "VCPKG_EXE="
    for /f "delims=" %%i in ('where vcpkg 2^>nul') do if not defined VCPKG_EXE set "VCPKG_EXE=%%i"
    if defined VCPKG_EXE (
        for %%i in ("!VCPKG_EXE!") do set "SELECTED_VCPKG_ROOT=%%~dpi"
        set "SELECTED_VCPKG_ROOT=!SELECTED_VCPKG_ROOT:~0,-1!"
    )
)
if not defined SELECTED_VCPKG_ROOT (
    echo [ERROR] vcpkg was not found in C:\ClibDT, Visual Studio, or PATH.
    exit /b 1
)

set "VCPKG_ROOT=!SELECTED_VCPKG_ROOT!"
if defined VS_INSTALL set "VCPKG_VISUAL_STUDIO_PATH=!VS_INSTALL!"

set "TOOLCHAIN=!VCPKG_ROOT!\scripts\buildsystems\vcpkg.cmake"
if not exist "!TOOLCHAIN!" (
    echo [ERROR] vcpkg toolchain not found: !TOOLCHAIN!
    exit /b 1
)

set "TOOLCHAIN_CMAKE=!TOOLCHAIN:\=/!"
echo [WOS] Visual Studio = !VS_INSTALL!
echo [WOS] VCPKG_ROOT = !VCPKG_ROOT!
echo [WOS] VCPKG_VISUAL_STUDIO_PATH = !VCPKG_VISUAL_STUDIO_PATH!

rem Recreate the build tree when generator, configuration, or toolchain changes.
if exist "%BUILD_DIR%\CMakeCache.txt" (
    set "CACHED_GENERATOR="
    set "CACHED_CONFIGS="
    set "CACHED_TOOLCHAIN="
    for /f "tokens=2 delims==" %%g in ('findstr /B /C:"CMAKE_GENERATOR:INTERNAL=" "%BUILD_DIR%\CMakeCache.txt" 2^>nul') do set "CACHED_GENERATOR=%%g"
    for /f "tokens=2 delims==" %%g in ('findstr /B /C:"CMAKE_CONFIGURATION_TYPES:STRING=" "%BUILD_DIR%\CMakeCache.txt" 2^>nul') do set "CACHED_CONFIGS=%%g"
    for /f "tokens=2 delims==" %%g in ('findstr /B /C:"CMAKE_TOOLCHAIN_FILE:" "%BUILD_DIR%\CMakeCache.txt" 2^>nul') do set "CACHED_TOOLCHAIN=%%g"
    if defined CACHED_TOOLCHAIN set "CACHED_TOOLCHAIN=!CACHED_TOOLCHAIN:\=/!"

    set "RECREATE_BUILD="
    if defined CACHED_GENERATOR if /I not "!CACHED_GENERATOR!"=="%EXPECTED_GENERATOR%" set "RECREATE_BUILD=1"
    if defined CACHED_CONFIGS if /I not "!CACHED_CONFIGS!"=="Release" set "RECREATE_BUILD=1"
    if defined CACHED_TOOLCHAIN if /I not "!CACHED_TOOLCHAIN!"=="!TOOLCHAIN_CMAKE!" set "RECREATE_BUILD=1"

    if defined RECREATE_BUILD (
        echo [WOS] The cache does not match the current Release preset.
        echo [WOS] Cached generator: !CACHED_GENERATOR!
        echo [WOS] Cached configurations: !CACHED_CONFIGS!
        echo [WOS] Cached toolchain: !CACHED_TOOLCHAIN!
        echo [WOS] Required toolchain: !TOOLCHAIN_CMAKE!
        echo [WOS] Recreating %BUILD_DIR%...
        rmdir /s /q "%BUILD_DIR%"
    )
)

rem Remove the legacy build\extern layout.
if exist "%BUILD_DIR%\extern" (
    echo [WOS] Legacy %BUILD_DIR%\extern layout detected.
    echo [WOS] Recreating %BUILD_DIR% with dependencies at the build root ...
    rmdir /s /q "%BUILD_DIR%"
)

rem Always configure; CMake remains incremental when nothing changed.
echo.
echo [WOS] ===== CMAKE CONFIGURE =====
"!CMAKE_EXE!" --preset release
if errorlevel 1 (
    echo.
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

rem Read the normalized project version from CMakeCache.txt.
set "PROJECT_VERSION="
for /f "tokens=2 delims==" %%v in ('findstr /B /C:"CMAKE_PROJECT_VERSION:" "%BUILD_DIR%\CMakeCache.txt" 2^>nul') do if not defined PROJECT_VERSION set "PROJECT_VERSION=%%v"
if not defined PROJECT_VERSION (
    echo [ERROR] Unable to read CMAKE_PROJECT_VERSION from %BUILD_DIR%\CMakeCache.txt.
    exit /b 1
)
set "PACKAGE_FILE=dist\WaterOnSteroids-!PROJECT_VERSION!.zip"
echo [WOS] Project version = !PROJECT_VERSION!

echo.
echo [WOS] ===== BUILD %CONFIG%  =====
"!CMAKE_EXE!" --build --preset release --parallel
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed.
    exit /b 1
)

set "DLL=%BUILD_DIR%\Release\WaterOnSteroids.dll"
if not exist "!DLL!" set "DLL="
if not defined DLL (
    echo [ERROR] Build completed but WaterOnSteroids.dll was not found.
    exit /b 1
)

echo [WOS] DLL: !DLL!

if not exist "!PACKAGE_DIR!\SKSE\Plugins\WaterOnSteroids.dll" (
    echo [ERROR] Package staging DLL not found:
    echo          !PACKAGE_DIR!\SKSE\Plugins\WaterOnSteroids.dll
    exit /b 1
)
if not exist "!PACKAGE_DIR!\SKSE\Plugins\WaterOnSteroids.ini" (
    echo [ERROR] Package staging INI not found:
    echo          !PACKAGE_DIR!\SKSE\Plugins\WaterOnSteroids.ini
    exit /b 1
)
if not exist "!PACKAGE_DIR!\SKSE\Plugins\WaterOnSteroids-LICENSE.txt" (
    echo [ERROR] Package staging license not found:
    echo          !PACKAGE_DIR!\SKSE\Plugins\WaterOnSteroids-LICENSE.txt
    exit /b 1
)
if not exist "!PACKAGE_DIR!\SKSE\Plugins\WaterOnSteroids-EXCEPTIONS.txt" (
    echo [ERROR] Package staging license exceptions not found:
    echo          !PACKAGE_DIR!\SKSE\Plugins\WaterOnSteroids-EXCEPTIONS.txt
    exit /b 1
)
if not exist "!PACKAGE_DIR!\SKSE\Plugins\WaterOnSteroids-THIRD_PARTY_NOTICES.txt" (
    echo [ERROR] Package staging third-party notices not found:
    echo          !PACKAGE_DIR!\SKSE\Plugins\WaterOnSteroids-THIRD_PARTY_NOTICES.txt
    exit /b 1
)

where powershell >nul 2>nul
if errorlevel 1 (
    echo [ERROR] PowerShell was not found; unable to create the ZIP package.
    exit /b 1
)

if exist "!PACKAGE_FILE!" del /q "!PACKAGE_FILE!"
echo.
echo [WOS] ===== CREATE PACKAGE =====
rem Built entry by entry: Compress-Archive in Windows PowerShell 5.1 writes backslash separators.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem; $root = (Resolve-Path -LiteralPath '!PACKAGE_DIR!').Path; $out = Join-Path (Get-Location).Path '!PACKAGE_FILE!'; $zip = [System.IO.Compression.ZipFile]::Open($out, 'Create'); try { Get-ChildItem -LiteralPath $root -Recurse -File | ForEach-Object { $name = $_.FullName.Substring($root.Length).TrimStart('\').Replace('\', '/'); [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $name, 'Optimal') } } finally { $zip.Dispose() }"
if errorlevel 1 (
    echo.
    echo [ERROR] ZIP package creation failed.
    exit /b 1
)

if not exist "!PACKAGE_FILE!" (
    echo [ERROR] PowerShell did not create the expected package: !PACKAGE_FILE!
    exit /b 1
)

echo [WOS] Package: !PACKAGE_FILE!

echo.
echo [WOS] DONE.
pause
endlocal
