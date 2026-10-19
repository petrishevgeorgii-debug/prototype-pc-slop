@echo off
rem Builds Prototype.exe (the launcher) in the port folder and a desktop shortcut.
rem Needs a Windows Python (python.org / Python install manager); makes its own out\pyenv.
setlocal
set PORT=%~dp0..\..
pushd "%PORT%"
set PORT=%CD%
set GAMEDIR=%~1
set PYEXE=
where py >nul 2>&1 && set PYEXE=py -3
if not defined PYEXE ( where python >nul 2>&1 && set PYEXE=python )
if not defined PYEXE (
  echo No Windows Python found: install it from python.org, then run setup again.
  popd & exit /b 1
)
if not exist out mkdir out
if not exist out\pyenv\Scripts\python.exe (
  echo   making the launcher's Python environment ...
  %PYEXE% -m venv out\pyenv || ( popd & exit /b 1 )
)
set VPY=out\pyenv\Scripts\python.exe
%VPY% -m pip install --disable-pip-version-check -q --upgrade pillow >nul 2>&1 || echo   Pillow could not be installed: the launcher shows no game art.
set ICON=
if exist out\prototype.ico del out\prototype.ico
%VPY% packaging\windows\make_prototype_icon.py "%GAMEDIR%" out\prototype.ico >nul 2>&1
if exist out\prototype.ico set ICON=--icon "%PORT%\out\prototype.ico"
%VPY% packaging\windows\seed_launcher.py "%GAMEDIR%" >nul 2>&1
echo   packaging Prototype.exe ...
%VPY% -m pip install --disable-pip-version-check -q --upgrade pyinstaller >nul 2>&1
%VPY% -m PyInstaller --noconfirm --onefile --windowed --name Prototype %ICON% ^
  --distpath "%PORT%" --workpath out\pyi --specpath out\pyi --paths "%PORT%\scripts" launcher\prototype_launcher.py > out\launcher-build.log 2>&1
if errorlevel 1 (
  echo   PyInstaller failed ^(see out\launcher-build.log^): using a small native starter instead.
  set "PATH=C:\msys64\clang64\bin;%PATH%"
  clang -O2 -municode -mwindows packaging\windows\prototype_stub.c -o Prototype.exe || ( popd & exit /b 1 )
)
rem Desktop shortcut.
powershell -NoProfile -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Desktop')+'\PROTOTYPE.lnk'); $s.TargetPath='%PORT%\Prototype.exe'; $s.WorkingDirectory='%PORT%'; if (Test-Path '%PORT%\out\prototype.ico') { $s.IconLocation='%PORT%\out\prototype.ico' }; $s.Save()" >nul 2>&1
echo   Prototype.exe ready (shortcut on the desktop).
popd
exit /b 0
