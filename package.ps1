param([string]$Executable = (Join-Path $PSScriptRoot "bin\MemeStudio.exe"))
$ErrorActionPreference = "Stop"
$project = $PSScriptRoot
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
foreach ($pluginName in @("example", "local-redraw", "waifu2x", "batch-convert")) {
    $pluginOutput = Join-Path $package "bin\plugins\$pluginName"
    New-Item -ItemType Directory -Force -Path $pluginOutput | Out-Null
    foreach ($name in @("$pluginName.dll", "plugin.json")) {
        Copy-Item -LiteralPath (Join-Path $project "bin\plugins\$pluginName\$name") -Destination $pluginOutput -Force
    }
}
$archive = Join-Path $project "MemeStudio-Windows-x64.zip"
Compress-Archive -LiteralPath $package -DestinationPath $archive -Force
Get-Item -LiteralPath $archive | Select-Object FullName,Length
$pluginArchive = Join-Path $project "MemeStudio-Plugins.zip"
Compress-Archive -LiteralPath (Join-Path $package "bin\plugins"),(Join-Path $package "PLUGINS.md") -DestinationPath $pluginArchive -Force
Get-Item -LiteralPath $pluginArchive | Select-Object FullName,Length
