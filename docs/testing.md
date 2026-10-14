# Build and validation

## Build and automated tests

On Windows, install Visual Studio 2022 with the **Desktop development with C++** workload and CMake 3.21 or later. From PowerShell at the repository root:

```powershell
.\tools\build.ps1 -Test
```

This configures an x64 Visual Studio build, builds the Release configuration, and runs CTest. The automated tests cover the audio pipeline, diagnostic signal metrics, panel settings persistence, and ASIO sample-format/lifecycle behavior with a fake driver. They do not require an Axe-Fx to be connected.

## Build the Windows installer

Install Inno Setup 6 or 7, then run:

```powershell
.\tools\build-installer.ps1
```

The script builds the Release executables and packages them with the project and third-party notices. It reads the installer version from `CMakeLists.txt` and writes the setup executable to `artifacts\installer`.

## Optional hardware diagnostics

Hardware diagnostics are built with the Windows application but run only when invoked. Stop the tray panel and any other FAS ASIO host before running them.

List the active render endpoints and copy the ID for **Speakers (Axe-Fx III)**:

```powershell
.\build\Release\axe-device-probe.exe
```

Check that the Console, Multimedia, and Communications Windows playback roles use that endpoint:

```powershell
.\tools\verify-fas-output.ps1
```

The endpoint check is read-only. The hardware test accepts the endpoint ID explicitly:

```powershell
.\tools\test-hardware.ps1 -EndpointId '<endpoint ID>' -DurationSeconds 30
.\tools\test-stability.ps1 -EndpointId '<endpoint ID>' -DurationSeconds 600
```

The hardware test sends quiet test tones and temporarily changes native endpoint volume, balance, and mute during its volume check; it restores those settings afterward. Its physical return check needs a temporary Axe-Fx preset connection from INPUT USB to OUT 1 USB so the ASIO return can be recorded. This check does not measure acoustic latency at the OUT 3 speakers. The stability check runs bounded, normal-priority CPU workers for part of the test and cleans them up when it exits.

These diagnostics verify one connected device and driver configuration. They are not a compatibility guarantee for every FAS driver version, exclusive-mode application, protected-content path, or suspend/reconnect scenario.
