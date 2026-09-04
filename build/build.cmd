@echo off
REM Full build: Zydis, wxWidgets (both skipped when already built), then the solution.
REM Run from a "x64 Native Tools" prompt or anywhere msbuild is on the PATH.
REM Output: bin\Release\MGS4Enabler.asi and bin\Release\MGS4Enabler.exe
setlocal
set "ROOT=%~dp0.."

call "%~dp0build_zydis.cmd" || exit /b 1
call "%~dp0build_wx.cmd"    || exit /b 1

msbuild "%ROOT%\MGS4Enabler.sln" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:m
if errorlevel 1 (
    echo Solution build failed.
    exit /b 1
)
echo Built %ROOT%\bin\Release
endlocal
