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

; Default matches the embedded installer's default. A different path can be
; chosen on the directory page, or passed as /D=... (must be the last
; parameter, unquoted) - it is forwarded to the embedded installer.
InstallDir "$PROGRAMFILES64\ownCloud"

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
  Sleep 1500

  ; Unpack and run the real installer silently.
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "/oname=client-setup.exe" "${SOURCE_EXE}"
  DetailPrint "Installiere owncloud.online Client ${VERSION} nach $INSTDIR / installing ..."
  ; forward the chosen install dir; /D= must be last and unquoted (NSIS rule)
  ExecWait '"$PLUGINSDIR\client-setup.exe" /S /D=$INSTDIR' $0
  DetailPrint "Installer exit code: $0"
  IntCmp $0 0 installOk
    SetDetailsPrint both
    DetailPrint "Installation fehlgeschlagen / installation failed (exit code $0)."
    Abort "Installation fehlgeschlagen / installation failed (exit code $0)."
  installOk:

  ; ---- Repair the shortcuts -----------------------------------------------
  ; The embedded installer hardcodes its Start menu shortcut to
  ; "$INSTDIR\bin\owncloud.exe" (the upstream binary name) and creates no
  ; desktop shortcut. Depending on the client generation the real binary is
  ; bin\owncloud.online.exe (current), owncloud.online.exe or owncloud.exe:
  ; find it and create correctly branded, working shortcuts.

  StrCpy $2 "$INSTDIR" ; the dir we just forwarded to the embedded installer
  Call FindClientExe
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
  ${Else}
    DetailPrint "Client-Programmdatei nicht gefunden / client binary not found - shortcuts unchanged."
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
