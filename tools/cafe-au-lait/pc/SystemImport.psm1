#requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$script:ToolRoot=Split-Path $PSScriptRoot -Parent
$script:Policy=Get-Content -LiteralPath (Join-Path $script:ToolRoot 'policy.json') -Encoding UTF8 -Raw | ConvertFrom-Json

function Assert-PlainPath([string]$Path) {
    $full=[IO.Path]::GetFullPath($Path)
    if($full.StartsWith('\\') -or $full.StartsWith('\\?\')){throw 'Use a local drive, not a network/device path.'}
    $item=$full
    while($item){
        if(Test-Path -LiteralPath $item){
            if((Get-Item -Force -LiteralPath $item).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Junction/symlink/cloud placeholder rejected: $item"}
        }
        $parent=[IO.Path]::GetDirectoryName($item.TrimEnd('\','/'))
        if($parent -eq $item){break};$item=$parent
    }
    return $full.TrimEnd('\','/')
}
function Get-SafeTree([string]$Root) {
    $rootPath=Assert-PlainPath $Root
    if(!(Test-Path -LiteralPath $rootPath -PathType Container)){return}
    $stack=New-Object 'Collections.Generic.Stack[string]';$stack.Push($rootPath)
    while($stack.Count){
        foreach($item in Get-ChildItem -Force -LiteralPath $stack.Pop()){
            if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Linked item rejected: $($item.FullName)"}
            if($item.Name -match '[<>:"|?*]' -or $item.Name -match '[ .]$'){throw 'Unsafe filename in dump.'}
            if($item.PSIsContainer){$stack.Push($item.FullName)}else{$item}
        }
    }
}
function Get-Xml([string]$Path) {
    Assert-PlainPath $Path | Out-Null
    if(!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -gt 2MB){throw "Missing/oversized title XML: $Path"}
    $settings=New-Object Xml.XmlReaderSettings
    $settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$settings.XmlResolver=$null
    $reader=[Xml.XmlReader]::Create($Path,$settings)
    try{$xml=New-Object Xml.XmlDocument;$xml.XmlResolver=$null;$xml.Load($reader);return ,$xml}finally{$reader.Dispose()}
}
function Test-RegionTitle([string]$Group,[string]$Id,[string]$Region) {
    $r=@{JPN='0';USA='1';EUR='2'}[$Region]
    if($Group -eq 'system_apps' -or $Group -eq 'applets' -or $Id -match '^100(59|67|69)') {return $Id.Substring(5,1) -eq $r}
    return $true
}
function Get-FstU32([byte[]]$Bytes,[int]$Offset) {
    if($Offset -lt 0 -or $Offset -gt $Bytes.Length-4){throw 'Truncated system title file table.'}
    return ([long]$Bytes[$Offset]*16777216+[long]$Bytes[$Offset+1]*65536+[long]$Bytes[$Offset+2]*256+$Bytes[$Offset+3])
}
function Assert-ContentFreeTitle([string]$TitleRoot) {
    $fst=Join-Path $TitleRoot 'code/title.fst'
    Assert-PlainPath $fst | Out-Null
    if(!(Test-Path -LiteralPath $fst -PathType Leaf)){throw 'Missing content directory and no title file table to prove it is unused.'}
    $length=(Get-Item -LiteralPath $fst).Length
    if($length -lt 48 -or $length -gt 4MB){throw 'Invalid system title file-table size.'}
    $b=[IO.File]::ReadAllBytes($fst)
    if([BitConverter]::ToString($b,0,4) -cne '46-53-54-00'){throw 'Invalid system title file-table magic.'}
    $clusters=Get-FstU32 $b 8
    $table=32+$clusters*32
    if($clusters -lt 1 -or $clusters -gt 20000 -or $table -gt $b.Length-16){throw 'Invalid system title cluster table.'}
    $count=Get-FstU32 $b ($table+8)
    $strings=$table+$count*16
    if($b[$table] -ne 1 -or (Get-FstU32 $b ($table+4)) -ne 0 -or $count -lt 1 -or $count -gt 20000 -or $strings -ge $b.Length){throw 'Invalid system title file-table root.'}
    $stack=New-Object 'Collections.Generic.List[object]'
    $stack.Add([pscustomobject]@{index=0;end=$count;path=''})
    $seen=New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $hasRpx=$false
    for($i=1;$i -lt $count;$i++){
        while($stack.Count -and $i -ge $stack[$stack.Count-1].end){$stack.RemoveAt($stack.Count-1)}
        if(!$stack.Count){throw 'System title directory hierarchy escaped root.'}
        $parent=$stack[$stack.Count-1];$entry=$table+$i*16
        $type=[int]$b[$entry]
        if($type -notin @(0,1)){throw 'Linked or unknown system title file-table entry.'}
        $nameAt=$strings+((Get-FstU32 $b $entry) -band 0xffffff)
        if($nameAt -ge $b.Length){throw 'System title filename is outside string table.'}
        $end=$nameAt
        while($end -lt $b.Length -and $b[$end] -ne 0){
            if($end-$nameAt -ge 255 -or $b[$end] -lt 32 -or $b[$end] -gt 126){throw 'Invalid system title filename.'}
            $end++
        }
        if($end -eq $b.Length -or $end -eq $nameAt){throw 'Unterminated/empty system title filename.'}
        $name=[Text.Encoding]::ASCII.GetString($b,$nameAt,$end-$nameAt)
        if($name -match '(^\.{1,2}$|[\\/:<>"|?*]|[. ]$)'){throw 'Unsafe system title filename.'}
        $path=if($parent.path){$parent.path+'/'+$name}else{$name}
        if($path.Length -gt 480 -or !$seen.Add($path)){throw 'Overlong/duplicate system title path.'}
        if($type -eq 1){
            $until=Get-FstU32 $b ($entry+8)
            if((Get-FstU32 $b ($entry+4)) -ne $parent.index -or $until -le $i -or $until -gt $parent.end -or $stack.Count -ge 16){throw 'Invalid system title directory range.'}
            if($path -ine 'content' -and !$path.StartsWith('content/',[StringComparison]::OrdinalIgnoreCase) -and !(Test-Path -LiteralPath (Join-Path $TitleRoot $path) -PathType Container)){throw 'A directory required by the system title file table is missing.'}
            $stack.Add([pscustomobject]@{index=$i;end=$until;path=$path})
        }else{
            if($path -ieq 'content' -or $path.StartsWith('content/',[StringComparison]::OrdinalIgnoreCase)){throw 'Title file table requires content assets; transfer is incomplete.'}
            if([int]$b[$entry+14]*256+$b[$entry+15] -ge $clusters){throw 'System title file references invalid cluster.'}
            $file=Join-Path $TitleRoot $path
            Assert-PlainPath $file | Out-Null
            if(!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -ne (Get-FstU32 $b ($entry+8))){throw 'A file required by the system title file table is missing or has the wrong size.'}
            if($path -match '^code/[^/]+\.rpx$'){$hasRpx=$true}
        }
    }
    if(!$hasRpx -or !$seen.Contains('code/app.xml') -or !$seen.Contains('meta/meta.xml')){throw 'System title file table does not describe the required executable/metadata.'}
}
function Get-ReceivedTransferIdentity {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Dump)
    $root=Assert-PlainPath $Dump
    if(!(Test-Path -LiteralPath (Join-Path $root 'DUMP-COMPLETE.txt') -PathType Leaf)){throw 'Select a COMPLETED dump session, not the SD root or an incomplete dump.'}
    foreach($name in 'DUMP-COMPLETE.txt','dump-info.json','otp.bin','seeprom.bin'){Assert-PlainPath (Join-Path $root $name) | Out-Null}
    $infoPath=Join-Path $root 'dump-info.json'
    if((Get-Item -LiteralPath $infoPath).Length -gt 1MB){throw 'Dump inventory is too large.'}
    $info=Get-Content -LiteralPath $infoPath -Encoding UTF8 -Raw | ConvertFrom-Json
    $full=($info.schema -eq 2 -and $info.tool -ceq 'cafe-au-lait-full-mlc' -and $info.full_mlc -eq $true)
    if(!$full -and ($info.schema -ne 1 -or $info.tool -cne 'dumpling-minimal-system' -or $info.game_files_copied -ne $false -or $info.game_saves_copied -ne $false)){throw 'Unsupported dump inventory.'}
    $account=[string]$info.account
    if($account -cnotmatch '^8[0-9a-f]{7}$'){throw 'Invalid original account ID; account renaming is not supported.'}
    return [pscustomobject]@{dump=$root;account=$account;full_mlc=$full;info=$info}
}
function Assert-NativeLibrary([string]$Path) {
    Assert-PlainPath $Path | Out-Null
    if(!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -gt 16MB){throw "Missing/oversized required native library: $([IO.Path]::GetFileName($Path))"}
    $stream=[IO.File]::OpenRead($Path)
    try{$header=New-Object byte[] 20;$read=$stream.Read($header,0,20)}finally{$stream.Dispose()}
    if($read -ne 20 -or [BitConverter]::ToString($header,0,6) -cne '7F-45-4C-46-01-02' -or $header[18] -ne 0 -or $header[19] -ne 20){throw "Invalid PowerPC native library: $([IO.Path]::GetFileName($Path))"}
}
function Get-NativeLibraryPlan([string]$Root) {
    foreach($name in $script:Policy.native_libraries){
        $path=Join-Path $Root ('cafeLibs/'+$name)
        Assert-NativeLibrary $path
        [pscustomobject]@{relative=('cafeLibs/'+$name);source=$path;length=(Get-Item -LiteralPath $path).Length;sha256=(Get-FileHash -LiteralPath $path).Hash}
    }
}
function Assert-SystemResources([string]$Root) {
    foreach($asset in @('10056000/content/FFLResHigh.dat','10056000/content/FFLResMiddle.dat',
        '10042400/content/CafeStd.ttf','10042400/content/CafeCn.ttf','10042400/content/CafeKr.ttf','10042400/content/CafeTw.ttf',
        '10051000/content/00/erreula/erreula.pack','1004f000/content/00/swkbd/swkbd.pack')){
        $path=Join-Path $Root ('mlc01/sys/title/0005001b/'+$asset)
        Assert-PlainPath $path | Out-Null
        if(!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0){throw "Missing required system resource: $asset"}
    }
    foreach($id in '10050000','10052000'){
        $path=Join-Path $Root "mlc01/sys/title/0005001b/$id/content/00"
        if(!(@(Get-SafeTree $path).Count)){throw "Missing required EULA/error messages: $id"}
    }
}
function Install-NativeLibraries([string]$Target,[object[]]$Libraries) {
    foreach($directory in 'cafeLibs','native-home/cafeLibs','native-home/portable/cafeLibs','native-manual/cafeLibs','native-manual/portable/cafeLibs'){
        foreach($library in $Libraries){
            $destination=Join-SafeRelative $Target ($directory+'/'+[IO.Path]::GetFileName($library.relative))
            Assert-PlainPath $destination | Out-Null
            if(Test-Path -LiteralPath $destination){
                if((Get-FileHash -LiteralPath $destination).Hash -cne $library.sha256){throw 'Refusing to replace a different private native library.'}
            }else{Copy-NewFile $library.source $destination $library.sha256}
        }
    }
}
function Get-DumpPlan {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Dump,[ValidateSet('Auto','JPN','USA','EUR')][string]$Region='Auto',[switch]$AllowPartialLegacy,[scriptblock]$Progress)
    $identity=Get-ReceivedTransferIdentity -Dump $Dump
    if($identity.full_mlc -and !$AllowPartialLegacy){return Get-FullMlcDumpPlan -Identity $identity -Region $Region -Progress $Progress}
    if(!$identity.full_mlc -and !$AllowPartialLegacy){throw 'This is an old PARTIAL dump. Install the updated SD app and receive a new FULL MLC transfer; the old copy is preserved but cannot be used as a full setup.'}
    $root=$identity.dump;$account=$identity.account
    foreach($spec in @(@('otp.bin',1024),@('seeprom.bin',512))){
        $p=Join-Path $root $spec[0]
        if((Get-Item -LiteralPath $p).Length -ne $spec[1]){throw "$($spec[0]) has the wrong size."}
        $bytes=[IO.File]::ReadAllBytes($p)
        if(@($bytes | Select-Object -Unique).Count -eq 1){throw "$($spec[0]) is blank/uniform. Redump from your console."}
    }
    $menus=@{JPN='10040000';USA='10040100';EUR='10040200'}
    if($Region -eq 'Auto'){
        $found=@($menus.Keys | Where-Object {Test-Path -LiteralPath (Join-Path $root ('mlc01/sys/title/00050010/'+$menus[$_])) -PathType Container})
        if($found.Count -ne 1){throw 'Cannot infer one console region. Select JPN, USA or EUR explicitly.'};$Region=$found[0]
    }
    $paths=New-Object 'Collections.Generic.List[string]'
    foreach($spec in @(@('system_apps','00050010'),@('applets','00050030'),@('shared','0005001b'))){
        foreach($id in $script:Policy.($spec[0])){
            if(!(Test-RegionTitle $spec[0] $id $Region)){continue}
            $paths.Add("sys/title/$($spec[1])/$id")
            if($spec[0] -ne 'shared'){$paths.Add("usr/save/$($spec[1])/$id")}
        }
    }
    foreach($p in $script:Policy.system_state){$paths.Add($p)}
    $paths.Add("usr/save/system/act/$account")
    $files=New-Object 'Collections.Generic.List[object]'
    $seen=New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach($relative in $paths){
        foreach($file in Get-SafeTree (Join-Path $root ('mlc01/'+$relative))){
            $rel='mlc01/'+$file.FullName.Substring((Join-Path $root 'mlc01').Length+1).Replace('\','/')
            if(!$seen.Add($rel)){throw 'Duplicate/case-colliding destination in dump.'}
            $files.Add([pscustomobject]@{relative=$rel;source=$file.FullName;length=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash})
        }
    }
    foreach($name in 'otp.bin','seeprom.bin'){
        $p=Join-Path $root $name
        $files.Add([pscustomobject]@{relative=$name;source=$p;length=(Get-Item $p).Length;sha256=(Get-FileHash -LiteralPath $p).Hash})
    }
    foreach($library in Get-NativeLibraryPlan $root){$files.Add($library)}
    $accountFile=Join-Path $root "mlc01/usr/save/system/act/$account/account.dat"
    if(!(Test-Path -LiteralPath $accountFile -PathType Leaf) -or (Get-Item $accountFile).Length -gt 1MB){throw 'Selected account.dat is missing or oversized.'}
    $accountText=[IO.File]::ReadAllText($accountFile)
    if($accountText -notmatch "(?m)^PersistentId=$account\s*$"){throw 'account.dat does not match its original account directory.'}
    $r=@{JPN='0';USA='1';EUR='2'}[$Region]
    $required=@(
        @('00050010',"10040${r}00",'Wii U Menu'),@('00050010',"10047${r}00",'System Settings'),
        @('00050010',"1004a${r}00",'Mii Maker'),@('00050010',"1004e${r}00",'Health and Safety Information'),@('00050030',"10010${r}0a",'HOME'),
        @('00050030',"10011${r}0a",'Error applet'),@('00050030',"10017${r}0a",'Manual'))
    $directories=New-Object 'Collections.Generic.List[string]'
    foreach($title in $required){
        $titleRoot=Join-Path $root "mlc01/sys/title/$($title[0])/$($title[1])"
        $xml=Get-Xml (Join-Path $titleRoot 'code/app.xml')
        $idNode=$xml.SelectSingleNode('//title_id')
        if(!$idNode -or $idNode.InnerText.Trim().ToLowerInvariant() -ne ($title[0]+$title[1])){throw "Title ID mismatch: $($title[2])"}
        $meta=Get-Xml (Join-Path $titleRoot 'meta/meta.xml')
        $mid=$meta.SelectSingleNode('//title_id')
        if(!$mid -or $mid.InnerText.Trim().ToLowerInvariant() -ne ($title[0]+$title[1])){throw "Meta title ID mismatch: $($title[2])"}
        # A files-only transfer omits empty folders; require FST proof before
        # accepting an absent content directory, rather than hiding lost assets.
        $content=Join-Path $titleRoot 'content'
        if(!(Test-Path -LiteralPath $content -PathType Container)){
            if(Test-Path -LiteralPath $content){throw "Content path is not a directory: $($title[2])"}
            Assert-ContentFreeTitle $titleRoot
        }
        $directories.Add("mlc01/sys/title/$($title[0])/$($title[1])/content")
        $rpx=@(Get-ChildItem -LiteralPath (Join-Path $titleRoot 'code') -Filter '*.rpx' -File)
        if(!$rpx.Count){throw "Missing RPX: $($title[2])"}
        foreach($exe in $rpx){
            $stream=[IO.File]::OpenRead($exe.FullName)
            try{$header=New-Object byte[] 20;$read=$stream.Read($header,0,20)}finally{$stream.Dispose()}
            if($read -ne 20 -or [BitConverter]::ToString($header,0,6) -ne '7F-45-4C-46-01-02' -or $header[18] -ne 0 -or $header[19] -ne 20){throw "Invalid PowerPC RPX header: $($title[2])"}
        }
    }
    Assert-SystemResources $root
    $requiredCerts=Get-Content -LiteralPath (Join-Path $script:ToolRoot 'online-required-files.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    $missingCerts=@(foreach($relative in $requiredCerts){
        $p=Join-Path $root ('mlc01/sys/title/0005001b/10054000/content/'+$relative)
        if(!(Test-Path -LiteralPath $p -PathType Leaf) -or (Get-Item $p).Length -eq 0){$relative}
    })
    $networkReady=($accountText -match '(?m)^AccountId=\S+' -and $accountText -match '(?m)^PrincipalId=0*[1-9a-fA-F][0-9a-fA-F]*\s*$' -and $accountText -match '(?m)^IsPasswordCacheEnabled=0*[1-9a-fA-F][0-9a-fA-F]?\s*$' -and $accountText -match '(?m)^AccountPasswordCache=[0-9a-fA-F]{64}\s*$' -and $accountText -notmatch '(?m)^AccountPasswordCache=0{64}\s*$')
    $warnings=New-Object 'Collections.Generic.List[string]'
    if($missingCerts.Count){$warnings.Add("Online certificate set incomplete ($($missingCerts.Count) missing).")}
    if(!$networkReady){$warnings.Add('Account is not structurally ready for online use. Check console account setup and saved password.')}
    $warnings.Add('Online authentication and native applet behavior still require testing. Files alone do not implement unsupported services.')
    [pscustomobject]@{dump=$root;region=$Region;account=$account;files=@($files.ToArray());directories=@($directories.ToArray());bytes=[long](($files | Measure-Object length -Sum).Sum);warnings=@($warnings.ToArray());online_files_present=($missingCerts.Count -eq 0 -and $networkReady);missing_certificates=$missingCerts;menu_title=('00050010'+$menus[$Region]);game_files_included=$false;game_saves_included=$false}
}
function Copy-NewFile([string]$Source,[string]$Destination,[string]$Expected) {
    Assert-PlainPath $Source | Out-Null;Assert-PlainPath $Destination | Out-Null
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Destination)) | Out-Null
    $input=[IO.File]::Open($Source,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try{
        $output=[IO.File]::Open($Destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try{$input.CopyTo($output);$output.Flush($true)}finally{$output.Dispose()}
    }finally{$input.Dispose()}
    if((Get-FileHash -LiteralPath $Destination).Hash -cne $Expected){throw 'Copied bytes failed SHA-256 verification. This import is incomplete.'}
}
function Join-SafeRelative([string]$Root,[string]$Relative) {
    if(!$Relative -or $Relative.Contains('\') -or $Relative -match '(^/|:|(^|/)\.{1,2}(/|$))'){throw 'Unsafe relative path in runtime manifest.'}
    $full=[IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if(!$full.StartsWith([IO.Path]::GetFullPath($Root).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Runtime path escaped private destination.'}
    return $full
}
function Get-PrivateRuntimePlan {
    $runtime=Assert-PlainPath (Join-Path $script:ToolRoot 'testing-runtime')
    $manifestPath=Assert-PlainPath (Join-Path $script:ToolRoot 'testing-runtime.json')
    if((Get-Item -LiteralPath $manifestPath).Length -gt 4MB){throw 'Private runtime manifest is oversized.'}
    # Windows PowerShell otherwise interprets BOM-less UTF-8 as the ANSI codepage.
    $manifest=Get-Content -LiteralPath $manifestPath -Encoding UTF8 -Raw | ConvertFrom-Json
    if(@($manifest).Count -lt 1 -or @($manifest).Count -gt 10000){throw 'Invalid private runtime file count.'}
    $seen=New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $files=New-Object 'Collections.Generic.List[object]';$bytes=[long]0
    foreach($f in $manifest){
        if($f.relative -cnotmatch '^(Cemu_release\.exe|resources/[^:]+|gameProfiles/[^:]+|native-(home|manual)/Cemu_release\.exe)$' -or $f.sha256 -cnotmatch '^[0-9A-F]{64}$'){throw 'Unexpected file in private runtime manifest.'}
        $source=Join-SafeRelative $runtime $f.relative
        Assert-PlainPath $source | Out-Null
        if(!$seen.Add($source)){throw 'Duplicate destination in private runtime manifest.'}
        if(!(Test-Path -LiteralPath $source -PathType Leaf)){throw "Private runtime file is missing: $($f.relative)"}
        if((Get-FileHash -LiteralPath $source).Hash -cne $f.sha256){throw "Private runtime file failed verification: $($f.relative)"}
        $length=(Get-Item -LiteralPath $source).Length;$bytes+=$length
        $files.Add([pscustomobject]@{relative=$f.relative;source=$source;sha256=$f.sha256;length=$length})
    }
    if(@($files | Where-Object relative -CEQ 'Cemu_release.exe').Count -ne 1){throw 'Private runtime executable is missing.'}
    return [pscustomobject]@{files=@($files.ToArray());bytes=$bytes}
}
function New-PrivateSystemImport {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Dump,[ValidateSet('Auto','JPN','USA','EUR')][string]$Region='Auto',[string]$StorageRoot=$script:ToolRoot,[switch]$AllowPartialLegacy,[scriptblock]$Progress)
    $plan=Get-DumpPlan -Dump $Dump -Region $Region -AllowPartialLegacy:$AllowPartialLegacy -Progress $Progress
    $isFull=$plan.PSObject.Properties.Name -contains 'full_mlc' -and $plan.full_mlc
    $runtimePlan=Get-PrivateRuntimePlan
    $launcher=Assert-PlainPath (Join-Path $script:ToolRoot 'Cafe au Lait.exe')
    $keyboard=Assert-PlainPath (Join-Path $script:ToolRoot 'pc/setup-keyboard.xml')
    $launcherHash=(Get-FileHash -LiteralPath $launcher).Hash
    $keyboardHash=(Get-FileHash -LiteralPath $keyboard).Hash
    $base=Assert-PlainPath (Join-Path (Assert-PlainPath $StorageRoot) 'PrivateImports')
    if($base.StartsWith($plan.dump+'\',[StringComparison]::OrdinalIgnoreCase) -or $plan.dump.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase) -or $base -eq $plan.dump){throw 'Source and private import destinations must not overlap.'}
    [IO.Directory]::CreateDirectory($base) | Out-Null
    $target=Join-Path $base ((Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
    if(Test-Path -LiteralPath $target){throw 'Refusing an existing destination.'}
    $drive=New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($target))
    if($drive.AvailableFreeSpace -lt ($plan.bytes+$runtimePlan.bytes+512MB)){throw 'Not enough free disk space (dump/runtime plus 512 MB safety margin).'}
    [IO.Directory]::CreateDirectory($target) | Out-Null
    $acl=New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true,$false)
    $user=[Security.Principal.WindowsIdentity]::GetCurrent().User
    foreach($sid in @($user,(New-Object Security.Principal.SecurityIdentifier 'S-1-5-18'))){
        $rule=New-Object Security.AccessControl.FileSystemAccessRule($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow');$acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $target -AclObject $acl
    [IO.File]::WriteAllText((Join-Path $target 'IMPORT-INCOMPLETE.txt'),'Do not use unless IMPORT-COMPLETE.json is present.')
    $copied=0
    foreach($f in $plan.files){
        Copy-NewFile $f.source (Join-Path $target $f.relative) $f.sha256
        if($Progress -and (++$copied % 25) -eq 0){& $Progress "Placing full MLC and online files: $copied / $($plan.files.Count) files"}
    }
    foreach($relative in $plan.directories){
        $directory=Join-SafeRelative $target $relative
        Assert-PlainPath $directory | Out-Null
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }
    foreach($f in $runtimePlan.files){
        Copy-NewFile $f.source (Join-SafeRelative $target $f.relative) $f.sha256
    }
    $mlc=Join-Path $target 'mlc01'
    $settings=New-Object Xml.XmlDocument
    $settings.LoadXml('<content><mlc_path/><permanent_storage>true</permanent_storage><automatic_controller_setup>false</automatic_controller_setup><console_language>1</console_language><start_in_wiiu_menu>true</start_in_wiiu_menu><GamePaths/><GameCache/><GraphicPack/><Account><PersistentId/></Account></content>')
    $settings.content.mlc_path=$mlc.Replace('\','/')
    $settings.content.Account.PersistentId=[string][Convert]::ToUInt32($plan.account,16)
    if($isFull){
        $services=$settings.CreateElement('AccountService')
        $service=$settings.CreateElement('SelectedService')
        $service.SetAttribute('PersistentId',[string][Convert]::ToUInt32($plan.account,16));$service.SetAttribute('Service','2')
        [void]$services.AppendChild($service);[void]$settings.DocumentElement.AppendChild($services)
    }
    if($plan.region -eq 'JPN'){$settings.content.console_language='0'}
    $settings.Save((Join-Path $target 'settings.xml'))
    foreach($role in 'native-home','native-manual'){
        [IO.Directory]::CreateDirectory((Join-Path $target "$role/portable")) | Out-Null
        $helperSettings=$settings.CloneNode($true)
        if($isFull){$helperSettings.content.AccountService.SelectedService.Service='0'}
        $helperSettings.Save((Join-Path $target "$role/settings.xml"))
        $helperSettings.Save((Join-Path $target "$role/portable/settings.xml"))
        if($isFull){foreach($name in 'otp.bin','seeprom.bin'){Copy-NewFile (Join-Path $target $name) (Join-Path $target "$role/portable/$name") (Get-FileHash -LiteralPath (Join-Path $target $name)).Hash}}
    }
    Install-NativeLibraries $target @($plan.files | Where-Object relative -Like 'cafeLibs/*')
    $setup=Join-Path $target 'cafe-au-lait'
    [IO.Directory]::CreateDirectory($setup) | Out-Null
    Copy-NewFile $launcher (Join-Path $setup 'Cafe au Lait.exe') $launcherHash
    [IO.File]::WriteAllText((Join-Path $setup 'tool-root.txt'),$script:ToolRoot)
    Copy-NewFile $keyboard (Join-Path $target 'controllerProfiles/controller0.xml') $keyboardHash
    $launch=@'
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
if(!(Test-Path -LiteralPath (Join-Path $root 'IMPORT-COMPLETE.json'))){throw 'Import is incomplete.'}
$receipt=Get-Content -LiteralPath (Join-Path $root 'IMPORT-COMPLETE.json') -Encoding UTF8 -Raw | ConvertFrom-Json
if($receipt.account -notmatch '^8[0-9a-fA-F]{7}$' -or $receipt.menu_title -notmatch '^0005001010040[012]00$'){throw 'Invalid private launch identity.'}
foreach($name in 'Cemu_release.exe','settings.xml','mlc01','otp.bin','seeprom.bin'){
    $p=Join-Path $root $name
    if((Get-Item -Force -LiteralPath $p).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Private runtime path changed into a link.'}
}
if((Get-FileHash -LiteralPath (Join-Path $root 'Cemu_release.exe')).Hash -ne $receipt.cemu_sha256){throw 'Private Cemu binary changed.'}
$settings=[xml](Get-Content -LiteralPath (Join-Path $root 'settings.xml') -Encoding UTF8 -Raw)
if([IO.Path]::GetFullPath($settings.content.mlc_path) -ne [IO.Path]::GetFullPath((Join-Path $root 'mlc01'))){throw 'Private MLC path changed. Refusing launch.'}
foreach($e in @(Get-ChildItem Env:CEMU_*)){Remove-Item -LiteralPath ('Env:'+ $e.Name)}
$menu=Join-Path $root ('mlc01/sys/title/00050010/'+$receipt.menu_title.Substring(8)+'/code')
for($item=$menu;$item -and $item.StartsWith($root,[StringComparison]::OrdinalIgnoreCase);$item=[IO.Path]::GetDirectoryName($item)){
    if((Get-Item -Force -LiteralPath $item).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Menu path changed into a link.'}
}
$rpx=@(Get-ChildItem -LiteralPath $menu -File -Filter '*.rpx')
if($rpx.Count -ne 1 -or ($rpx[0].Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Expected one native Wii U Menu executable.'}
# Direct launch avoids a first-run race with the asynchronous title-ID cache.
Start-Process -FilePath (Join-Path $root 'Cemu_release.exe') -ArgumentList @('--mlc',('"'+(Join-Path $root 'mlc01')+'"'),'--account',$receipt.account,'--no-auto-menu','-g',('"'+$rpx[0].FullName+'"')) -WorkingDirectory $root
'@
    [IO.File]::WriteAllText((Join-Path $target 'Open-Private-Cemu.ps1'),$launch)
    [IO.File]::WriteAllText((Join-Path $target 'Open-Private-Cemu.cmd'),"@echo off`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Open-Private-Cemu.ps1`"`r`nif errorlevel 1 pause`r`n")
    # No original account, save, Cemu configuration or dump file is rewritten.
    if($Progress){& $Progress 'Finalizing verified private setup; original data is unchanged...'}
    $receipt=[ordered]@{schema=2;status='private_import_complete_not_hardware_verified';utc=[DateTime]::UtcNow.ToString('o');region=$plan.region;account=$plan.account;menu_title=$plan.menu_title;source_session=[IO.Path]::GetFileName($plan.dump);bytes=$plan.bytes;files=$plan.files.Count;full_mlc=[bool]$isFull;online_files_present=$plan.online_files_present;warnings=$plan.warnings;cemu_sha256=($runtimePlan.files | Where-Object relative -eq 'Cemu_release.exe').sha256;game_files_included=[bool]$isFull;game_saves_included=[bool]$isFull;pretendo_selected=[bool]$isFull;helper_network_service='Offline';main_release_modified=$false}
    $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $target 'IMPORT-COMPLETE.json') -Encoding UTF8
    # This inventory describes only the newly copied private data; it contains no key bytes.
    $plan.files | Select-Object relative,length,sha256 | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $target 'imported-files.json') -Encoding UTF8
    return $target
}
function Find-PrivateSystemImport {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Dump,[Parameter(Mandatory)][string]$Account,[string]$StorageRoot=$script:ToolRoot,[switch]$AllowPartialLegacy)
    $base=Assert-PlainPath (Join-Path (Assert-PlainPath $StorageRoot) 'PrivateImports')
    if(!(Test-Path -LiteralPath $base -PathType Container)){return}
    $session=[IO.Path]::GetFileName((Assert-PlainPath $Dump))
    foreach($directory in Get-ChildItem -LiteralPath $base -Directory | Sort-Object LastWriteTimeUtc -Descending){
        try{
            $candidate=Assert-PlainPath $directory.FullName
            $receiptPath=Assert-PlainPath (Join-Path $candidate 'IMPORT-COMPLETE.json')
            if(!(Test-Path -LiteralPath $receiptPath -PathType Leaf) -or (Get-Item -LiteralPath $receiptPath).Length -gt 1MB){continue}
            $receipt=Get-Content -LiteralPath $receiptPath -Encoding UTF8 -Raw | ConvertFrom-Json
            if(!$AllowPartialLegacy -and (!($receipt.PSObject.Properties.Name -contains 'full_mlc') -or !$receipt.full_mlc)){continue}
            if($receipt.source_session -cne $session -or $receipt.account -cne $Account -or $receipt.main_release_modified -ne $false -or $receipt.status -cne 'private_import_complete_not_hardware_verified'){continue}
            if($receipt.cemu_sha256 -cnotmatch '^[0-9A-F]{64}$' -or $receipt.menu_title -cnotmatch '^0005001010040[012]00$'){continue}
            $exe=Assert-PlainPath (Join-Path $candidate 'Cemu_release.exe')
            if((Get-FileHash -LiteralPath $exe).Hash -cne $receipt.cemu_sha256){continue}
            $settings=Get-Xml (Join-Path $candidate 'settings.xml')
            if([IO.Path]::GetFullPath($settings.content.mlc_path) -ine [IO.Path]::GetFullPath((Join-Path $candidate 'mlc01'))){continue}
            Assert-PlainPath (Join-Path $candidate 'mlc01') | Out-Null
            Assert-SystemResources $candidate
            $libraries=@(Get-NativeLibraryPlan $candidate)
            foreach($directory in 'native-home/cafeLibs','native-home/portable/cafeLibs','native-manual/cafeLibs','native-manual/portable/cafeLibs'){
                foreach($library in $libraries){
                    $path=Join-SafeRelative $candidate ($directory+'/'+[IO.Path]::GetFileName($library.relative))
                    Assert-PlainPath $path | Out-Null
                    if((Get-FileHash -LiteralPath $path).Hash -cne $library.sha256){throw 'Private helper native library is missing or differs.'}
                }
            }
            $regionCode=@{JPN='0';USA='1';EUR='2'}[[string]$receipt.region]
            if($null -eq $regionCode){continue}
            $health=Join-Path $candidate "mlc01/sys/title/00050010/1004e${regionCode}00"
            $healthXml=Get-Xml (Join-Path $health 'code/app.xml')
            if($healthXml.SelectSingleNode('//title_id').InnerText.Trim().ToLowerInvariant() -cne "000500101004e${regionCode}00"){continue}
            if(!(Test-Path -LiteralPath (Join-Path $health 'content') -PathType Container) -or !(Get-ChildItem -LiteralPath (Join-Path $health 'code') -Filter '*.rpx' -File)){continue}
            foreach($file in 'otp.bin','seeprom.bin','Open-Private-Cemu.ps1'){
                $path=Assert-PlainPath (Join-Path $candidate $file)
                if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw 'Incomplete private setup.'}
            }
            return $candidate
        }catch{continue}
    }
}
. (Join-Path $PSScriptRoot 'FullMlcImport.ps1')
Export-ModuleMember -Function Get-ReceivedTransferIdentity,Get-DumpPlan,New-PrivateSystemImport,Find-PrivateSystemImport
