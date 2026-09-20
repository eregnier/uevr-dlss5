@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"

echo ======================================================================
echo  Building DLSS 5 ^<^> UEVR Universal Installer (GUI)
echo ======================================================================

where cl.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo [INFO] Locating Visual Studio MSVC environment...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
            set "VS_DIR=%%i"
        )
    )
    if defined VS_DIR (
        if exist "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat" (
            call "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
)

where cl.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] MSVC cl.exe compiler not found! Please run from x64 Native Tools Command Prompt.
    exit /b 1
)

echo [INFO] Compiling installer.cpp -> VR-DLSS5-UEVR-Installer.exe...
cl.exe /nologo /O2 /std:c++17 /EHsc /W3 /DUNICODE /D_UNICODE installer.cpp /link /SUBSYSTEM:WINDOWS /OUT:VR-DLSS5-UEVR-Installer.exe user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib shlwapi.lib advapi32.lib wininet.lib

if %errorlevel% equ 0 (
    echo.
    echo [SUCCESS] VR-DLSS5-UEVR-Installer.exe built successfully!
    del installer.obj >nul 2>&1
) else (
    echo.
    echo [FAILED] Compilation failed with error %errorlevel%.
    exit /b %errorlevel%
)

endlocal
