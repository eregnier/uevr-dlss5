@echo off
setlocal enabledelayedexpansion

echo ======================================================================
echo  Packaging DLSS 5 ^<^> UEVR Full Release Distribution
echo ======================================================================

set "ROOT_DIR=%~dp0"
set "DIST_DIR=%ROOT_DIR%dist\VR-DLSS5-UEVR-Release"
set "ZIP_OUT=%ROOT_DIR%dist\VR-DLSS5-UEVR-Release.zip"

:: 1. Build Plugin
echo [1/3] Building UEVR Plugin...
cd /d "%ROOT_DIR%plugin"
call build.bat
if %errorlevel% neq 0 (
    echo [ERROR] Plugin build failed!
    exit /b 1
)

:: 2. Build Proxy
echo [2/3] Building Dual-Proxy...
cd /d "%ROOT_DIR%proxy"
call build.bat
if %errorlevel% neq 0 (
    echo [ERROR] Proxy build failed!
    exit /b 1
)

:: 3. Build Installer
echo [3/3] Building Installer...
cd /d "%ROOT_DIR%installer"
call build.bat
if %errorlevel% neq 0 (
    echo [ERROR] Installer build failed!
    exit /b 1
)

:: 4. Assemble release folder
echo [4/5] Assembling release folder...
cd /d "%ROOT_DIR%"

if exist "%DIST_DIR%" rd /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"
mkdir "%DIST_DIR%\uevr\plugins"
mkdir "%DIST_DIR%\uevr\scripts"

copy "%ROOT_DIR%installer\VR-DLSS5-UEVR-Installer.exe" "%DIST_DIR%\" >nul
copy "%ROOT_DIR%installer\VR-DLSS5-UEVR-Installer.exe.manifest" "%DIST_DIR%\" >nul
copy "%ROOT_DIR%plugin\VRDLSS5_UEVR_Plugin.dll" "%DIST_DIR%\" >nul
copy "%ROOT_DIR%plugin\VRDLSS5_UEVR_Plugin.dll" "%DIST_DIR%\uevr\plugins\" >nul
copy "%ROOT_DIR%scripts\VRDLSS5.lua" "%DIST_DIR%\" >nul
copy "%ROOT_DIR%scripts\VRDLSS5.lua" "%DIST_DIR%\uevr\scripts\" >nul
copy "%ROOT_DIR%proxy\dxgi.dll" "%DIST_DIR%\" >nul
copy "%ROOT_DIR%proxy\openvr_api.dll" "%DIST_DIR%\" >nul
copy "%ROOT_DIR%deps\OptiScaler.dll" "%DIST_DIR%\" >nul
copy "%ROOT_DIR%deps\OptiScaler.ini" "%DIST_DIR%\" >nul
copy "%ROOT_DIR%deps\cudart64_12.dll" "%DIST_DIR%\" >nul
copy "%ROOT_DIR%LICENSE" "%DIST_DIR%\" >nul
copy "%ROOT_DIR%README.md" "%DIST_DIR%\" >nul
if not exist "%DIST_DIR%\LICENSES" mkdir "%DIST_DIR%\LICENSES"
copy "%ROOT_DIR%deps\LICENSES\*" "%DIST_DIR%\LICENSES\" >nul

:: 5. Create ZIP archive
echo [5/5] Creating ZIP archive...
if exist "%ZIP_OUT%" del "%ZIP_OUT%"
powershell -Command "Compress-Archive -Path '%DIST_DIR%\*' -DestinationPath '%ZIP_OUT%' -Force"

echo.
echo ======================================================================
echo  [SUCCESS] Packaging complete!
echo  Folder : %DIST_DIR%
echo  Archive: %ZIP_OUT%
echo ======================================================================

endlocal
