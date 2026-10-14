# Publishing a release

Releases are built and published by GitHub Actions when a version tag is pushed. The release contains the per-user Windows installer and its SHA-256 checksum.

## Prepare a version

1. Set the version in the `project(... VERSION ...)` declaration in `CMakeLists.txt`.
2. Update release notes or user documentation as needed, then commit the changes to `main`.
3. Create a `vMAJOR.MINOR.PATCH` tag pointing to that commit and push it:

   ```powershell
   git tag -a v0.1.0 -m "Release 0.1.0"
   git push origin main
   git push origin v0.1.0
   ```

The tag version must exactly match the CMake project version. The workflow rejects other tag formats and mismatched versions.

## What the workflow does

The release workflow runs on a Windows runner. It checks out the tagged source with a commit-pinned GitHub Action, validates the version, obtains the pinned Inno Setup 7.1.0 compiler from its official release, verifies its GitHub release attestation and Authenticode signature, and builds the installer with `tools/build-installer.ps1`. If the build succeeds, GitHub CLI creates a release with generated notes and uploads:

- `axe-fx-usb78-setup-<version>.exe`
- `axe-fx-usb78-setup-<version>.exe.sha256`

The workflow uses the repository's built-in GitHub token with release-only write permission. It does not need a separately configured secret.

## Install and verify

Download the setup executable from [the latest release](https://github.com/DTrescher/axe-fx-usb78/releases/latest), then run it. Setup installs for the current Windows account, registers the application with **Apps & features / Installed apps**, and provides an uninstaller in Windows and the Start menu. Administrator access is not required.

The current installer is not Authenticode-signed because this project has no code-signing certificate. Windows may identify the publisher as unknown when starting the downloaded file. The workflow publishes a SHA-256 checksum alongside the installer; compare it with the file's hash before running the installer:

```powershell
Get-FileHash .\axe-fx-usb78-setup-0.1.0.exe -Algorithm SHA256
```

The hash must match the value in the corresponding `.sha256` release asset.

To make an installer locally, install Inno Setup 6 or 7 and run `tools/build-installer.ps1`. See [Build and validation](testing.md) for the local build instructions.
