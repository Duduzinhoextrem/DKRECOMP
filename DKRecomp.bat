@echo off
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\pythonw.exe" (
  echo O ambiente do DKRecomp ainda nao foi preparado.
  echo Consulte o README.md para criar a .venv e instalar requirements-dev.txt.
  pause
  exit /b 1
)
set "PYTHONPATH=%~dp0tools"
start "" ".venv\Scripts\pythonw.exe" -m dkrecomp.launcher
