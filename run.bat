@echo off
setlocal
cd /d "%~dp0"
set VK_LAYER_PATH=%~dp0toolchain\vulkan-validation
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
