@echo off
REM Builds the static Zydis library the ASI links (external\zydis\msvc\bin\ReleaseX64\Zydis.lib).
REM Skips the build when the library already exists; pass "clean" to force a rebuild.
setlocal

set "ROOT=%~dp0.."
set "PROJECT=%ROOT%\external\zydis\msvc\zydis\Zydis.vcxproj"
set "LIB=%ROOT%\external\zydis\msvc\bin\ReleaseX64\Zydis.lib"

if not exist "%PROJECT%" (
    echo Zydis project not found: %PROJECT%
    echo Run "git submodule update --init --recursive" first.
    exit /b 1
)

if /i "%~1"=="clean" del /q "%LIB%" 2>nul
if exist "%LIB%" (
    echo Zydis: %LIB% is present, nothing to do.
    exit /b 0
)

REM Zydis pins v143 in its project; build it with the same toolset as the ASI instead.
msbuild "%PROJECT%" /p:Configuration="Release MT" /p:Platform=x64 /p:PlatformToolset=v145 /m /nologo /v:m
if errorlevel 1 (
    echo Zydis build failed.
    exit /b 1
)
echo Zydis: built %LIB%
endlocal
