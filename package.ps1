param(
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
    [string]$Executable = ""
)
$ErrorActionPreference = "Stop"
$project = $PSScriptRoot
if ($Architecture -eq "x64") { $binaryDirectory = Join-Path $project "bin" }
else { $binaryDirectory = Join-Path $project "bin\arm64" }
if (!$Executable) {
    $executableName = if ($Architecture -eq "x64") { "MemeStudio.exe" } else { "MemeStudio-arm64.exe" }
    $Executable = Join-Path $binaryDirectory $executableName
}
if (!(Test-Path -LiteralPath $executable)) { throw "Run build.ps1 first." }
$package = Join-Path $project "dist\MemeStudio"
New-Item -ItemType Directory -Force -Path $package,(Join-Path $package "bin") | Out-Null
foreach ($name in @("src", "tests", "vendor", "sdk", "plugins")) {
    Copy-Item -LiteralPath (Join-Path $project $name) -Destination $package -Recurse -Force
}
foreach ($name in @("README.md", "PLUGINS.md", "VALIDATION.md", "CMakeLists.txt", "build.ps1", "package.ps1", ".gitignore")) {
    Copy-Item -LiteralPath (Join-Path $project $name) -Destination $package -Force
}
Copy-Item -LiteralPath $executable -Destination (Join-Path $package "bin\MemeStudio.exe") -Force
foreach ($pluginName in @("example", "local-redraw", "waifu2x", "batch-convert", "prompt-draw", "api-parameter-test", "api-load-balancing")) {
    $pluginOutput = Join-Path $package "bin\plugins\$pluginName"
    New-Item -ItemType Directory -Force -Path $pluginOutput | Out-Null
    foreach ($name in @("$pluginName.dll", "plugin.json")) {
        Copy-Item -LiteralPath (Join-Path $binaryDirectory "plugins\$pluginName\$name") -Destination $pluginOutput -Force
    }
}
$archive = Join-Path $project "MemeStudio-Windows-$Architecture.zip"
Compress-Archive -LiteralPath $package -DestinationPath $archive -Force
Get-Item -LiteralPath $archive | Select-Object FullName,Length
$pluginArchive = Join-Path $project "MemeStudio-Plugins-$Architecture.zip"
Compress-Archive -LiteralPath (Join-Path $package "bin\plugins"),(Join-Path $package "PLUGINS.md") -DestinationPath $pluginArchive -Force
Get-Item -LiteralPath $pluginArchive | Select-Object FullName,Length
