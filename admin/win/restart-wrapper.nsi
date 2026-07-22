; owncloud.online client setup wrapper.
;
; Wraps the craft-built NSIS installer and adds what a managed rollout needs:
;  1. stops a running client first, so the installer can actually replace the
;     binaries (a running instance otherwise keeps the old version alive),
;  2. runs the embedded installer silently,
;  3. requires the Windows restart that loads the Explorer integration
;     (overlay icons, context menu, virtual files sync root) - and performs it.
;
; Interactive: the finish page offers "Reboot now" (default) / "reboot later".
; Silent (/S, e.g. baramundi): reboots automatically after the installation
; unless /norestart is passed.
;
; Build (see .github/workflows/main.yml):
;   makensis /DSOURCE_EXE=<craft-installer.exe> /DOUTFILE=<out.exe> \
;            /DVERSION=<x.y.z> admin/win/restart-wrapper.nsi

Unicode true

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif

Name "owncloud.online Client ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "BW-Tech GmbH - owncloud.online"

; Branded default install directory. A different path can be chosen on the
; directory page, or passed as /D=... (must be the last parameter, unquoted) -
; it is forwarded to the embedded installer. An existing installation in the
; old default directory ($PROGRAMFILES64\ownCloud) is uninstalled first, so
; upgrades move over cleanly (accounts and sync folders live in the user
; profile and are not touched by that).
InstallDir "$PROGRAMFILES64\owncloud.online"
!define OLD_INSTDIR "$PROGRAMFILES64\ownCloud"

!define CLIENT_PROCESS "owncloud.online.exe"

!define MUI_ICON "..\..\src\resources\theme\universal\owncloud-online.ico"

; The finish page shows the reboot-now/-later choice because the install
; section sets the reboot flag.
!define MUI_FINISHPAGE_TEXT_REBOOT "Um die Installation abzuschliessen, muss Windows neu gestartet werden, damit die Explorer-Integration korrekt funktioniert.$\r$\n$\r$\nTo complete the installation, Windows must be restarted so that the Explorer integration works correctly."
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "English"

Var ClientExe

; in: $2 = candidate install dir. out: $ClientExe = full path of the client
; binary, or "" if none found. Newest layout first (7.5.x installs
; bin\owncloud.online.exe), older/upstream layouts as fallback.
Function FindClientExe
  ${If} ${FileExists} "$2\bin\owncloud.online.exe"
    StrCpy $ClientExe "$2\bin\owncloud.online.exe"
  ${ElseIf} ${FileExists} "$2\owncloud.online.exe"
    StrCpy $ClientExe "$2\owncloud.online.exe"
  ${ElseIf} ${FileExists} "$2\bin\owncloud.exe"
    StrCpy $ClientExe "$2\bin\owncloud.exe"
  ${ElseIf} ${FileExists} "$2\owncloud.exe"
    StrCpy $ClientExe "$2\owncloud.exe"
  ${Else}
    StrCpy $ClientExe ""
  ${EndIf}
FunctionEnd

