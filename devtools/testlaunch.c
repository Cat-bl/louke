// Development helper for automated tests (not part of the user tool):
//   testlaunch.exe play|record <movie> [--game <exe>] [--export <mp4> [quality]]
// Starts the game with ..\bin\louke.dll exactly like the control window does, but without it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../src/shared.h"

int wmain(int argc, wchar_t **argv) {
    const wchar_t *game = L"E:\\KOF\\The King Of Fighters XIII\\kofxiii.exe";
    if (argc < 3) { fwprintf(stderr, L"usage: testlaunch play|record <movie> [--game exe]\n"); return 2; }
    const wchar_t *exportPath = NULL;
    int quality = 1;
    for (int i = 3; i + 1 < argc; i++) {
        if (!wcscmp(argv[i], L"--game")) game = argv[++i];
        else if (!wcscmp(argv[i], L"--export")) {
            exportPath = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != L'-') quality = _wtoi(argv[++i]);
        }
    }
    wchar_t movie[MAX_PATH], dll[MAX_PATH], log[MAX_PATH], dir[MAX_PATH], cmd[MAX_PATH + 4];
    GetFullPathNameW(argv[2], MAX_PATH, movie, NULL);
    GetModuleFileNameW(NULL, dll, MAX_PATH);
    *wcsrchr(dll, L'\\') = 0;
    *wcsrchr(dll, L'\\') = 0;                       // ..\ (project root)
    wcscpy(log, dll);
    wcscat(dll, L"\\bin\\louke.dll");
    wcscat(log, L"\\bin\\louke.log");
    wcscpy(dir, game);
    *wcsrchr(dir, L'\\') = 0;
    SetEnvironmentVariableW(L"LOUKE_MODE", argv[1]);
    SetEnvironmentVariableW(L"LOUKE_MOVIE", movie);
    SetEnvironmentVariableW(L"LOUKE_LOG", log);
    if (exportPath) {                               // like the control window's "export the whole movie"
        static const uint32_t hk[HKA_COUNT] = {0x13, 0x20, 0x09, 0x2D, 0x253,
            0x170, 0x171, 0x172, 0x173, 0x174, 0x175, 0x176, 0x177, 0x178, 0x179,
            0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79};
        wchar_t shm[64];
        swprintf(shm, 64, L"Local\\LOUKE_TEST_%lu", GetCurrentProcessId());
        HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(TasShared), shm);
        TasShared *sh = map ? (TasShared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TasShared)) : NULL;
        if (!sh) return 4;
        memset(sh, 0, sizeof *sh);
        sh->magic = TAS_SHARED_MAGIC;
        sh->version = TAS_SHARED_VERSION;
        sh->guiPid = GetCurrentProcessId();
        for (int a = 0; a < HKA_COUNT; a++) sh->hotkeys[a] = hk[a];
        GetFullPathNameW(exportPath, 260, sh->exportPath, NULL);
        sh->exportQuality = (uint32_t)quality;
        sh->exportMovie = 1;
        SetEnvironmentVariableW(L"LOUKE_SHM", shm);
    }
    swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", game);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(game, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi)) return 3;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    GetThreadContext(pi.hThread, &ctx);
    uint8_t *remote = VirtualAllocEx(pi.hProcess, NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    DWORD pLoad = (DWORD)(uintptr_t)GetProcAddress(k32, "LoadLibraryW");
    DWORD pExit = (DWORD)(uintptr_t)GetProcAddress(k32, "ExitProcess");
    DWORD pathAddr = (DWORD)(uintptr_t)remote + 0x100, entry = ctx.Eax, code = 0x7A5F;
    uint8_t page[0x100 + MAX_PATH * 2];
    memset(page, 0, sizeof page);
    uint8_t *s = page;
    *s++ = 0x60;
    *s++ = 0x68; memcpy(s, &pathAddr, 4); s += 4;
    *s++ = 0xB8; memcpy(s, &pLoad, 4); s += 4;
    *s++ = 0xFF; *s++ = 0xD0;
    *s++ = 0x85; *s++ = 0xC0;
    *s++ = 0x75; *s++ = 0x0C;
    *s++ = 0x68; memcpy(s, &code, 4); s += 4;
    *s++ = 0xB8; memcpy(s, &pExit, 4); s += 4;
    *s++ = 0xFF; *s++ = 0xD0;
    *s++ = 0x61;
    *s++ = 0x68; memcpy(s, &entry, 4); s += 4;
    *s++ = 0xC3;
    memcpy(page + 0x100, dll, (wcslen(dll) + 1) * sizeof(wchar_t));
    SIZE_T n;
    WriteProcessMemory(pi.hProcess, remote, page, sizeof page, &n);
    FlushInstructionCache(pi.hProcess, remote, sizeof page);
    ctx.Eax = (DWORD)(uintptr_t)remote;
    SetThreadContext(pi.hThread, &ctx);
    ResumeThread(pi.hThread);
    printf("pid %lu\n", pi.dwProcessId);
    fflush(stdout);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    printf("exit 0x%08lX\n", ec);
    return 0;
}
