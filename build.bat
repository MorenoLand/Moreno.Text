@echo off
setlocal

echo [1/5] Configuring CMake...

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

echo [2/5] Building...
cmake --build build --config Release
if %errorlevel% neq 0 (
    echo ERROR: Build failed.
    exit /b 1
)

echo [3/5] Setting up output directory...
if not exist bin mkdir bin

echo [4/5] Provisioning bundled Python plugin runtime...
set "PY_RUNTIME_DIR=%cd%\bin\Release\Data\Lib\Runtime\Python38"
set "PY_RUNTIME_ZIP=%TEMP%\moreno-python-3.8.10-embed-amd64.zip"
if not exist "%PY_RUNTIME_DIR%\python.exe" (
    if not exist "%PY_RUNTIME_DIR%" mkdir "%PY_RUNTIME_DIR%"
    if not exist "%PY_RUNTIME_ZIP%" (
        powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-WebRequest -Uri 'https://www.python.org/ftp/python/3.8.10/python-3.8.10-embed-amd64.zip' -OutFile '%PY_RUNTIME_ZIP%'"
        if %errorlevel% neq 0 (
            echo ERROR: Python runtime download failed.
            exit /b 1
        )
    )
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -Force '%PY_RUNTIME_ZIP%' '%PY_RUNTIME_DIR%'"
    if %errorlevel% neq 0 (
        echo ERROR: Python runtime extraction failed.
        exit /b 1
    )
)

echo [5/5] Creating desktop shortcut...
powershell -Command "$ws = New-Object -ComObject WScript.Shell; $sc = $ws.CreateShortcut('%USERPROFILE%\Desktop\Moreno Text.lnk'); $sc.TargetPath = '%cd%\bin\Release\moreno_text.exe'; $sc.WorkingDirectory = '%cd%\bin\Release'; $sc.Description = 'Moreno Text Editor'; $sc.Save()" 2>nul

echo.
echo Done. Run bin\Release\moreno_text.exe or use the desktop shortcut.
endlocal
