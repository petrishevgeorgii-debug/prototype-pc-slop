/* Prototype.exe when PyInstaller is not available: starts the launcher with the port's own
 * Python environment (out\pyenv, made by build_launcher.bat) and no console window. */
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show) {
    (void)instance; (void)previous; (void)args; (void)show;
    wchar_t dir[MAX_PATH], line[3 * MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    _snwprintf(line, sizeof line / sizeof *line,
               L"\"%ls\\out\\pyenv\\Scripts\\pythonw.exe\" \"%ls\\launcher\\prototype_launcher.py\"", dir, dir);
    line[sizeof line / sizeof *line - 1] = 0;
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        MessageBoxW(NULL, L"Could not start the launcher (out\\pyenv missing?). Run \"Setup Prototype Port.bat\" again.",
                    L"PROTOTYPE", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
