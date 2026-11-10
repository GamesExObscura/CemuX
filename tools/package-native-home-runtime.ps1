param(
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [string]$SettingsTemplate="$env:APPDATA/Cemu/settings.xml"
)
$ErrorActionPreference='Stop'
$runtime=[IO.Path]::GetFullPath($RuntimeDirectory)
foreach($file in 'Cemu_release.exe','Cemu_release.pdb'){
    if(-not(Test-Path -LiteralPath "$runtime/$file" -PathType Leaf)){throw "Missing build output: $file"}
}
[xml]$template=Get-Content -LiteralPath $SettingsTemplate
foreach($role in 'native-home','native-manual'){
    $folder=Join-Path $runtime $role
    $portable=Join-Path $folder 'portable'
    $ancestor=[IO.Path]::GetFullPath($portable)
    while($ancestor){
        if((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Refusing helper packaging across a reparse point.'}
        $ancestor=Split-Path $ancestor -Parent
    }
    New-Item -ItemType Directory -Path "$portable/controllerProfiles" -Force | Out-Null
    if(@(Get-ChildItem -LiteralPath "$portable/controllerProfiles" -File).Count){throw 'Native helpers must not claim physical controller profiles.'}
    foreach($file in 'Cemu_release.exe','Cemu_release.pdb'){
        $destination=Join-Path $folder $file
        if(Test-Path -LiteralPath $destination){
            $probe=[IO.File]::Open($destination,'Open','ReadWrite','None');$probe.Dispose()
        }
        Copy-Item -LiteralPath "$runtime/$file" -Destination $destination
        if((Get-FileHash -LiteralPath "$runtime/$file").Hash -ne (Get-FileHash -LiteralPath $destination).Hash){throw 'Packaged helper differs from the build output.'}
    }
    if(-not(Test-Path -LiteralPath "$portable/settings.xml")){
        [xml]$config=$template.OuterXml
        foreach($key in 'start_in_wiiu_menu','confirm_stop_game','fullscreen','window_maximized','pad_maximized'){
            $node=$config.content.SelectSingleNode($key)
            if(!$node){$node=$config.CreateElement($key);[void]$config.content.AppendChild($node)}
            $node.InnerText='false'
        }
        $config.content.open_pad='true'
        $config.content.Graphic.api='0'
        $config.content.Account.OnlineEnabled='false'
        foreach($key in 'GamePaths','RecentLaunchFiles','GraphicPack'){
            $node=$config.content.SelectSingleNode($key)
            if($node){$node.RemoveAll()}
        }
        $config.Save("$portable/settings.xml")
    }
}
Write-Output 'Native HOME/manual runtimes packaged. No game, save/account data or original settings were launched or modified.'
