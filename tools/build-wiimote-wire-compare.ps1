param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$out=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $out -Force | Out-Null
$vs='C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/Tools/VsDevCmd.bat'
$dev='"'+$vs+'" -arch=x64 -host_arch=x64 >nul && set'
foreach($line in (& $env:COMSPEC /d /s /c $dev)){
    if($line -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}
}
$libs=Join-Path $repo 'build/vcpkg_installed/x64-windows-static/lib'
$arguments=@('/nologo','/std:c++20','/EHsc','/O2','/MT','/UNDEBUG',"/I$repo/src", "/I$repo/build/vcpkg_installed/x64-windows-static/include",
    "$PSScriptRoot/wiimote-speaker-wire-compare.cpp","/Fe:$out/wiimote-speaker-wire-compare.exe","/Fo:$out/wire-compare.obj",'/link',"/LIBPATH:$libs",'SDL3-static.lib',
    'kernel32.lib','user32.lib','gdi32.lib','winmm.lib','imm32.lib','ole32.lib','oleaut32.lib','version.lib','uuid.lib','advapi32.lib','setupapi.lib','shell32.lib','dinput8.lib','hid.lib','cfgmgr32.lib','ws2_32.lib','dxguid.lib')
& cl.exe @arguments *> "$out/build.log"
if($LASTEXITCODE -ne 0){throw "Comparison compile/link failed; see $out/build.log"}
& "$out/wiimote-speaker-wire-compare.exe" --self-test "$out/speaker-replay.bin" *> "$out/self-test.log"
if($LASTEXITCODE -ne 0){throw 'Comparison self-test failed'}
[ordered]@{status='built_and_self_tested';exe_sha256=(Get-FileHash "$out/wiimote-speaker-wire-compare.exe").Hash;hardware_accessed=$false;game_launched=$false} |
    ConvertTo-Json | Set-Content -LiteralPath "$out/build-result.json"
Get-Content -LiteralPath "$out/self-test.log"
Get-Content -LiteralPath "$out/build-result.json"
