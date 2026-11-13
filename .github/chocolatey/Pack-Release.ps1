param(
    [Parameter(Mandatory = $true)][string]$MetadataPath,
    [Parameter(Mandatory = $true)][string]$WorkDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
$tag = $metadata.source_version
if ($tag -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'Invalid stable release tag' }
$expectedUrl = "https://github.com/supermerill/SuperSlicer/releases/download/$tag/SuperSlicer_${tag}_win64.msi"
if ($metadata.msi_url -cne $expectedUrl -or $metadata.repository -cne 'supermerill/SuperSlicer') {
    throw 'MSI URL must identify the selected public upstream release'
}
New-Item -ItemType Directory -Path $WorkDirectory | Out-Null
$msi = Join-Path (Resolve-Path -LiteralPath $WorkDirectory).Path 'installer.msi'
Invoke-WebRequest -UseBasicParsing -Uri $expectedUrl -OutFile $msi
if ((Get-Item -LiteralPath $msi).Length -ne $metadata.size) { throw 'Release MSI size mismatch' }
$digest = (Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash.ToLowerInvariant()
if ($metadata.digest -and $metadata.digest -cne "sha256:$digest") { throw 'Release MSI digest mismatch' }
$signature = Get-AuthenticodeSignature -LiteralPath $msi
if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate) {
    throw 'Chocolatey publication requires a valid, publicly trusted MSI signature; self-signed test output is forbidden'
}

# Open the MSI database read-only; never install or execute the release here.
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $null
$view = $null
try {
    $database = $installer.OpenDatabase($msi, 0)
    $view = $database.OpenView('SELECT `Property`, `Value` FROM `Property`')
    $view.Execute()
    $properties = @{}
    while ($null -ne ($record = $view.Fetch())) {
        $properties[$record.StringData(1)] = $record.StringData(2)
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($record)
    }
    if ($properties['ProductName'] -cne 'SuperSlicer' -or $properties['Manufacturer'] -cne 'Supermerill' -or
        ($properties['ProductVersion'] -cne $tag -and $properties['ProductVersion'] -cne $metadata.package_version)) {
        throw 'MSI product name, publisher or version does not match this stable release'
    }
} finally {
    if ($null -ne $view) {
        $view.Close()
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view)
    }
    if ($null -ne $database) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($database) }
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
}

$renderer = Join-Path (Split-Path $PSScriptRoot -Parent) 'scripts/chocolatey.py'
& python -B $renderer stage --metadata $MetadataPath --msi $msi --destination $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Chocolatey package rendering failed' }
& choco pack (Join-Path $OutputDirectory 'superslicer.nuspec') --output-directory $OutputDirectory --no-progress
if ($LASTEXITCODE -ne 0) { throw 'Chocolatey packing failed' }
$expectedPackage = Join-Path $OutputDirectory "superslicer.$($metadata.package_version).nupkg"
if (-not (Test-Path -LiteralPath $expectedPackage -PathType Leaf)) { throw 'Expected normalized nupkg was not generated' }
