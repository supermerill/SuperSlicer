param(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    # When given, every binary must report exactly this ProductVersion.
    [string]$ExpectedProductVersion = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# The XML is also the CI allowlist, so a broad *.dll copy cannot sign upstream DLLs.
[xml]$configuration = Get-Content -LiteralPath "$PSScriptRoot/artifact-configuration-binaries.xml"
$namespaces = New-Object System.Xml.XmlNamespaceManager($configuration.NameTable)
$namespaces.AddNamespace('s', 'http://signpath.io/artifact-configuration/v1')
$fileSet = $configuration.SelectSingleNode('/s:artifact-configuration/s:zip-file/s:pe-file-set', $namespaces)
$version = $null
$files = foreach ($include in $fileSet.SelectNodes('s:include', $namespaces)) {
    $name = $include.GetAttribute('path')
    if ($name -notmatch '^[a-zA-Z0-9_.-]+\.(exe|dll)$') {
        throw "Expected a literal binary filename in the signing allowlist: $name"
    }
    $file = Get-Item -LiteralPath (Join-Path $SourceDirectory $name)
    if ($file.VersionInfo.ProductName -cne $fileSet.GetAttribute('product-name')) {
        throw "Unexpected ProductName on $name"
    }
    $productVersion = $file.VersionInfo.ProductVersion
    if ([string]::IsNullOrWhiteSpace($productVersion)) { throw "Missing ProductVersion on $name" }
    if ($null -ne $version -and $productVersion -cne $version) {
        throw "ProductVersion mismatch on $name"
    }
    if ($ExpectedProductVersion -and $productVersion -cne $ExpectedProductVersion) {
        throw "Unexpected ProductVersion on $name (expected $ExpectedProductVersion)"
    }
    $version = $productVersion
    $file
}
if (@($files).Count -ne 5) { throw 'Expected exactly five SuperSlicer binaries' }

# Fail if the directory already exists rather than accidentally reusing stale inputs.
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$files | Copy-Item -Destination $OutputDirectory
Write-Host "Signing five SuperSlicer binaries with ProductVersion $version"
if ($env:GITHUB_OUTPUT) {
    "product-version=$version" | Out-File -LiteralPath $env:GITHUB_OUTPUT -Encoding utf8 -Append
}
