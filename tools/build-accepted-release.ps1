param(
    [int]$TimeoutSeconds = 1200,
    [string]$AuditDirectory,
    [string]$RuntimeOutputDirectory
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$audit = if ($AuditDirectory) { [IO.Path]::GetFullPath($AuditDirectory) } else {
    Join-Path $repo ('artifacts/source-build-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$output = if ($RuntimeOutputDirectory) { [IO.Path]::GetFullPath($RuntimeOutputDirectory) } else {
    Join-Path $repo 'build/validated-release'
}
New-Item -ItemType Directory -Path $audit -Force | Out-Null
function Get-SourceFingerprint {
    $rows=Get-ChildItem -LiteralPath "$repo/src" -File -Recurse | ForEach-Object {
        $relative=$_.FullName.Substring("$repo/src/".Length).Replace('\','/')
        "$relative`:$((Get-FileHash -LiteralPath $_.FullName).Hash)"
    } | Sort-Object
    $data=[Text.Encoding]::UTF8.GetBytes(($rows -join "`n"))
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($data))
}
$sourceFingerprint=Get-SourceFingerprint
[ordered]@{status='building';source_fingerprint=$sourceFingerprint} | ConvertTo-Json |
    Set-Content -LiteralPath "$audit/build-execution.json"
$vs = 'C:/Program Files/Microsoft Visual Studio/2022/Community'
$git = if (Get-Command git -ErrorAction SilentlyContinue) { Split-Path (Get-Command git).Source } else { '' }
$dev = '"' + (Join-Path $vs 'Common7/Tools/VsDevCmd.bat') + '" -arch=x64 -host_arch=x64 >nul && set'
foreach ($line in (& $env:COMSPEC /d /s /c $dev)) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$env:PATH = $git + ';' + $env:PATH
& 'C:/Program Files/CMake/bin/cmake.exe' -S $repo -B "$repo/build" -DVCPKG_MANIFEST_INSTALL=OFF `
    "-DCEMU_RUNTIME_OUTPUT_DIRECTORY=$output" `
    '-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG:FULL /INCREMENTAL:NO' *> "$audit/configure.log"
if ($LASTEXITCODE -ne 0) { throw 'Main source configuration failed; see configure.log.' }
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = Join-Path $vs 'MSBuild/Current/Bin/MSBuild.exe'
$start.WorkingDirectory = $repo
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
# Normalize duplicate Path/PATH keys from the inherited Windows tool environment.
$start.Environment.Clear()
foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
    $start.Environment[$entry.Key.ToUpperInvariant()] = [string]$entry.Value
}
$arguments = @(
    (Join-Path $repo 'build/src/CemuBin.vcxproj'), '/t:Build',
    '/p:Configuration=Release', '/p:Platform=x64', '/m:1', '/nr:false',
    '/v:minimal', '/nologo', "/flp:logfile=$audit/main-build.log;verbosity=normal"
)
foreach ($argument in $arguments) { $start.ArgumentList.Add($argument) }
$process = [Diagnostics.Process]::Start($start)
$started = [DateTime]::UtcNow
[ordered]@{pid=$process.Id;started=$started.ToString('o');arguments=$arguments} |
    ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$audit/build-launch.json" -Encoding utf8
while (!$process.WaitForExit(30000)) {
    $elapsed = ([DateTime]::UtcNow - $started).TotalSeconds
    Write-Output ("Build running: {0:N0}s; owned MSBuild PID {1}" -f $elapsed, $process.Id)
    if ($elapsed -gt $TimeoutSeconds) {
        $process.Kill($true)
        $process.WaitForExit()
        throw 'Timed out; stopped only the build process tree launched by this script.'
    }
}
if ($process.ExitCode -ne 0) {
    [ordered]@{status='failed';exit_code=$process.ExitCode;source_fingerprint=$sourceFingerprint} | ConvertTo-Json |
        Set-Content -LiteralPath "$audit/build-execution.json"
    throw "Main source build failed: $($process.ExitCode)"
}
$binary = Join-Path $output 'Cemu_release.exe'
$symbols = Join-Path $output 'Cemu_release.pdb'
if((Get-SourceFingerprint) -ne $sourceFingerprint){throw 'Source changed during build; do not package this output'}
[ordered]@{status='passed';exit_code=0;source_fingerprint=$sourceFingerprint;finished=[DateTime]::UtcNow.ToString('o');executable_sha256=(Get-FileHash -LiteralPath $binary).Hash;symbols_sha256=(Get-FileHash -LiteralPath $symbols).Hash} | ConvertTo-Json |
    Set-Content -LiteralPath "$audit/build-execution.json"
Get-FileHash -LiteralPath $binary,$symbols
