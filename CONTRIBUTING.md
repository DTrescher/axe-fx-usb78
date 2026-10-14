# Contributing

Thanks for considering a contribution. Issues and pull requests are welcome.

## Build and run the tests

Use Windows 10 or later with Visual Studio 2022, the **Desktop development with C++** workload, and CMake 3.21 or later. To build the x64 Release configuration and run the automated tests:

```powershell
.\tools\build.ps1 -Test
```

The automated tests do not need an Axe-Fx connected. The bridge itself uses the official Fractal Audio USB/ASIO driver and an Axe-Fx III.

## Where things live

- `include/axe/` contains the reusable audio pipeline and ASIO host interface.
- `src/` contains the Windows audio capture, routing bridge, control panel, and diagnostic support used by the bridge's test mode.
- `tests/` contains automated tests, test-only seams, and standalone hardware diagnostics.
- `tools/` contains PowerShell build, installer, launch, startup, and hardware-test scripts.
- `installer/` contains the Inno Setup script used to package a per-user Windows installer.
- `third_party/asio/` contains upstream Steinberg ASIO interface headers. Keep their notices and license terms with them.

## Code changes

- Keep the ASIO render callback bounded and free of locks, allocations, logging, and blocking operations.
- Add or update a hardware-independent test when changing reusable audio or settings behavior.
- Keep hardware diagnostics opt-in; they can open the audio device or send test signals.
- Keep changes to vendored ASIO files separate from normal project changes and preserve their upstream notices.
- Describe the Windows version, Axe-Fx driver, reproduction steps, and observed behavior when reporting an audio issue.
- Use the repository's `.clang-format` and `.editorconfig` settings for C++ and text files.

Small, focused pull requests with a clear description are easiest to review.

To prepare a tagged version and publish its installer, follow [the release guide](docs/releasing.md).

By submitting a contribution, you agree to make it available under the MIT License in the repository's root `LICENSE` file.
