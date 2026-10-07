param(
    [Parameter(Mandatory = $true)]
    [string]$AdbPath,
    [Parameter(Mandatory = $true)]
    [string]$Serial,
    [Parameter(Mandatory = $true)]
    [string]$RemotePath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [Parameter(Mandatory = $true)]
    [string]$StatusPath
)

$ErrorActionPreference = 'Stop'
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $AdbPath
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
foreach ($argument in @('-s', $Serial, 'exec-out', 'cat', $RemotePath)) {
    [void]$startInfo.ArgumentList.Add($argument)
}

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) {
    throw 'Unable to start ADB binary export.'
}

$stderrTask = $process.StandardError.ReadToEndAsync()
$stream = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $process.StandardOutput.BaseStream.CopyTo($stream)
}
finally {
    $stream.Dispose()
}

$process.WaitForExit()
$stderr = $stderrTask.GetAwaiter().GetResult().Trim()
if ($process.ExitCode -ne 0) {
    throw "ADB export failed with exit $($process.ExitCode): $stderr"
}

$file = Get-Item -LiteralPath $OutputPath
$result = [ordered]@{
    status = 'complete'
    outputPath = $file.FullName
    bytes = $file.Length
    sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    completedAt = [DateTimeOffset]::UtcNow.ToString('O')
}
$result | ConvertTo-Json | Set-Content -LiteralPath $StatusPath -Encoding utf8
