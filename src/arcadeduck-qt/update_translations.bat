@echo off
setlocal

set "LINGUIST=..\..\dep\msvc\deps-x64\bin"
set "LUPDATE=%LINGUIST%\lupdate.exe"
set "CONTEXT=./ ../core/ ../util/"
set "ALIASES=QT_TRANSLATE_NOOP+=TRANSLATE,QT_TRANSLATE_NOOP+=TRANSLATE_SV,QT_TRANSLATE_NOOP+=TRANSLATE_STR,QT_TRANSLATE_NOOP+=TRANSLATE_FS,QT_TRANSLATE_N_NOOP3+=TRANSLATE_FMT,QT_TRANSLATE_NOOP+=TRANSLATE_NOOP,translate+=TRANSLATE_PLURAL_STR,translate+=TRANSLATE_PLURAL_SSTR,translate+=TRANSLATE_PLURAL_FS"

if not exist "%LUPDATE%" (
  echo ERROR: lupdate.exe not found at "%LUPDATE%"
  exit /b 1
)

where py.exe >nul 2>nul
if %errorlevel%==0 (
  py.exe -3 ..\..\scripts\generate_fullscreen_ui_translation_strings.py
) else (
  python.exe ..\..\scripts\generate_fullscreen_ui_translation_strings.py
)
if errorlevel 1 exit /b %errorlevel%

"%LUPDATE%" %CONTEXT% -tr-function-alias %ALIASES% -pluralonly -no-obsolete -ts translations\arcadeduck-qt_en.ts
if errorlevel 1 exit /b %errorlevel%

for %%L in (de es fr ja ko pt-BR ru zh-CN) do (
  "%LUPDATE%" %CONTEXT% -tr-function-alias %ALIASES% -no-obsolete -ts translations\arcadeduck-qt_%%L.ts
  if errorlevel 1 exit /b %errorlevel%
)

echo.
echo ArcadeDuck translation catalogs updated successfully.
endlocal