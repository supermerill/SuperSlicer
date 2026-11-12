param(
    [Parameter(Mandatory = $true)][string]$WorkDirectory,
    [Parameter(Mandatory = $true)][string]$RedistDirectory,
    [Parameter(Mandatory = $true)][string]$DumpbinPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Path $WorkDirectory | Out-Null
$WorkDirectory = (Resolve-Path -LiteralPath $WorkDirectory).Path
$script = Join-Path (Split-Path $PSScriptRoot -Parent) 'Copy-VcRuntime.ps1'
& cmake -S $PSScriptRoot -B "$WorkDirectory/build" -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Fixture CMake configuration failed' }
& cmake --build "$WorkDirectory/build" --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Fixture build failed' }

foreach ($name in @('runtime-probe', 'delay-probe')) {
    $package = "$WorkDirectory/$name"
    New-Item -ItemType Directory -Path $package | Out-Null
    Copy-Item -LiteralPath "$WorkDirectory/build/Release/$name.exe" -Destination $package
    # A stale debug runtime must not become an entry point or survive the cleanup.
    'not a redistributable runtime' | Set-Content -LiteralPath "$package/msvcp140d.dll"
    & $script -PackageDirectory $package -RedistDirectory $RedistDirectory -DumpbinPath $DumpbinPath
    if (-not (Test-Path "$package/msvcp140.dll")) { throw "Missing $name C++ runtime" }
    if (Test-Path "$package/msvcp140d.dll") { throw 'Stale debug runtime retained' }
    if (Test-Path "$package/msvcp140_2.dll") { throw 'Unreferenced runtime retained' }
    foreach ($file in Get-ChildItem -LiteralPath $package -Filter *.dll) {
        if ((Get-FileHash $file.FullName).Hash -ne (Get-FileHash "$RedistDirectory/$($file.Name)").Hash) {
            throw 'Runtime was not copied from the selected redistributable'
        }
    }
    & "$package/$name.exe"
    if ($LASTEXITCODE -ne 0) { throw "Packaged $name did not start" }
    # A second preparation is idempotent, including after an ordinary build relaunch.
    & $script -PackageDirectory $package -RedistDirectory $RedistDirectory -DumpbinPath $DumpbinPath
    Write-Host "PASS: $name normal/delay imports, redistributable source, stale cleanup, startup and repeat run"
}

$missing = "$WorkDirectory/missing-redist"
New-Item -ItemType Directory -Path $missing | Out-Null
Get-ChildItem -LiteralPath $RedistDirectory -File | Where-Object Name -NE 'msvcp140.dll' |
    Copy-Item -Destination $missing
$failed = $false
try {
    & $script -PackageDirectory "$WorkDirectory/runtime-probe" -RedistDirectory $missing -DumpbinPath $DumpbinPath
} catch {
    if ($_.Exception.Message -notlike '*Required runtime msvcp140.dll*') { throw }
    $failed = $true
}
if (-not $failed) { throw 'Missing redistributable was silently taken from System32 or stale package' }
Write-Host 'PASS: missing redistributable fails instead of using System32 or a stale copy'

& cmake --build "$WorkDirectory/build" --config Debug --target runtime-probe
if ($LASTEXITCODE -ne 0) { throw 'Debug fixture build failed' }
$debugPackage = "$WorkDirectory/debug-package"
New-Item -ItemType Directory -Path $debugPackage | Out-Null
Copy-Item -LiteralPath "$WorkDirectory/build/Debug/runtime-probe.exe" -Destination $debugPackage
$failed = $false
try {
    & $script -PackageDirectory $debugPackage -RedistDirectory $RedistDirectory -DumpbinPath $DumpbinPath
} catch {
    if ($_.Exception.Message -notlike '*Non-redistributable debug/CLR runtime requested*') { throw }
    $failed = $true
}
if (-not $failed) { throw 'A debug application was allowed to ship debug runtimes' }
Write-Host 'PASS: a debug binary is rejected instead of bundling non-redistributable debug runtimes'
