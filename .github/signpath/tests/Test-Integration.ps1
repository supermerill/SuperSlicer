param(
    [Parameter(Mandatory = $true)][string]$WorkDirectory,
    [string]$WixDirectory = ''
)

# Windows-only smoke tests. Requires VS 2022, CMake, Windows SDK and WiX v3.
# Builds tiny fixtures, never SuperSlicer, installs nothing and calls no SignPath API.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($WixDirectory) {
    $candle = (Get-Item -LiteralPath "$WixDirectory/candle.exe").FullName
    $light = (Get-Item -LiteralPath "$WixDirectory/light.exe").FullName
    $heat = (Get-Item -LiteralPath "$WixDirectory/heat.exe").FullName
} else {
    $candle = (Get-Command candle.exe).Source
    $light = (Get-Command light.exe).Source
    $heat = (Get-Command heat.exe).Source
}
$wixUI = Join-Path (Split-Path $light -Parent) 'WixUIExtension.dll'

function Assert-Fails([scriptblock]$Action, [string]$Description) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (-not $failed) { throw "Expected failure: $Description" }
    Write-Host "PASS: rejects $Description"
}

function Assert-NoTemporaryTrust {
    if (Test-Path "Cert:\CurrentUser\Root\$($certificate.Thumbprint)") {
        throw 'Root trust was unexpectedly changed'
    }
}

New-Item -ItemType Directory -Path $WorkDirectory | Out-Null
$WorkDirectory = (Resolve-Path -LiteralPath $WorkDirectory).Path
$scripts = Split-Path $PSScriptRoot -Parent
& cmake -S $PSScriptRoot -B "$WorkDirectory/build" -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Fixture CMake configuration failed' }
& cmake --build "$WorkDirectory/build" --config RelWithDebInfo --parallel
if ($LASTEXITCODE -ne 0) { throw 'Fixture build failed' }

$fixtures = "$WorkDirectory/build/RelWithDebInfo"
$unsigned = "$WorkDirectory/unsigned"
& "$scripts/Prepare-Binaries.ps1" -SourceDirectory $fixtures -OutputDirectory $unsigned
if (@(Get-ChildItem -LiteralPath $unsigned).Count -ne 5 -or
    (Test-Path "$unsigned/libgmp-10.dll") -or (Test-Path "$unsigned/libmpfr-4.dll")) {
    throw 'Third-party DLLs were not excluded'
}
Write-Host 'PASS: exactly five binaries with consistent metadata, no third-party DLLs'

$missing = "$WorkDirectory/missing"
Copy-Item -LiteralPath $unsigned -Destination $missing -Recurse
Remove-Item -LiteralPath "$missing/OCCTWrapper.dll"
Assert-Fails { & "$scripts/Prepare-Binaries.ps1" -SourceDirectory $missing -OutputDirectory "$WorkDirectory/not-created" } 'missing input binary'
Copy-Item -LiteralPath "$fixtures/libgmp-10.dll" -Destination "$missing/OCCTWrapper.dll"
Assert-Fails { & "$scripts/Prepare-Binaries.ps1" -SourceDirectory $missing -OutputDirectory "$WorkDirectory/not-created" } 'wrong input metadata'

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending | Select-Object -First 1
if (-not $signtool) { throw 'signtool.exe not found' }

