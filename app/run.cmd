@echo off
setlocal

:: Pass your console's URL as the first argument, or set GLINT_URL in
:: your environment. Example:
::   run.cmd http://192.168.1.10:7231
set URL=%1
if "%URL%"=="" set URL=%GLINT_URL%
if "%URL%"=="" (
  echo Set GLINT_URL to http://^<ps5-ip^>:7231 or pass it as the first argument.
  exit /b 1
)
start "" "%~dp0build\Glint.exe" --url %URL%
