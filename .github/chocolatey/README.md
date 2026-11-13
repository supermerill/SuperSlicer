# Chocolatey publication

`release_chocolatey.yml` submits a script-only `superslicer` package referencing the
**published stable Windows MSI**. It never publishes the RC draft or embeds the MSI
inside the nupkg. URL, SHA-256, source version and normalized package version are
fixed when packing; installing the package does not query "latest".

## One-time setup outside this repository

1. **Coordinate package ownership first.** As checked on 2026-11-13,
   [superslicer](https://community.chocolatey.org/packages/superslicer) is maintained
   by **verbalinsurection**, with package source at
   [Verbalinsurection/Chocolatey-SuperSlicer](https://github.com/Verbalinsurection/Chocolatey-SuperSlicer).
   Use [Contact Maintainers](https://community.chocolatey.org/packages/superslicer/ContactOwners)
   to request co-maintainer rights or agree a handover. Owning the software/GitHub
   repository or putting a name in a nuspec does not grant Chocolatey publishing
   rights. Stop the old updater by agreement to avoid two publishers racing.

2. **Agree and test the portable-to-MSI transition.** The existing package embeds a
   portable ZIP and creates a `superslicer.exe` shim and desktop shortcut. This new
   package installs the MSI for all users and relies on the MSI's shortcuts and
   Chocolatey's automatic MSI uninstaller; it does not create a new CLI shim.
   Back up profiles/settings (including any portable configuration) before removing
   an old package. Test fresh MSI installation, MSI-to-MSI upgrade/uninstall and the
   old portable-package removal in a disposable VM/Sandbox. The new install script
   fails when it detects a legacy portable directory or shim: it does **not** delete
   old files, user settings or arbitrary shortcuts to make an upgrade succeed.
   A transparent in-place portable-to-MSI migration is not claimed or implemented.
   Validate this policy with the maintainer/moderators before enabling publication.

3. **Finish SignPath production activation and publish a real stable release.**
   A beta/nightly or self-signed test build is not eligible. Publish the draft manually
   after all RC uploads finish. For tag `2.7.63.0`, the MSI is
   `SuperSlicer_2.7.63.0_win64.msi`. The Windows packer checks its size, GitHub digest
   when available, public Authenticode trust, ProductName `SuperSlicer`, Manufacturer
   `Supermerill`, and matching ProductVersion without executing/installing the MSI.

4. **Create/use the authorized Chocolatey account's API key.** Log into
   [Chocolatey Community](https://community.chocolatey.org/account), find the API key
   in the account settings and store it directly in **SuperSlicer → Settings → Secrets
   and variables → Actions → New repository secret → `CHOCO_API_KEY`**. It must belong
   to an account authorized to push `superslicer`. This is not a GitHub PAT or a
   SignPath token. Never put the key in Git, chat, nuspecs or package files.

5. **Enable and supervise the first submission.** After the ownership and migration
   checks, set the Actions repository variable **`CHOCO_ENABLED=true`**. Deploy this
   workflow with the release and to the default branch for manual retries. Inspect
   the generated nupkg (available as an Actions artifact) before rollout. Follow
   Chocolatey's validation, verification and moderation results. A successful push
   means **submitted**, not necessarily approved/visible to ordinary users. Trusted
   package policies may automate some reviews; there is no guaranteed turnaround.
   Rotate the key as needed; remove/disable CHOCO_ENABLED to stop new submissions.

## Version contract, retries and safety

Chocolatey 2.x [normalizes versions](https://docs.chocolatey.org/en-us/choco/features/version-number-normalization/):
`2.7.63.0` becomes package `2.7.63`, while `2.7.63.12` stays `2.7.63.12`. Leading zeroes
are normalized too. The **MSI URL always retains the exact four-part GitHub tag**;
never construct it from `$env:ChocolateyPackageVersion` at install time.

Automatic submission uses `release: released`; drafts/betas/nightlies are excluded
by this integration's policy. A missed event can be retried with **Publish stable
release to Chocolatey**, selecting the **default branch** and the stable tag. Publishing
via another workflow's `GITHUB_TOKEN` does not trigger a second release workflow;
manual publication in the GitHub UI does.

The preflight checks the current GitHub release, exact MSI and Chocolatey's public
version endpoint. A visible existing version is skipped; network/API errors fail
instead of being treated as absence. A pending/unlisted submission may not be visible
through the public feed: inspect the maintainer dashboard before retrying. Push errors
(including duplicates or missing ownership) fail visibly. Never force-replace/delete
packages, change an existing published MSI, or silently invent a revision to bypass
moderation. Concurrency serializes current submissions but is not a moderation lock.

Only the final `choco push` step receives `CHOCO_API_KEY`, with no debug logging and
without saving it to Chocolatey configuration. Build jobs have read-only GitHub access.
The nupkg contains only `chocolateyInstall.ps1` and `VERIFICATION.txt` plus NuGet metadata.
It installs the x64 MSI with SHA-256 validation and `/qn /norestart`; successful MSI exit
codes include reboot-required results. No existing user installation is changed by CI.

## Local checks (no publishing)

```powershell
python -B -m unittest discover -s .github/scripts/tests -v
actionlint .github/workflows/release_chocolatey.yml
```

Tests use fake release/feed responses and fixture bytes, test the install script with
Chocolatey helpers mocked, and exercise `choco pack` when Chocolatey is installed.
They never install SuperSlicer or push a package. These do not replace a real signed
MSI, Sandbox migration testing or community validation/moderation.
