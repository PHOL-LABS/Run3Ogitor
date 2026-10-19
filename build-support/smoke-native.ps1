param(
    [Parameter(Mandatory=$true)][string]$Map,
    [Parameter(Mandatory=$true)][string]$ContentRoot,
    [string]$Overlay = '',
    [int]$TimeoutSeconds = 150
)
$ErrorActionPreference = 'Stop'
$engineRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$bin = "$engineRoot\build\run3ogitor-windows-msvc142\RunPath\bin"
$stem = "$engineRoot\build\ogitor-smoke-$([IO.Path]::GetFileName([IO.Path]::GetDirectoryName($Map)))"
$env:RUN3_EDITOR_CONTENT_ROOT = $ContentRoot
$env:RUN3_EDITOR_OVERLAYS = $Overlay
$env:RUN3_EDITOR_SMOKE = "$stem.png"
$process = Start-Process "$bin\qtOgitor.exe" -ArgumentList ('"' + $Map + '"') -WorkingDirectory $bin -WindowStyle Hidden -PassThru -RedirectStandardOutput "$stem.log" -RedirectStandardError "$stem-errors.log"
$null = $process.Handle
try {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while(!$process.WaitForExit(1000) -and [DateTime]::UtcNow -lt $deadline) {}
    if(!$process.HasExited) { throw "Native smoke test exceeded $TimeoutSeconds seconds. See $stem.log" }
    $process.WaitForExit()
    if($process.ExitCode -ne 0 -or !(Select-String -LiteralPath "$stem.log" -Pattern 'Run3 native smoke PASS' -Quiet)) {
        throw "Native smoke test failed (exit $($process.ExitCode)). See $stem-errors.log"
    }
    Write-Output "PASS: $Map; screenshot: $stem.png"
} finally {
    # Stop only the process created by this test, never another Ogitor session.
    if(!$process.HasExited) { Stop-Process -Id $process.Id }
}
