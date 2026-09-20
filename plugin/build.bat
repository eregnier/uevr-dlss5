@echo off
setlocal enabledelayedexpansion

echo ======================================================================
echo  Building VRDLSS5 UEVR Plugin (VRDLSS5_UEVR_Plugin.dll)
echo ======================================================================

set "PLUGIN_DIR=%~dp0"
set "UEVR_DIR=%PLUGIN_DIR%..\UEVR"

:: Search for MSVC environment
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

cd /d "%PLUGIN_DIR%"

set "INCLUDES=/I. /I"%UEVR_DIR%\include" /I"%UEVR_DIR%\examples\renderlib" /I"%UEVR_DIR%\dependencies\submodules\imgui""

set "SOURCES=VRDLSS5_Plugin.cpp "%UEVR_DIR%\examples\renderlib\imgui\imgui_impl_dx11.cpp" "%UEVR_DIR%\examples\renderlib\imgui\imgui_impl_dx12.cpp" "%UEVR_DIR%\examples\renderlib\imgui\imgui_impl_win32.cpp" "%UEVR_DIR%\examples\renderlib\rendering\d3d11.cpp" "%UEVR_DIR%\examples\renderlib\rendering\d3d12.cpp" "%UEVR_DIR%\dependencies\submodules\imgui\imgui.cpp" "%UEVR_DIR%\dependencies\submodules\imgui\imgui_draw.cpp" "%UEVR_DIR%\dependencies\submodules\imgui\imgui_tables.cpp" "%UEVR_DIR%\dependencies\submodules\imgui\imgui_widgets.cpp""

set "LIBS=d3d11.lib d3d12.lib dxgi.lib user32.lib gdi32.lib"

echo [BUILD] Compiling VRDLSS5_UEVR_Plugin.dll...
cl.exe /nologo /O2 /std:c++20 /MD /EHa /MP %INCLUDES% %SOURCES% /LD /Fe:VRDLSS5_UEVR_Plugin.dll /link %LIBS%

if %errorlevel% equ 0 (
    echo [SUCCESS] VRDLSS5_UEVR_Plugin.dll built successfully!
    del *.obj >nul 2>&1
    del *.exp >nul 2>&1
    del *.lib >nul 2>&1
) else (
    echo [ERROR] Compilation failed!
    exit /b 1
)
