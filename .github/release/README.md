# RC release pipeline

`release_rc.yml` is the **only** workflow triggered by a push to `rc`. It prepares an
immutable version tag and a draft release, then calls the five RC workflows for
platforms with missing files. No build passes through a maintainer's computer.

## Version and files

The version is `SLIC3R_RC_VERSION_DOTS` + `SLIC3R_VERSION_TAG` with `+UNKNOWN` removed.
For example, `2.7.63.0` and `-beta+UNKNOWN` produce tag `2.7.63.0-beta` and:

| Platform | Release files |
| --- | --- |
| Windows | `SuperSlicer_2.7.63.0-beta_win64.zip`, `SuperSlicer_2.7.63.0-beta_win64.msi` |
| macOS Intel | `SuperSlicer_2.7.63.0-beta_macos.dmg` |
| macOS ARM | `SuperSlicer_2.7.63.0-beta_macos_arm.dmg` |
| Linux GTK3 | `SuperSlicer-ubuntu_22.04-2.7.63.0-beta.AppImage`, `SuperSlicer_2.7.63.0-beta_linux64.tgz` |
| Linux GTK2 | `SuperSlicer-ubuntu_22.04-gtk2-2.7.63.0-beta.AppImage` |

Release names have **no date**. Nightly version/date handling is unchanged: those
workflows can replace `+UNKNOWN` with the date as before. Internal EXE/DLL names and
package layout are not changed; the portable ZIP still has a single named root folder.
DMG/MSI/AppImage/TGZ files are uploaded directly as release assets, without the ZIP
envelope used by GitHub Actions' artifact storage. Extra TGZ diagnostics remain
available in the individual macOS/GTK2 build artifacts.

## Trigger and permissions

Push the complete release pipeline and the intended version to `rc`, or manually run
**RC release orchestrator**, selecting branch `rc`. A manual run on another branch
fails before any GitHub write. GitHub only shows the manual button after the workflow
is also available on the repository's default branch.

