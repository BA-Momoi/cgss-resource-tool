param(
    [Parameter(Mandatory = $true)]
    [string]$Destination
)

$ErrorActionPreference = 'Stop'

$dotnetRoot = Join-Path $env:ProgramFiles 'dotnet'
if (-not (Test-Path -LiteralPath (Join-Path $dotnetRoot 'dotnet.exe'))) {
    $dotnetCommand = Get-Command dotnet.exe -ErrorAction Stop
    $dotnetRoot = Split-Path -Parent $dotnetCommand.Source
}

function Get-LatestNet7Runtime([string]$frameworkName) {
    $frameworkRoot = Join-Path $dotnetRoot "shared\$frameworkName"
    $runtime = Get-ChildItem -LiteralPath $frameworkRoot -Directory |
        Where-Object { $_.Name -match '^7\.0\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
    if (-not $runtime) {
        throw "No .NET 7 runtime found for $frameworkName under $frameworkRoot"
    }
    return $runtime
}

function Copy-DirectoryContents([string]$source, [string]$destination) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Get-ChildItem -LiteralPath $source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $destination -Recurse -Force
    }
}

$coreRuntime = Get-LatestNet7Runtime 'Microsoft.NETCore.App'
$desktopRuntime = Get-LatestNet7Runtime 'Microsoft.WindowsDesktop.App'
$fxrRoot = Join-Path $dotnetRoot 'host\fxr'
$fxr = Get-ChildItem -LiteralPath $fxrRoot -Directory |
    Where-Object { $_.Name -match '^7\.0\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $fxr) {
    throw "No .NET 7 hostfxr found under $fxrRoot"
}

New-Item -ItemType Directory -Path $Destination -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $dotnetRoot 'dotnet.exe') -Destination $Destination -Force
Copy-DirectoryContents $fxr.FullName (Join-Path $Destination "host\fxr\$($fxr.Name)")
Copy-DirectoryContents $coreRuntime.FullName (Join-Path $Destination "shared\Microsoft.NETCore.App\$($coreRuntime.Name)")
Copy-DirectoryContents $desktopRuntime.FullName (Join-Path $Destination "shared\Microsoft.WindowsDesktop.App\$($desktopRuntime.Name)")

foreach ($license in @('LICENSE.txt', 'ThirdPartyNotices.txt')) {
    $licensePath = Join-Path $dotnetRoot $license
    if (Test-Path -LiteralPath $licensePath) {
        Copy-Item -LiteralPath $licensePath -Destination $Destination -Force
    }
}

$localDotnet = Join-Path $Destination 'dotnet.exe'
$runtimeList = (& $localDotnet --list-runtimes | Out-String)
if ($LASTEXITCODE -ne 0 -or
    $runtimeList -notmatch 'Microsoft\.NETCore\.App 7\.0\.' -or
    $runtimeList -notmatch 'Microsoft\.WindowsDesktop\.App 7\.0\.') {
    throw 'The bundled .NET 7 runtimes failed their local host check.'
}

Write-Host "Bundled .NET Core $($coreRuntime.Name) and Windows Desktop $($desktopRuntime.Name)."
