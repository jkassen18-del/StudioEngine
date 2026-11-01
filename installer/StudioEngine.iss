; Studio Engine - Windows installer (Inno Setup 6)
; Built by CI:  iscc /DAppVersion=1.0.0 /DSrcDir=<artefacts dir> installer\StudioEngine.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SrcDir
  #define SrcDir "..\build\StudioEngine_artefacts\Release"
#endif

[Setup]
AppId={{6F3B8E2A-5C1D-4E7A-9B2F-3D8C1A7E5F40}
AppName=Studio Engine
AppVersion={#AppVersion}
AppPublisher=Studio Engine
DefaultDirName={autopf}\Studio Engine
DefaultGroupName=Studio Engine
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=StudioEngine-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayName=Studio Engine

[Types]
Name: "full"; Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (for your DAW)"; Types: full custom; Flags: fixed
Name: "standalone"; Description: "Standalone app (use without a DAW)"; Types: full
Name: "profiles"; Description: "Starter profiles"; Types: full

[Files]
; VST3 bundle -> C:\Program Files\Common Files\VST3\Studio Engine.vst3
Source: "{#SrcDir}\VST3\Studio Engine.vst3\*"; DestDir: "{commoncf64}\VST3\Studio Engine.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcDir}\Standalone\Studio Engine.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "..\profiles\*.json"; DestDir: "{commondocs}\Studio Engine\Profiles"; Components: profiles; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Studio Engine (Standalone)"; Filename: "{app}\Studio Engine.exe"; Components: standalone
Name: "{group}\Studio Engine Profiles"; Filename: "{commondocs}\Studio Engine\Profiles"; Components: profiles
Name: "{group}\Uninstall Studio Engine"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\Studio Engine.exe"; Description: "Launch the standalone app now"; Components: standalone; Flags: nowait postinstall skipifsilent unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Studio Engine.vst3"
