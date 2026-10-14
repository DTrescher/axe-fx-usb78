#ifndef AppVersion
#define AppVersion "0.1.0"
#endif

#define BuildDir "..\build\installer\Release"

[Setup]
AppId={{A5CC3C01-9B0C-4A49-86F9-59D27934611D}
AppName=Axe-Fx USB 7-8 Audio Router
AppVersion={#AppVersion}
AppPublisher=axe-fx-usb78 contributors
AppPublisherURL=https://github.com/DTrescher/axe-fx-usb78
AppSupportURL=https://github.com/DTrescher/axe-fx-usb78/issues
AppUpdatesURL=https://github.com/DTrescher/axe-fx-usb78/releases/latest
AppCopyright=Copyright (C) 2026 axe-fx-usb78 contributors
DefaultDirName={localappdata}\Programs\axe-fx-usb78
DefaultGroupName=Axe-Fx USB 7-8 Audio Router
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
Uninstallable=yes
UninstallDisplayName=Axe-Fx USB 7-8 Audio Router
UninstallDisplayIcon={app}\axe-usb78-panel.exe
OutputDir=..\artifacts\installer
OutputBaseFilename=axe-fx-usb78-setup-{#AppVersion}
VersionInfoVersion={#AppVersion}
VersionInfoCompany=axe-fx-usb78 contributors
VersionInfoDescription=Axe-Fx USB 7-8 Audio Router Setup
VersionInfoProductName=Axe-Fx USB 7-8 Audio Router
VersionInfoProductVersion={#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "startup"; Description: "Start the control panel when I sign in"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "{#BuildDir}\axe-usb78.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\axe-usb78-panel.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\CONTRIBUTING.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\third_party\asio\README.md"; DestDir: "{app}\third_party\asio"; Flags: ignoreversion
Source: "..\third_party\asio\LICENSE.txt"; DestDir: "{app}\third_party\asio"; Flags: ignoreversion

[Icons]
Name: "{group}\Axe-Fx USB 7-8 Audio Router"; Filename: "{app}\axe-usb78-panel.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall Axe-Fx USB 7-8 Audio Router"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Axe-Fx USB 7-8 Audio Router"; Filename: "{app}\axe-usb78-panel.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{userstartup}\Axe-Fx USB 7-8 Audio Router"; Filename: "{app}\axe-usb78-panel.exe"; Parameters: "--minimized"; WorkingDir: "{app}"; Tasks: startup

[Run]
Filename: "{app}\axe-usb78-panel.exe"; Description: "Launch Axe-Fx USB 7-8 Audio Router"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{app}\axe-usb78-panel.exe"; Parameters: "--shutdown"; WorkingDir: "{app}"; StatusMsg: "Stopping the audio router..."; Flags: runhidden waituntilterminated
