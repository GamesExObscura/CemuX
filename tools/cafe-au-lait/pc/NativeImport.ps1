#requires -Version 5.1
param([Parameter(Mandatory)][string]$JobDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$job=$null
$parentProcess=$null
$root=Split-Path $PSScriptRoot -Parent

function Assert-LocalPlainPath([string]$Path) {
    $full=[IO.Path]::GetFullPath($Path)
    if($full.StartsWith('\\')){throw 'Choose a local drive, not a network/device path.'}
    for($item=$full;$item;$item=[IO.Path]::GetDirectoryName($item.TrimEnd('\','/'))){
        if((Test-Path -LiteralPath $item) -and ((Get-Item -Force -LiteralPath $item).Attributes -band [IO.FileAttributes]::ReparsePoint)){
            throw 'Linked paths and cloud placeholders are not accepted.'
        }
    }
    return $full
}
function Write-Status([string]$Message,[string]$Pair='', [bool]$Finished=$false,[bool]$Success=$false,[string]$Import='') {
    $xml=New-Object Xml.XmlDocument
    $status=$xml.CreateElement('status')
    $status.SetAttribute('finished',$Finished.ToString().ToLowerInvariant())
    $status.SetAttribute('success',$Success.ToString().ToLowerInvariant())
    [void]$xml.AppendChild($status)
    foreach($field in @{message=$Message;pair=$Pair;import=$Import}.GetEnumerator()){
        $node=$xml.CreateElement($field.Key);$node.InnerText=$field.Value;[void]$status.AppendChild($node)
    }
    $temporary=Join-Path $JobDirectory 'status.next.xml'
    $destination=Join-Path $JobDirectory 'status.xml'
    $xml.Save($temporary)
    # PowerShell 5.1 otherwise converts $null to an invalid empty backup path.
    if(Test-Path -LiteralPath $destination){[IO.File]::Replace($temporary,$destination,[System.Management.Automation.Language.NullString]::Value)}else{[IO.File]::Move($temporary,$destination)}
}
function Assert-Active {
    if(Test-Path -LiteralPath (Join-Path $JobDirectory 'cancel')){throw [OperationCanceledException]::new('Transfer cancelled. Incomplete output will not be opened.')}
    if($parentProcess){
        $parentProcess.Refresh()
        if($parentProcess.HasExited){throw [OperationCanceledException]::new('Cemu closed. Incomplete output will not be opened.')}
    }
}
try {
    $JobDirectory=Assert-LocalPlainPath $JobDirectory
    $request=Join-Path $JobDirectory 'request.xml'
    if(!(Test-Path -LiteralPath $request -PathType Leaf) -or (Get-Item -LiteralPath $request).Length -gt 16384){throw 'Import request is missing or oversized.'}
    Assert-LocalPlainPath $request | Out-Null
    $acl=New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true,$false)
    foreach($sid in @([Security.Principal.WindowsIdentity]::GetCurrent().User,(New-Object Security.Principal.SecurityIdentifier 'S-1-5-18'))){
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow')))
    }
    Set-Acl -LiteralPath $JobDirectory -AclObject $acl
    $readerSettings=New-Object Xml.XmlReaderSettings
    $readerSettings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$readerSettings.XmlResolver=$null
    $reader=[Xml.XmlReader]::Create($request,$readerSettings)
    try{$xml=New-Object Xml.XmlDocument;$xml.XmlResolver=$null;$xml.Load($reader)}finally{$reader.Dispose()}
    if($xml.DocumentElement.Name -cne 'request' -or $xml.request.consent -cne 'true'){throw 'Trusted-LAN consent is required.'}
    $parentProcess=[Diagnostics.Process]::GetProcessById([int]$xml.request.parent)
    $region=$xml.DocumentElement.GetAttribute('region')
    if(!$region){$region='Auto'}
    if($region -cnotin @('Auto','JPN','USA','EUR')){throw 'Select Auto, JPN, USA or EUR for the console region.'}
    Assert-Active
    $storage=Assert-LocalPlainPath ([string]$xml.request.storage)
    if(!(Test-Path -LiteralPath $storage -PathType Container)){throw 'Choose an existing local storage folder.'}
    $address=$null
    if(![Net.IPAddress]::TryParse([string]$xml.request.address,[ref]$address)){throw 'Enter a private IPv4 address.'}
    Add-Type -AssemblyName System.Web.Extensions
    Add-Type -Path @((Join-Path $PSScriptRoot 'CafeAuLaitClient.cs'),(Join-Path $PSScriptRoot 'FullMlcClient.cs')) -ReferencedAssemblies System.Web.Extensions,System.Core
    if(![CafeAuLait.DownloadJob]::PrivateAddress($address)){throw 'Only private LAN IPv4 addresses are accepted.'}
    Import-Module (Join-Path $PSScriptRoot 'SystemImport.psm1') -Force
    $dumpBase=Assert-LocalPlainPath (Join-Path $storage 'NetworkDumps')
    [IO.Directory]::CreateDirectory($dumpBase) | Out-Null
    $dump=Join-Path $dumpBase ([guid]::NewGuid().ToString('N'))
    $job=New-Object CafeAuLait.DownloadJob (Join-Path $root 'policy.json')
    Write-Status 'Connecting to Wii U. Compare the pairing code and approve on the console.'
    $job.Start($address.ToString(),27581,$dump,$true)
    while(!$job.Finished){
        Assert-Active
        Write-Status ($job.Status+"`nEntries: $($job.FilesDone) / $($job.FileCount)`nReceived: $([math]::Round($job.BytesDone/1MB,1)) / $([math]::Round($job.TotalBytes/1MB,1)) MB") $job.PairCode
        Start-Sleep -Milliseconds 250
    }
    if(!$job.Complete){throw $job.Error}
    Assert-Active
    Write-Status 'Download verified. Checking and placing the full MLC in a new Cemu setup...' $job.PairCode
    $import=New-PrivateSystemImport -Dump $dump -Region $region -StorageRoot $storage -Progress {
        param($message)
        Assert-Active
        Write-Status $message $job.PairCode
    }
    Assert-Active
    Write-Status ("Import complete: $import`nSelected account is configured for Pretendo. Turn off the Wii U before using that account in Cemu.`nThis blank Cemu and all existing setups remain unchanged.") $job.PairCode $true $true $import
}
catch {
    $failure=$_.Exception.ToString()
    if($job){$job.Cancel()}
    try{Write-Status ("Import not completed: "+$_.Exception.Message+"`nDo not use incomplete output. Existing setups are unchanged.") '' $true $false}catch{
        try{[IO.File]::WriteAllText((Join-Path $JobDirectory 'worker-error.txt'),$failure+"`nStatus publication failure: "+$_.Exception.ToString())}catch{}
    }
    exit 1
}
finally {if($parentProcess){$parentProcess.Dispose()}}
