# RC release pipeline

A release starts when a maintainer **pushes a version tag**. `release_rc.yml` runs only
for the push that creates that tag: it prepares a draft release for it, then builds and
uploads the platforms with missing files. No build passes through a maintainer's computer.

| Event | Workflow | Result |
| --- | --- | --- |
| Push of a new tag `n.n.n.n` or `n.n.n.n-suffix` | `release_rc.yml` | Draft release of that tag, Windows signed in production |
| Push to branch `rc` | `ci_rc.yml` | Five unsigned RC builds as Actions artifacts; no tag, no draft, read-only |
| Push to `debug_win`, or manual run | `ccpp_win_rc.yml` | Unsigned Windows RC diagnostic |
| Push to `rc-macintel`, or manual run of a macOS/Linux RC workflow | that workflow | Unsigned diagnostic of one target |

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

Create the tag on the release commit and push **that tag alone**:

```bash
git tag 2.7.63.0-beta <commit>
git push origin 2.7.63.0-beta
```

The tag must equal the version of `version.inc` at that commit (`2.7.63.0`,
`2.7.63.0-beta`, `2.7.63.0-rc.1`; no `v` prefix, no `+...`). Preparation refuses, before
any GitHub write: another event, a deleted or force-updated tag, a tag that does not match
`version.inc`, and a tag that does not point at the checked-out commit (annotated tags are
resolved to their commit). There is no manual run and no branch run. Push one tag per
`git push`: GitHub does not start workflows for a push of more than three tags.

CI **never creates, moves or deletes a tag**. If the tag disappears, preparation stops
before creating the draft: creating a release would recreate the tag implicitly. Before
the first tag, protect version tags in the repository rules so that only maintainers can
create them and nobody (including GitHub Actions) can update or delete them.

**Deploy the workflow definitions to the default branch before the first RC run.**
GitHub can refuse release creation/update with `GITHUB_TOKEN` when the target commit's
`.github/workflows/` files differ from the default branch: that requires the separate
Workflows write permission, which `GITHUB_TOKEN` cannot have. Keep these definitions
synchronized on the default branch and `rc`; do not work around the refusal by moving
a version tag or adding a maintainer PAT. See [GitHub's create-release restrictions](https://docs.github.com/en/rest/releases/releases#create-a-release).

All dependency and application jobs check out the **tagged commit**, never a floating
branch. Local `workflow_call` references resolve to the workflow revision of that tag, so
a re-run uses the definitions of the tag, not later fixes. Only `release_rc.yml` passes
`stage_release` to the build workflows; they never publish anything themselves.

Offline tests run first, in a job without write access. Preparation has
`contents: write` to create the draft. Compilation/signing jobs have read-only repository
access and do not persist checkout credentials. Each platform has its own publisher job
(`publish_windows`, `publish_macos_intel`, ...) that calls `release_upload.yml`, with
`contents: write`, on a fresh hosted runner, after that platform's build only.
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
- The tag must exist and still point at the tagged commit before every write. A
  missing tag is an error, never an invitation to recreate it.
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

The master's concurrency group serializes runs of the same tag and does not cancel a
running release; different tags do not wait for each other. GitHub can replace an older *pending* run with a newer pending run. Ref and
draft state are checked again before each publication. GitHub does not offer an
atomic "upload only while draft" operation: **do not publish the draft manually
until the workflow has finished**. Never publish it while jobs are still uploading.

## Signing and notes

The Windows release requires `SIGNPATH_ENABLED=true`, the production policy and a
valid production certificate. Its first job fails **before compilation** if production
signing is disabled, rather than filling a draft with unsigned final Windows files. The
packaging worker also refuses production signing unless it is called by
`release_rc.yml` for a tag push.
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
the standard library. Also run `actionlint` on the master, `ci_rc.yml`, the publisher and the workers,
plus the local SignPath/runtime packaging smoke tests where their Windows prerequisites
are installed. These do not replace five real platform builds or a production-signing
integration test. The master runs the offline tests, with PyYAML, before its first
GitHub write.

`create_release.py` is now a **legacy manual recovery tool**, not part of the release
pipeline. Do not use it to assemble the new RC drafts: it selects Actions artifacts
by branch/date rather than exact commit. Its token logging is removed, but it still
uses a local PAT if explicitly invoked. No old release/tag or local release folder
is migrated, overwritten or deleted automatically. `create_releasemacos.py` is left
untouched.
