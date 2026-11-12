param(
    [Parameter(Mandatory = $true)][string]$UnsignedDirectory,
    [Parameter(Mandatory = $true)][string]$SignedDirectory,
    [Parameter(Mandatory = $true)][string]$DestinationDirectory,
    [AllowEmptyString()][string]$TestCertificateThumbprint = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$unsigned = @(Get-ChildItem -LiteralPath $UnsignedDirectory)
$signed = @(Get-ChildItem -LiteralPath $SignedDirectory)
if ($unsigned.Count -eq 0 -or @($unsigned | Where-Object PSIsContainer).Count -ne 0 -or
    @($signed | Where-Object PSIsContainer).Count -ne 0) {
    throw 'Expected non-empty, flat signing artifact directories'
}
if ($unsigned.Count -ne $signed.Count -or
    @(Compare-Object ($unsigned.Name | Sort-Object) ($signed.Name | Sort-Object)).Count -ne 0) {
    throw 'Signed artifact filenames do not exactly match the submitted files'
}
if (-not (Test-Path -LiteralPath $DestinationDirectory -PathType Container)) {
    throw "Destination directory does not exist: $DestinationDirectory"
}

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending | Select-Object -First 1
if (-not $signtool) { throw 'signtool.exe not found' }

$thumbprint = ($TestCertificateThumbprint -replace '\s', '').ToUpperInvariant()
if ($thumbprint -and $thumbprint -notmatch '^[0-9A-F]{40}$') {
    throw 'The test certificate SHA-1 thumbprint must contain exactly 40 hexadecimal characters'
}

if ($thumbprint -and -not ('SignPathTestSignature' -as [type])) {
    Add-Type -Path "$PSScriptRoot/Verify-TestSignature.cs"
}
foreach ($file in $signed) {
    if ($thumbprint) {
        # Test signatures cannot satisfy public-root trust. Pin the exact signer,
        # validate its dates and code-signing usage, and let WinVerifyTrust check
        # the file's signature/digest. Accept ONLY success or CERT_E_UNTRUSTEDROOT,
        # never generic UnknownError, HashMismatch or any other trust error.
        $certificate = (Get-AuthenticodeSignature -LiteralPath $file.FullName).SignerCertificate
        if ($null -eq $certificate -or $certificate.Thumbprint -ne $thumbprint -or
            $certificate.Subject -ne $certificate.Issuer) {
            throw "Wrong or missing self-signed test certificate on $($file.Name)"
        }
        $chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
        try {
            $chain.ChainPolicy.VerificationFlags = [Security.Cryptography.X509Certificates.X509VerificationFlags]::AllowUnknownCertificateAuthority
            $chain.ChainPolicy.RevocationMode = [Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
            [void]$chain.ChainPolicy.ApplicationPolicy.Add((New-Object Security.Cryptography.Oid('1.3.6.1.5.5.7.3.3')))
            if (-not $chain.Build($certificate)) { throw "Invalid test certificate on $($file.Name)" }
        } finally { $chain.Dispose() }
        $result = [SignPathTestSignature]::Verify($file.FullName)
        if ($result -ne 0 -and $result -ne 2148204809) { # 0x800B0109: CERT_E_UNTRUSTEDROOT
            throw ('Invalid test signature on {0}: 0x{1:X8}' -f $file.Name, $result)
        }
        Write-Host "Verified pinned test signature: $($file.Name)"
    } else {
        & $signtool.FullName verify /pa /all /v $file.FullName
        if ($LASTEXITCODE -ne 0) { throw "Invalid signature: $($file.Name)" }
    }
}

# No fallback to unsigned originals, and copy nothing until all signatures pass.
$signed | Copy-Item -Destination $DestinationDirectory -Force
