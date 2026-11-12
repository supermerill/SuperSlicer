param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [string]$RedistDirectory = '',
    [string]$DumpbinPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PackageDirectory = (Get-Item -LiteralPath $PackageDirectory).FullName
if (-not $DumpbinPath) { $DumpbinPath = (Get-Command dumpbin.exe).Source }
if (-not $RedistDirectory) {
    if (-not $env:VCToolsRedistDir) { throw 'VCToolsRedistDir is missing; initialize the MSVC environment first' }
    $directories = @(Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory |
        Where-Object Name -Match '^Microsoft\.VC\d+\.CRT$')
    if ($directories.Count -ne 1) { throw 'Expected exactly one x64 MSVC CRT redistributable directory' }
    $RedistDirectory = $directories[0].FullName
}

# Do not scan all the copied runtimes as entry points: that would retain unused ones.
# Scan every other packaged PE, including dynamically loaded Slic3r, OCCT and Mesa.
# dumpbin /dependents includes normal and delay-load import tables.
$runtimePattern = '^(?:msvcp|msvcr|vcruntime|concrt|vcomp)\d+[^\\/:]*\.dll$'
$packageFiles = @(Get-ChildItem -LiteralPath $PackageDirectory -Recurse -File |
    Where-Object { $_.Extension -in '.exe', '.dll' })
$roots = @($packageFiles | Where-Object { $_.Name -notmatch $runtimePattern })
if ($roots.Count -eq 0) { throw 'No application binaries found in the package' }
$available = @{}
foreach ($file in Get-ChildItem -LiteralPath $RedistDirectory -File) { $available[$file.Name] = $file }
$localNames = @{}
foreach ($file in $packageFiles) { $localNames[$file.Name] = $true }
$queue = New-Object 'Collections.Generic.Queue[string]'
foreach ($file in $roots) { $queue.Enqueue($file.FullName) }
$visited = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$required = @{}
$importers = @{}
while ($queue.Count -gt 0) {
    $path = $queue.Dequeue()
    if (-not $visited.Add($path)) { continue }
    $lines = & $DumpbinPath /nologo /dependents $path
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect PE dependencies: $path" }
    foreach ($line in $lines) {
        if ($line -notmatch '^\s+([a-zA-Z0-9_.-]+\.dll)\s*$') { continue }
        $name = $Matches[1].ToLowerInvariant()
        if ($name -match $runtimePattern) {
            if ($name -match 'd(?:_|\.dll$)|_clr\d+\.dll$') {
                throw "Non-redistributable debug/CLR runtime requested: $name"
            }
            if (-not $available.ContainsKey($name)) {
                throw "Required runtime $name was not found in the selected redistributable: $RedistDirectory"
            }
            if (-not $required.ContainsKey($name)) {
                $required[$name] = $available[$name]
                $importers[$name] = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
                $queue.Enqueue($available[$name].FullName)
            }
            [void]$importers[$name].Add([IO.Path]::GetFileName($path))
        } elseif (-not $localNames.ContainsKey($name) -and
                  $name -notmatch '^(api-ms-|ext-ms-)' -and
                  -not (Test-Path -LiteralPath (Join-Path "$env:SystemRoot/System32" $name))) {
            throw "Missing dependency $name imported by $path"
        }
    }
}

# Modify only the generated package, and only after resolving the complete closure.
# Never copy a broad System32 wildcard (it can include non-redistributable debug DLLs).
foreach ($file in Get-ChildItem -LiteralPath $PackageDirectory -File) {
    if ($file.Name -match $runtimePattern -and -not $required.ContainsKey($file.Name)) {
        Remove-Item -LiteralPath $file.FullName
    }
}
foreach ($name in $required.Keys | Sort-Object) {
    Copy-Item -LiteralPath $required[$name].FullName -Destination $PackageDirectory -Force
    Write-Host "VC runtime: $name (imported by $(@($importers[$name] | Sort-Object) -join ', '))"
}
