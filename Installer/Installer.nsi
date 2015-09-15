!define PPSSPP_ARCH x64
!define BASE_INSTALL_DIR "$PROGRAMFILES64"
!define PRODUCT_NAME "PPSSPP VR"

!define PRODUCT_VERSION 1.0.1

!define BASE_DIR ".."

; HM NIS Edit Wizard helper defines
!define PRODUCT_PUBLISHER "Carl Kenner"
!define PRODUCT_WEB_SITE "https://github.com/CarlKenner/ppsspp"
!define PRODUCT_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\${PRODUCT_NAME}.exe"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_UNINST_ROOT_KEY "HKLM"

SetCompressor /SOLID lzma

; MUI 1.67 compatible ------
!include "MUI.nsh"

; MUI Settings
!define MUI_ABORTWARNING
!define MUI_ICON "..\Windows\ppsspp.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

; Language Selection Dialog Settings
!define MUI_LANGDLL_REGISTRY_ROOT "${PRODUCT_UNINST_ROOT_KEY}"
!define MUI_LANGDLL_REGISTRY_KEY "${PRODUCT_UNINST_KEY}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "NSIS:Language"

; License page
!insertmacro MUI_PAGE_LICENSE "${BASE_DIR}\LICENSE.TXT"
; Components page
!insertmacro MUI_PAGE_COMPONENTS
; Directory page
!insertmacro MUI_PAGE_DIRECTORY
; Instfiles page
!insertmacro MUI_PAGE_INSTFILES
; Finish page
!insertmacro MUI_PAGE_FINISH

; Uninstaller pages
!insertmacro MUI_UNPAGE_INSTFILES

; Language files
!insertmacro MUI_LANGUAGE "Afrikaans"
!insertmacro MUI_LANGUAGE "Albanian"
!insertmacro MUI_LANGUAGE "Arabic"
!insertmacro MUI_LANGUAGE "Basque"
!insertmacro MUI_LANGUAGE "Belarusian"
!insertmacro MUI_LANGUAGE "Bosnian"
!insertmacro MUI_LANGUAGE "Breton"
!insertmacro MUI_LANGUAGE "Bulgarian"
!insertmacro MUI_LANGUAGE "Catalan"
!insertmacro MUI_LANGUAGE "Croatian"
!insertmacro MUI_LANGUAGE "Czech"
!insertmacro MUI_LANGUAGE "Danish"
!insertmacro MUI_LANGUAGE "Dutch"
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Estonian"
!insertmacro MUI_LANGUAGE "Farsi"
!insertmacro MUI_LANGUAGE "Finnish"
!insertmacro MUI_LANGUAGE "French"
!insertmacro MUI_LANGUAGE "Galician"
!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "Greek"
!insertmacro MUI_LANGUAGE "Hebrew"
!insertmacro MUI_LANGUAGE "Hungarian"
!insertmacro MUI_LANGUAGE "Icelandic"
!insertmacro MUI_LANGUAGE "Indonesian"
!insertmacro MUI_LANGUAGE "Irish"
!insertmacro MUI_LANGUAGE "Italian"
!insertmacro MUI_LANGUAGE "Japanese"
!insertmacro MUI_LANGUAGE "Korean"
!insertmacro MUI_LANGUAGE "Kurdish"
!insertmacro MUI_LANGUAGE "Latvian"
!insertmacro MUI_LANGUAGE "Lithuanian"
!insertmacro MUI_LANGUAGE "Luxembourgish"
!insertmacro MUI_LANGUAGE "Macedonian"
!insertmacro MUI_LANGUAGE "Malay"
!insertmacro MUI_LANGUAGE "Mongolian"
!insertmacro MUI_LANGUAGE "Norwegian"
!insertmacro MUI_LANGUAGE "NorwegianNynorsk"
!insertmacro MUI_LANGUAGE "Polish"
!insertmacro MUI_LANGUAGE "Portuguese"
!insertmacro MUI_LANGUAGE "PortugueseBR"
!insertmacro MUI_LANGUAGE "Romanian"
!insertmacro MUI_LANGUAGE "Russian"
!insertmacro MUI_LANGUAGE "Serbian"
!insertmacro MUI_LANGUAGE "SerbianLatin"
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "Slovak"
!insertmacro MUI_LANGUAGE "Slovenian"
!insertmacro MUI_LANGUAGE "Spanish"
!insertmacro MUI_LANGUAGE "SpanishInternational"
!insertmacro MUI_LANGUAGE "Swedish"
!insertmacro MUI_LANGUAGE "Thai"
!insertmacro MUI_LANGUAGE "TradChinese"
!insertmacro MUI_LANGUAGE "Turkish"
!insertmacro MUI_LANGUAGE "Ukrainian"
!insertmacro MUI_LANGUAGE "Uzbek"
!insertmacro MUI_LANGUAGE "Welsh"

