; HYPE DJ PRO - Windows x64 installer (NSIS 3)
;
;   makensis -DSRC=<package folder> -DVERSION=0.1.0 -DOUTFILE=<setup.exe> packaging/windows/installer.nsi
;
; <package folder> is the folder produced by scripts/build-windows.sh
; (build-win/package/HYPE-DJ-PRO-windows-x64).

Unicode true
Target amd64-unicode      ; native 64-bit installer (the app is 64-bit only)
SetCompressor /SOLID lzma

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef SRC
  !error "Pass -DSRC=<package folder>"
!endif
!ifndef OUTFILE
  !define OUTFILE "HYPE-DJ-PRO-${VERSION}-Setup.exe"
!endif

!define APPNAME   "HYPE DJ PRO"
!define COMPANY   "HYPE"
!define EXE       "HYPE DJ PRO.exe"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\HYPE DJ PRO"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${COMPANY}\${APPNAME}" "InstallDir"
RequestExecutionLevel admin
ShowInstDetails show

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "CompanyName" "${COMPANY}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "See licenses folder"

!include "MUI2.nsh"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APPNAME}"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Show the README"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRC}\licenses\THIRD-PARTY-NOTICES.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetRegView 64
  SetShellVarContext all     ; machine-wide install: shortcuts for all users
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "${APPNAME} (required)" SecApp
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${SRC}\${EXE}"
  File "${SRC}\hype_cli.exe"
  File "${SRC}\avcodec-60.dll"
  File "${SRC}\avformat-60.dll"
  File "${SRC}\avutil-58.dll"
  File "${SRC}\swresample-4.dll"
  File "${SRC}\README.txt"
  File "${SRC}\TESTING.md"
  SetOutPath "$INSTDIR\licenses"
  File /r "${SRC}\licenses\*.*"

  SetOutPath "$INSTDIR"
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${EXE}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\README.lnk" "$INSTDIR\README.txt"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\${COMPANY}\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "${COMPANY}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXE}"
SectionEnd

Section /o "Automated test suites" SecTests
  SetOutPath "$INSTDIR\tests"
  File "${SRC}\tests\hype_phase1_tests.exe"
  File "${SRC}\tests\hype_engine_tests.exe"
  File "${SRC}\tests\avcodec-60.dll"
  File "${SRC}\tests\avformat-60.dll"
  File "${SRC}\tests\avutil-58.dll"
  File "${SRC}\tests\swresample-4.dll"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp}     "The application, command-line tool, FFmpeg decoder DLLs and documentation."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Add a ${APPNAME} shortcut to the desktop."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecTests}   "Phase 1 acceptance tests and engine tests (for verifying a system; not needed to DJ)."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  RMDir /r "$INSTDIR\licenses"
  RMDir /r "$INSTDIR\tests"
  Delete "$INSTDIR\${EXE}"
  Delete "$INSTDIR\hype_cli.exe"
  Delete "$INSTDIR\avcodec-60.dll"
  Delete "$INSTDIR\avformat-60.dll"
  Delete "$INSTDIR\avutil-58.dll"
  Delete "$INSTDIR\swresample-4.dll"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\TESTING.md"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "Software\${COMPANY}\${APPNAME}"
  DeleteRegKey /ifempty HKLM "Software\${COMPANY}"
  ; User settings (%APPDATA%\HYPE DJ PRO) are kept, like most applications do.
SectionEnd
