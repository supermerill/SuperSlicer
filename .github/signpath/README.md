# Windows code signing (SignPath)

Release signing is requested by `.github/workflows/ccpp_win_rc.yml` (`Release` binaries) and
performed by its packaging worker `windows_package.yml`, after `windows_build.yml` has compiled
the commit in a separate job without any SignPath secret.
It is called by the [RC release orchestrator](../release/README.md), which prepares
the version tag and draft and only builds missing platforms.
`.github/workflows/ccpp_win.yml` (`RelWithDebInfo`) supports **manual test signing only**,
through the same two workers.
`ccpp_win_debug.yml` is not integrated. Signing is **off by default**.

| Build | Signing behavior |
| --- | --- |
| Ordinary nightly push (including `debug_win`) | Never submits a signing request |
| Manually dispatched nightly, `sign` unchecked | Ordinary unsigned build |
| Manually dispatched nightly, `sign` checked | Uses the pinned self-signed certificate and test policy; two requests |
| Push to `rc` | The master calls Windows when its release files are missing; requires production signing, otherwise the Windows worker fails without publishing unsigned release files |
| RC workflow on `debug_win` | Never signs automatically |
| Manually dispatched RC workflow | Signs only with `sign` checked **and** `SIGNPATH_ENABLED=true` |

## GitHub settings

In the repository, open **Settings → Secrets and variables → Actions**.
Add the following repository **variables** (not secrets):

| Variable | Value |
| --- | --- |
| `SIGNPATH_ENABLED` | enables **RC production** signing only; leave unset/`false` until the production certificate is ready. Does not enable nightly signing |
| `SIGNPATH_ORGANIZATION_ID` | UUID of the organization actually containing the project and certificate |
| `SIGNPATH_PROJECT_SLUG` | `superslicer` by default; override if the existing project has another slug |
| `SIGNPATH_SIGNING_POLICY_SLUG` | RC **production** policy slug, e.g. `release-signing` |
| `SIGNPATH_TEST_SIGNING_POLICY_SLUG` | nightly **test** policy slug, e.g. `test-signing` |
| `SIGNPATH_TEST_CERTIFICATE_THUMBPRINT` | nightly **test only**, mandatory: SHA-1 thumbprint (40 hex characters) of the self-signed certificate, copied from SignPath or its exported public certificate |

Add `SIGNPATH_API_TOKEN` as a repository **secret**. Do not put it in these files.
Its user must have **submitter** permission on each selected signing policy. Missing
required settings fail early when signing is enabled; signing failures never silently
fall back to publishing unsigned binaries.

## Objects to create in SignPath

In the selected organization, create or check these objects. Slugs must match CI exactly.

| Object | Slug used by CI | Notes |
| --- | --- | --- |
| Organization | — | its UUID goes in the `SIGNPATH_ORGANIZATION_ID` GitHub variable |
| Trusted Build System | — | add *GitHub.com*, then link it to the project. SignPath then checks that the artifact really comes from a GitHub workflow |
| Project | `superslicer` | Settings → connect the GitHub repository |
| Test Signing Policy | e.g. `test-signing` | assign the self-signed test certificate and put the slug in `SIGNPATH_TEST_SIGNING_POLICY_SLUG` |
| Production Signing Policy | e.g. `release-signing` | assign the production certificate and put the slug in `SIGNPATH_SIGNING_POLICY_SLUG`; authorize the submitter and require maintainer approval |
| Artifact Configuration | `windows-binaries` | content of `artifact-configuration-binaries.xml` |
| Artifact Configuration | `windows-msi` | content of `artifact-configuration-msi.xml` |
| API token | — | token of the project's CI user, stored as the `SIGNPATH_API_TOKEN` GitHub secret |

For OSS trusted-build verification, use the existing **CI builds** technical user as
Submitter. Do not substitute an interactive administrator's personal token. A personal
account can remain an Approver; a CI user does not need a dashboard login or email
confirmation to authenticate with its token.