**Deploy the workflow definitions to the default branch before the first RC run.**
GitHub can refuse release creation/update with `GITHUB_TOKEN` when the target commit's
`.github/workflows/` files differ from the default branch: that requires the separate
Workflows write permission, which `GITHUB_TOKEN` cannot have. Keep these definitions
synchronized on the default branch and `rc`; do not work around the refusal by moving
a version tag or adding a maintainer PAT. See [GitHub's create-release restrictions](https://docs.github.com/en/rest/releases/releases#create-a-release).

Preparation creates the tag at the **triggering commit SHA**. All dependency and
application jobs check out that same SHA, including Linux (never a floating `rc`).
Local `workflow_call` references also resolve to the caller's workflow revision.
The legacy independent `push rc` triggers are removed to prevent double builds.
`debug_win`, `rc-macintel` and individual manual diagnostic builds do not publish
release assets without the master's inputs.

Preparation has `contents: write` to create refs/drafts. Compilation/signing jobs have
read-only repository access and do not persist checkout credentials. Each platform's
publisher calls `release_upload.yml`, with `contents: write`, on a fresh hosted runner.
It downloads the **exact artifact ID from the same workflow run**. A manifest binds
the filenames, platform, version and source SHA and verifies each file's SHA-256 after
transfer, before posting the raw files to GitHub Releases. The manifest itself is an
internal Actions artifact, not an additional release download.

The final summary also needs `contents: write` because GitHub restricts draft-release
visibility to push-capable tokens, even for GET requests. Its `verify` command only
reads GitHub state and never changes or publishes the release.

The API helper uses only the job's `GH_TOKEN` (`github.token`). It never reads
`githubtoken.ini`, requires no maintainer PAT and never prints authentication headers.

## Required failures and safe resume

- A version tag pointing at another commit makes the build **fail**. Never move or
  recreate that tag automatically. Increment the version when changing source code,
  including release scripts after a tag has already been created.
- An existing **public** release makes the build **fail**, even if some assets are
  missing. This pipeline only creates/completes **drafts**.
- Existing drafts must still have their matching tag. A missing tag is an error,
  not an invitation to silently recreate it.
- A complete platform is skipped with a warning. A partially uploaded platform is
  rebuilt, but its publisher sends only the absent names. Existing uploaded assets
  are kept unchanged and never overwritten or deleted, even if a rebuild produces
  different bytes. This assumes existing assets are trusted; their presence alone
  is not proof of their origin or signature.
- An empty/incomplete GitHub upload (`starter`, zero bytes, etc.) is an explicit
  error. Inspect/remove only that failed upload manually before retrying. The helper
  does not delete remote assets on your behalf.
- If one platform fails, the others continue and can populate the draft. The final
  summary lists complete/missing files and platform results and fails if files remain
  missing. A complete release is **still a draft**, never published automatically.

Use **Re-run failed jobs** or re-run the workflow on the **same commit** to resume.
Release filenames do not change the next day. Intermediate Actions artifact names
include `run_attempt`; diagnostics can be replaced inside the same run, but release
assets cannot. Expired intermediate artifacts may require rebuilding their platform.

The master's concurrency group serializes RC runs and does not cancel a running
release. GitHub can replace an older *pending* run with a newer pending run. Ref and
draft state are checked again before each publication. GitHub does not offer an
atomic "upload only while draft" operation: **do not publish the draft manually
until the workflow has finished**. Never publish it while jobs are still uploading.

## Signing and notes

The Windows release worker requires `SIGNPATH_ENABLED=true`, the production policy
and a valid production certificate. It fails **before compilation** if production
signing is disabled, rather than filling a draft with unsigned final Windows files.
Other platforms are independent. Self-signed integration tests remain exclusively in
the manually opted-in nightly workflow; see [SignPath setup](../signpath/README.md).

The footer is read from [`.github/release-footer.md`](../release-footer.md) at the
release commit. `{{CODE_SIGNING_POLICY_URL}}` becomes a link to the policy at that tag.
The helper owns only its delimited footer block: handwritten release notes before
and after it are preserved across retries. You still write the release notes and
publish the release manually.

After a **stable** release is publicly published, the separate
[`release_winget.yml`](../workflows/release_winget.yml) can submit its Windows MSI to
WinGet. It is disabled until the first package is accepted and `WINGET_ENABLED=true`;
see the [WinGet setup and manual first submission](../winget/README.md). Drafts,
beta releases and nightly builds are not submitted. The RC master never receives
the external WinGet token.

The separate [`release_chocolatey.yml`](../workflows/release_chocolatey.yml) can submit
the same public stable MSI to Chocolatey after `CHOCO_ENABLED=true`. It requires
publishing rights for the existing `superslicer` package and an agreed/tested transition
from its portable ZIP package; see [Chocolatey setup](../chocolatey/README.md). The RC
master never receives `CHOCO_API_KEY`, and a push does not bypass community moderation.

## Offline verification and migration

```powershell
python -B -m unittest discover -s .github/scripts/tests -v
```

Tests use a fake API: no credentials, real refs/releases, upload or remote builds.
Install PyYAML to include the workflow-structure tests; core release tests use only
the standard library. Also run `actionlint` on the master, publisher and five workers,
plus the local SignPath/runtime packaging smoke tests where their Windows prerequisites
are installed. These do not replace five real platform builds or a production-signing
integration test. The master runs the offline tests before its first GitHub write.

`create_release.py` is now a **legacy manual recovery tool**, not part of the release
pipeline. Do not use it to assemble the new RC drafts: it selects Actions artifacts
by branch/date rather than exact commit. Its token logging is removed, but it still
uses a local PAT if explicitly invoked. No old release/tag or local release folder
is migrated, overwritten or deleted automatically. `create_releasemacos.py` is left
untouched.
