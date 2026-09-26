@echo off
echo Set the ArcadeDuck translation language to edit.
echo.
echo Supported non-English catalogs:
echo   de      German
echo   es      Spanish
echo   fr      French
echo   ja      Japanese
echo   ko      Korean
echo   pt-BR   Brazilian Portuguese
echo   ru      Russian
echo   zh-CN   Simplified Chinese
echo.
echo Current value: %lang%
set /p newlang="Enter supported language code (blank removes setting): "

if defined newlang (
  setx lang %newlang%
) else (
  echo Removing language setting...
  setx lang "" 1>nul
  reg delete HKCU\Environment /F /V lang 2>nul
  reg delete "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /F /V lang 2>nul
)
pause