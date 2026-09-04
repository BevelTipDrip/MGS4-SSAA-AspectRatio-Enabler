@echo off
REM Full build: Zydis, wxWidgets (both skipped when already built), then the solution.
REM Run from a "x64 Native Tools" prompt or anywhere msbuild is on the PATH.
REM Output: bin\Release\MGS4Enabler.asi and bin\Release\MGS4Enabler.exe
REM
REM   build.cmd lab     builds the Lab configuration instead: the ASI with the research
REM                     instrumentation compiled in (bin\Lab\). Never ship that one.
setlocal
set "ROOT=%~dp0.."
set "CFG=Release"
if /i "%~1"=="lab" set "CFG=Lab"

call "%~dp0build_zydis.cmd" || exit /b 1
call "%~dp0build_wx.cmd"    || exit /b 1

msbuild "%ROOT%\MGS4Enabler.sln" /p:Configuration=%CFG% /p:Platform=x64 /m /nologo /v:m
if errorlevel 1 (
    echo Solution build failed.
    exit /b 1
)
echo Built %ROOT%\bin\%CFG%
endlocal
