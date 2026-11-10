function Get-FullMlcDumpPlan {
    param([object]$Identity,[string]$Region,[scriptblock]$Progress)
    $root=$Identity.dump;$info=$Identity.info
    $manifest=Assert-PlainPath (Join-Path $root 'full-mlc-manifest.jsonl')
    if(!(Test-Path -LiteralPath $manifest -PathType Leaf) -or (Get-Item -LiteralPath $manifest).Length -gt 256MB -or $info.manifest_sha256 -cnotmatch '^[0-9A-F]{64}$' -or (Get-FileHash -LiteralPath $manifest).Hash -cne $info.manifest_sha256){throw 'Full MLC inventory is missing, changed or oversized.'}
    if($info.entries -lt 1 -or $info.entries -gt 250000 -or $info.bytes -lt 0 -or $info.bytes -gt 1TB){throw 'Invalid full MLC inventory bounds.'}
    if($Progress){& $Progress 'Validating required system apps, account and Pretendo files...'}
    $base=Get-DumpPlan -Dump $root -Region $Region -AllowPartialLegacy
    if(!$base.online_files_present){throw 'Pretendo setup is incomplete: selected account needs a saved password and all console certificates. Select the linked PNID and enable Save password on the console before a new full dump.'}
    $actual=New-Object 'Collections.Generic.Dictionary[string,object]' ([StringComparer]::OrdinalIgnoreCase)
    $stack=New-Object 'Collections.Generic.Stack[string]';$stack.Push((Assert-PlainPath (Join-Path $root 'mlc01')))
    $actualDirectories=0
    while($stack.Count){
        $dir=$stack.Pop()
        $relative=$dir.Substring($root.Length+1).Replace('\','/')
        $actual.Add($relative,[pscustomobject]@{relative=$relative;path=$dir;directory=$true;length=[long]0})
        $actualDirectories++
        foreach($item in Get-ChildItem -LiteralPath $dir -Force){
            if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Linked file/directory in full MLC rejected.'}
            if($item.Name -match '[<>:"|?*]' -or $item.Name -match '[ .]$'){throw 'Unrepresentable filename in full MLC; nothing will be skipped.'}
            if($item.PSIsContainer){$stack.Push($item.FullName)}else{
                $rel=$item.FullName.Substring($root.Length+1).Replace('\','/')
                $actual.Add($rel,[pscustomobject]@{relative=$rel;path=$item.FullName;directory=$false;length=$item.Length})
            }
        }
        if($actual.Count -gt 250000 -or $stack.Count -gt 250000){throw 'Full MLC entry safety limit exceeded.'}
    }
    foreach($rel in @('otp.bin','seeprom.bin')+@($script:Policy.native_libraries | ForEach-Object {'cafeLibs/'+$_})){
        $p=Assert-PlainPath (Join-Path $root $rel)
        $actual.Add($rel,[pscustomobject]@{relative=$rel;path=$p;directory=$false;length=(Get-Item -LiteralPath $p).Length})
    }
    $seen=New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $files=New-Object 'Collections.Generic.List[object]';$directories=New-Object 'Collections.Generic.List[string]'
    $reader=New-Object IO.StreamReader($manifest,(New-Object Text.UTF8Encoding($false,$true)))
    $total=[long]0;$entries=0;$header=$null
    try{
        $line=$reader.ReadLine()
        if(!$line -or $line.Length -gt 8192){throw 'Invalid full MLC manifest header.'}
        $header=$line | ConvertFrom-Json
        if($header.schema -ne 2 -or $header.type -cne 'header' -or $header.scope -cne 'full_mlc' -or $header.account -cne $Identity.account -or $header.entries -ne $info.entries -or $header.bytes -ne $info.bytes){throw 'Full MLC manifest header differs from completion receipt.'}
        while($null -ne ($line=$reader.ReadLine())){
            if(!$line -or $line.Length -gt 8192 -or ++$entries -gt 250000){throw 'Invalid full MLC manifest entry.'}
            $entry=$line | ConvertFrom-Json
            if($entry.type -cnotin @('file','directory') -or $entry.sha256 -cnotmatch '^[0-9A-F]{64}$' -or $entry.size -lt 0 -or $entry.size -gt [uint32]::MaxValue -or !$seen.Add([string]$entry.path)){throw 'Invalid or duplicate full MLC manifest entry.'}
            $relative=[string]$entry.path
            $path=Join-SafeRelative $root $relative
            if(!$actual.ContainsKey($relative)){throw 'Full MLC inventory describes a missing path.'}
            $item=$actual[$relative]
            if($item.relative -cne $relative -or $item.directory -ne ($entry.type -ceq 'directory') -or $item.length -ne $entry.size){throw 'Full MLC path type/name/size mismatch.'}
            if($item.directory){
                if($entry.sha256 -cne 'E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855'){throw 'Invalid full MLC directory checksum.'}
                $directories.Add($relative)
            }else{
                $hash=(Get-FileHash -LiteralPath $path).Hash
                if($hash -cne $entry.sha256){throw 'Full MLC file differs from received checksum; incomplete/corrupt snapshot rejected.'}
                $files.Add([pscustomobject]@{relative=$relative;source=$path;length=$item.length;sha256=$hash})
                $total+=$item.length
            }
            if($Progress -and ($entries % 100) -eq 0){& $Progress "Verified full MLC inventory: $entries / $($info.entries) entries"}
        }
    }finally{$reader.Dispose()}
    if($entries -ne $info.entries -or $seen.Count -ne $actual.Count -or $total -ne $info.bytes -or $files.Count -ne $info.files -or $directories.Count -ne $info.directories -or !$seen.Contains('mlc01')){throw 'Full MLC inventory coverage/count/size mismatch. Nothing will be imported.'}
    foreach($dir in $base.directories){if(!$directories.Contains($dir)){$directories.Add($dir)}}
    $base.files=@($files.ToArray());$base.directories=@($directories.ToArray());$base.bytes=$total
    $base.game_files_included=$true;$base.game_saves_included=$true
    $base | Add-Member NoteProperty full_mlc $true
    $base | Add-Member NoteProperty full_inventory_entries $entries
    $base | Add-Member NoteProperty full_manifest_sha256 $info.manifest_sha256
    $base.warnings=@('Full internal MLC copied, including installed games, saves, all accounts and empty folders. Originals remain unchanged.','Selected account is configured for Pretendo in the private setup. Its PNID must already be linked on your Wii U; server login is still a user test.')
    return $base
}
