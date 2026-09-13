# Packages a Peace Walker preview zip for testers, from bin\Release: the plugin, the loader and a
# commented settings file, no tool. The full release packager (package.ps1) is untouched; this
# one has no version gate and no changelog gate, since previews are not releases.
#
#   MGSPWEnabler_preview_<date>[_<tag>].zip
#     README.txt                      install notes for testers (build\pw_preview\README.txt)
#     UltimateASILoader_LICENSE.md
#     MGSPWEnabler.settings           the settings with comments (build\pw_preview\MGSPWEnabler.settings)
#     mgspw\winmm.dll                 Ultimate ASI Loader (ThirteenAG), renamed from dinput8.dll
#     mgspw\scripts\MGSPWEnabler.asi  the Peace Walker plugin, Release build
#
# The loader is not in this repository. Pass -LoaderZip with the Ultimate-ASI-Loader_x64.zip
# from https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases (or drop it in build\loader\).
# -Tag adds a word to the zip name (e.g. -Tag hud2). Output goes to release\preview\.

param(
    [string]$LoaderZip = "",
    [string]$Tag = ""
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'

$asiPw = Join-Path $root 'bin\Release\MGSPWEnabler.asi'
if (-not (Test-Path $asiPw)) { throw "Missing $asiPw - build the Release configuration first." }

# A Lab build must never go out, even to testers: it carries the research instrumentation.
if (Select-String -Path $asiPw -Pattern 'LAB MODE: research instrumentation' -Quiet) {
    throw "$asiPw is a Lab build. Build the Release configuration before packaging."
}

if (-not $LoaderZip) { $LoaderZip = Join-Path $PSScriptRoot 'loader\Ultimate-ASI-Loader_x64.zip' }
if (-not (Test-Path $LoaderZip)) {
    throw "Ultimate ASI Loader zip not found at $LoaderZip. Download Ultimate-ASI-Loader_x64.zip from " +
          "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases and pass it with -LoaderZip."
}
if (-not (Test-Path $sevenZip)) { throw "7-Zip not found at $sevenZip." }

$out = Join-Path $root 'release\preview'
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'mgspw\scripts') | Out-Null

$loaderDir = Join-Path $out 'loader'
if (Test-Path $loaderDir) { Remove-Item $loaderDir -Recurse -Force }
& $sevenZip x -y "-o$loaderDir" $LoaderZip | Out-Null
$loaderDll = Get-ChildItem $loaderDir -Filter '*.dll' | Select-Object -First 1
if (-not $loaderDll) { throw "No DLL inside $LoaderZip." }
Copy-Item $loaderDll.FullName (Join-Path $stage 'mgspw\winmm.dll')

Copy-Item $asiPw (Join-Path $stage 'mgspw\scripts\MGSPWEnabler.asi')
Copy-Item (Join-Path $PSScriptRoot 'pw_preview\MGSPWEnabler.settings') (Join-Path $stage 'MGSPWEnabler.settings')
Copy-Item (Join-Path $PSScriptRoot 'pw_preview\README.txt') (Join-Path $stage 'README.txt')
Copy-Item (Join-Path $root 'UltimateASILoader_LICENSE.md') (Join-Path $stage 'UltimateASILoader_LICENSE.md')

$name = 'MGSPWEnabler_preview_' + (Get-Date).ToString('yyyy-MM-dd')
if ($Tag) { $name += "_$Tag" }
$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
& $sevenZip a -tzip -mx=9 $zip (Join-Path $stage '*') | Out-Null
Remove-Item $stage -Recurse -Force
Remove-Item $loaderDir -Recurse -Force

Write-Host "Packaged $zip"
Write-Host "  MGSPWEnabler.asi $((Get-Item $asiPw).Length) bytes, loader $($loaderDll.Name) $((Get-Item $loaderDll.FullName).VersionInfo.ProductVersion)"
