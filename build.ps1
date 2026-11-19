param(
    [switch]$Test,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
    [ValidateSet("x64", "arm64")][string]$HostArchitecture = "x64",
    [string]$ExecutableName = "",
    [string]$OutputDirectory = ""
)
$ErrorActionPreference = "Stop"
if (!$ExecutableName) { $ExecutableName = if ($Architecture -eq "x64") { "MemeStudio.exe" } else { "MemeStudio-arm64.exe" } }
if ($ExecutableName -notmatch '^[A-Za-z0-9_-]+\.exe$') { throw "ExecutableName must be a simple .exe filename." }
if ($ExecutableName -ieq "MemeStudio-cli.exe") { throw "MemeStudio-cli.exe is reserved for the console executable." }
$project = $PSScriptRoot
$requestedOutput = $OutputDirectory
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) { $vswhere = Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\Installer\vswhere.exe" }
if (!(Test-Path -LiteralPath $vswhere)) { throw "Install Visual Studio 2022 or newer with Desktop development with C++." }
$installation = & $vswhere -latest -prerelease -products * -property installationPath
if (!$installation) { throw "Visual Studio C++ toolchain not found." }
$developerCommand = Join-Path $installation "Common7\Tools\VsDevCmd.bat"
$buildDirectory = Join-Path $project "build\$Architecture"
if ($Architecture -eq "x64") { $outputDirectory = Join-Path $project "bin" }
else { $outputDirectory = Join-Path $project "bin\arm64" }
if ($requestedOutput) {
    $outputDirectory = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($requestedOutput)) { $requestedOutput } else { Join-Path $project $requestedOutput }))
}
New-Item -ItemType Directory -Force -Path $outputDirectory,$buildDirectory | Out-Null
Push-Location $buildDirectory
try {
    $core = @("util.cpp", "image.cpp", "api.cpp", "api_routing.cpp", "api_probe.cpp", "plugins.cpp", "refine.cpp", "upscale.cpp", "job.cpp", "job_config.cpp", "batch.cpp", "drawing.cpp")
    $sources = $core + @("api_routing_dialog.cpp", "api_comparison.cpp", "api_probe_dialog.cpp", "drawing_dialog.cpp", "gui.cpp", "dsh.cpp", "dsh_dialog.cpp", "main.cpp", "..\tests\tests.cpp", "..\tests\api_probe_test.cpp", "..\tests\routing_test.cpp", "..\tests\dsh_test.cpp")
    $arguments = ($sources | ForEach-Object { '"' + (Join-Path "$project\src" $_) + '"' }) -join " "
    $libraries = "user32.lib gdi32.lib msimg32.lib windowscodecs.lib ole32.lib oleaut32.lib uuid.lib comctl32.lib comdlg32.lib shell32.lib shlwapi.lib winhttp.lib wintrust.lib crypt32.lib bcrypt.lib propsys.lib advapi32.lib"
    $executable = Join-Path $outputDirectory $ExecutableName
    $cliExecutable = Join-Path $outputDirectory "MemeStudio-cli.exe"
    $compile = 'call "' + $developerCommand + '" -arch=' + $Architecture + ' -host_arch=' + $HostArchitecture + ' >nul && cl /nologo /std:c++20 /MP4 /O2 /EHsc /utf-8 /MT /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN '
    $manifest = ' /MANIFEST:EMBED /MANIFESTINPUT:"' + $project + '\src\app.manifest"'
    $command = $compile + $arguments + ' /Fe:"' + $executable + '" /link ' + $libraries + ' /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup' + $manifest
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed." }
    $objects = ($core | ForEach-Object { [IO.Path]::ChangeExtension($_, ".obj") }) -join " "
    $cliCommand = $compile + '"' + $project + '\src\cli.cpp" "' + $project + '\src\cli_config.cpp" ' + $objects +
        ' /Fe:"' + $cliExecutable + '" /link ' + $libraries + ' /SUBSYSTEM:CONSOLE' + $manifest
    & $env:ComSpec /d /s /c $cliCommand
    if ($LASTEXITCODE -ne 0) { throw "Console executable build failed." }
    foreach ($pluginName in @("example", "local-redraw", "waifu2x", "batch-convert", "prompt-draw", "api-parameter-test", "api-load-balancing", "qwen-image", "deepseek-harness")) {
        $pluginOutput = Join-Path $outputDirectory "plugins\$pluginName"
        New-Item -ItemType Directory -Force -Path $pluginOutput | Out-Null
        $pluginCommand = 'call "' + $developerCommand + '" -arch=' + $Architecture + ' -host_arch=' + $HostArchitecture + ' >nul && cl /nologo /std:c++20 /O2 /EHsc /utf-8 /MT /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /LD "' + $project + '\plugins\' + $pluginName + '\plugin.cpp" /Fo:' + $pluginName + '-plugin.obj /Fe:"' + $pluginOutput + '\' + $pluginName + '.dll" /link user32.lib gdi32.lib ole32.lib uuid.lib winhttp.lib crypt32.lib bcrypt.lib /IMPLIB:' + $pluginName + '-plugin.lib'
        & $env:ComSpec /d /s /c $pluginCommand
        if ($LASTEXITCODE -ne 0) { throw "$pluginName plugin build failed." }
        Copy-Item -LiteralPath "$project\plugins\$pluginName\plugin.json" -Destination $pluginOutput -Force
        if ($pluginName -eq "qwen-image") {
            foreach ($file in @("server.py", "environment.py", "model_info.py", "sdcpp_backend.py", "requirements.txt", "README.md")) {
                Copy-Item -LiteralPath (Join-Path $project "plugins\qwen-image\$file") -Destination $pluginOutput -Force
            }
        }
        if ($pluginName -eq "deepseek-harness") {
            foreach ($file in @("package.json", "cordis.patch.yml", "index.js", "icon.svg", "locale", "README.md")) {
                Copy-Item -LiteralPath (Join-Path $project "plugins\deepseek-harness\$file") -Destination $pluginOutput -Recurse -Force
            }
        }
    }
    if ($Test) {
        & $cliExecutable --help | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "CLI smoke check failed." }
        $process = Start-Process -FilePath $executable -ArgumentList @("--self-test", "$project\tmp\tests") -WindowStyle Hidden -PassThru -Wait
        Get-Content -LiteralPath "$project\tmp\tests\results.txt"
        if ($process.ExitCode -ne 0) { throw "Self tests failed." }
        $process = Start-Process -FilePath $executable -ArgumentList @("--ui-check", "$project\tmp\tests") -WindowStyle Hidden -PassThru -Wait
        if ($process.ExitCode -ne 0) { throw "UI rendering check failed." }
    }
    Write-Host "Ready: $executable"
    Write-Host "Ready: $cliExecutable"
} finally { Pop-Location }
