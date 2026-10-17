$ErrorActionPreference = 'Stop'
$taskRuntime = $PSScriptRoot
$taskLogDirectory = Join-Path $taskRuntime 'logs'
$taskRunId = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss-fff')
$taskSession = Join-Path $taskLogDirectory "session-$taskRunId"
New-Item -ItemType Directory -Path $taskSession -Force | Out-Null
$taskLaunchLog = Join-Path $taskSession 'launch.log'
$taskStarted = Get-Date
@(
    "Started UTC: $($taskStarted.ToUniversalTime().ToString('o'))"
    "Executable: $(Join-Path $taskRuntime 'Spaghettify.exe')"
    "SHA256: $((Get-FileHash -LiteralPath (Join-Path $taskRuntime 'Spaghettify.exe')).Hash)"
) | Set-Content -LiteralPath $taskLaunchLog -Encoding UTF8
try {
    $taskProcess = Start-Process -FilePath (Join-Path $taskRuntime 'Spaghettify.exe') `
        -WorkingDirectory $taskRuntime -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $taskSession 'stdout.log') `
        -RedirectStandardError (Join-Path $taskSession 'stderr.log')
    $taskCode = $taskProcess.ExitCode
    Add-Content -LiteralPath $taskLaunchLog -Encoding UTF8 -Value @(
        "Ended UTC: $((Get-Date).ToUniversalTime().ToString('o'))"
        "Process ID: $($taskProcess.Id)"
        "Exit code: $taskCode"
    )
    foreach ($taskName in @('Spaghetti Kart.log', 'diagnostics.log')) {
        $taskPath = Join-Path $taskLogDirectory $taskName
        if (Test-Path -LiteralPath $taskPath) {
            Copy-Item -LiteralPath $taskPath -Destination (Join-Path $taskSession $taskName)
        }
    }
    Get-ChildItem -LiteralPath $taskLogDirectory -File | Where-Object {
        ($_.Name -like "crash-*-p$($taskProcess.Id)*" -or
         $_.Name -eq "crash-monitor-p$($taskProcess.Id).log" -or
         $_.Name -like 'diagnostics.*.log') -and $_.LastWriteTime -ge $taskStarted
    } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $taskSession $_.Name)
    }
    if ($taskCode -ne 0) {
        # Preserve the matching executable and symbols for later dump analysis.
        foreach ($taskName in @('Spaghettify.exe', 'Spaghettify.pdb')) {
            $taskPath = Join-Path $taskRuntime $taskName
            if (Test-Path -LiteralPath $taskPath) {
                Copy-Item -LiteralPath $taskPath -Destination (Join-Path $taskSession $taskName)
            }
        }
        $taskEvents = Get-WinEvent -FilterHashtable @{
            LogName = 'Application'; StartTime = $taskStarted; Id = @(1000,1001)
        } -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'Spaghettify' }
        $taskEvents | Format-List TimeCreated,Id,Message | Out-String |
            Set-Content -LiteralPath (Join-Path $taskSession 'windows-events.log') -Encoding UTF8
    }
    exit $taskCode
} catch {
    Add-Content -LiteralPath $taskLaunchLog -Encoding UTF8 -Value "Launcher error: $($_.Exception.Message)"
    exit 1
}
