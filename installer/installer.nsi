; Recast OBS Plugin — NSIS Windows Installer
; Builds an installer that copies the plugin DLL and data to the
; correct OBS Studio directories.

!include "MUI2.nsh"

; ---- General ----
Name "Recast OBS Plugin"
OutFile "recast-obs-plugin-installer.exe"
InstallDir "$PROGRAMFILES64\obs-studio"
InstallDirRegKey HKLM "Software\OBS Studio" ""
RequestExecutionLevel admin

; ---- Interface ----
!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"

; ---- Pages ----
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---- Install Section ----
Section "Recast OBS Plugin" SecMain
    SectionIn RO

    ; Plugin DLL
    SetOutPath "$INSTDIR\obs-plugins\64bit"
    File "..\build_windows\Release\recast-obs-plugin.dll"

    ; Data files (locale)
    SetOutPath "$INSTDIR\data\obs-plugins\recast-obs-plugin\locale"
    File "..\data\locale\en-US.ini"

    ; Uninstaller
    WriteUninstaller "$INSTDIR\recast-obs-plugin-uninstall.exe"

    ; Registry for Add/Remove Programs
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RecastOBSPlugin" \
        "DisplayName" "Recast OBS Plugin"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RecastOBSPlugin" \
        "UninstallString" "$\"$INSTDIR\recast-obs-plugin-uninstall.exe$\""
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RecastOBSPlugin" \
        "DisplayVersion" "1.0.0"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RecastOBSPlugin" \
        "Publisher" "Recast"
SectionEnd

; ---- Uninstall Section ----
Section "Uninstall"
    Delete "$INSTDIR\obs-plugins\64bit\recast-obs-plugin.dll"
    RMDir /r "$INSTDIR\data\obs-plugins\recast-obs-plugin"
    Delete "$INSTDIR\recast-obs-plugin-uninstall.exe"

    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RecastOBSPlugin"
SectionEnd
