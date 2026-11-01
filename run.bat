@echo off
setlocal
cd /d "%~dp0"
REM Defina DKR_VK_VALIDATION=1 se desejar ativar as camadas de validacao Vulkan para depuracao.
REM set VK_LAYER_PATH=%~dp0toolchain\vulkan-validation
REM set DKR_VK_VALIDATION=1

cd generated\dkctf_v0
echo ==============================================================
echo  Donkey Kong Country: Tropical Freeze - Recompilacao Nativa
echo ==============================================================
echo  Controles:
echo    - Teclado: WASD (Movimento), Espaco / K (Pular / B),
echo               J / Q (Ataque / Rolamento / Y),
echo               L / E (Confirmar / A), Shift / Ctrl (ZL / L),
echo               Enter (Pausa / +), Esc (-)
echo    - Controle: XInput (Xbox / Gamepad compativel plug & play)
echo ==============================================================
echo.
dkctf.exe . 0
pause
