param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$WorkDirectory
)

# No Chocolatey module, network, installed software or real installer is used.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Path "$WorkDirectory/bin" -Force | Out-Null
$env:ChocolateyInstall = $WorkDirectory
$env:ChocolateyPackageName = 'superslicer'
$env:ChocolateyForceX86 = 'false'
$global:ChocoFixtureState = @{ bits = 64; received = $null }
function Get-OSArchitectureWidth { param($Compare) return $global:ChocoFixtureState.bits -eq $Compare }
function Install-ChocolateyPackage {
    param($packageName, $softwareName, $fileType, $url64bit, $checksum64, $checksumType64, $silentArgs, $validExitCodes)
    $global:ChocoFixtureState.received = $PSBoundParameters
}
function Assert-Fails([scriptblock]$Action, [string]$Pattern) {
    $failed = $false
    try { & $Action } catch {
        if ($_.Exception.Message -notlike $Pattern) { throw }
        $failed = $true
    }
    if (-not $failed) { throw "Expected failure: $Pattern" }
    if ($null -ne $global:ChocoFixtureState.received) { throw 'Called installer helper before rejecting unsafe input' }
}
$install = Join-Path $PackageDirectory 'tools/chocolateyInstall.ps1'
& $install
$received = $global:ChocoFixtureState.received
if ($received.fileType -ne 'msi' -or $received.silentArgs -ne '/qn /norestart' -or
    $received.checksumType64 -ne 'sha256' -or $received.checksum64 -notmatch '^[0-9A-F]{64}$' -or
    $received.url64bit -notmatch '^https://github.com/supermerill/SuperSlicer/releases/download/.+_win64\.msi$' -or
    (@($received.validExitCodes) -join ',') -ne '0,1641,3010') {
    throw 'Unexpected installer helper arguments'
}
$global:ChocoFixtureState.received = $null
$global:ChocoFixtureState.bits = 32
Assert-Fails { & $install } '*requires Windows x64*'
$global:ChocoFixtureState.bits = 64
$env:ChocolateyForceX86 = 'true'
Assert-Fails { & $install } '*requires Windows x64*'
$env:ChocolateyForceX86 = 'false'
'portable shim fixture' | Set-Content -LiteralPath "$WorkDirectory/bin/superslicer.exe"
Assert-Fails { & $install } '*Legacy portable SuperSlicer detected*'
if (-not (Test-Path "$WorkDirectory/bin/superslicer.exe")) { throw 'Legacy shim was deleted' }
Remove-Item -LiteralPath "$WorkDirectory/bin/superslicer.exe"
$portable = Join-Path $PackageDirectory 'tools/SuperSlicer_old'
New-Item -ItemType Directory -Path $portable | Out-Null
try {
    Assert-Fails { & $install } '*Legacy portable SuperSlicer detected*'
    if (-not (Test-Path -LiteralPath $portable)) { throw 'Portable directory was deleted' }
} finally { Remove-Item -LiteralPath $portable }
Write-Host 'PASS: MSI arguments, SHA-256, exit codes, architecture guards and nondestructive legacy detection'
