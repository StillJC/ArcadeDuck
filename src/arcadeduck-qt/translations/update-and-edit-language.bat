@echo off
setlocal

if not defined lang (
  echo Please run set-language.bat first.
  pause
  exit /b 1
)

set "supported=0"
for %%L in (de es fr ja ko pt-BR ru zh-CN) do (
  if /I "%lang%"=="%%L" set "supported=1"
)

if "%supported%"=="0" (
  echo Unsupported ArcadeDuck translation language: %lang%
  echo Supported: de es fr ja ko pt-BR ru zh-CN
  pause
  exit /b 1
)

set "linguist=..\..\..\dep\msvc\deps-x64\bin"
set "context=../ ../../core/ ../../util/"
set "aliases=QT_TRANSLATE_NOOP+=TRANSLATE,QT_TRANSLATE_NOOP+=TRANSLATE_SV,QT_TRANSLATE_NOOP+=TRANSLATE_STR,QT_TRANSLATE_NOOP+=TRANSLATE_FS,QT_TRANSLATE_N_NOOP3+=TRANSLATE_FMT,QT_TRANSLATE_NOOP+=TRANSLATE_NOOP,translate+=TRANSLATE_PLURAL_STR,translate+=TRANSLATE_PLURAL_SSTR,translate+=TRANSLATE_PLURAL_FS"

"%linguist%\lupdate.exe" %context% -tr-function-alias %aliases% -no-obsolete -ts arcadeduck-qt_%lang%.ts
if errorlevel 1 (
  pause
  exit /b %errorlevel%
)

cd "%linguist%"
start /B linguist.exe "%~dp0\arcadeduck-qt_%lang%.ts"
endlocal