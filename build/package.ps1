# Packages a release zip from bin\Release:
#
#   PFCompanion_<version>.zip
#     PF Companion.exe
#     README.md
#     UltimateASILoader_LICENSE.md
#     MGS4\winmm.dll                  Ultimate ASI Loader (ThirteenAG), renamed from dinput8.dll
#     MGS4\scripts\PFCompanion.asi
#
# The loader is not in this repository. Pass -LoaderZip with the Ultimate-ASI-Loader_x64.zip
# from https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases (or drop it in build\loader\).
# Output goes to release\<version>\ under the repository root.

param(
    [string]$LoaderZip = ""
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'

# Version from the shared header, so the zip name cannot drift from the binaries.
$versionHeader = Get-Content (Join-Path $root 'shared\version.hpp') -Raw
$parts = 'MAJOR', 'MINOR', 'PATCH' | ForEach-Object {
    [regex]::Match($versionHeader, "#define\s+PFC_VERSION_$_\s+(\d+)").Groups[1].Value
}
$version = $parts -join '.'
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Could not read the version from shared\version.hpp (got '$version')." }

$asi = Join-Path $root 'bin\Release\PFCompanion.asi'
$exe = Join-Path $root 'bin\Release\PF Companion.exe'
foreach ($f in $asi, $exe) {
    if (-not (Test-Path $f)) { throw "Missing $f - run build\build.cmd first." }
}
if (-not (Test-Path $sevenZip)) { throw "7-Zip not found at $sevenZip." }

if (-not $LoaderZip) { $LoaderZip = Join-Path $PSScriptRoot 'loader\Ultimate-ASI-Loader_x64.zip' }
if (-not (Test-Path $LoaderZip)) {
    throw "Ultimate ASI Loader zip not found at $LoaderZip. Download Ultimate-ASI-Loader_x64.zip from " +
          "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases and pass it with -LoaderZip."
}

$out = Join-Path $root "release\$version"
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'MGS4\scripts') | Out-Null

# Loader: the zip holds dinput8.dll; MGS4 loads it under the winmm.dll name.
$loaderDir = Join-Path $out 'loader'
if (Test-Path $loaderDir) { Remove-Item $loaderDir -Recurse -Force }
& $sevenZip x -y "-o$loaderDir" $LoaderZip | Out-Null
$loaderDll = Get-ChildItem $loaderDir -Filter '*.dll' | Select-Object -First 1
if (-not $loaderDll) { throw "No DLL inside $LoaderZip." }
$loaderVersion = (Get-Item $loaderDll.FullName).VersionInfo.ProductVersion
Copy-Item $loaderDll.FullName (Join-Path $stage 'MGS4\winmm.dll')

Copy-Item $asi (Join-Path $stage 'MGS4\scripts\PFCompanion.asi')
Copy-Item $exe (Join-Path $stage 'PF Companion.exe')
Copy-Item (Join-Path $root 'README.md') (Join-Path $stage 'README.md')
Copy-Item (Join-Path $root 'UltimateASILoader_LICENSE.md') (Join-Path $stage 'UltimateASILoader_LICENSE.md')

$zip = Join-Path $out "PFCompanion_$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Push-Location $stage
try {
    & $sevenZip a -tzip -mx=9 $zip '*' | Out-Null
}
finally { Pop-Location }

Remove-Item $stage -Recurse -Force
Remove-Item $loaderDir -Recurse -Force

Write-Host "Packaged $zip"
Write-Host "  PFCompanion.asi  $((Get-Item $asi).Length) bytes"
Write-Host "  PF Companion.exe $((Get-Item $exe).Length) bytes"
Write-Host "  Ultimate ASI Loader $loaderVersion as MGS4\winmm.dll"
