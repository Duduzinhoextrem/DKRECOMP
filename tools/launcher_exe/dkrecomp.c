/* DKRecomp.exe: double-click entry point equivalent to DKRecomp.bat, without a console window.
 * Starts the Tkinter launcher (tools/dkrecomp/launcher.py) with the project's .venv, from the folder that holds this
 * executable. Build it with: .venv/Scripts/python tools/build_launcher_exe.py */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <wchar.h>

static void message(const wchar_t *text) {
    MessageBoxW(NULL, text, L"DKRecomp", MB_OK | MB_ICONWARNING);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
    (void)instance; (void)previous; (void)arguments; (void)show;
    wchar_t root[MAX_PATH * 4], python[MAX_PATH * 4], tools[MAX_PATH * 4], command[MAX_PATH * 5];
    DWORD n = GetModuleFileNameW(NULL, root, MAX_PATH * 4);
    if (!n || n >= MAX_PATH * 4) { message(L"Nao foi possivel localizar a pasta do DKRecomp."); return 1; }
    wchar_t *slash = wcsrchr(root, L'\\');
    if (!slash) { message(L"Nao foi possivel localizar a pasta do DKRecomp."); return 1; }
    *slash = 0;
    swprintf(python, MAX_PATH * 4, L"%ls\\.venv\\Scripts\\pythonw.exe", root);
    swprintf(tools, MAX_PATH * 4, L"%ls\\tools", root);
    if (GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES) {
        message(L"O ambiente do DKRecomp ainda nao foi preparado.\n\n"
                L"Consulte o README.md para criar a .venv e instalar requirements-dev.txt.");
        return 1;
    }
    if (GetFileAttributesW(tools) == INVALID_FILE_ATTRIBUTES) {
        message(L"Coloque o DKRecomp.exe na pasta principal do DKRecomp (a que contem tools e .venv).");
        return 1;
    }
    SetEnvironmentVariableW(L"PYTHONPATH", tools);
    swprintf(command, MAX_PATH * 5, L"\"%ls\" -m dkrecomp.launcher", python);
    STARTUPINFOW startup = {.cb = sizeof startup};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(python, command, NULL, NULL, FALSE, 0, NULL, root, &startup, &process)) {
        message(L"Nao foi possivel abrir o launcher do DKRecomp.");
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