Section "Install"
  SetDetailsPrint both

  ; Stop a running client; ignore errors (it may simply not be running).
  DetailPrint "Beende laufenden owncloud.online Client / stopping running client ..."
  nsExec::ExecToLog 'taskkill /F /IM "${CLIENT_PROCESS}"'
  Pop $0
  ; older generations shipped the binary as owncloud.exe - stop that one too
  nsExec::ExecToLog 'taskkill /F /IM "owncloud.exe"'
  Pop $0
  Sleep 1500

  ; ---- Steer the embedded installer to $INSTDIR -----------------------------
  ; The embedded craft installer IGNORES /D= completely: its MultiUser setup
  ; unconditionally resets $INSTDIR in .onInit and then applies the Install_Dir
  ; value from HKLM "Software\ownCloud GmbH\ownCloud" (32-bit view) when
  ; present. Writing that value beforehand is the only reliable way to choose
  ; its target directory - it is the same mechanism its own upgrades use.
  ; (This wrapper is a 32-bit process, so a plain HKLM write lands in the same
  ; WOW6432Node view the embedded installer reads.)
  ; $8 remembers the previous value so a failed install can restore it.
  ReadRegStr $8 HKLM "Software\ownCloud GmbH\ownCloud" "Install_Dir"
  WriteRegStr HKLM "Software\ownCloud GmbH\ownCloud" "Install_Dir" "$INSTDIR"

  ; Unpack and run the real installer silently.
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "/oname=client-setup.exe" "${SOURCE_EXE}"
  DetailPrint "Installiere owncloud.online Client ${VERSION} nach $INSTDIR / installing ..."
  ; /D= is kept as belt and braces although the registry value above decides
  ExecWait '"$PLUGINSDIR\client-setup.exe" /S /D=$INSTDIR' $0
  DetailPrint "Installer exit code: $0"
  IntCmp $0 0 installOk
    SetDetailsPrint both
    DetailPrint "Installation fehlgeschlagen / installation failed (exit code $0)."
    ; restore the previous steering value - whatever installation the machine
    ; really has still lives where it did before this attempt
    ${If} $8 != ""
      WriteRegStr HKLM "Software\ownCloud GmbH\ownCloud" "Install_Dir" "$8"
    ${Else}
      DeleteRegValue HKLM "Software\ownCloud GmbH\ownCloud" "Install_Dir"
    ${EndIf}
    Abort "Installation fehlgeschlagen / installation failed (exit code $0)."
  installOk:

  ; ---- Verify where the client actually landed ------------------------------
  ; $9 = client binary strictly under $INSTDIR, or "" - the migration below
  ; must only ever run when the new client verifiably lives OUTSIDE the old
  ; directory, so it can never delete the installation we just made.
  StrCpy $2 "$INSTDIR"
  Call FindClientExe
  StrCpy $9 "$ClientExe"

  ; ---- Migrate away from the old default directory --------------------------
  ; Earlier versions installed to "$PROGRAMFILES64\ownCloud". Remove such an
  ; installation only now, after the new install has been VERIFIED in its new
  ; location. The old uninstaller deletes the shared Uninstall\ownCloud and
  ; "ownCloud GmbH" registry keys, so the entries for the NEW install are
  ; rewritten right afterwards.
  ${If} $9 != ""
  ${AndIf} "$INSTDIR" != "${OLD_INSTDIR}"
  ${AndIf} ${FileExists} "${OLD_INSTDIR}\*.*"
    DetailPrint "Entferne alte Installation in ${OLD_INSTDIR} / removing old installation ..."
    ; unregister the old shell extensions first - Explorer holds those DLLs
    StrCpy $6 "$WINDIR\SysNative\regsvr32.exe"
    ${IfNot} ${FileExists} "$6"
      StrCpy $6 "$SYSDIR\regsvr32.exe"
    ${EndIf}
    ${If} ${FileExists} "${OLD_INSTDIR}\bin\OCOverlays.dll"
      nsExec::ExecToLog '"$6" /u /s "${OLD_INSTDIR}\bin\OCOverlays.dll"'
      Pop $0
    ${EndIf}
    ${If} ${FileExists} "${OLD_INSTDIR}\bin\OCContextMenu.dll"
      nsExec::ExecToLog '"$6" /u /s "${OLD_INSTDIR}\bin\OCContextMenu.dll"'
      Pop $0
    ${EndIf}
    ; The old uninstaller is deliberately NOT executed: its MultiUser
    ; un.onInit re-reads the Install_Dir registry value - which now points at
    ; the NEW installation - and overrides _?= with it, so it would delete
    ; the client we just installed. Everything it would clean up is covered
    ; here directly: the files below, the shared registry entries right after.
    ; Files still locked (e.g. shell DLLs loaded by Explorer) are removed at
    ; the reboot this installer mandates anyway.
    Delete /REBOOTOK "${OLD_INSTDIR}\uninstall.exe"
    RMDir /r /REBOOTOK "${OLD_INSTDIR}"
    ; make sure the registry entries describe the NEW installation (the
    ; embedded installer generation writes in the default 32-bit view)
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName" "owncloud.online Client ${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "Software\ownCloud GmbH\ownCloud" "Install_Dir" "$INSTDIR"
  ${EndIf}

  ; ---- Rebrand the Apps & Features entry ------------------------------------
  ; The embedded installer writes its uninstall entry with upstream branding
  ; ("ownCloud", Publisher "ownCloud GmbH"). Rewrite the user-visible values in
  ; whichever registry view the entry landed in.
  DetailPrint "Aktualisiere Programme-und-Features-Eintrag / rebranding uninstall entry ..."
  ReadRegStr $7 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName"
  ${If} $7 != ""
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName" "owncloud.online Client ${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "Publisher" "BW.Tech GmbH"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "URLInfoAbout" "https://github.com/GrossLukas/client"
  ${EndIf}
  SetRegView 64
  ReadRegStr $7 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName"
  ${If} $7 != ""
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName" "owncloud.online Client ${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "Publisher" "BW.Tech GmbH"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "URLInfoAbout" "https://github.com/GrossLukas/client"
  ${EndIf}
  SetRegView 32

  ; ---- Repair the shortcuts -----------------------------------------------
  ; The embedded installer hardcodes its Start menu shortcut to
  ; "$INSTDIR\bin\owncloud.exe" (the upstream binary name) and creates no
  ; desktop shortcut. Depending on the client generation the real binary is
  ; bin\owncloud.online.exe (current), owncloud.online.exe or owncloud.exe:
  ; find it and create correctly branded, working shortcuts.

  StrCpy $2 "$INSTDIR" ; the dir the embedded installer was steered to
  StrCpy $ClientExe "$9" ; verified above
  ${If} $ClientExe == ""
    ; fallback: resolve the install dir from the uninstall entry
    ReadRegStr $1 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "UninstallString"
    ${If} $1 == ""
      SetRegView 64
      ReadRegStr $1 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "UninstallString"
      SetRegView 32
    ${EndIf}
    ${If} $1 != ""
      StrCpy $4 $1 1
      ${If} $4 == '"'
        StrCpy $1 $1 -1 1 ; strip the surrounding quotes
      ${EndIf}
      ${GetParent} $1 $2
      Call FindClientExe
    ${EndIf}
  ${EndIf}

  ${If} $ClientExe != ""
    DetailPrint "Client-Programmdatei / client binary: $ClientExe"
    ; remove the broken upstream-named shortcuts, in both shell contexts and
    ; the possible locations (top level and start menu folder)
    SetShellVarContext all
    Delete "$SMPROGRAMS\ownCloud.lnk"
    Delete "$SMPROGRAMS\ownCloud\ownCloud.lnk"
    RMDir "$SMPROGRAMS\ownCloud"
    Delete "$SMPROGRAMS\ownCloud Desktop Client\ownCloud.lnk"
    RMDir "$SMPROGRAMS\ownCloud Desktop Client"
    Delete "$DESKTOP\ownCloud.lnk"
    SetShellVarContext current
    Delete "$SMPROGRAMS\ownCloud.lnk"
    Delete "$SMPROGRAMS\ownCloud\ownCloud.lnk"
    RMDir "$SMPROGRAMS\ownCloud"
    Delete "$SMPROGRAMS\ownCloud Desktop Client\ownCloud.lnk"
    RMDir "$SMPROGRAMS\ownCloud Desktop Client"
    Delete "$DESKTOP\ownCloud.lnk"
    ; branded icon if the installer shipped it, else the binary's own icon
    ${If} ${FileExists} "$2\owncloud.ico"
      StrCpy $5 "$2\owncloud.ico"
    ${Else}
      StrCpy $5 "$ClientExe"
    ${EndIf}
    ; create the branded, working shortcuts for all users
    SetShellVarContext all
    ${GetParent} $ClientExe $4
    SetOutPath "$4" ; becomes the shortcuts' working directory
    CreateShortCut "$SMPROGRAMS\owncloud.online.lnk" "$ClientExe" "" "$5"
    CreateShortCut "$DESKTOP\owncloud.online.lnk" "$ClientExe" "" "$5"
    DetailPrint "Startmenue- und Desktop-Verknuepfung erstellt / start menu and desktop shortcuts created."

    ; branded icon for the Apps & Features entry too
    ReadRegStr $7 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName"
    ${If} $7 != ""
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayIcon" "$5"
    ${EndIf}
    SetRegView 64
    ReadRegStr $7 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName"
    ${If} $7 != ""
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayIcon" "$5"
    ${EndIf}
    SetRegView 32

    ; ---- Register the Explorer shell integration --------------------------
    ; The embedded installer ships OCOverlays.dll and OCContextMenu.dll next
    ; to the client binary but never registers them, so Explorer shows
    ; neither overlay icons nor the context menu. Their DllRegisterServer
    ; writes all required registry entries (CLSIDs and the
    ; ShellIconOverlayIdentifiers keys, HKLM 64-bit view). The DLLs are
    ; 64-bit, so use the 64-bit regsvr32: from this 32-bit installer process
    ; that is $WINDIR\SysNative; fall back to $SYSDIR just in case.
    StrCpy $6 "$WINDIR\SysNative\regsvr32.exe"
    ${IfNot} ${FileExists} "$6"
      StrCpy $6 "$SYSDIR\regsvr32.exe"
    ${EndIf}
    ${If} ${FileExists} "$4\OCOverlays.dll"
      DetailPrint "Registriere Explorer-Overlay-Symbole / registering overlay icons ..."
      nsExec::ExecToLog '"$6" /s "$4\OCOverlays.dll"'
      Pop $0
      DetailPrint "OCOverlays.dll: regsvr32 exit code $0"
    ${Else}
      DetailPrint "OCOverlays.dll nicht gefunden / not found in $4 - overlays skipped."
    ${EndIf}
    ${If} ${FileExists} "$4\OCContextMenu.dll"
      DetailPrint "Registriere Explorer-Kontextmenue / registering context menu ..."
      nsExec::ExecToLog '"$6" /s "$4\OCContextMenu.dll"'
      Pop $0
      DetailPrint "OCContextMenu.dll: regsvr32 exit code $0"
    ${Else}
      DetailPrint "OCContextMenu.dll nicht gefunden / not found in $4 - context menu skipped."
    ${EndIf}
  ${Else}
    ; must not happen: the embedded installer reported success but no client
    ; binary can be found - say so loudly instead of finishing silently
    DetailPrint "FEHLER: Client-Programmdatei nicht gefunden / ERROR: client binary not found."
    MessageBox MB_OK|MB_ICONEXCLAMATION "Die Installation scheint unvollstaendig zu sein: die Client-Programmdatei wurde nicht gefunden. Bitte den Setup erneut ausfuehren und andernfalls den Support kontaktieren.$\r$\n$\r$\nThe installation appears to be incomplete: the client binary was not found. Please re-run the setup." /SD IDOK
  ${EndIf}
  ; --------------------------------------------------------------------------

  ; The Explorer integration only loads reliably after a restart.
  SetRebootFlag true
SectionEnd

Function .onInstSuccess
  ; In silent mode there is no finish page: reboot directly, unless the
  ; caller (e.g. a baramundi job that manages reboots itself) passed /norestart.
  IfSilent 0 interactiveDone
    ${GetParameters} $R0
    ClearErrors
    ${GetOptions} $R0 "/norestart" $R1
    IfErrors 0 interactiveDone ; option found -> caller handles the reboot
    DetailPrint "Starte Windows neu / rebooting ..."
    Reboot
  interactiveDone:
FunctionEnd
