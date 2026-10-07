param([string]$Repository = (Split-Path $PSScriptRoot -Parent))

$ErrorActionPreference = 'Stop'
$repoPath = (Resolve-Path -LiteralPath $Repository).Path
$testPath = Join-Path $repoPath 'build_static\update_compat'
$caseBase = Join-Path $testPath ('cases-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $caseBase | Out-Null
$packagePath = Join-Path $testPath 'CGSS_ResourceTool.zip'
$newExeHash = (Get-FileHash -LiteralPath (Join-Path $repoPath 'build_static\CGSS_Script.exe')).Hash
$results = [Collections.Generic.List[object]]::new()

function New-Case([string]$Name, [string]$Directory = 'CGSS_ResourceTool') {
    $root = Join-Path $caseBase $Name
    $target = Join-Path $root $Directory
    New-Item -ItemType Directory -Path (Join-Path $target 'CGSS_DOWN') -Force | Out-Null
    foreach ($name in @('CGSS_Script.exe', 'master.mdb', 'master.mdb.sync', 'manifest_10133900.db', 'local-extra.txt', 'CGSS_DOWN\keep.bin')) {
        [IO.File]::WriteAllText((Join-Path $target $name), 'local-test-sentinel', [Text.Encoding]::ASCII)
    }
    return @{ Root = $root; Target = $target; Module = (Join-Path $target 'CGSS_Script.exe') }
}

function Invoke-Probe($Case, [string]$Variant, [string]$Title, [string]$Version, [int]$Expected) {
    $executable = Join-Path $testPath ($Variant + '_update_test.exe')
    $logPath = Join-Path $Case.Root 'probe.log'
    & $executable $Case.Module $packagePath $Title $Version "$Expected" *> $logPath
    if ($LASTEXITCODE -ne 0) { throw ('Updater probe failed; see ' + $logPath) }
    return (Get-Content -LiteralPath $logPath | Where-Object { $_ -like 'TEST result=*' } | Select-Object -Last 1)
}

function Invoke-Replacement($Case, [string]$Variant) {
    $root = (Resolve-Path -LiteralPath $Case.Root).Path
    $target = (Resolve-Path -LiteralPath $Case.Target).Path
    $safePrefix = [IO.Path]::GetFullPath($caseBase).TrimEnd('\') + '\'
    if (!$root.StartsWith($safePrefix, [StringComparison]::OrdinalIgnoreCase) -or
        !$target.StartsWith($safePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Replacement paths escaped the isolated test directory'
    }
    $scriptPath = Join-Path $root 'update.test.bat'
    $arguments = if ($Variant -eq 'legacy') {
        '/d /c ""' + $scriptPath + '" "' + $root + '" "CGSS_Script.exe""'
    } else {
        '/d /c ""' + $scriptPath + '" "' + $root + '" "' + $target + '" "CGSS_Script.exe" "2147483647""'
    }
    $process = Start-Process -FilePath $env:ComSpec -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $root 'replace.log') -RedirectStandardError (Join-Path $root 'replace-errors.log')
    if (!$process.WaitForExit(30000)) {
        $process.Kill()
        throw ('Isolated update script timed out: ' + $root)
    }
    if ($process.ExitCode -ne 0) { throw ('Replacement script failed: ' + $root) }
    $actualTarget = if ($Variant -eq 'legacy') { Join-Path $root 'CGSS_ResourceTool' } else { $target }
    $restartPath = Join-Path $actualTarget 'restarted-version.txt'
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if ((Test-Path -LiteralPath $restartPath) -and (Get-Item -LiteralPath $restartPath).Length -gt 0) { break }
        Start-Sleep -Milliseconds 100
    }
    if (!(Test-Path -LiteralPath $restartPath) -or (Get-Content -LiteralPath $restartPath -Raw).Trim() -ne '1.70') {
        throw ('Restarted program version was not 1.70: ' + $root)
    }
    if ((Get-FileHash -LiteralPath (Join-Path $actualTarget 'CGSS_Script.exe')).Hash -ne $newExeHash) { throw 'Replacement EXE hash mismatch' }
    foreach ($name in @('spine_preview\preview.html', 'spine_preview\cgss_skel_parser.js', 'AssetStudio\AssetStudio.CLI.exe', 'dotnet\dotnet.exe', 'ffmpeg.exe', 'acb2wavs.exe', 'stage_live_map.csv')) {
        if (!(Test-Path -LiteralPath (Join-Path $actualTarget $name))) { throw ('Missing installed dependency: ' + $name) }
    }
    if ((Get-Content -LiteralPath (Join-Path $target 'CGSS_DOWN\keep.bin') -Raw) -ne 'local-test-sentinel') { throw 'Downloaded user resource was changed' }
    return $actualTarget
}

$legacy = New-Case 'legacy normal installation'
$probe = Invoke-Probe $legacy 'legacy' 'v1.7' '1.61' 2
$actual = Invoke-Replacement $legacy 'legacy'
$results.Add([pscustomobject]@{ Case = 'v1.61 to v1.7, standard installation'; Result = 'PASS'; Probe = $probe; RestartedVersion = '1.70'; DownloadsPreserved = $true; OtherLocalFilesPreserved = (Test-Path -LiteralPath (Join-Path $actual 'local-extra.txt')) })

$current = New-Case 'current updater'
$probe = Invoke-Probe $current 'current' 'v1.7' '1.61' 2
$actual = Invoke-Replacement $current 'current'
foreach ($name in @('master.mdb', 'master.mdb.sync', 'manifest_10133900.db', 'local-extra.txt')) {
    if ((Get-Content -LiteralPath (Join-Path $actual $name) -Raw) -ne 'local-test-sentinel') { throw ('Current updater changed local data: ' + $name) }
}
$results.Add([pscustomobject]@{ Case = 'Current updater replacement and data preservation'; Result = 'PASS'; Probe = $probe; RestartedVersion = '1.70'; LocalDatabasesPreserved = $true })

$same = New-Case 'same version'
$probe = Invoke-Probe $same 'current' 'v1.7' '1.7' 0
$results.Add([pscustomobject]@{ Case = 'No repeated update after installing v1.7'; Result = 'PASS'; Probe = $probe })

foreach ($title in @('CGSS Resource Tool v1.7', 'v1.6.2')) {
    $negative = New-Case ('invalid-title-' + $results.Count)
    $probe = Invoke-Probe $negative 'legacy' $title '1.61' 0
    $results.Add([pscustomobject]@{ Case = ('Legacy incompatible Release title: ' + $title); Result = 'CONFIRMED_NOT_DETECTED'; Probe = $probe })
}

$renamed = New-Case 'renamed installation' 'MyCGSSTool'
$oldHash = (Get-FileHash -LiteralPath $renamed.Module).Hash
$probe = Invoke-Probe $renamed 'legacy' 'v1.7' '1.61' 2
$actual = Invoke-Replacement $renamed 'legacy'
if ((Get-FileHash -LiteralPath $renamed.Module).Hash -ne $oldHash -or $actual -eq $renamed.Target) { throw 'Expected legacy renamed-folder limitation did not reproduce' }
$results.Add([pscustomobject]@{ Case = 'Legacy renamed installation directory'; Result = 'CONFIRMED_WRONG_TARGET'; Probe = $probe; OriginalExecutableUnchanged = $true })

$live = New-Case 'live GitHub metadata'
$probe = Invoke-Probe $live 'legacy' '--live' '1.61' 0
$publishedVersion = Get-Content -LiteralPath (Join-Path $live.Root 'probe.log') | Where-Object { $_ -match '^Version:' } | Select-Object -First 1
if (!$publishedVersion) { throw 'Live release version was not detected' }
$results.Add([pscustomobject]@{ Case = 'Unmodified legacy WinHTTP metadata request to GitHub'; Result = 'PASS'; Probe = $probe; PublishedVersion = $publishedVersion.Substring(8).Trim() })

$reportPath = Join-Path $testPath 'report.json'
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding UTF8
$results | Format-Table Case, Result -AutoSize
Write-Output ('Report: ' + $reportPath)
