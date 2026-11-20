# Windows DLL packaging

Both nightly (`ccpp_win.yml`) and release (`ccpp_win_rc.yml`) call
`Copy-VcRuntime.ps1` after collecting the application files and before signing/packaging.
It scans the normal and delay-load imports of **all** packaged EXE/DLL files, then
recursively resolves Microsoft VC runtime imports from the x64 CRT redistributable
selected by `VCToolsRedistDir`. The MSVC developer environment must be initialized.

Do not copy `msvcp140*` or `VCRUNTIME140*` from `System32`: these wildcards can pick up
debug, CLR and unused runtime variants installed on the build machine. Debug CRTs are
not redistributable. Missing runtimes fail the package build rather than relying on
whatever happens to be installed on the runner.

The script changes only the generated package. It also removes stale, unreferenced
VC runtime copies at the package root. Third-party DLLs remain outside the SignPath
allowlist and retain any upstream signatures.

## Audited example (not a permanent runtime allowlist)

The local `SuperSlicer_2.7.62.1-beta_win64_260505` release was inspected with MSVC
`dumpbin /dependents` and `/imports`:

| File | Why it is needed |
| --- | --- |
| `Slic3r.dll` | Application core, explicitly loaded by the three launchers |
| `OCCTWrapper.dll` | STEP import, explicitly loaded on demand |
| `libgmp-10.dll`, `libmpfr-4.dll` | Imported by `Slic3r.dll`; MPFR also imports GMP |
| `msvcp140.dll` | Imported by the application binaries and OCCT |
| `msvcp140_codecvt_ids.dll` | Imported by `Slic3r.dll` |
| `vcruntime140.dll`, `vcruntime140_1.dll` | Imported by the application binaries and the C++ runtime |
| `mesa/opengl32.dll` | Optional software renderer for `--sw-renderer`, remote sessions and insufficient OpenGL support |

That release contained 15 additional unreferenced Microsoft runtime files (2,980,560
bytes): debug variants, CLR variants, `msvcp140_1.dll`, `msvcp140_2.dll`,
`msvcp140_atomic_wait.dll` and `vcruntime140_threads.dll`.

**Recompute for each build.** Another local, newer Slic3r build imports
`msvcp140_2.dll`; removing it unconditionally would break that build. Do not infer
unnecessary DLLs merely from one launcher EXE: include the two dynamically loaded
project DLLs and their transitive dependencies.

Mesa must be copied to `package/mesa/opengl32.dll`, not flattened into the package root
and deleted. It is not required with a working hardware OpenGL driver, but removing
it disables the software-rendering fallback. The Mesa DLL in dependency release 1.8
imports only Windows system libraries and does not add a VC runtime dependency.
Windows system DLLs and UCRT/API-set contracts are supplied by the supported OS, not
copied wholesale from the build machine. Dynamically loaded application modules need
to be included explicitly when adding new plugins/features; import tables alone do
not reveal `LoadLibrary` calls.

## Local test

With VS 2022, CMake and the Windows SDK installed:

```powershell
./.github/windows/tests/Test-Packaging.ps1 -WorkDirectory <new-test-directory> -RedistDirectory <x64-Microsoft.VC143.CRT-directory> -DumpbinPath <dumpbin.exe>
```

Builds tiny normal/delay-import C++ executables, checks runtime selection, removal of
stale debug copies, source hashes, startup, repeatability and missing-redist failure.

## MSVC dependency discovery on GitHub Actions

The Windows nightly, RC and Debug use `Invoke-MsvcBuild.ps1` for the dependency build
and the main configure/build. The script strips Strawberry Perl/MinGW/MSYS tool
directories from the process `PATH` so that CMake and its dependency sub-builds
do not discover their GNU archives. Perl setup is not needed by these workflows.

The main build checks that the GMP/MPFR headers, MSVC import libraries and
matching DLLs exist in `deps/build/destdir/usr/local`; it explicitly passes the
headers and `.lib` files to CGAL, using the same GMP import library in Debug and
Release. The CMake log should report `libgmp-10.lib` and `libmpfr-4.lib` in that
prefix, not `libgmp.a` or `libmpfr.a` from the runner. The shared cache key includes
the dependency recipes and build script and does not fall back to `win_2.7.63`.

The cache contains both Release and Debug dependencies. The `debug_win` workflow
builds in `Debug` and uploads the binaries and `.pdb` files as a CI artifact, without
signing or publishing a release. Run these binaries on a machine with the VS 2022
C++ development tools and debug runtime installed; the debug CRT is not redistributable
and is not copied into the artifact.

## Nightly MSI version and date

The Windows nightly calls `.github/scripts/nightly_version.py` before compilation.
It captures one date for both `version.date.inc` and the MSI filename, even if packaging
happens after midnight. `+UNKNOWN` is still replaced by `-YYYY-MM-DD` in the binary
version strings. The source `version.inc` is not modified.

For `2.7.63.0`, `-beta+UNKNOWN` and 2026-11-14:

- MSI `ProductVersion` / WiX `InstallerVersion`: **`2.7.63.0`**, unchanged numerically.
- MSI filename: **`SuperSlicer_2.7.63.0-beta-2026-11-14.msi`**.
- Binary ProductVersion string: `SuperSlicer_2.7.63.0-beta-2026-11-14`.
- MSI display-name suffix: `-Beta-Nightly`, unchanged.

`MSI_FILE_NAME` is shared by linking, signing-input collection and final upload.
Malformed/mismatched version fields, missing nightly markers and out-of-range numeric
components fail before compilation; there is no silent `0.0.0.0` fallback. Unit tests
are included in `python -B -m unittest discover -s .github/scripts/tests -v`.

Windows Installer compares only the first three version components. The existing MSI
template keeps `Product Id="*"` and `AllowSameVersionUpgrades="yes"`: a rebuilt nightly
can replace one with the same version, but this does not order nightlies chronologically
or prevent returning to an older nightly. Installation/upgrade behavior still needs a
VM test. RC versions and the manual-only SignPath test-signing rules are unchanged.
