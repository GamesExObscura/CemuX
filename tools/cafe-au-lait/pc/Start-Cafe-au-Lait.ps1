#requires -Version 5.1
param([switch]$CheckLayout,[switch]$CheckReceivedTransfer,[switch]$CheckImportedSetup)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms,System.Drawing,System.Web.Extensions
if(!('CafeAuLait.DownloadJob' -as [type])){Add-Type -Path @((Join-Path $PSScriptRoot 'CafeAuLaitClient.cs'),(Join-Path $PSScriptRoot 'FullMlcClient.cs')) -ReferencedAssemblies System.Web.Extensions,System.Core}
Import-Module (Join-Path $PSScriptRoot 'SystemImport.psm1') -Force
$root=Split-Path $PSScriptRoot -Parent
[Windows.Forms.Application]::EnableVisualStyles()
$form=New-Object Windows.Forms.Form
$form.Text='Cafe au Lait - Full MLC and Pretendo Setup';$form.Size=New-Object Drawing.Size(800,730);$form.MinimumSize=$form.Size;$form.StartPosition='CenterScreen'
$form.BackColor=[Drawing.Color]::FromArgb(246,239,225)
$title=New-Object Windows.Forms.Label;$title.Text='Cafe au Lait';$title.Font=New-Object Drawing.Font('Georgia',23,[Drawing.FontStyle]::Bold);$title.SetBounds(24,18,730,48);$form.Controls.Add($title)
$intro=New-Object Windows.Forms.Label;$intro.Text="FULL internal MLC + online files. Includes all internal games, saves, accounts and empty folders.`r`nSelect your linked PNID in the Wii U app, then approve the read-only transfer.";$intro.SetBounds(26,76,735,42);$form.Controls.Add($intro)
$ip=New-Object Windows.Forms.TextBox;$ip.SetBounds(26,135,225,28);$form.Controls.Add($ip)
$region=New-Object Windows.Forms.ComboBox;$region.DropDownStyle='DropDownList';$region.SetBounds(267,135,95,28);foreach($r in 'Auto','JPN','USA','EUR'){[void]$region.Items.Add($r)};$region.SelectedIndex=0;$form.Controls.Add($region)
$connect=New-Object Windows.Forms.Button;$connect.Text='Connect to Wii U';$connect.SetBounds(380,132,172,32);$form.Controls.Add($connect)
$cancel=New-Object Windows.Forms.Button;$cancel.Text='Cancel transfer';$cancel.SetBounds(569,132,172,32);$cancel.Enabled=$false;$form.Controls.Add($cancel)
$consent=New-Object Windows.Forms.CheckBox;$consent.Text='Private test: I trust this home LAN. Account/console data is NOT encrypted, like FTP.';$consent.SetBounds(26,179,735,27);$form.Controls.Add($consent)
$pair=New-Object Windows.Forms.Label;$pair.Text='Pairing code appears here after connecting.';$pair.Font=New-Object Drawing.Font('Consolas',12,[Drawing.FontStyle]::Bold);$pair.SetBounds(26,218,735,30);$form.Controls.Add($pair)
$status=New-Object Windows.Forms.TextBox;$status.Multiline=$true;$status.ReadOnly=$true;$status.ScrollBars='Vertical';$status.SetBounds(26,264,715,245);$form.Controls.Add($status)
$status.Text="Nothing has been transferred.`r`n`r`nThe Wii U must approve your PC before data is sent. Use only your trusted home LAN.`r`n`r`nThe full MLC is received and verified, then placed in a NEW private Cemu setup. OTP/SEEPROM go in its active Cemu data folder; all MLC contents and accounts stay in their original relative folders. Selected account is configured for Pretendo. Console originals and main Cemu are not modified."
$install=New-Object Windows.Forms.Button;$install.Text='Create private Cemu setup';$install.SetBounds(26,529,240,38);$install.Enabled=$false;$form.Controls.Add($install)
$open=New-Object Windows.Forms.Button;$open.Text='Open imported private Cemu';$open.SetBounds(284,529,240,38);$open.Enabled=$false;$form.Controls.Add($open)
$storage=New-Object Windows.Forms.Button;$storage.Text='Choose storage folder...';$storage.SetBounds(26,588,240,34);$form.Controls.Add($storage)
$storageLabel=New-Object Windows.Forms.Label;$storageLabel.SetBounds(26,635,715,45);$form.Controls.Add($storageLabel)
$script:storageRoot=$root
$locationFile=Join-Path $root 'storage-location.txt'
if(Test-Path -LiteralPath $locationFile){$candidate=[IO.File]::ReadAllText($locationFile).Trim();if(Test-Path -LiteralPath $candidate -PathType Container){$script:storageRoot=$candidate}}
$storageLabel.Text='Storage: '+$script:storageRoot+"`r`nSpace is checked for both the received full MLC and the separate private import."
$script:job=$null;$script:download=$null;$script:privateCemu=$null;$script:importWorker=$null;$script:importAsync=$null;$script:importState=$null
function Update-ReceivedTransfer {
    $install.Enabled=$false;$open.Enabled=$false;$script:privateCemu=$null
    if(!$script:download){return}
    try{
        $identity=Get-ReceivedTransferIdentity -Dump $script:download
        if(!$identity.full_mlc){throw 'Old partial dump preserved. Install the updated full-MLC SD package and run a NEW transfer.'}
        $existing=Find-PrivateSystemImport -Dump $identity.dump -Account $identity.account -StorageRoot $script:storageRoot
        if($existing){
            $script:privateCemu=$existing;$open.Enabled=$true
            $status.Text="A completed private setup already exists for this received transfer:`r`n$existing`r`n`r`nClick Open imported private Cemu. No new transfer or import is needed. Native Menu/account/online operation still requires your verification."
            return
        }
        $status.Text="A completed FULL MLC transfer is available.`r`nSelected account ID preserved: $($identity.account)`r`n`r`nCreate private Cemu setup to validate every manifest path/checksum and the required system/account/certificate files, then place the complete MLC in its proper folders. Main Cemu is not changed."
        $install.Enabled=$true
    }catch{$status.Text='Transfer received, but not ready to import: '+$_.Exception.Message}
}
$timer=New-Object Windows.Forms.Timer;$timer.Interval=200
$timer.Add_Tick({
    if($script:importAsync){
        $status.Text=$script:importState.message
        if($script:importAsync.IsCompleted){
            try{
                $null=$script:importWorker.EndInvoke($script:importAsync)
                if($script:importState.error){throw $script:importState.error}
                if(!$script:importState.path){throw 'Private import did not complete.'}
                $script:privateCemu=$script:importState.path;$open.Enabled=$true
                $status.Text="FULL MLC placed and verified:`r`n$script:privateCemu`r`n`r`nSelected PNID is configured for Pretendo. Exit the console app and turn the Wii U off before opening private Cemu for Menu/login testing. Originals and main release are unchanged."
            }catch{$status.Text='Private import incomplete; do not use its output. '+$_.Exception.Message}
            finally{$script:importWorker.Dispose();$script:importWorker=$null;$script:importAsync=$null;$timer.Stop();$connect.Enabled=$true;$ip.Enabled=$true;$region.Enabled=$true;$storage.Enabled=$true;$cancel.Enabled=$false;$cancel.Text='Cancel transfer'}
        }
        return
    }
    if(!$script:job){return}
    $pair.Text=if($script:job.PairCode){'Compare on Wii U: '+$script:job.PairCode}else{'Connecting...'}
    $status.Text=$script:job.Status+"`r`n`r`nEntries: $($script:job.FilesDone) / $($script:job.FileCount)`r`nReceived: $([math]::Round($script:job.BytesDone/1MB,1)) / $([math]::Round($script:job.TotalBytes/1MB,1)) MB"
    if($script:job.Finished){
        $timer.Stop();$cancel.Enabled=$false;$connect.Enabled=$true;$ip.Enabled=$true;$region.Enabled=$true;$storage.Enabled=$true
        if($script:job.Complete){Update-ReceivedTransfer}
    }
})
$connect.Add_Click({
    try{
        if(!$consent.Checked){throw 'Approve trusted-LAN private testing first. This version is not encrypted.'}
        $script:job=New-Object CafeAuLait.DownloadJob((Join-Path $root 'policy.json'))
        $base=Join-Path $script:storageRoot 'NetworkDumps';[IO.Directory]::CreateDirectory($base) | Out-Null
        $script:download=Join-Path $base ([guid]::NewGuid().ToString('N'))
        $script:job.Start($ip.Text.Trim(),27581,$script:download,$consent.Checked)
        $connect.Enabled=$false;$ip.Enabled=$false;$region.Enabled=$false;$storage.Enabled=$false;$cancel.Enabled=$true;$install.Enabled=$false;$open.Enabled=$false;$timer.Start()
    }catch{$status.Text=$_.Exception.Message}
})
$cancel.Add_Click({if($script:importAsync){$script:importState.cancel=$true;$status.Text='Stopping private import safely; original data is unchanged.'}elseif($script:job){$script:job.Cancel()}})
$region.Add_SelectedIndexChanged({if(!$script:job -or $script:job.Finished){Update-ReceivedTransfer}})
$install.Add_Click({
    try{
        $install.Enabled=$false;$open.Enabled=$false;$connect.Enabled=$false;$ip.Enabled=$false;$region.Enabled=$false;$storage.Enabled=$false
        $script:importState=[hashtable]::Synchronized(@{message='Validating FULL MLC and required Pretendo files...';path=$null;error=$null;cancel=$false})
        $script:importWorker=[PowerShell]::Create()
        [void]$script:importWorker.AddScript({param($modulePath,$dump,$region,$destination,$state)
            try{
                Import-Module $modulePath -Force
                $progress={param($message) if($state.cancel){throw 'Import cancelled; originals unchanged.'};$state.message=$message}
                $state.path=New-PrivateSystemImport -Dump $dump -Region $region -StorageRoot $destination -Progress $progress
            }catch{$state.error=$_.Exception.Message}
        }).AddArgument((Join-Path $PSScriptRoot 'SystemImport.psm1')).AddArgument($script:download).AddArgument([string]$region.SelectedItem).AddArgument($script:storageRoot).AddArgument($script:importState)
        $script:importAsync=$script:importWorker.BeginInvoke();$cancel.Text='Cancel import';$cancel.Enabled=$true;$timer.Start()
    }catch{$status.Text='Private import failed; do not use incomplete output. '+$_.Exception.Message;$connect.Enabled=$true;$storage.Enabled=$true}
})
$open.Add_Click({if($script:privateCemu){
    $s=New-Object Diagnostics.ProcessStartInfo
    $s.FileName=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $s.Arguments='-NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $script:privateCemu 'Open-Private-Cemu.ps1')+'"'
    $s.WorkingDirectory=$script:privateCemu;$s.UseShellExecute=$false;$s.CreateNoWindow=$true
    [void][Diagnostics.Process]::Start($s)
}})
$storage.Add_Click({
    $dialog=New-Object Windows.Forms.FolderBrowserDialog;$dialog.Description='Choose a storage folder for the received full MLC and its separate private Cemu setup.';$dialog.SelectedPath=$script:storageRoot
    if($dialog.ShowDialog() -eq [Windows.Forms.DialogResult]::OK){
        try{
            $module=Get-Module SystemImport
            $script:storageRoot=& $module {param($p) Assert-PlainPath $p} $dialog.SelectedPath
            [IO.File]::WriteAllText($locationFile,$script:storageRoot)
            $storageLabel.Text='Storage: '+$script:storageRoot+"`r`nFull received dump and private import stay separate; originals are not overwritten."
            $script:download=$null;$script:privateCemu=$null;$install.Enabled=$false;$open.Enabled=$false
        }catch{$status.Text=$_.Exception.Message}
    };$dialog.Dispose()
})
$form.Add_FormClosing({param($sender,$e) if($script:importAsync -and !$script:importAsync.IsCompleted){$script:importState.cancel=$true;$e.Cancel=$true;$status.Text='Cancelling private copy safely; close this window after it stops.';return};if($script:job -and !$script:job.Finished){$script:job.Cancel()};$timer.Stop()})
if($CheckLayout){
    if([string]$region.SelectedItem -cne 'Auto'){throw 'Default region selection was lost.'}
    $null=$form.Handle;foreach($control in $form.Controls){$null=$control.Handle};$form.PerformLayout()
    $bitmap=New-Object Drawing.Bitmap($form.Width,$form.Height)
    $form.DrawToBitmap($bitmap,(New-Object Drawing.Rectangle(0,0,$form.Width,$form.Height)))
    $bitmap.Save((Join-Path $root 'tests/setup-ui.png'));$bitmap.Dispose();$form.Dispose();$timer.Dispose();'Cafe au Lait UI rendered without showing a window.';exit
}
$received=Join-Path $script:storageRoot 'NetworkDumps'
if(Test-Path -LiteralPath $received -PathType Container){
    $latest=Get-ChildItem -LiteralPath $received -Directory | Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'DUMP-COMPLETE.txt') -PathType Leaf} | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if($latest){$script:download=$latest.FullName;Update-ReceivedTransfer}
}
if($CheckReceivedTransfer){
    if(!$script:download -or (!$install.Enabled -and !$open.Enabled)){throw 'Saved completed transfer did not become import/open-ready.'}
    $form.Dispose();$timer.Dispose();'Saved completed transfer resumes and enables private import; no window shown or import created.';exit
}
if($CheckImportedSetup){
    if(!$script:privateCemu -or !$open.Enabled){throw 'Completed private setup was not restored into the wizard.'}
    $form.Dispose();$timer.Dispose();'Completed private setup restored; Open imported private Cemu enabled without launching it.';exit
}
[void]$form.ShowDialog();$form.Dispose();$timer.Dispose()
