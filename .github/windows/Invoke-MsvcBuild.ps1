param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Dependencies', 'Debug', 'Release', 'RelWithDebInfo')]
    [string]$Target
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$depsBuild = Join-Path $repository 'deps/build'
$prefix = Join-Path $depsBuild 'destdir/usr/local'

function Invoke-CMake([string[]]$Arguments) {
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) { throw "CMake failed with exit code $LASTEXITCODE" }
}

# CMake searches prefixes inferred from PATH (including <prefix>/bin).
# Keep the cleaned PATH through the build: ExternalProject configures dependencies
# in child processes, and MSBuild can rerun CMake while building the application.
$originalPath = $env:Path
$gnuPath = '(^|[/\\])(strawberry(-perl)?|mingw(-w64|32|64)?|msys(2|32|64)?|ucrt64|clang(32|64|arm64))($|[/\\])'
try {
    $env:Path = (($originalPath -split ';') | Where-Object { $_.Trim(' ', '"') -notmatch $gnuPath }) -join ';'
    $generator = @('-G', 'Visual Studio 17 2022', '-A', 'x64')

    if ($Target -eq 'Dependencies') {
        Invoke-CMake (@('-S', "$repository/deps", '-B', $depsBuild) + $generator)
        Invoke-CMake @('--build', $depsBuild, '--parallel')
    } else {
        $include = "$prefix/include"
        $gmp = "$prefix/lib/libgmp-10.lib"
        $mpfr = "$prefix/lib/libmpfr-4.lib"
        foreach ($file in @("$include/gmp.h", "$include/mpfr.h", $gmp, $mpfr,
                            "$prefix/bin/libgmp-10.dll", "$prefix/bin/libmpfr-4.dll")) {
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
                throw "Missing MSVC dependency: $file. Rebuild the Windows dependency cache."
            }
        }

        $build = Join-Path $repository 'build'
        # These are the inputs to CGAL 5.6.2's FindGMP and FindMPFR modules.
        # Both GMP configurations use the same import library from the cache.
        Invoke-CMake (@('-S', $repository, '-B', $build) + $generator + @(
            "-DCMAKE_PREFIX_PATH:PATH=$prefix"
            "-DGMP_INCLUDE_DIR:PATH=$include"
            "-DGMP_LIBRARY_RELEASE:FILEPATH=$gmp"
            "-DGMP_LIBRARY_DEBUG:FILEPATH=$gmp"
            "-DMPFR_INCLUDE_DIR:PATH=$include"
            "-DMPFR_LIBRARIES:FILEPATH=$mpfr"
        ))
        Invoke-CMake @('--build', $build, '--config', $Target, '--target', 'INSTALL', '--parallel')
    }
} finally {
    $env:Path = $originalPath
}
