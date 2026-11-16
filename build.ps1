param(
    [switch]$Test,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
    [ValidateSet("x64", "arm64")][string]$HostArchitecture = "x64",
    [string]$ExecutableName = ""
)
$ErrorActionPreference = "Stop"
if (!$ExecutableName) { $ExecutableName = if ($Architecture -eq "x64") { "MemeStudio.exe" } else { "MemeStudio-arm64.exe" } }
if ($ExecutableName -notmatch '^[A-Za-z0-9_-]+\.exe$') { throw "ExecutableName must be a simple .exe filename." }
$project = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) { $vswhere = Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\Installer\vswhere.exe" }
if (!(Test-Path -LiteralPath $vswhere)) { throw "Install Visual Studio 2022 or newer with Desktop development with C++." }
$installation = & $vswhere -latest -prerelease -products * -property installationPath
if (!$installation) { throw "Visual Studio C++ toolchain not found." }
$developerCommand = Join-Path $installation "Common7\Tools\VsDevCmd.bat"
$buildDirectory = Join-Path $project "build\$Architecture"
if ($Architecture -eq "x64") { $outputDirectory = Join-Path $project "bin" }
else { $outputDirectory = Join-Path $project "bin\arm64" }
New-Item -ItemType Directory -Force -Path $outputDirectory,$buildDirectory | Out-Null
Push-Location $buildDirectory
try {
    $sources = @("util.cpp", "image.cpp", "api.cpp", "api_routing.cpp", "api_routing_dialog.cpp", "api_comparison.cpp", "api_probe.cpp", "api_probe_dialog.cpp", "plugins.cpp", "refine.cpp", "upscale.cpp", "job.cpp", "batch.cpp", "drawing.cpp", "drawing_dialog.cpp", "gui.cpp", "main.cpp", "..\tests\tests.cpp", "..\tests\api_probe_test.cpp", "..\tests\routing_test.cpp")
    $arguments = ($sources | ForEach-Object { '"' + (Join-Path "$project\src" $_) + '"' }) -join " "
    $libraries = "user32.lib gdi32.lib msimg32.lib windowscodecs.lib ole32.lib oleaut32.lib uuid.lib comctl32.lib comdlg32.lib shell32.lib shlwapi.lib winhttp.lib crypt32.lib bcrypt.lib propsys.lib"
    $executable = Join-Path $outputDirectory $ExecutableName
    $command = 'call "' + $developerCommand + '" -arch=' + $Architecture + ' -host_arch=' + $HostArchitecture + ' >nul && cl /nologo /std:c++20 /MP4 /O2 /EHsc /utf-8 /MT /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ' + $arguments + ' /Fe:"' + $executable + '" /link ' + $libraries + ' /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /MANIFEST:EMBED /MANIFESTINPUT:"' + $project + '\src\app.manifest"'
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed." }
    foreach ($pluginName in @("example", "local-redraw", "waifu2x", "batch-convert", "prompt-draw", "api-parameter-test", "api-load-balancing")) {
        $pluginOutput = Join-Path $outputDirectory "plugins\$pluginName"
        New-Item -ItemType Directory -Force -Path $pluginOutput | Out-Null
        $pluginCommand = 'call "' + $developerCommand + '" -arch=' + $Architecture + ' -host_arch=' + $HostArchitecture + ' >nul && cl /nologo /std:c++20 /O2 /EHsc /utf-8 /MT /W4 /LD "' + $project + '\plugins\' + $pluginName + '\plugin.cpp" /Fo:' + $pluginName + '-plugin.obj /Fe:"' + $pluginOutput + '\' + $pluginName + '.dll" /link /IMPLIB:' + $pluginName + '-plugin.lib'
        & $env:ComSpec /d /s /c $pluginCommand
        if ($LASTEXITCODE -ne 0) { throw "$pluginName plugin build failed." }
        Copy-Item -LiteralPath "$project\plugins\$pluginName\plugin.json" -Destination $pluginOutput -Force
    }
    if ($Test) {
        $process = Start-Process -FilePath $executable -ArgumentList @("--self-test", "$project\tmp\tests") -WindowStyle Hidden -PassThru -Wait
        Get-Content -LiteralPath "$project\tmp\tests\results.txt"
        if ($process.ExitCode -ne 0) { throw "Self tests failed." }
        $process = Start-Process -FilePath $executable -ArgumentList @("--ui-check", "$project\tmp\tests") -WindowStyle Hidden -PassThru -Wait
        if ($process.ExitCode -ne 0) { throw "UI rendering check failed." }
    }
    Write-Host "Ready: $executable"
} finally { Pop-Location }
