@echo off
REM Build the UE4SS updater proxy (dwmapi.dll), x64.
REM   build.bat            - full build with the self-updater
REM   build.bat skeleton   - proxy clone only (UPDATER_ENABLED=OFF), for verifying forwarding
REM Build tree lives in temp; the final dwmapi.dll is copied to dist\.

setlocal
set VS=D:\Program Files\Microsoft Visual Studio\2022\Community
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja
set BUILD=%TEMP%\UE4SS-Bootstrap-build
set SRC=%~dp0

set UPDATER=ON
if /I "%~1"=="skeleton" set UPDATER=OFF

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set PATH=%NINJA%;%PATH%

"%CMAKE%" -G Ninja -S "%SRC%." -B "%BUILD%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DUPDATER_ENABLED=%UPDATER% || exit /b 1
"%CMAKE%" --build "%BUILD%" || exit /b 1

if not exist "%SRC%dist" mkdir "%SRC%dist"
copy /Y "%BUILD%\dwmapi.dll" "%SRC%dist\dwmapi.dll" >nul

REM Always ship the default config next to the DLL.
if not exist "%SRC%dist\ue4ss" mkdir "%SRC%dist\ue4ss"
copy /Y "%SRC%config\updater.ini" "%SRC%dist\ue4ss\updater.ini" >nul

echo.
echo === built: %SRC%dist\dwmapi.dll + dist\ue4ss\updater.ini (UPDATER=%UPDATER%) ===
endlocal
