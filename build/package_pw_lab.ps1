# Packages a Peace Walker LAB zip for trusted testers, from bin\Lab: the Lab plugin (the full
# research instrumentation, the private module compiled in), the loader, the settings with the
# Lab keys commented, the log-analysis scripts and a guide. No tool. The full release packager
# (package.ps1) and the preview packager (package_pw_preview.ps1) are untouched.
#
#   MGSPWEnabler_lab_<date>[_<tag>].zip
#     LAB-README.txt                  what the build records, how to capture, what to send
#     UltimateASILoader_LICENSE.md
#     MGSPWEnabler.settings           build\pw_lab\MGSPWEnabler.settings
#     mgspw\winmm.dll                 Ultimate ASI Loader (ThirteenAG), renamed from dinput8.dll
#     mgspw\scripts\MGSPWEnabler.asi  the Peace Walker plugin, LAB build
#     tools\*.py                      census analysis (Python 3, standard library only)
#
# Pass -LoaderZip with Ultimate-ASI-Loader_x64.zip (or drop it in build\loader\). -Tag adds a
# word to the zip name. Output goes to release\lab\.

param(
    [string]$LoaderZip = "",
    [string]$Tag = ""
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'

$asiPw = Join-Path $root 'bin\Lab\MGSPWEnabler.asi'
if (-not (Test-Path $asiPw)) { throw "Missing $asiPw - build the Lab configuration first." }
# The opposite check to the release packagers: this one must be the Lab build.
if (-not (Select-String -Path $asiPw -Pattern 'LAB MODE: research instrumentation' -Quiet)) {
    throw "$asiPw is not a Lab build. Build the Lab configuration before packaging."
}

if (-not $LoaderZip) { $LoaderZip = Join-Path $PSScriptRoot 'loader\Ultimate-ASI-Loader_x64.zip' }
if (-not (Test-Path $LoaderZip)) {
    throw "Ultimate ASI Loader zip not found at $LoaderZip. Download Ultimate-ASI-Loader_x64.zip from " +
          "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases and pass it with -LoaderZip."
}
if (-not (Test-Path $sevenZip)) { throw "7-Zip not found at $sevenZip." }

$out = Join-Path $root 'release\lab'
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'mgspw\scripts') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'tools') | Out-Null

$loaderDir = Join-Path $out 'loader'
if (Test-Path $loaderDir) { Remove-Item $loaderDir -Recurse -Force }
& $sevenZip x -y "-o$loaderDir" $LoaderZip | Out-Null
$loaderDll = Get-ChildItem $loaderDir -Filter '*.dll' | Select-Object -First 1
if (-not $loaderDll) { throw "No DLL inside $LoaderZip." }
$loaderVersion = (Get-Item $loaderDll.FullName).VersionInfo.ProductVersion
Copy-Item $loaderDll.FullName (Join-Path $stage 'mgspw\winmm.dll')

Copy-Item $asiPw (Join-Path $stage 'mgspw\scripts\MGSPWEnabler.asi')
Copy-Item (Join-Path $PSScriptRoot 'pw_lab\MGSPWEnabler.settings') (Join-Path $stage 'MGSPWEnabler.settings')
Copy-Item (Join-Path $PSScriptRoot 'pw_lab\LAB-README.txt') (Join-Path $stage 'LAB-README.txt')
Copy-Item (Join-Path $root 'UltimateASILoader_LICENSE.md') (Join-Path $stage 'UltimateASILoader_LICENSE.md')
# The analysis scripts: the ones that read a census log with nothing but the standard library.
$toolsSrc = Join-Path $root 'external\ultrawide\pw\tools'
foreach ($t in 'hud_rows.py', 'hud_frames.py', 'hud_identity.py', 'hud_right.py') {
    $f = Join-Path $toolsSrc $t
    if (Test-Path $f) { Copy-Item $f (Join-Path $stage "tools\$t") }
}

$name = 'MGSPWEnabler_lab_' + (Get-Date).ToString('yyyy-MM-dd')
if ($Tag) { $name += "_$Tag" }
$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
& $sevenZip a -tzip -mx=9 $zip (Join-Path $stage '*') | Out-Null
Remove-Item $stage -Recurse -Force
Remove-Item $loaderDir -Recurse -Force

Write-Host "Packaged $zip"
Write-Host "  MGSPWEnabler.asi (Lab) $((Get-Item $asiPw).Length) bytes, loader $($loaderDll.Name) $loaderVersion"
