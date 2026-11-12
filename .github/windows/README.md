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
