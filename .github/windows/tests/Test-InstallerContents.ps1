param(
    [Parameter(Mandatory = $true)][string]$WorkDirectory,
    [string]$WixDirectory = ''
)

# Windows-only fixture test of Test-InstallerPayload.ps1. Requires WiX v3 and Python.
# Builds small MSIs from the real WiX template with fake binaries; installs nothing.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not $WixDirectory) { $WixDirectory = Split-Path (Get-Command light.exe).Source -Parent }
$repository = (Resolve-Path -LiteralPath "$PSScriptRoot/../../..").Path
$checker = Join-Path $PSScriptRoot '../Test-InstallerPayload.ps1'
New-Item -ItemType Directory -Path $WorkDirectory | Out-Null
$WorkDirectory = (Resolve-Path -LiteralPath $WorkDirectory).Path
$version = '2.7.63.12'
$cases = 0

function New-Package([string]$Path) {
    New-Item -ItemType Directory -Path "$Path/mesa", "$Path/resources" | Out-Null
    # A real executable for the shortcut icon; the other binaries only need distinct bytes.
    Copy-Item -LiteralPath "$env:windir/System32/where.exe" -Destination "$Path/superslicer.exe"
    foreach ($name in 'superslicer_console.exe', 'superslicer-gcodeviewer.exe', 'Slic3r.dll', 'OCCTWrapper.dll', 'mesa/opengl32.dll') {
        Set-Content -LiteralPath "$Path/$name" -Value "fixture $name" -Encoding ASCII
    }
    Set-Content -LiteralPath "$Path/resources/profile.ini" -Value '[fixture]' -Encoding ASCII
    '{\rtf1\ansi Fixture license. Not for distribution.}' | Set-Content -LiteralPath "$Path/LICENSE.rtf" -Encoding ASCII
}

