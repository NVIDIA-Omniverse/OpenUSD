@echo off
setlocal

if "%HFS%"=="" (
    echo Set HFS to the Houdini installation used to build the package. 1>&2
    exit /b 2
)

set "SCRIPT_DIR=%~dp0"
set "PACKAGE_ROOT=%SCRIPT_DIR%build\package"
set "PACKAGE_DIR=%PACKAGE_ROOT%\typhoon"
set "OUTPUT_PATH=%SCRIPT_DIR%build\smoke.exr"
set "HOUDINI_PACKAGE_SKIP=1"

if not exist "%PACKAGE_ROOT%\typhoon.json" exit /b 3

call "%PACKAGE_DIR%\husk-typhoon.cmd" --list-renderers 2>&1 ^
    | findstr /c:"HdEmbreeRendererPlugin" >nul
if errorlevel 1 exit /b 4

set "HOUDINI_PATH=%PACKAGE_DIR%;&"
"%HFS%\bin\hython.exe" "%SCRIPT_DIR%smoke_ui.py"
if errorlevel 1 exit /b 5

call "%PACKAGE_DIR%\husk-typhoon.cmd" ^
    -R HdEmbreeRendererPlugin ^
    -o "%OUTPUT_PATH%" ^
    --headlight distant ^
    "%SCRIPT_DIR%smoke.usda"
if errorlevel 1 exit /b 6

if not exist "%OUTPUT_PATH%" exit /b 7
