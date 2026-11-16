param(
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
    [ValidateSet("x64", "arm64")][string]$HostArchitecture = "x64"
)
$ErrorActionPreference = "Stop"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) { throw "Install Visual Studio C++ Build Tools first." }
$installation = & $vswhere -latest -prerelease -products * -property installationPath
if (!$installation) { throw "Visual Studio C++ toolchain not found." }
$build = Join-Path $PSScriptRoot "build\$Architecture"
$output = Join-Path $PSScriptRoot "dist\$Architecture\plugins\qwen-image"
New-Item -ItemType Directory -Force -Path $build,$output | Out-Null
$developerCommand = Join-Path $installation "Common7\Tools\VsDevCmd.bat"
$command = 'call "' + $developerCommand + '" -arch=' + $Architecture + ' -host_arch=' +
    $HostArchitecture + ' >nul && cl /nologo /std:c++20 /O2 /EHsc /utf-8 /W4 /MT ' +
    '/DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /LD "' +
    (Join-Path $PSScriptRoot "plugins\qwen-image\plugin.cpp") + '" /Fo:"' +
    (Join-Path $build "plugin.obj") + '" /Fe:"' + (Join-Path $output "qwen-image.dll") +
    '" /link user32.lib gdi32.lib ole32.lib uuid.lib winhttp.lib crypt32.lib bcrypt.lib /IMPLIB:"' +
    (Join-Path $build "qwen-image.lib") + '"'
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Plugin build failed." }
foreach ($name in @("plugin.json", "server.py", "environment.py", "model_info.py",
                     "sdcpp_backend.py", "requirements.txt", "README.md")) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "plugins\qwen-image\$name") -Destination $output -Force
}
Write-Host "Ready: $output"
