;
;
;
!define PRODUCT_NAME "MKCL"
!define PRODUCT_VERSION_MAJOR "1"
!define PRODUCT_VERSION_MINOR "1"
!define PRODUCT_VERSION_PATCH "8"
!define PRODUCT_VERSION "${PRODUCT_VERSION_MAJOR}.${PRODUCT_VERSION_MINOR}"
!define PRODUCT_FULL_VERSION "${PRODUCT_VERSION_MAJOR}.${PRODUCT_VERSION_MINOR}.${PRODUCT_VERSION_PATCH}"
!define PRODUCT_PUBLISHER "Jean-Claude Beaudoin"
!define PRODUCT_WEB_SITE "http://www.common-lisp.net/project/mkcl/"
!define PRODUCT_PATH_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\mkcl.exe"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME} ${PRODUCT_VERSION}"
!define PRODUCT_UNINST_ROOT_KEY "HKLM"


Name "MKCL 1.1.8"
OutFile "mkcl-1.1.8_win32_setup.exe"
InstallDir "$PROGRAMFILES\MKCL ${PRODUCT_VERSION}\"
InstallDirRegKey HKLM "${PRODUCT_PATH_REGKEY}\HOME" ""


;LicenseData Copyright_plus_LGPL.txt

;Page license
;#Page components
;Page directory
;Page instfiles
;#UninstPage uninstConfirm
;#UninstPage instfiles


!include "MUI2.nsh"

!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "Copyright_plus_LGPL.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"



Section
  SetOutPath "$INSTDIR"
  File /r mkcl\*.*
  File /r slime
  CreateDirectory "$SMPROGRAMS\MKCL ${PRODUCT_VERSION}"
  SetOutPath "%HOMEDRIVE%%HOMEPATH%" ; clear OUTDIR for the following shortcut creations.
  CreateShortCut "$DESKTOP\MKCL ${PRODUCT_VERSION} in REPL mode.lnk" "$INSTDIR\bin\mkcl.exe"
  CreateShortCut "$SMPROGRAMS\MKCL ${PRODUCT_VERSION}\MKCL ${PRODUCT_VERSION} in REPL mode.lnk" "$INSTDIR\bin\mkcl.exe"
SectionEnd

Section -AdditionalIcons
  WriteIniStr "$INSTDIR\${PRODUCT_NAME}.url" "InternetShortcut" "URL" "${PRODUCT_WEB_SITE}"
  CreateShortCut "$SMPROGRAMS\MKCL ${PRODUCT_VERSION}\MKCL's Website.lnk" "$INSTDIR\${PRODUCT_NAME}.url"
  CreateShortCut "$SMPROGRAMS\MKCL ${PRODUCT_VERSION}\Uninstall MKCL.lnk" "$INSTDIR\uninst.exe"
SectionEnd

Section -Post
  WriteUninstaller "$INSTDIR\uninst.exe"
  WriteRegStr HKLM "${PRODUCT_PATH_REGKEY}" "" "$INSTDIR\bin\mkcl.exe"
  WriteRegStr HKLM "${PRODUCT_PATH_REGKEY}" "HOME" "$INSTDIR\"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayName" "$(^Name)"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninst.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\bin\mkcl.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_FULL_VERSION}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR\"
  ReadRegStr $0 HKLM "${PRODUCT_PATH_REGKEY}" "HOME"
SectionEnd


Function un.onUninstSuccess
  HideWindow
  MessageBox MB_ICONINFORMATION|MB_OK "$(^Name) has been succesfully uninstalled."
FunctionEnd

Function un.onInit
  MessageBox MB_ICONQUESTION|MB_YESNO|MB_DEFBUTTON2 "Do you really want to uninstall $(^Name)?" IDYES +2
  Abort
FunctionEnd

Section Uninstall
  ReadRegStr $0 HKLM "${PRODUCT_UNINST_KEY}" "InstallLocation"
  RMDir /r $0

  Delete "$SMPROGRAMS\MKCL ${PRODUCT_VERSION}\Uninstall MKCL.lnk"
  Delete "$SMPROGRAMS\MKCL ${PRODUCT_VERSION}\MKCL's Website.lnk"
  Delete "$DESKTOP\MKCL ${PRODUCT_VERSION} in REPL mode.lnk"
  Delete "$SMPROGRAMS\MKCL ${PRODUCT_VERSION}\MKCL ${PRODUCT_VERSION} in REPL mode.lnk"

  RMDir "$SMPROGRAMS\MKCL ${PRODUCT_VERSION}"

  DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}"
  SetAutoClose true
SectionEnd

#Section Uninstall
#  Delete /r "$PROGRAMSFILES\mkcl"
#SectionEnd
