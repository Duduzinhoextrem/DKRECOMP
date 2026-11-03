/* DKRecomp.exe: double-click entry point, without a console window.
 * - In a development checkout (with .venv) it opens the Tkinter launcher (tools/dkrecomp/launcher.py), like DKRecomp.bat.
 * - In a portable package (tools/make_portable.py: no Python, game already built) it starts the game directly and,
 *   when the game stops with an error, shows the reason the runtime wrote to dkctf_boot.log.
 * Build it with: .venv/Scripts/python tools/build_launcher_exe.py */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define PATH_CHARS (MAX_PATH * 4)

static void message(const wchar_t *text) {
    MessageBoxW(NULL, text, L"DKRecomp", MB_OK | MB_ICONWARNING);
}
static int exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

static int open_launcher(const wchar_t *root, const wchar_t *python) {
    wchar_t tools[PATH_CHARS], command[PATH_CHARS + 64];
    swprintf(tools, PATH_CHARS, L"%ls\\tools", root);
    if (!exists(tools)) {
        message(L"Coloque o DKRecomp.exe na pasta principal do DKRecomp (a que contem tools e .venv).");
        return 1;
    }
    SetEnvironmentVariableW(L"PYTHONPATH", tools);
    swprintf(command, PATH_CHARS + 64, L"\"%ls\" -m dkrecomp.launcher", python);
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

static HANDLE output_file(const wchar_t *directory, const wchar_t *name) {
    wchar_t path[PATH_CHARS];
    swprintf(path, PATH_CHARS, L"%ls\\%ls", directory, name);
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* The runtime ends every stop with "boot: STOP after N SVCs: <reason>"; closing the window is a normal exit. */
static void report_stop(const wchar_t *game_directory) {
    wchar_t path[PATH_CHARS];
    swprintf(path, PATH_CHARS, L"%ls\\dkctf_boot.log", game_directory);
    FILE *file = _wfopen(path, L"rb");
    if (!file) return;
    char line[1024], reason[1024] = "";
    while (fgets(line, sizeof line, file)) {
        const char *stop = strstr(line, "boot: STOP after ");
        const char *colon = stop ? strstr(stop, ": ") : NULL;
        if (colon) { snprintf(reason, sizeof reason, "%s", colon + 2); reason[strcspn(reason, "\r\n")] = 0; }
    }
    fclose(file);
    if (!reason[0] || strstr(reason, "graphics window closed")) return;
    wchar_t wide[1024], text[1400];
    if (!MultiByteToWideChar(CP_UTF8, 0, reason, -1, wide, 1024)) return;
    swprintf(text, 1400, L"O jogo fechou.\n\nMotivo informado pelo runtime:\n%ls\n\nDetalhes em generated\\dkctf_v0\\dkctf_boot.log", wide);
    MessageBoxW(NULL, text, L"DKRecomp", MB_OK | MB_ICONERROR);
}

static int start_game(const wchar_t *root, const wchar_t *game) {
    wchar_t directory[PATH_CHARS], command[PATH_CHARS + 16];
    swprintf(directory, PATH_CHARS, L"%ls\\generated\\dkctf_v0", root);
    swprintf(command, PATH_CHARS + 16, L"\"%ls\" . 0", game);
    HANDLE out = output_file(directory, L"boot_stdout.txt"), err = output_file(directory, L"boot_stderr.txt");
    STARTUPINFOW startup = {.cb = sizeof startup, .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = GetStdHandle(STD_INPUT_HANDLE), .hStdOutput = out, .hStdError = err};
    PROCESS_INFORMATION process;
    BOOL ok = CreateProcessW(game, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, directory, &startup, &process);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!ok) { message(L"Nao foi possivel iniciar o jogo (generated\\dkctf_v0\\dkctf.exe)."); return 1; }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hProcess);
    report_stop(directory);
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
    (void)instance; (void)previous; (void)arguments; (void)show;
    wchar_t root[PATH_CHARS], python[PATH_CHARS], game[PATH_CHARS];
    DWORD n = GetModuleFileNameW(NULL, root, PATH_CHARS);
    wchar_t *slash = n && n < PATH_CHARS ? wcsrchr(root, L'\\') : NULL;
    if (!slash) { message(L"Nao foi possivel localizar a pasta do DKRecomp."); return 1; }
    *slash = 0;
    swprintf(python, PATH_CHARS, L"%ls\\.venv\\Scripts\\pythonw.exe", root);
    swprintf(game, PATH_CHARS, L"%ls\\generated\\dkctf_v0\\dkctf.exe", root);
    if (exists(python)) return open_launcher(root, python);
    if (exists(game)) return start_game(root, game);
    message(L"O ambiente do DKRecomp ainda nao foi preparado.\n\n"
            L"Consulte o README.md para criar a .venv e instalar requirements-dev.txt, "
            L"ou copie um pacote portatil criado com tools/make_portable.py.");
    return 1;
}