function New-Msi([string]$Package, [string]$Output) {
    $directory = Split-Path $Output -Parent
    & python -B "$repository/.github/scripts/windows_package.py" wxs --template "$repository/src/platform/msw/winInstaller.wxs.in" `
        --version-file "$repository/version.inc" --output "$directory/winInstaller.wxs" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'WiX source rendering failed' }
    & "$WixDirectory/heat.exe" dir $Package -dr INSTALLFOLDER -cg Package -gg -sreg -srd -var var.SourceDir -out "$directory/files.wxs" -sfrag | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Fixture harvest failed' }
    & "$WixDirectory/candle.exe" "-dSourceDir=$Package" "-dInstallerVersion=$version" '-dReleaseSuffix=-Payload-Test' `
        "$directory/winInstaller.wxs" "$directory/files.wxs" -out "$directory/" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Fixture compilation failed' }
    & "$WixDirectory/light.exe" -b $Package -ext "$WixDirectory/WixUIExtension.dll" "$directory/winInstaller.wixobj" "$directory/files.wixobj" `
        -o $Output -spdb -sval -sw1076 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Fixture linking failed' }
}

function New-Zip([string]$Package, [string]$Output, [string[]]$Roots = @('SuperSlicer_fixture'), [string]$Extra = '') {
    $archive = [IO.Compression.ZipFile]::Open($Output, 'Create')
    try {
        foreach ($root in $Roots) {
            foreach ($file in Get-ChildItem -LiteralPath $Package -Recurse -File) {
                $relative = $file.FullName.Substring($Package.Length + 1).Replace('\', '/')
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, "$root/$relative")
            }
        }
        if ($Extra) { [void]$archive.CreateEntry($Extra) }
    } finally { $archive.Dispose() }
}

function Invoke-Checker([string]$Msi, [string]$Package, [string]$Zip = '', [string]$Expected = $version) {
    $script:cases++
    $arguments = @{ MsiPath = $Msi; PackageDirectory = $Package; ExpectedVersion = $Expected; Launcher = 'superslicer.exe'
                    WorkDirectory = Join-Path $WorkDirectory "check-$script:cases" }
    if ($Zip) { $arguments.ZipPath = $Zip }
    & $checker @arguments | Out-Host
}

function Assert-Rejected([scriptblock]$Action, [string]$Pattern, [string]$Description) {
    $message = $null
    try { & $Action } catch { $message = $_.Exception.Message }
    if ($null -eq $message) { throw "Expected rejection: $Description" }
    if ($message -notmatch $Pattern) { throw "Rejected '$Description' for another reason: $message" }
    Write-Host "PASS: rejects $Description"
}

$package = Join-Path $WorkDirectory 'package'
New-Package $package
New-Item -ItemType Directory -Path "$WorkDirectory/msi" | Out-Null
$msi = Join-Path $WorkDirectory 'msi/SuperSlicer_fixture.msi'
New-Msi $package $msi
$zip = Join-Path $WorkDirectory 'portable.zip'
New-Zip $package $zip
Invoke-Checker $msi $package $zip

Assert-Rejected { Invoke-Checker $msi $package '' '2.7.63.0' } 'ProductVersion' 'a wrong MSI ProductVersion'

# The package changes after the MSI was built: one modified, one added, one removed binary.
$changed = Join-Path $WorkDirectory 'changed'
Copy-Item -LiteralPath $package -Destination $changed -Recurse
Set-Content -LiteralPath "$changed/Slic3r.dll" -Value 'rebuilt' -Encoding ASCII
Assert-Rejected { Invoke-Checker $msi $changed } 'different: slic3r\.dll' 'a modified binary'
Copy-Item -LiteralPath "$package/Slic3r.dll" -Destination "$changed/Slic3r.dll" -Force
Set-Content -LiteralPath "$changed/mesa/extra.dll" -Value 'extra' -Encoding ASCII
Assert-Rejected { Invoke-Checker $msi $changed } 'missing: mesa/extra\.dll' 'a binary missing from the MSI'
Remove-Item -LiteralPath "$changed/mesa/extra.dll"
Remove-Item -LiteralPath "$changed/mesa/opengl32.dll"
Assert-Rejected { Invoke-Checker $msi $changed } 'unexpected: mesa/opengl32\.dll' 'a binary the package does not have'
Remove-Item -LiteralPath "$changed/OCCTWrapper.dll"
Assert-Rejected { Invoke-Checker $msi $changed } 'no OCCTWrapper\.dll' 'a package without a signed binary'

# Portable ZIP problems.
$badZip = Join-Path $WorkDirectory 'two-roots.zip'
New-Zip $package $badZip @('one', 'two')
Assert-Rejected { Invoke-Checker $msi $package $badZip } 'one root folder' 'a ZIP with two root folders'
$badZip = Join-Path $WorkDirectory 'with-msi.zip'
New-Zip $package $badZip -Extra 'SuperSlicer_fixture/setup.msi'
Assert-Rejected { Invoke-Checker $msi $package $badZip } 'installer' 'a ZIP containing an MSI'
$badZip = Join-Path $WorkDirectory 'traversal.zip'
New-Zip $package $badZip -Extra 'SuperSlicer_fixture/../evil.dll'
Assert-Rejected { Invoke-Checker $msi $package $badZip } 'Unsafe ZIP entry' 'a ZIP path traversal'
$otherPackage = Join-Path $WorkDirectory 'other'
Copy-Item -LiteralPath $package -Destination $otherPackage -Recurse
Set-Content -LiteralPath "$otherPackage/superslicer_console.exe" -Value 'other build' -Encoding ASCII
$badZip = Join-Path $WorkDirectory 'other-build.zip'
New-Zip $otherPackage $badZip
Assert-Rejected { Invoke-Checker $msi $package $badZip } 'portable ZIP.*different: superslicer_console\.exe' 'a ZIP from another build'

# An MSI that runs a custom action during administrative extraction is refused before extraction.
$custom = Join-Path $WorkDirectory 'custom/SuperSlicer_custom.msi'
New-Item -ItemType Directory -Path (Split-Path $custom -Parent) | Out-Null
Copy-Item -LiteralPath $msi -Destination $custom
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.OpenDatabase($custom, 1)
try {
    foreach ($query in "INSERT INTO ``CustomAction`` (``Action``, ``Type``, ``Source``, ``Target``) VALUES ('FixtureAction', 51, 'FixtureProperty', '1')",
                       "INSERT INTO ``AdminExecuteSequence`` (``Action``, ``Sequence``) VALUES ('FixtureAction', 1450)") {
        $view = $database.OpenView($query)
        [void]$view.Execute()
        [void]$view.Close()
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view)
    }
    [void]$database.Commit()
} finally {
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($database)
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
    # Close the write handle before the checker opens the database.
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
}
Assert-Rejected { Invoke-Checker $custom $package } 'custom actions.*FixtureAction' 'a custom action in the administrative sequence'

Write-Host "All installer contents checks passed ($script:cases checker runs). Nothing installed."