$certificate = $null
try {
    $certificate = New-SelfSignedCertificate -Subject "CN=SuperSlicer fixture $([guid]::NewGuid())" `
        -Type CodeSigningCert -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddDays(1)
    $signed = "$WorkDirectory/signed"
    Copy-Item -LiteralPath $unsigned -Destination $signed -Recurse
    foreach ($file in Get-ChildItem -LiteralPath $signed) {
        & $signtool.FullName sign /s My /sha1 $certificate.Thumbprint /fd SHA256 $file.FullName
        if ($LASTEXITCODE -ne 0) { throw "Fixture signing failed: $($file.Name)" }
    }

    $destination = "$WorkDirectory/package"
    New-Item -ItemType Directory -Path $destination | Out-Null
    $arguments = @{
        UnsignedDirectory = $unsigned
        SignedDirectory = $signed
        DestinationDirectory = $destination
    }
    Assert-Fails { & "$scripts/Update-SignedArtifacts.ps1" -UnsignedDirectory $unsigned -SignedDirectory $unsigned -DestinationDirectory $destination -TestCertificateThumbprint $certificate.Thumbprint } 'unsigned output'
    Assert-Fails { & "$scripts/Update-SignedArtifacts.ps1" @arguments } 'untrusted self-signed certificate without opt-in'
    Assert-Fails { & "$scripts/Update-SignedArtifacts.ps1" @arguments -TestCertificateThumbprint ('0' * 40) } 'wrong certificate pin'
    & "$scripts/Update-SignedArtifacts.ps1" @arguments -TestCertificateThumbprint $certificate.Thumbprint
    Assert-NoTemporaryTrust
    foreach ($file in Get-ChildItem -LiteralPath $signed) {
        if ((Get-FileHash $file.FullName).Hash -ne (Get-FileHash "$destination/$($file.Name)").Hash) {
            throw 'Signed copy differs'
        }
    }
    Write-Host 'PASS: pinned test signatures verified and copied without changing root trust'

    $incomplete = "$WorkDirectory/incomplete"
    Copy-Item -LiteralPath $signed -Destination $incomplete -Recurse
    Remove-Item -LiteralPath "$incomplete/Slic3r.dll"
    Assert-Fails { & "$scripts/Update-SignedArtifacts.ps1" -UnsignedDirectory $unsigned -SignedDirectory $incomplete -DestinationDirectory $destination } 'missing signed output'
    Copy-Item -LiteralPath "$signed/Slic3r.dll" -Destination $incomplete
    Copy-Item -LiteralPath "$fixtures/libgmp-10.dll" -Destination $incomplete
    Assert-Fails { & "$scripts/Update-SignedArtifacts.ps1" -UnsignedDirectory $unsigned -SignedDirectory $incomplete -DestinationDirectory $destination } 'unexpected signed output'

    $tampered = "$WorkDirectory/tampered"
    Copy-Item -LiteralPath $signed -Destination $tampered -Recurse
    $stream = [IO.File]::OpenWrite("$tampered/Slic3r.dll")
    try {
        $stream.Position = 1024
        $stream.WriteByte(123)
    } finally { $stream.Dispose() }
    $emptyDestination = "$WorkDirectory/empty-package"
    New-Item -ItemType Directory -Path $emptyDestination | Out-Null
    Assert-Fails {
        & "$scripts/Update-SignedArtifacts.ps1" -UnsignedDirectory $unsigned -SignedDirectory $tampered `
            -DestinationDirectory $emptyDestination -TestCertificateThumbprint $certificate.Thumbprint
    } 'tampered binary'
    Assert-NoTemporaryTrust
    if (@(Get-ChildItem -LiteralPath $emptyDestination).Count -ne 0) { throw 'Copied files before all checks passed' }

    # Build the actual MSI template with fixture binaries; never run this installer.
    $msiUnsigned = "$WorkDirectory/msi-unsigned"
    $msiSigned = "$WorkDirectory/msi-signed"
    New-Item -ItemType Directory -Path $msiUnsigned, $msiSigned | Out-Null
    $msiPackage = "$WorkDirectory/msi-package"
    Copy-Item -LiteralPath $signed -Destination $msiPackage -Recurse
    '{\rtf1\ansi SignPath signature verification fixture. Not for distribution.}' |
        Set-Content -LiteralPath "$msiPackage/LICENSE.rtf" -Encoding ASCII
    & $heat dir $msiPackage -dr INSTALLFOLDER -cg Package -gg -sreg -srd -var var.SourceDir -out "$WorkDirectory/files.wxs" -sfrag
    if ($LASTEXITCODE -ne 0) { throw 'Fixture MSI harvest failed' }
    & $candle "-dSourceDir=$msiPackage" '-dInstallerVersion=2.7.63.0' '-dReleaseSuffix=-Signature-Test' `
        "$WorkDirectory/build/winInstaller.wxs" "$WorkDirectory/files.wxs" -out "$WorkDirectory/"
    if ($LASTEXITCODE -ne 0) { throw 'Fixture MSI compilation failed' }
    & $light -b $msiPackage -ext $wixUI "$WorkDirectory/winInstaller.wixobj" "$WorkDirectory/files.wixobj" `
        -o "$msiUnsigned/SuperSlicer_fixture.msi" -spdb -sval -sw1076
    if ($LASTEXITCODE -ne 0) { throw 'Fixture MSI linking failed' }
    Copy-Item -LiteralPath "$msiUnsigned/SuperSlicer_fixture.msi" -Destination $msiSigned
    & $signtool.FullName sign /s My /sha1 $certificate.Thumbprint /fd SHA256 "$msiSigned/SuperSlicer_fixture.msi"
    if ($LASTEXITCODE -ne 0) { throw 'Fixture MSI signing failed' }
    & "$scripts/Update-SignedArtifacts.ps1" -UnsignedDirectory $msiUnsigned -SignedDirectory $msiSigned `
        -DestinationDirectory $destination -TestCertificateThumbprint $certificate.Thumbprint
    Assert-NoTemporaryTrust
    Write-Host 'PASS: signed MSI verified and copied'

    # Exercise normal public-root verification without SignPath or our test cert.
    $publiclySigned = "$WorkDirectory/publicly-signed"
    New-Item -ItemType Directory -Path $publiclySigned | Out-Null
    Copy-Item -LiteralPath $signtool.FullName -Destination $publiclySigned
    & "$scripts/Update-SignedArtifacts.ps1" -UnsignedDirectory $publiclySigned -SignedDirectory $publiclySigned `
        -DestinationDirectory $destination
    Write-Host 'PASS: normal Windows trust-chain verification on a Microsoft-signed SDK binary'
} finally {
    if ($null -ne $certificate) {
        Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -DeleteKey
    }
}
Write-Host 'All local SignPath integration smoke tests passed.'
