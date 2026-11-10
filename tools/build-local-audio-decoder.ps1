param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$out=[IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($out)|Out-Null
$vs='C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/Tools/VsDevCmd.bat'
$dev='"'+$vs+'" -arch=x64 -host_arch=x64 >nul && set'
foreach($line in (& $env:COMSPEC /d /s /c $dev)){
    if($line -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}
}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT "$PSScriptRoot/decode-local-audio.cpp" "/Fe:$out/decode-local-audio.exe" "/Fo:$out/decode-local-audio.obj" /link mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib *> "$out/decoder-build.log"
if($LASTEXITCODE -ne 0){throw 'Local file decoder failed to build'}
Get-FileHash -LiteralPath "$out/decode-local-audio.exe"|Select-Object -ExpandProperty Hash
