param([string]$Configuration = "Release")
$ErrorActionPreference = "Stop"
if ($Configuration -notin @("Release", "Debug")) { throw "Configuration must be Release or Debug." }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) { throw "Install Visual Studio C++ Build Tools first." }
$installation = & $vswhere -latest -prerelease -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw "Visual Studio C++ x64 toolchain not found." }

$output = Join-Path $PSScriptRoot "dist\plugins\example"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$compiler = Join-Path $installation "VC\Auxiliary\Build\vcvars64.bat"
$runtime = if ($Configuration -eq "Debug") { "/MTd" } else { "/MT" }
$command = 'call "' + $compiler + '" >nul && cl /nologo /std:c++20 /O2 /EHsc /utf-8 /W4 ' +
    $runtime + ' /LD "' + (Join-Path $PSScriptRoot "plugin.cpp") +
    '" /I "' + $PSScriptRoot + '" /Fo:"' + (Join-Path $output "plugin.obj") +
    '" /Fe:"' + (Join-Path $output "example.dll") +
    '" /link /IMPLIB:"' + (Join-Path $output "example.lib") + '"'
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Plugin build failed." }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "plugin.json") -Destination $output -Force
Write-Host "Ready: $output"
