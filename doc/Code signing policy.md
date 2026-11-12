# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

The SignPath integration is being set up. Older releases and builds from workflows
without signing enabled are not covered. Test-signed artifacts are for integration
testing only, not production releases.

When signing is enabled, only these Windows binaries built from this repository by
GitHub Actions are submitted for signing: `superslicer.exe`, `superslicer_console.exe`,
`superslicer-gcodeviewer.exe`, `Slic3r.dll`, `OCCTWrapper.dll` and the Windows installer.
Everything else shipped in the package — Microsoft Visual C++ runtime, mesa/OpenGL
libraries, GMP/MPFR and other third-party components — is redistributed without being
re-signed with the SuperSlicer certificate; existing upstream signatures are preserved.

## Roles

- Maintainer/committer: [supermerill](https://github.com/supermerill), the owner of this
  repository.
- Reviewers: the repository's [code owners](../.github/CODEOWNERS), currently
  [supermerill](https://github.com/supermerill) and
  [lordofhyphens](https://github.com/lordofhyphens). External pull requests require review
  before merge.
- Approvers: [supermerill](https://github.com/supermerill). Each release is approved for
  signing by the approver, in SignPath.

## Privacy policy

SuperSlicer can contact GitHub and its content/download services to check for new
releases and download printer and print configuration profiles. Profile downloads may
also contact repositories selected by the user. Automatic update features can be
disabled in the application preferences. These requests expose normal connection
information, such as the user's IP address, to the contacted service; see
[GitHub's privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)
and the policies of any configured third-party service.

User-configured printer/print-server integrations transmit print files, connection
credentials and other information needed for the requested operations to the selected
printer or service. SuperSlicer does not send telemetry or usage statistics to its
maintainers.

SuperSlicer will not transfer any other information to networked systems unless
specifically requested by the user or the person installing or operating it.
