# Windows code signing (SignPath)

The integration is in `.github/workflows/ccpp_win.yml` (Windows **nightly** only).
`ccpp_win_rc.yml` and `ccpp_win_debug.yml` are not integrated yet. Signing is **off by
default**: existing unsigned builds continue until explicitly enabled in GitHub.

## GitHub settings

In the repository, open **Settings → Secrets and variables → Actions**.
Add the following repository **variables** (not secrets):

| Variable | Value |
| --- | --- |
| `SIGNPATH_ENABLED` | `true` only when all settings below are ready; unset or `false` disables signing |
| `SIGNPATH_ORGANIZATION_ID` | UUID of the organization actually containing the project and certificate |
| `SIGNPATH_PROJECT_SLUG` | `superslicer` by default; override if the existing project has another slug |
| `SIGNPATH_SIGNING_POLICY_SLUG` | the actual policy slug; use a test policy first, e.g. `test-signing` |
| `SIGNPATH_TEST_CERTIFICATE_THUMBPRINT` | **test only**: SHA-1 thumbprint (40 hex characters) of the self-signed certificate, copied from SignPath or its exported public certificate |

Add `SIGNPATH_API_TOKEN` as a repository **secret**. Do not put it in these files.
Its user must have **submitter** permission on the selected signing policy. Missing
required settings fail early when signing is enabled; signing failures never silently
fall back to publishing unsigned binaries.

## Objects to create in SignPath

In the selected organization, create or check these objects. Slugs must match CI exactly.

| Object | Slug used by CI | Notes |
| --- | --- | --- |
| Organization | — | its UUID goes in the `SIGNPATH_ORGANIZATION_ID` GitHub variable |
| Trusted Build System | — | add *GitHub.com*, then link it to the project. SignPath then checks that the artifact really comes from a GitHub workflow |
| Project | `superslicer` | Settings → connect the GitHub repository |
| Signing Policy | e.g. `test-signing` | assign the test certificate, authorize the submitter and configure manual approval by the maintainer; put the slug in `SIGNPATH_SIGNING_POLICY_SLUG` |
| Artifact Configuration | `windows-binaries` | content of `artifact-configuration-binaries.xml` |
| Artifact Configuration | `windows-msi` | content of `artifact-configuration-msi.xml` |
| API token | — | user profile → API tokens, stored as the `SIGNPATH_API_TOKEN` GitHub secret |

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
Only ask SignPath OSS support for the production certificate after a complete run.

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
4. Enable signing and complete an end-to-end test in the OSS organization.

Once SignPath has reviewed the setup and imported a **production** certificate, assign
it to a manually approved release policy, select that policy in GitHub, and **remove
`SIGNPATH_TEST_CERTIFICATE_THUMBPRINT`**. Production verification then uses the normal
Windows trust chain, with no private certificate imported.

## Why the artifact is not the whole package

`build/package/` contains `VCRUNTIME140*`, `msvcp140*` and the mesa DLLs, which are not
ours to sign. Moreover, `src/RelWithDebInfo/` also contains copied `libgmp-10.dll` and
`libmpfr-4.dll`, so **copying `*.dll` from there is unsafe too**.

`Prepare-Binaries.ps1` uses the XML's explicit five-file allowlist, validates their
`ProductName` and common `ProductVersion`, and submits only those files. All five
carry VERSIONINFO resources (including the two DLLs); the viewer keeps its distinct
file description but uses the same product name. The actual product version is passed
as the required `version` parameter and enforced by SignPath on all five files.

The verified signed result is copied into `build/package/` before the MSI is harvested.

The MSI is signed in a second request, after `light.exe`.

## First run

`wait-for-completion: true` means CI waits for the signing request to be approved.
If the signing policy requires manual approval, open SignPath → *My Requests* and approve
while the workflow is running. If it times out, raise
`wait-for-completion-timeout-in-seconds` in the workflow.

There are **two signing requests per build**: binaries, then MSI. Both need approval
when manual approval is configured. Check trial quotas before testing: a trial plan
may not allow both requests; use the OSS organization's test certificate for integration.

The workflow runs on its listed nightly/debug branches or via `workflow_dispatch`
(GitHub's manual Run workflow button requires the workflow on the default branch).
Push these changes to the branch being built before trying the integration.

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
