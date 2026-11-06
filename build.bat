@echo off
rem Builds the recompiler, recompiles assets\default.xbe and compiles the result.
rem "build.bat tools" builds only the recompiler and the semantics test, without the game.
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio 2022 with C++ Clang tools not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if /i not "%PRESET%"=="tools" if not exist assets\default.xbe (
    echo assets\default.xbe missing, extract the game first:
    echo   powershell -File tools\xiso.ps1 -Iso ^<game.iso^> -Out assets
    exit /b 1
)

cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% %2 %3 %4 || exit /b 1
