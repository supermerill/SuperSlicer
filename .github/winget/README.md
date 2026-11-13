# WinGet publication

`.github/workflows/release_winget.yml` submits **published stable Windows MSI releases**
as updates to `Supermerill.SuperSlicer` in `microsoft/winget-pkgs`. It opens a PR;
Microsoft's checks/review and catalog ingestion still happen afterwards. It does not
publish the GitHub draft, build/sign Windows, or add Chocolatey automation.

The RC master is deliberately unchanged. Finish all uploads, review the release and
publish it manually. Only then can the WinGet workflow run. Beta/nightly suffixes,
drafts, missing/empty MSI assets and another repository are rejected. Stable-only is
**our policy**, not a general limitation of WinGet: WinGet can represent prereleases,
but this integration does not send them to the stable package.

## One-time setup outside this repository

1. **Obtain a production-signed stable MSI.** Complete SignPath production review and
   activation first (see [SignPath setup](../signpath/README.md)). The current
   `2.7.63.0-beta` cannot be used as a stable package. Choose a genuinely stable
   four-part version, remove the beta suffix in the version source for that release,
   and publish its draft only after the RC jobs finish. For `2.7.63.0`, the direct
   installer asset is `SuperSlicer_2.7.63.0_win64.msi`. Do not rename an existing beta
   or strip its suffix just to get it into WinGet. Do not replace the published MSI:
   manifests bind to its SHA-256.

2. **Fork [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs/fork)** under
   the GitHub account that will submit the PRs (normally `supermerill`, or a dedicated
   automation account). Read the [first-contribution checklist](https://github.com/microsoft/winget-pkgs/blob/master/doc/FirstContribution.md)
   and check for an existing package or open PR before submitting. As checked on
   2026-11-13, `manifests/s/Supermerill/SuperSlicer` does not exist yet.

3. **Submit the first version manually and wait for its PR to be merged.**
   `winget-releaser` updates an existing package; it cannot create the first entry.
   On Windows, install Microsoft's manifest creator, then use the public MSI URL:

   ```powershell
   winget install --exact --id Microsoft.WingetCreate
   # Example only: replace with the real stable version that you have published.
   $version = '2.7.63.0'
   $url = "https://github.com/supermerill/SuperSlicer/releases/download/$version/SuperSlicer_${version}_win64.msi"
   wingetcreate new $url
   ```

   Use these metadata values when prompted/reviewing the generated manifests:

   - `PackageIdentifier: Supermerill.SuperSlicer` (case-sensitive).
   - `PackageVersion`: the exact four-part release/MSI version.
   - `Publisher: Supermerill`, `PackageName: SuperSlicer` (match the MSI).
   - `License: AGPL-3.0`; license URL points to `LICENSE` at this version's tag.
   - Package URL: `https://github.com/supermerill/SuperSlicer`.
   - Installer: `Architecture: x64`, `InstallerType: wix`, `Scope: machine`;
     `InstallerUrl`: the direct MSI URL above. Let the tool compute `InstallerSha256`
     and inspect the actual MSI's ProductCode/version; never invent these values.
   - Inspect any `AppsAndFeaturesEntries` against the actual installer. MSI metadata
     and silent install/upgrade/uninstall behavior must be checked, preferably in
     Windows Sandbox. Do not change the MSI template just to fit guessed metadata.

   Place the three generated files in your winget-pkgs fork, not in SuperSlicer:

   ```text
   manifests/s/Supermerill/SuperSlicer/<version>/
     Supermerill.SuperSlicer.yaml
     Supermerill.SuperSlicer.installer.yaml
     Supermerill.SuperSlicer.locale.en-US.yaml
   ```

   The version file holds the identifier, version and default locale; license,
   publisher and links belong in the default-locale file. Validate the directory:

   ```powershell
   winget validate --manifest "manifests/s/Supermerill/SuperSlicer/$version"
   ```

   Open a PR against `microsoft/winget-pkgs` containing only this one package version.
   Respond to validation/reviewer feedback; acceptance is not automatic and has no
   fixed turnaround. See [Microsoft's authoring/testing guide](https://github.com/microsoft/winget-pkgs/blob/master/doc/Authoring.md).

4. **Create a classic GitHub PAT** on the submitting account with `public_repo` and
   `workflow` scopes, an appropriate expiration, and access to its winget-pkgs fork.
   The action does not support fine-grained PATs. `workflow` permits fork sync when
   upstream changes workflow files; without it you may need to sync the fork manually.
   This is separate from `SIGNPATH_API_TOKEN` and the RC master's `GITHUB_TOKEN`.
   Store it directly in **SuperSlicer → Settings → Secrets and variables → Actions →
   New repository secret → `WINGET_TOKEN`**. Never put it in a file or chat. Treat its
   classic scopes as broad permissions; a dedicated account limits exposure.

5. **Enable only after the first package is merged.** In Actions repository variables,
   set `WINGET_ENABLED=true`. Set `WINGET_FORK_USER` only if the fork is not under
   `supermerill`; its value is the fork owner's GitHub login, not a URL. Ensure repository
   Actions settings allow the pinned `vedantmgoyal9/winget-releaser` action. Rotate the
   PAT before it expires. Remove/disable `WINGET_ENABLED` to stop new submissions.

## Subsequent stable releases and retries

The `release: released` event starts the workflow after a stable release is published
(including promotion to a stable release). Publishing with the GitHub web UI produces
the event; publication by another workflow using only `GITHUB_TOKEN` will not trigger
it. This workflow must be deployed with the release; manual dispatch additionally
requires it on the default branch.

For a missed event or a release published before enabling WinGet, run **Publish stable
release to WinGet** manually **from the default branch**, with the exact stable tag.
The preflight fetches the current public release and the catalog again. It selects only
`SuperSlicer_<version>_win64.msi`, never a portable ZIP, DMG or Actions artifact. An
already merged version is skipped. Before retrying a submission that created a PR,
inspect that PR: update it rather than creating duplicates while review is pending.
Concurrency prevents simultaneous fork synchronizations but is not a pending-PR lock.

The job's `GITHUB_TOKEN` is read-only. Only the submission action receives the external
PAT (plus a prior presence check). It uses Komac to download/inspect the real MSI,
calculate the hash, update the three manifests and open the PR. It does not remove old
versions (`max-versions-to-keep: 0`). Its PR link appears in the workflow summary.
The action itself is pinned to a reviewed commit; its upstream composite still installs
Komac dynamically, so this is not a fully hermetic toolchain.

## Verification

```powershell
python -B -m unittest discover -s .github/scripts/tests -v
actionlint .github/workflows/release_winget.yml
```

These tests mock GitHub responses and do not submit any PR or validate a real MSI.
Only a published signed MSI, `winget validate`, sandbox installation checks, and the
first real winget-pkgs PR can complete that integration validation.
