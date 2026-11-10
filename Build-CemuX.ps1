#requires -Version 7.2
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot 'build-cemux'),
    [string]$ReleaseDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'CemuX release')
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$runtime=Join-Path $BuildDirectory 'runtime'
& cmake -S $PSScriptRoot -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 -T host=x64 "-DCEMU_RUNTIME_OUTPUT_DIRECTORY:PATH=$runtime"
if($LASTEXITCODE){throw 'CMake configuration failed. See BUILD.md for dependencies.'}
& cmake --build $BuildDirectory --config Release --target CemuBin
if($LASTEXITCODE){throw 'CemuX source build failed'}
[void][IO.Directory]::CreateDirectory($ReleaseDirectory)
Copy-Item -LiteralPath (Join-Path $runtime 'Cemu_release.exe') -Destination (Join-Path $ReleaseDirectory 'CemuX.exe') -Force
foreach($name in 'resources','gameProfiles'){
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "bin/$name") -Destination $ReleaseDirectory -Recurse -Force
}
foreach($name in 'native-home','native-manual'){
    $to=Join-Path $ReleaseDirectory $name
    [void][IO.Directory]::CreateDirectory($to)
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "bin/$name/Cemu_release.exe") -Destination $to -Force
}
Copy-Item -LiteralPath (Join-Path $runtime 'cafe-au-lait') -Destination $ReleaseDirectory -Recurse -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'src/resource/logo_icon.ico') -Destination (Join-Path $ReleaseDirectory 'CemuX.ico') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE.txt') -Destination $ReleaseDirectory -Force
Write-Host (Join-Path $ReleaseDirectory 'CemuX.exe')