Use MFA for both GitHub and SignPath accounts. Restrict the project's allowed source
repository/branches to the intended SuperSlicer builds. When requested by SignPath's
integration or policy checks, install the [SignPath GitHub App](https://github.com/apps/signpath)
with access limited to this repository.

Artifact configurations are created with **Add → Custom** and the XML pasted in.
**Upload an artifact sample** instead only if the XML drifts from the real package:
it regenerates the file list from an actual ZIP and is the fastest way to see what
SignPath recognised.

## Certificate

A self-signed test certificate is enough to validate the pipeline, but Windows runners
do **not** trust it by default. Set `SIGNPATH_TEST_CERTIFICATE_THUMBPRINT` to pin this
exact certificate. Test verification checks the certificate's validity and code-signing
usage and calls Windows `WinVerifyTrust` to verify each signature and file digest. Only
the expected untrusted-root error is tolerated for this pinned self-signed certificate;
no certificate is installed in a trust store. It rejects missing files, unexpected
files, unsigned or tampered files and signatures made with another test certificate.
The MSI is verified too. Production uses `signtool verify /pa /all` without exceptions.

Test outputs are named `test-signed_nightly_...` and **must not be published as trusted
releases**. Self-signed test signatures do not remove Windows publisher warnings.
Only ask SignPath OSS support for the production certificate after a complete manually
dispatched nightly test. Ordinary nightly runs ignore the signing configuration entirely.

If the project is invalid, read its validation details and check the policy's
certificate, permissions and artifact configuration. For an expired OSS test
certificate, ask OSS support to replace it if you cannot do so in that organization.
Update the pinned thumbprint after replacing a test certificate.

## Switching from the trial organization to the OSS organization

Keep signing disabled while switching. The old trial organization's objects and
certificate do not automatically move to the OSS organization:

1. Accept the invitation and select the **OSS** organization.
2. Check/create the project, GitHub trusted build system link, signing policy,
   submitter/approver permissions and both artifact configurations there.
3. Change `SIGNPATH_ORGANIZATION_ID`, policy/project slugs if different, the
   `SIGNPATH_API_TOKEN` secret and the test certificate thumbprint in GitHub.
4. Manually dispatch the nightly workflow with `sign` checked and complete an end-to-end
   test in the OSS organization. Keep RC production signing disabled during this test.

Once SignPath has reviewed the setup and imported a **production** certificate, assign
it to a manually approved release policy, set `SIGNPATH_SIGNING_POLICY_SLUG`, then enable
`SIGNPATH_ENABLED=true` for RC. RC verification always uses normal Windows trust: it
never accepts the nightly test certificate. The test policy and thumbprint may remain
configured for explicitly requested nightly tests.

## Why the artifact is not the whole package

`build/package/` contains the required VC runtime DLLs and Mesa, which are not ours to
sign. Moreover, `src/Release/` and `src/RelWithDebInfo/` also contain copied `libgmp-10.dll` and
`libmpfr-4.dll`, so **copying `*.dll` from there is unsafe too**.

`Prepare-Binaries.ps1` uses the XML's explicit five-file allowlist, validates their
`ProductName` and common `ProductVersion`, and submits only those files. All five
carry VERSIONINFO resources (including the two DLLs); the viewer keeps its distinct
file description but uses the same product name. The actual product version is passed
as the required `version` parameter and enforced by SignPath on all five files.

The verified signed result is copied into `build/package/` before the MSI is harvested.

The MSI is signed in a second request, after `light.exe`. See the
[DLL packaging notes](../windows/README.md) for actual runtime dependencies and Mesa.

## First run

`wait-for-completion: true` means CI waits for the signing request to be approved.
If the signing policy requires manual approval, open SignPath → *Browse* and approve
while the workflow is running. If it times out, raise
`wait-for-completion-timeout-in-seconds` in the workflow.

There are **two signing requests per build**: binaries, then MSI. Both need approval
when manual approval is configured. Check trial quotas before testing: a trial plan
may not allow both requests; use the OSS organization's test certificate for integration.

The manual Run workflow button requires the workflow on GitHub's default branch.
Push these changes to the branch being built before trying the integration.

Ordinary nightly runs can be repeated without any signing credentials or SignPath quota.
Input artifacts include `github.run_attempt` in their names and final artifacts use
`overwrite: true`, so GitHub job re-runs do not conflict with immutable artifact names
left by an earlier attempt. **Re-running an explicitly signed build submits new signing
requests**, consumes quota again and still requires approval. SignPath pipeline policies
may disallow re-runs (or cap supported attempts); use a fresh workflow run if required.

For RC draft publication, an already complete Windows platform is skipped before
building or submitting a signing request. A partial draft may rebuild Windows and
submit both signing requests again, but keeps existing release assets unchanged.
The orchestrator always keeps the tag's original SHA and never modifies a public release.

Before requesting the production review, link the [code signing policy](../../doc/Code%20signing%20policy.md)
from the actual project home/download/release pages as well, and confirm the listed
team roles and privacy statements accurately describe the project.

References: [GitHub integration](https://docs.signpath.io/trusted-build-systems/github),
[artifact configuration](https://docs.signpath.io/artifact-configuration),
[OSS requirements](https://signpath.org/terms).

## Local verification

On Windows with Visual Studio 2022, CMake, the Windows SDK and WiX v3 installed:

```powershell
./.github/signpath/tests/Test-Integration.ps1 -WorkDirectory <new-test-directory> -WixDirectory <wix-v3-bin-directory>
```

This builds tiny EXE/DLL fixtures using the actual resource templates and checks the
allowlist, metadata, certificate pinning, missing/unexpected outputs, tampering,
absence of changes to root trust and MSI verification using the actual installer template.
It also checks normal public-root verification on the Microsoft-signed SDK tool.
It creates and deletes its own temporary test certificate in the current user's
certificate store. It does not contact SignPath,
install an MSI, or replace an end-to-end CI test with the real SignPath organization.
