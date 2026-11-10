#requires -Version 5.1
param([Parameter(Mandatory)][string]$Executable,[Parameter(Mandatory)][string]$AssetsRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
# MSBuild may inherit PowerShell 7's module search path while invoking 5.1.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1')
$Executable=[IO.Path]::GetFullPath($Executable)
$destination=Join-Path ([IO.Path]::GetDirectoryName($Executable)) 'cafe-au-lait'
function Plain([string]$Path){
    for($p=[IO.Path]::GetFullPath($Path);$p;$p=[IO.Path]::GetDirectoryName($p.TrimEnd('\','/'))){
        if((Test-Path -LiteralPath $p) -and ((Get-Item -Force -LiteralPath $p).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Linked package path rejected.'}
    }
}
function Copy-Known([string]$From,[string]$To){
    Plain $From;Plain $To
    if(!(Test-Path -LiteralPath $From -PathType Leaf)){throw "Missing runtime input: $From"}
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($To)) | Out-Null
    if(!(Test-Path -LiteralPath $To) -or (Get-FileHash -LiteralPath $From).Hash -cne (Get-FileHash -LiteralPath $To).Hash){[IO.File]::Copy($From,$To,$true)}
}
Plain $destination
[IO.Directory]::CreateDirectory((Join-Path $destination 'pc')) | Out-Null
foreach($name in 'policy.json','online-required-files.json') {Copy-Known (Join-Path $PSScriptRoot $name) (Join-Path $destination $name)}
foreach($f in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'pc') -File){Copy-Known $f.FullName (Join-Path $destination ('pc/'+$f.Name))}
foreach($f in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'SD-card') -File -Recurse){
    $relative=$f.FullName.Substring($PSScriptRoot.Length+1)
    Copy-Known $f.FullName (Join-Path $destination $relative)
}
foreach($name in 'LICENSE-DUMPLING.txt','LICENSE-LIBMOCHA.txt','LICENSE-WUT.txt','THIRD-PARTY.txt'){
    Copy-Known (Join-Path $PSScriptRoot $name) (Join-Path $destination $name)
}
$compiler=Join-Path $env:SystemRoot 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
& $compiler /nologo /target:winexe /optimize+ /reference:System.Windows.Forms.dll (('/out:'+ (Join-Path $destination 'Cafe au Lait.exe'))) (Join-Path $PSScriptRoot 'pc/Launcher.cs')
if($LASTEXITCODE){throw 'Cafe au Lait support launcher compilation failed.'}
$files=New-Object 'Collections.Generic.List[object]'
function Add-Runtime([string]$Source,[string]$Relative){
    $target=Join-Path $destination ('testing-runtime/'+$Relative)
    Copy-Known $Source $target
    $files.Add([pscustomobject]@{relative=$Relative;sha256=(Get-FileHash -LiteralPath $target).Hash;length=(Get-Item -LiteralPath $target).Length})
}
Add-Runtime $Executable 'Cemu_release.exe'
foreach($role in 'native-home','native-manual'){
    $helper=Join-Path $AssetsRoot ($role+'/Cemu_release.exe')
    if(!(Test-Path -LiteralPath $helper -PathType Leaf)){$helper=$Executable}
    Add-Runtime $helper ($role+'/Cemu_release.exe')
}
foreach($folder in 'resources','gameProfiles'){
    $root=[IO.Path]::GetFullPath((Join-Path $AssetsRoot $folder))
    Plain $root
    foreach($f in Get-ChildItem -LiteralPath $root -File -Recurse){
        $relative=$folder+'/'+$f.FullName.Substring($root.Length+1).Replace('\','/')
        Add-Runtime $f.FullName $relative
    }
}
$files.ToArray() | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $destination 'testing-runtime.json') -Encoding UTF8
'Cafe au Lait runtime assembled from source; no console/account/game data included.'
