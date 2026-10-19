@echo off
setlocal
rem Builds the PROTOTYPE native port from this source folder: the game engine (MSYS2 CLANG64,
rem see README.md) and the launcher Prototype.exe. Run it again after every update.
set PORT=%~dp0
if "%PORT:~-1%"=="\" set PORT=%PORT:~0,-1%
if not exist "C:\msys64\usr\bin\env.exe" (
  echo MSYS2 is needed: see "Building" in README.md.
  pause & exit /b 1
)
echo [1/2] Building the engine (the first build takes a while) ...
pushd "%PORT%"
C:\msys64\usr\bin\env.exe MSYSTEM=CLANG64 CHERE_INVOKING=1 /usr/bin/bash -lc "bash build.sh" > build.log 2>&1
set BUILD_ERROR=%errorlevel%
popd
if not "%BUILD_ERROR%"=="0" (
  echo BUILD FAILED: see build.log
  pause & exit /b 1
)
echo [2/2] Building the launcher ...
call "%PORT%\packaging\windows\build_launcher.bat"
if errorlevel 1 (
  echo LAUNCHER BUILD FAILED: see out\launcher-build.log
  pause & exit /b 1
)
echo.
echo Done. Start Prototype.exe (a PROTOTYPE shortcut is on your desktop).
pause
