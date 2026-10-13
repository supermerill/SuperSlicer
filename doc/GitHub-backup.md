# GitHub preset backup

File > Export > Back Up Presets to GitHub uploads saved user print, filament,
printer, SLA print, and SLA material presets in an importable configuration bundle.
It targets `SuperSlicer_config_bundle.ini` on an existing repository's default
branch. GitHub commits retain previous backups. A private repository is recommended.

Enter `owner/repository` and a GitHub personal access token with Contents read/write
permission for that repository. The repository is remembered after a successful
backup; the token is requested each time and is never written to application
settings or the backup. API calls disable verbose transport logging and redirects.

The dialog offers the existing unsaved-preset handling before exporting. Physical
printer records and print-host connection options (including API keys, passwords,
and host URLs) are excluded. Review custom G-code for embedded private information.
UI preferences, system presets, external models/textures, and physical printer
connections are not included. The current limit is 1 MiB per exported bundle.

Downloads can be restored through File > Import > Import Config Bundle. Reinstall
any vendor profiles on which custom presets depend and recreate printer connections.
Use GitHub's file history to download an earlier backup.

Requests run off the UI thread with connection and total request timeouts. Updates
include the current file SHA so concurrent changes are rejected rather than silently
overwritten. Protected branches may reject writes. On an upload timeout, inspect the
repository before retrying because GitHub may have committed the upload already.

## Validation

The dependency-free helper tests cover repository input rejection, connection-option
exclusion, Base64 padding, binary bytes, and UTF-8 bytes:

```text
cmake -S tests/github_backup -B build/github-backup-tests
cmake --build build/github-backup-tests --config Release
ctest --test-dir build/github-backup-tests -C Release --output-on-failure
```

These tests passed on Windows with MSVC. Full application configuration was blocked
by missing Boost. The dependency build then failed because bundled zlib requires
CMake compatibility removed in the installed CMake 4.3. GUI compilation and live
GitHub create/update/error scenarios remain unverified. No installed executable
has been replaced, and no presets have been uploaded during development.

Before release, build with the project's supported dependency toolchain and test:
first upload, repeat upload, restore in an isolated profile directory, invalid token,
missing repository, branch protection, concurrent change, and network interruption.
