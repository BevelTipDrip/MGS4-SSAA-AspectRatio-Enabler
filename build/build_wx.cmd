@echo off
REM Builds the static wxWidgets libraries the settings tool links
REM (external\wxWidgets\lib\vc_x64_lib, Release x64, Unicode, DLL runtime).
REM Skips the build when the libraries already exist; pass "clean" to force a rebuild.
setlocal

set "ROOT=%~dp0.."
set "SLN=%ROOT%\external\wxWidgets\build\msw\wx_vc17.sln"
set "LIBDIR=%ROOT%\external\wxWidgets\lib\vc_x64_lib"

if not exist "%SLN%" (
    echo wxWidgets solution not found: %SLN%
    echo Run "git submodule update --init --recursive" first.
    exit /b 1
)

if /i "%~1"=="clean" rmdir /s /q "%LIBDIR%" 2>nul
if exist "%LIBDIR%\wxmsw33u_core.lib" if exist "%LIBDIR%\mswu\wx\setup.h" (
    echo wxWidgets: %LIBDIR% is present, nothing to do.
    exit /b 0
)

REM Only the projects the tool links; the rest of the solution is not needed.
msbuild "%SLN%" /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /m /nologo /v:m ^
    /t:_custom_build;wxzlib;wxpng;wxregex;base;core
if errorlevel 1 (
    echo wxWidgets build failed.
    exit /b 1
)
echo wxWidgets: built into %LIBDIR%
endlocal
