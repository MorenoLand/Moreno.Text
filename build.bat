@echo off
setlocal

echo [1/4] Configuring CMake...

if not defined VCPKG_ROOT (
    for /f "delims=" %%p in ('where vcpkg 2^>nul') do set "VCPKG_ROOT=%%~dp"
    if not defined VCPKG_ROOT (
        if exist "D:\Software\Microsoft\18\Community\VC\vcpkg\scripts\buildsystems\vcpkg.cmake" (
            set "VCPKG_ROOT=D:\Software\Microsoft\18\Community\VC\vcpkg"
        )
    )
)

if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT not set and vcpkg not found.
    echo Set VCPKG_ROOT or install vcpkg.
    exit /b 1
)

echo Using vcpkg: %VCPKG_ROOT%

if not exist build mkdir build
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release
if %errorlevel% neq 0 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

echo [2/4] Building...
cmake --build build --config Release
if %errorlevel% neq 0 (
    echo ERROR: Build failed.
    exit /b 1
)

echo [3/4] Setting up output directory...
if not exist bin mkdir bin

echo [4/4] Creating desktop shortcut...
powershell -Command "$ws = New-Object -ComObject WScript.Shell; $sc = $ws.CreateShortcut('%USERPROFILE%\Desktop\Moreno Text.lnk'); $sc.TargetPath = '%cd%\bin\moreno_text.exe'; $sc.WorkingDirectory = '%cd%\bin'; $sc.Description = 'Moreno Text Editor'; $sc.Save()" 2>nul

echo.
echo Done. Run bin\moreno_text.exe or use the desktop shortcut.
endlocal
