$ErrorActionPreference = "Stop"
$project = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw "MSVC not found." }
$developerCommand = Join-Path $installation "Common7\Tools\VsDevCmd.bat"
New-Item -ItemType Directory -Force -Path "$project\tmp\waifu-mock" | Out-Null
Push-Location "$project\tmp\waifu-mock"
try {
    $command = 'call "' + $developerCommand + '" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++20 /MP3 /O2 /EHsc /utf-8 /MT /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN "' + $project + '\tests\mock-waifu.cpp" "' + $project + '\src\util.cpp" "' + $project + '\src\image.cpp" /Fe:mock-waifu.exe /link windowscodecs.lib ole32.lib oleaut32.lib uuid.lib crypt32.lib bcrypt.lib propsys.lib shell32.lib'
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Mock build failed." }
} finally { Pop-Location }
