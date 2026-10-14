# axe-fx-usb78

Send Windows audio through your Axe-Fx III without setting up a virtual cable or a software mixer.

Ever wanted windows audio to come through the speakers connected to your Axe-Fx? Tired of routing audio through Voicemeeter just to get it into a preset? **axe-fx-usb78** is a small Windows audio router that copies playback to Axe-Fx USB channels 7 and 8.

It uses the Axe-Fx III's existing Windows playback endpoint and official FAS ASIO driver. It installs no custom audio driver, creates no virtual Windows device, and does not require Voicemeeter.

**[Download the latest Windows installer](https://github.com/DTrescher/axe-fx-usb78/releases/latest).**

## How it works

```text
Windows shared-mode audio
        ↓
Speakers (Axe-Fx III), the existing Windows playback endpoint
        ↓ WASAPI loopback
axe-fx-usb78 user-mode bridge
        ↓ official FAS ASIO driver, USB channels 7/8
Axe-Fx USB block in your preset → the output you choose
```

Add a USB block to the preset or scene where you want to hear computer audio, then route that block to the desired Axe-Fx output. You can shape or mix the computer audio in the preset like any other USB input.

The Windows endpoint continues sending its normal playback to USB 1/2. This application adds a copy on USB 7/8; it does not replace or mute the original USB 1/2 signal.

## Quick start

### Install from a release

Download and run `axe-fx-usb78-setup-<version>.exe` from [GitHub Releases](https://github.com/DTrescher/axe-fx-usb78/releases/latest). The installer is per-user and does not need administrator access. It adds **Axe-Fx USB 7-8 Audio Router** to Windows **Settings → Apps → Apps & features** on Windows 10 or **Installed apps** on Windows 11, and creates a Start menu shortcut. Setup does not install or change audio drivers.

During setup, you can optionally create a desktop shortcut or start the panel automatically when you sign in. To remove the application, use its **Uninstall** Start menu shortcut or choose it in Windows **Apps & features / Installed apps**. The uninstaller stops the panel and bridge, removes the installed files and shortcuts, and keeps your routing preference for a later reinstall.

### Requirements to use the application

- Windows 10 or later, x64 (Intel or AMD)
- Axe-Fx III with the official Fractal Audio USB/ASIO driver installed

### Build from source

To build the application yourself, use Visual Studio 2022 with the **Desktop development with C++** workload and CMake 3.21 or later.

Open PowerShell in the repository folder and run:

```powershell
.\tools\build.ps1 -Test
```

The application and control panel are built in `build\Release`.

To create the Windows installer, install Inno Setup 6 or 7 and run:

```powershell
.\tools\build-installer.ps1
```

The installer is written to `artifacts\installer`. The build uses the static Visual C++ runtime, so users do not need a separate runtime installation.

### Route audio

1. In Windows sound settings, use **Speakers (Axe-Fx III)** as the playback device. Set it as the default, or select it in apps that have their own output-device setting.
2. Start the control panel from the Start menu, or from the source checkout with:

   ```powershell
   .\tools\start-bridge.ps1
   ```

3. Enable routing in the panel. It stays in the notification area when you close its window.
4. In your Axe-Fx preset or scene, add a USB block and connect it to the output you want.

The panel remembers the enabled/disabled choice for your Windows account. The tray menu can show the panel, change routing, restart the bridge, or exit.

For a source checkout, start the panel at sign-in with:

```powershell
.\tools\register-startup.ps1
```

Remove that registration with:

```powershell
.\tools\register-startup.ps1 -Remove
```

For an installed copy, select **Start the control panel when I sign in** during setup instead. When upgrading a source checkout from the earlier bridge, stop it before registering startup. If its scheduled task is enabled, the registration script disables that task and leaves it available for rollback; it restores the task if registration fails.

## What is routed

The bridge captures shared-mode audio sent to the native **Speakers (Axe-Fx III)** endpoint. Apps assigned to another output and exclusive-mode audio are outside that route. The application does not create a new Windows playback device.

The ASIO callback uses a bounded, preallocated audio queue and does not allocate, lock, or log while rendering. The default queue target is 13 ms; this is a buffering target, not a measurement of total speaker latency. See [architecture](docs/architecture.md) and [hardware validation](docs/hardware-validation.md).

## Project layout

- `include/axe/` — reusable audio pipeline and ASIO interfaces
- `src/` — Windows capture, routing bridge, and control panel
- `tests/` — automated tests, hardware diagnostics, and test support
- `tools/` — build, installer, launch, startup, and diagnostic PowerShell scripts
- `installer/` — Inno Setup script for the per-user Windows installer
- `docs/` — architecture and validation notes
- `third_party/asio/` — unmodified Steinberg ASIO interface headers and their license

Contributor setup and coding notes are in [CONTRIBUTING.md](CONTRIBUTING.md). Build and diagnostic instructions are in [docs/testing.md](docs/testing.md). The automated release process is described in [docs/releasing.md](docs/releasing.md).

## License

The project's own code is available under the MIT License. I don't charge for it, and contributions are welcome. The bundled ASIO headers have separate Steinberg/GPLv3 license terms; see [third-party notices](THIRD_PARTY_NOTICES.md).