; Reserve files
!insertmacro MUI_RESERVEFILE_INSTALLOPTIONS

; MUI end ------

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
!define UN_NAME "Uninstall $(^Name)"
OutFile "ppsspp-vr-${PRODUCT_VERSION}.exe"
InstallDir "${BASE_INSTALL_DIR}\$(^Name)"
InstallDirRegKey HKLM "${PRODUCT_DIR_REGKEY}" ""
ShowInstDetails show
ShowUnInstDetails show

Function .onInit
  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

Function unix2dos
    ; strips all CRs
    ; and then converts all LFs into CRLFs
    ; (this is roughly equivalent to "cat file | dos2unix | unix2dos")
    ;
    ; usage:
    ;    Push "infile"
    ;    Push "outfile"
    ;    Call unix2dos
    ;
    ; beware that this function destroys $0 $1 $2

    ClearErrors

    Pop $2
    FileOpen $1 $2 w 

    Pop $2
    FileOpen $0 $2 r

    Push $2 ; save name for deleting

    IfErrors unix2dos_done

    ; $0 = file input (opened for reading)
    ; $1 = file output (opened for writing)

unix2dos_loop:
    ; read a byte (stored in $2)
    FileReadByte $0 $2
    IfErrors unix2dos_done ; EOL
    ; skip CR
    StrCmp $2 13 unix2dos_loop
    ; if LF write an extra CR
    StrCmp $2 10 unix2dos_cr unix2dos_write

unix2dos_cr:
    FileWriteByte $1 13

unix2dos_write:
    ; write byte
    FileWriteByte $1 $2
    ; read next byte
    Goto unix2dos_loop

unix2dos_done:

    ; close files
    FileClose $0
    FileClose $1

    ; delete original
    Pop $0
    Delete $0

FunctionEnd

Section "Base" SEC01
  SectionIn RO
  SetShellVarContext all
  ; PPSSPP exe and dlls
  ; TODO: Make a nice subsection-ized display
  SetOutPath "$INSTDIR"
  SetOverwrite ifnewer
  File "${BASE_DIR}\PPSSPPWindows64.exe"
  File /oname=LICENSE.TMP "${BASE_DIR}\LICENSE.TXT"
  Push "$INSTDIR\LICENSE.TMP"
  Push "$INSTDIR\LICENSE.TXT"
  Call unix2dos
  File "${BASE_DIR}\README.md"
  Push "$INSTDIR\README.md"
  Push "$INSTDIR\README.txt"
  Call unix2dos
  File /nonfatal "${BASE_DIR}\*.dll"
  File /r /x .git "${BASE_DIR}\lang"
  File /r /x .git "${BASE_DIR}\flash0"
  File /r /x unix-icons /x *.icns /x .git /x android /x ios "${BASE_DIR}\assets"
  
  ; This needs to be done after PPSSPPWindows64.exe is copied
  CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
  CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" "$INSTDIR\PPSSPPWindows64.exe"
  CreateShortCut "$DESKTOP\${PRODUCT_NAME}.lnk" "$INSTDIR\PPSSPPWindows64.exe"
SectionEnd

Section -AdditionalIcons
  CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\${UN_NAME}.lnk" "$INSTDIR\uninst.exe"
SectionEnd

Section -Post
  WriteUninstaller "$INSTDIR\uninst.exe"
  WriteRegStr HKLM "${PRODUCT_DIR_REGKEY}" "" "$INSTDIR\PPSSPPWindows64.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayName" "$(^Name)"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninst.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\PPSSPPWindows64.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
SectionEnd

; Section descriptions
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC01} "Installs all files required to run the PPSSPP emulator."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section Uninstall
  SetShellVarContext all
  ; Only uninstall what we put there; all $INSTDIR\User is left as is
  Delete "$INSTDIR\uninst.exe"
  Delete "$INSTDIR\LICENSE.TXT"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\PPSSPPWindows64.exe"

  Delete "$SMPROGRAMS\${PRODUCT_NAME}\${UN_NAME}.lnk"
  Delete "$DESKTOP\${PRODUCT_NAME}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk"

  RMDir "$SMPROGRAMS\${PRODUCT_NAME}"
  RMDir /r "$INSTDIR\assets"
  RMDir /r "$INSTDIR\lang"
  RMDir /r "$INSTDIR\flash0"
  RMDir "$INSTDIR"

  DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKLM "${PRODUCT_DIR_REGKEY}"
  SetAutoClose true
SectionEnd
