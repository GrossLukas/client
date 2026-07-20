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

!ifndef VERSION
  !define VERSION "0.0.0"
!endif

Name "owncloud.online Client ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "BW-Tech GmbH - owncloud.online"

!define CLIENT_PROCESS "owncloud.online.exe"

!define MUI_ICON "..\..\src\resources\theme\universal\owncloud-online.ico"

; The finish page shows the reboot-now/-later choice because the install
; section sets the reboot flag.
!define MUI_FINISHPAGE_TEXT_REBOOT "Um die Installation abzuschliessen, muss Windows neu gestartet werden, damit die Explorer-Integration korrekt funktioniert.$\r$\n$\r$\nTo complete the installation, Windows must be restarted so that the Explorer integration works correctly."
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "English"

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
  DetailPrint "Installiere owncloud.online Client ${VERSION} / installing ..."
  ExecWait '"$PLUGINSDIR\client-setup.exe" /S' $0
  DetailPrint "Installer exit code: $0"
  IntCmp $0 0 installOk
    SetDetailsPrint both
    DetailPrint "Installation fehlgeschlagen / installation failed (exit code $0)."
    Abort "Installation fehlgeschlagen / installation failed (exit code $0)."
  installOk:

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
