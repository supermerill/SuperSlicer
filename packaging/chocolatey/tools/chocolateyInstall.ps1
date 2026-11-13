$ErrorActionPreference = 'Stop'

if (-not (Get-OSArchitectureWidth -Compare 64) -or $env:ChocolateyForceX86 -eq 'true') {
    throw 'SuperSlicer MSI requires Windows x64; a 32-bit installation is not available.'
}

# Do not silently replace a portable installation or erase its profiles/settings.
$toolsDirectory = Split-Path -Parent $MyInvocation.MyCommand.Definition
$portableDirectories = @(Get-ChildItem -LiteralPath $toolsDirectory -Directory -Filter 'SuperSlicer_*')
$portableShim = Join-Path $env:ChocolateyInstall 'bin/superslicer.exe'
if ($portableDirectories.Count -gt 0 -or (Test-Path -LiteralPath $portableShim)) {
    throw 'Legacy portable SuperSlicer detected. Back up your settings and uninstall the old portable Chocolatey package before installing the MSI package. No files have been removed.'
}

$packageArgs = @{
    packageName    = $env:ChocolateyPackageName
    softwareName   = 'SuperSlicer'
    fileType       = 'msi'
    url64bit       = '@MSI_URL@'
    checksum64     = '@SHA256@'
    checksumType64 = 'sha256'
    silentArgs     = '/qn /norestart'
    validExitCodes = @(0, 1641, 3010)
}
Install-ChocolateyPackage @packageArgs
