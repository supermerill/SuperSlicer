param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$Launcher,
    [Parameter(Mandatory = $true)][string]$WorkDirectory,
    [string]$MsiPath = '',
    [string]$ExpectedVersion = '',
    [string]$ZipPath = ''
)

# Compares the contents of a MSI built by the Windows workflow (-MsiPath) and/or of the
# portable ZIP (-ZipPath) with the package folder they were made from: exactly the same
# EXE/DLL files, same relative paths and SHA-256. The MSI must also have the expected
# ProductVersion. The MSI is unpacked by an administrative extraction (msiexec /a), refused
# if its administrative sequences schedule a custom action; it is not installed. This does
# not validate a normal installation or upgrade: that is a separate test in a VM.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem

$executable = '^\.(exe|dll)$'

# Map 'relative/path' (lower case, '/') -> SHA-256 for every EXE/DLL below $Root.
function Get-BinaryMap([string]$Root) {
    $root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    $map = @{}
    foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File -Force) {
        if ($file.Extension -notmatch $executable) { continue }
        $key = $file.FullName.Substring($root.Length + 1).Replace('\', '/').ToLowerInvariant()
        if ($map.ContainsKey($key)) { throw "Duplicate binary path: $key" }
        $map[$key] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $map
}

function Assert-SameBinaries([hashtable]$Expected, [hashtable]$Actual, [string]$What) {
    $missing = @($Expected.Keys | Where-Object { -not $Actual.ContainsKey($_) } | Sort-Object)
    $extra = @($Actual.Keys | Where-Object { -not $Expected.ContainsKey($_) } | Sort-Object)
    $changed = @($Expected.Keys | Where-Object { $Actual.ContainsKey($_) -and $Actual[$_] -cne $Expected[$_] } | Sort-Object)
    $problems = @()
    if ($missing) { $problems += "missing: $($missing -join ', ')" }
    if ($extra) { $problems += "unexpected: $($extra -join ', ')" }
    if ($changed) { $problems += "different: $($changed -join ', ')" }
    if ($problems) { throw "$What does not contain the package binaries ($($problems -join '; '))" }
}

# A ZIP path segment that Windows extracts under exactly this name.
function Test-WindowsName([string]$Segment) {
    return $Segment -ne '' -and $Segment -ne '.' -and $Segment -ne '..' -and $Segment -notmatch '[. ]$' -and
        $Segment -notmatch '[\x00-\x1F<>:"/\\|?*]'
}

if (-not $MsiPath -and -not $ZipPath) { throw 'Give -MsiPath, -ZipPath or both' }
if ($MsiPath -and -not $ExpectedVersion) { throw '-MsiPath requires -ExpectedVersion' }
if (Test-Path -LiteralPath $WorkDirectory) { throw "Work directory already exists: $WorkDirectory" }
New-Item -ItemType Directory -Path $WorkDirectory | Out-Null

# The package must at least contain the launcher and the five binaries we sign.
$package = Get-BinaryMap $PackageDirectory
# upload-artifact drops hidden paths from the portable artifact: such a binary would be lost.
$hidden = @($package.Keys | Where-Object { ($_.Split('/') | Where-Object { $_.StartsWith('.') }) } | Sort-Object)
if ($hidden) { throw "Binaries under a hidden path would be missing from the portable artifact: $($hidden -join ', ')" }
[xml]$allowlist = Get-Content -LiteralPath "$PSScriptRoot/../signpath/artifact-configuration-binaries.xml"
$required = @($Launcher) + @($allowlist.SelectNodes("//*[local-name()='include']") | ForEach-Object { $_.GetAttribute('path') })
foreach ($name in $required | Sort-Object -Unique) {
    if (-not $package.ContainsKey($name.ToLowerInvariant())) { throw "The package has no $name at its root" }
}

if ($MsiPath) {
$msi = (Resolve-Path -LiteralPath $MsiPath).Path
# ProductVersion, and no custom action that an administrative extraction would run.
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.OpenDatabase($msi, 0)
function Get-Column([string]$Query) {
    $view = $database.OpenView($Query)
    $values = @()
    try {
        [void]$view.Execute()
        while ($null -ne ($record = $view.Fetch())) {
            $values += $record.StringData(1)
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($record)
        }
    } finally {
        [void]$view.Close()
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view)
    }
    return $values
}
try {
    $tables = Get-Column 'SELECT `Name` FROM `_Tables`'
    $version = @(Get-Column "SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = 'ProductVersion'")
    if ($version.Count -ne 1 -or $version[0] -cne $ExpectedVersion) {
        throw "MSI ProductVersion is '$($version -join ',')', expected '$ExpectedVersion'"
    }
    if ($tables -contains 'CustomAction') {
        $custom = Get-Column 'SELECT `Action` FROM `CustomAction`'
        foreach ($sequence in 'AdminExecuteSequence', 'AdminUISequence') {
            if ($tables -notcontains $sequence) { continue }
            $scheduled = @(Get-Column "SELECT ``Action`` FROM ``$sequence``" | Where-Object { $custom -contains $_ })
            if ($scheduled) { throw "The MSI runs custom actions during administrative extraction: $($scheduled -join ', ')" }
        }
    }
} finally {
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($database)
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
    # msiexec needs the database closed.
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
}

$image = Join-Path $WorkDirectory 'admin'
$log = Join-Path $WorkDirectory 'extract.log'
$process = Start-Process -FilePath msiexec.exe -Wait -PassThru -ArgumentList @(
    '/a', "`"$msi`"", '/qn', '/norestart', "TARGETDIR=`"$image`"", '/L*v', "`"$log`"")
if ($process.ExitCode -ne 0) {
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 60 }
    throw "Administrative extraction of the MSI failed with exit code $($process.ExitCode)"
}
$launchers = @(Get-ChildItem -LiteralPath $image -Recurse -File -Force | Where-Object Name -ieq $Launcher)
if ($launchers.Count -ne 1) { throw "Expected one $Launcher in the MSI, found $($launchers.Count)" }
$installRoot = $launchers[0].DirectoryName
$outside = @(Get-ChildItem -LiteralPath $image -Recurse -File -Force | Where-Object {
    $_.Extension -match $executable -and -not $_.FullName.StartsWith("$installRoot\", [StringComparison]::OrdinalIgnoreCase) })
if ($outside) { throw "The MSI installs binaries outside the application folder: $($outside.Name -join ', ')" }
Assert-SameBinaries $package (Get-BinaryMap $installRoot) 'The MSI'
Write-Host "PASS: MSI ProductVersion $ExpectedVersion installs the $($package.Count) package binaries unchanged"
}

if ($ZipPath) {
    $zipMap = @{}
    $root = $null
    $archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ZipPath).Path)
    try {
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            # Every segment (a directory entry ends with '/') must be a plain Windows name.
            $segments = $name.TrimEnd('/').Split('/')
            if (@($segments | Where-Object { -not (Test-WindowsName $_) }).Count -ne 0) { throw "Unsafe ZIP entry: $name" }
            $parts = $name.Split('/', 2)
            if ($parts.Count -lt 2) { throw "ZIP entry outside the root folder: $name" }
            if ($null -eq $root) { $root = $parts[0] }
            if ($parts[0] -ine $root) { throw 'The portable ZIP must have exactly one root folder' }
            if ($name.EndsWith('/')) { continue }
            if ([IO.Path]::GetExtension($name) -ieq '.msi') { throw "The portable ZIP contains an installer: $name" }
            if ([IO.Path]::GetExtension($name) -notmatch $executable) { continue }
            $key = $parts[1].ToLowerInvariant()
            if ($zipMap.ContainsKey($key)) { throw "Duplicate ZIP entry: $name" }
            $stream = $entry.Open()
            try { $zipMap[$key] = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash } finally { $stream.Dispose() }
        }
    } finally { $archive.Dispose() }
    if ($null -eq $root) { throw 'The portable ZIP is empty' }
    Assert-SameBinaries $package $zipMap 'The portable ZIP'
    Write-Host 'PASS: the portable ZIP contains the same binaries as the MSI'
}
