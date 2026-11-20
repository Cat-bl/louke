// louke.exe — control window of Louke (漏刻), an Hourglass-style TAS tool for Windows games.
// Starts the game suspended, loads louke.dll before the game's entry point, and talks to the
// DLL through a shared-memory block (shared.h): hotkey settings and button commands go in,
// frame / movie / mode status comes out.
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "shared.h"

#define DEFAULT_GAME L""
#define EXIT_DLL_FAILED 0x7A5F
#define TIMER_ID 1

// Interface language (louke.ini [ui] language): 0 Chinese (default), 1 English.
static int g_lang;
#define TR(zh, en) (g_lang ? (en) : (zh))

enum {
    IDC_GAME = 100, IDC_GAME_BROWSE, IDC_MOVIE, IDC_MOVIE_BROWSE,
    IDC_RECORD, IDC_PLAY, IDC_STOP, IDC_READONLY,
    IDC_PAUSE, IDC_ADVANCE, IDC_STATUS1, IDC_STATUS2,
    IDC_EXPORT_PATH, IDC_EXPORT_BROWSE, IDC_EXPORT_QUALITY, IDC_EXPORT_MOVIE, IDC_EXPORT_START, IDC_EXPORT_STOP,
    IDC_EXPORT_STATUS, IDC_LANG,
    IDC_HK_FIRST = 200,           // + HKA_*
    IDC_HK_APPLY = 260,
    IDC_SLOT_FIRST = 300,         // + slot: savestate status
};

#define HKA_GENERAL HKA_STATE_SAVE  // hotkeys before the savestate ones
static const wchar_t *kGeneralLabels[2][HKA_GENERAL] = {
    {L"暂停 / 继续", L"逐帧前进（按住连续前进）", L"快进（按住）", L"接管录制（取消只读）", L"立即保存录像"},
    {L"Pause / resume", L"Frame advance (hold to repeat)", L"Fast-forward (hold)", L"Take over (leave read-only)", L"Save movie now"},
};
static const wchar_t *kGeneralIni[HKA_GENERAL] = {L"pause", L"advance", L"fastforward", L"takeover", L"save"};
static wchar_t kHotkeyIni[HKA_COUNT][16];
// Savestates: Shift+F1..F10 save, F1..F10 load (modifiers: 1 Shift, 2 Ctrl, 4 Alt).
#define HK(vk, mods) ((uint32_t)(vk) | ((uint32_t)(mods) << 8))
static const uint32_t kDefaultHotkeys[HKA_COUNT] = {
    HK(VK_PAUSE, 0), HK(VK_SPACE, 0), HK(VK_TAB, 0), HK(VK_INSERT, 0), HK('S', 2),
    HK(VK_F1, 1), HK(VK_F2, 1), HK(VK_F3, 1), HK(VK_F4, 1), HK(VK_F5, 1),
    HK(VK_F6, 1), HK(VK_F7, 1), HK(VK_F8, 1), HK(VK_F9, 1), HK(VK_F10, 1),
    HK(VK_F1, 0), HK(VK_F2, 0), HK(VK_F3, 0), HK(VK_F4, 0), HK(VK_F5, 0),
    HK(VK_F6, 0), HK(VK_F7, 0), HK(VK_F8, 0), HK(VK_F9, 0), HK(VK_F10, 0),
};

static void init_hotkey_names(void) {
    for (int a = 0; a < HKA_GENERAL; a++) wcscpy(kHotkeyIni[a], kGeneralIni[a]);
    for (int k = 0; k < TAS_SLOTS; k++) {
        swprintf(kHotkeyIni[HKA_STATE_SAVE + k], 16, L"statesave%d", k + 1);
        swprintf(kHotkeyIni[HKA_STATE_LOAD + k], 16, L"stateload%d", k + 1);
    }
}

static void hotkey_label(int a, wchar_t *out, int n) {
    if (a < HKA_GENERAL) swprintf(out, n, L"%ls", kGeneralLabels[g_lang][a]);
    else if (a < HKA_STATE_LOAD) swprintf(out, n, TR(L"存档到栏位 %d", L"Save state to slot %d"), a - HKA_STATE_SAVE + 1);
    else swprintf(out, n, TR(L"读取栏位 %d", L"Load state from slot %d"), a - HKA_STATE_LOAD + 1);
}

static HINSTANCE g_inst;
static HWND g_wnd;
static HFONT g_font;
static int g_dpi = 96;
static wchar_t g_ini[MAX_PATH], g_dll[MAX_PATH], g_log[MAX_PATH];
static uint32_t g_hotkeys[HKA_COUNT];

static HANDLE g_proc, g_map;
static TasShared *g_sh;

// Hotkey editing: click a hotkey button, then press any key (with Ctrl/Shift/Alt if wanted).
static uint32_t g_pending[HKA_COUNT];  // shown in the buttons, applied by "保存快捷键"
static int g_capture = -1;             // index of the hotkey being captured, -1 if none

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void key_name(uint32_t hk, wchar_t *out, int len) {
    UINT vk = hk & 0xFF, mods = (hk >> 8) & 0xFF;
    out[0] = 0;
    if (mods & 2) wcscat(out, L"Ctrl + ");
    if (mods & 1) wcscat(out, L"Shift + ");
    if (mods & 4) wcscat(out, L"Alt + ");
    wchar_t name[64] = L"";
    switch (vk) {
    case 0: wcscpy(name, TR(L"（未设置）", L"(not set)")); break;
    case VK_SPACE: wcscpy(name, TR(L"空格", L"Space")); break;
    case VK_RETURN: wcscpy(name, TR(L"回车", L"Enter")); break;
    case VK_PAUSE: wcscpy(name, L"Pause"); break;
    default: {
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        int ext = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_INSERT ||
                  vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
                  vk == VK_DIVIDE || vk == VK_NUMLOCK || vk == VK_RCONTROL || vk == VK_RMENU;
        if (!GetKeyNameTextW((LONG)((sc << 16) | (ext ? 1u << 24 : 0)), name, 64)) swprintf(name, 64, TR(L"键码 %u", L"Key code %u"), vk);
    }
    }
    wcsncat(out, name, (size_t)len - wcslen(out) - 1);
}

static void show_hotkey(int a) {
    wchar_t t[128];
    key_name(g_pending[a], t, 128);
    SetDlgItemTextW(g_wnd, IDC_HK_FIRST + a, t);
}

static void begin_capture(int a) {
    if (g_capture >= 0) show_hotkey(g_capture);
    g_capture = a;
    SetDlgItemTextW(g_wnd, IDC_HK_FIRST + a, TR(L"请按键…（Esc 取消）", L"Press a key…"));
    SetFocus(GetDlgItem(g_wnd, IDC_HK_FIRST + a));
}

// Called from the message loop for every key-down while capturing; returns 1 if consumed.
static int capture_key(UINT vk) {
    if (g_capture < 0) return 0;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN)
        return 1;                                    // wait for the main key
    int a = g_capture;
    g_capture = -1;
    if (vk != VK_ESCAPE) {
        uint32_t mods = ((GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? 2 : 0) |
                        ((GetKeyState(VK_MENU) & 0x8000) ? 4 : 0);
        g_pending[a] = (vk & 0xFF) | (mods << 8);
    }
    show_hotkey(a);
    return 1;
}

static void msg(UINT icon, const wchar_t *fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);
    MessageBoxW(g_wnd, buf, L"Louke", icon);
}

static int file_exists(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    DWORD ex = !wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0;
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g_wnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

// Controls with a fixed text in each language, set again when the language changes.
typedef struct { HWND h; const wchar_t *text[2]; } LangText;
static LangText g_texts[64];
static int g_ntexts;
static HWND addT(const wchar_t *cls, const wchar_t *zh, const wchar_t *en, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = add(cls, g_lang ? en : zh, style, x, y, w, h, id);
    if (g_ntexts < 64) {
        g_texts[g_ntexts].h = c;
        g_texts[g_ntexts].text[0] = zh;
        g_texts[g_ntexts].text[1] = en;
        g_ntexts++;
    }
    return c;
}

static void fill_quality(int sel) {
    static const wchar_t *kQuality[2][4] = {
        {L"标准（8 Mbps）", L"高（16 Mbps）", L"很高（32 Mbps）", L"最高（60 Mbps）"},
        {L"Standard (8 Mbps)", L"High (16 Mbps)", L"Very high (32 Mbps)", L"Maximum (60 Mbps)"},
    };
    HWND q = GetDlgItem(g_wnd, IDC_EXPORT_QUALITY);
    SendMessageW(q, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 4; i++) SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)kQuality[g_lang][i]);
    SendMessageW(q, CB_SETCURSEL, sel, 0);
}

static int export_quality(void) {
    LRESULT q = SendDlgItemMessageW(g_wnd, IDC_EXPORT_QUALITY, CB_GETCURSEL, 0, 0);
    return q < 0 || q > 3 ? 1 : (int)q;
}

// ---------------------------------------------------------------- settings (louke.ini, UTF-16)
static void save_settings(void);
static void load_settings(void) {
    if (!file_exists(g_ini)) {               // create as UTF-16 so non-ASCII paths survive
        HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) { DWORD n; WriteFile(f, "\xFF\xFE", 2, &n, NULL); CloseHandle(f); }
    }
    wchar_t buf[MAX_PATH];
    GetPrivateProfileStringW(L"paths", L"game", DEFAULT_GAME, buf, MAX_PATH, g_ini);
    SetDlgItemTextW(g_wnd, IDC_GAME, buf);
    GetPrivateProfileStringW(L"paths", L"movie", L"", buf, MAX_PATH, g_ini);
    SetDlgItemTextW(g_wnd, IDC_MOVIE, buf);
    GetPrivateProfileStringW(L"export", L"path", L"", buf, MAX_PATH, g_ini);
    SetDlgItemTextW(g_wnd, IDC_EXPORT_PATH, buf);
    UINT q = GetPrivateProfileIntW(L"export", L"quality", 1, g_ini);
    SendDlgItemMessageW(g_wnd, IDC_EXPORT_QUALITY, CB_SETCURSEL, q > 3 ? 1 : q, 0);
    int oldFormat = GetPrivateProfileIntW(L"hotkeys", L"stateload1", -1, g_ini) == (UINT)-1;
    for (int a = 0; a < HKA_COUNT; a++)
        g_hotkeys[a] = (uint32_t)GetPrivateProfileIntW(L"hotkeys", kHotkeyIni[a], (int)kDefaultHotkeys[a], g_ini) & 0xFFFF;
    if (oldFormat) {
        // Settings from before savestates: a hotkey now taken by a savestate key gets its new default.
        int changed = 0;
        for (int a = 0; a < HKA_GENERAL; a++)
            for (int b = HKA_GENERAL; b < HKA_COUNT; b++)
                if (g_hotkeys[a] == g_hotkeys[b]) { g_hotkeys[a] = kDefaultHotkeys[a]; changed = 1; break; }
        if (changed) save_settings();
    }
    for (int a = 0; a < HKA_COUNT; a++) {
        g_pending[a] = g_hotkeys[a];
        show_hotkey(a);
    }
}

static void save_settings(void) {
    wchar_t buf[MAX_PATH];
    GetDlgItemTextW(g_wnd, IDC_GAME, buf, MAX_PATH);
    WritePrivateProfileStringW(L"paths", L"game", buf, g_ini);
    GetDlgItemTextW(g_wnd, IDC_MOVIE, buf, MAX_PATH);
    WritePrivateProfileStringW(L"paths", L"movie", buf, g_ini);
    GetDlgItemTextW(g_wnd, IDC_EXPORT_PATH, buf, MAX_PATH);
    WritePrivateProfileStringW(L"export", L"path", buf, g_ini);
    swprintf(buf, MAX_PATH, L"%d", export_quality());
    WritePrivateProfileStringW(L"export", L"quality", buf, g_ini);
    for (int a = 0; a < HKA_COUNT; a++) {
        swprintf(buf, MAX_PATH, L"%u", g_hotkeys[a]);
        WritePrivateProfileStringW(L"hotkeys", kHotkeyIni[a], buf, g_ini);
    }
    WritePrivateProfileStringW(L"ui", L"language", g_lang ? L"en" : L"zh", g_ini);
}

static void apply_hotkeys(void) {
    uint32_t hk[HKA_COUNT];
    if (g_capture >= 0) { show_hotkey(g_capture); g_capture = -1; }
    for (int a = 0; a < HKA_COUNT; a++) {
        hk[a] = g_pending[a];
        wchar_t la[64], lb[64];
        hotkey_label(a, la, 64);
        if (!(hk[a] & 0xFF)) { msg(MB_ICONWARNING, TR(L"“%ls”还没有设置快捷键。", L"No hotkey is set for \"%ls\"."), la); return; }
        for (int b = 0; b < a; b++)
            if (hk[a] == hk[b]) {
                hotkey_label(b, lb, 64);
                msg(MB_ICONWARNING, TR(L"“%ls”和“%ls”的快捷键重复了。", L"\"%ls\" and \"%ls\" have the same hotkey."), lb, la);
                return;
            }
    }
    memcpy(g_hotkeys, hk, sizeof hk);
    if (g_sh) for (int a = 0; a < HKA_COUNT; a++) g_sh->hotkeys[a] = g_hotkeys[a];
    save_settings();
    msg(MB_ICONINFORMATION,
        TR(L"快捷键已保存%ls。\n注意：设为快捷键的按键在录制时不会被当作游戏按键记录"
           L"（带 Ctrl/Shift/Alt 的快捷键只在按住这些键时排除）。",
           L"Hotkeys saved%ls.\nNote: keys used as hotkeys are not recorded as game input "
           L"(a hotkey with Ctrl/Shift/Alt is excluded only while those keys are held)."),
        g_proc ? TR(L"，并已应用到正在运行的游戏", L" and applied to the running game") : L"");
}

// ---------------------------------------------------------------- file dialogs
static int browse(int id, int forGame) {
    wchar_t path[MAX_PATH];
    GetDlgItemTextW(g_wnd, id, path, MAX_PATH);
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_wnd;
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    if (forGame) {
        of.lpstrFilter = TR(L"程序 (*.exe)\0*.exe\0", L"Programs (*.exe)\0*.exe\0");
        of.lpstrTitle = TR(L"选择游戏程序", L"Select the game program");
        of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        if (!GetOpenFileNameW(&of)) return 0;
    } else if (id == IDC_EXPORT_PATH) {
        of.lpstrFilter = TR(L"MP4 视频 (*.mp4)\0*.mp4\0", L"MP4 video (*.mp4)\0*.mp4\0");
        of.lpstrDefExt = L"mp4";
        of.lpstrTitle = TR(L"导出视频到", L"Export the video to");
        of.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&of)) return 0;
    } else {
        of.lpstrFilter = TR(L"TAS 录像 (*.txt)\0*.txt\0所有文件\0*.*\0", L"TAS movies (*.txt)\0*.txt\0All files\0*.*\0");
        of.lpstrDefExt = L"txt";
        of.lpstrTitle = TR(L"选择录像文件（可以输入新文件名）", L"Select a movie file (or type a new name)");
        of.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&of)) return 0;
    }
    SetDlgItemTextW(g_wnd, id, path);
    save_settings();
    return 1;
}

// ---------------------------------------------------------------- launching
static void update_controls(void) {
    int running = g_proc != NULL;
    EnableWindow(GetDlgItem(g_wnd, IDC_RECORD), !running);
    EnableWindow(GetDlgItem(g_wnd, IDC_PLAY), !running);
    EnableWindow(GetDlgItem(g_wnd, IDC_GAME), !running);
    EnableWindow(GetDlgItem(g_wnd, IDC_GAME_BROWSE), !running);
    EnableWindow(GetDlgItem(g_wnd, IDC_MOVIE), !running);
    EnableWindow(GetDlgItem(g_wnd, IDC_MOVIE_BROWSE), !running);
    EnableWindow(GetDlgItem(g_wnd, IDC_STOP), running);
    EnableWindow(GetDlgItem(g_wnd, IDC_PAUSE), running);
    EnableWindow(GetDlgItem(g_wnd, IDC_ADVANCE), running);
    EnableWindow(GetDlgItem(g_wnd, IDC_EXPORT_MOVIE), !running);
    if (!running) {
        EnableWindow(GetDlgItem(g_wnd, IDC_EXPORT_START), FALSE);
        EnableWindow(GetDlgItem(g_wnd, IDC_EXPORT_STOP), FALSE);
        CheckDlgButton(g_wnd, IDC_READONLY, BST_UNCHECKED);
        EnableWindow(GetDlgItem(g_wnd, IDC_READONLY), FALSE);
    }
}

static void game_exited(void) {
    DWORD code = 0;
    if (g_proc) { GetExitCodeProcess(g_proc, &code); CloseHandle(g_proc); }
    g_proc = NULL;
    if (g_sh) UnmapViewOfFile(g_sh);
    if (g_map) CloseHandle(g_map);
    g_sh = NULL; g_map = NULL;
    update_controls();
    if (code == EXIT_DLL_FAILED) msg(MB_ICONERROR, TR(L"TAS 模块加载失败，原因见日志：\n%ls", L"The TAS module failed to load. See the log:\n%ls"), g_log);
}

// Start the game suspended and redirect the initial thread's entry (EAX) to a stub:
//   pushad; LoadLibraryW(dll); if failed -> ExitProcess(0x7A5F); popad; jmp entry
static int launch(const wchar_t *game, wchar_t *err, size_t errLen) {
    wchar_t dir[MAX_PATH], cmd[MAX_PATH + 4];
    wcscpy(dir, game);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", game);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(game, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
        swprintf(err, errLen, TR(L"启动游戏失败（错误 %lu）", L"Cannot start the game (error %lu)"), GetLastError());
        return 0;
    }
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    uint8_t *remote = NULL;
    if (GetThreadContext(pi.hThread, &ctx))
        remote = VirtualAllocEx(pi.hProcess, NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote) {
        swprintf(err, errLen, TR(L"准备注入失败（错误 %lu）", L"Cannot prepare loading the TAS module (error %lu)"), GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 0;
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    DWORD pLoad = (DWORD)(uintptr_t)GetProcAddress(k32, "LoadLibraryW");
    DWORD pExit = (DWORD)(uintptr_t)GetProcAddress(k32, "ExitProcess");
    DWORD pathAddr = (DWORD)(uintptr_t)remote + 0x100, entry = ctx.Eax, code = EXIT_DLL_FAILED;
    uint8_t page[0x100 + MAX_PATH * 2];
    memset(page, 0, sizeof page);
    uint8_t *s = page;
    *s++ = 0x60;                                            // pushad
    *s++ = 0x68; memcpy(s, &pathAddr, 4); s += 4;           // push path
    *s++ = 0xB8; memcpy(s, &pLoad, 4); s += 4;              // mov eax, LoadLibraryW
    *s++ = 0xFF; *s++ = 0xD0;                               // call eax
    *s++ = 0x85; *s++ = 0xC0;                               // test eax, eax
    *s++ = 0x75; *s++ = 0x0C;                               // jnz ok
    *s++ = 0x68; memcpy(s, &code, 4); s += 4;               // push 0x7A5F
    *s++ = 0xB8; memcpy(s, &pExit, 4); s += 4;              // mov eax, ExitProcess
    *s++ = 0xFF; *s++ = 0xD0;                               // call eax
    *s++ = 0x61;                                            // ok: popad
    *s++ = 0x68; memcpy(s, &entry, 4); s += 4;              // push entry
    *s++ = 0xC3;                                            // ret
    memcpy(page + 0x100, g_dll, (wcslen(g_dll) + 1) * sizeof(wchar_t));
    SIZE_T written;
    ctx.Eax = (DWORD)(uintptr_t)remote;
    if (!WriteProcessMemory(pi.hProcess, remote, page, sizeof page, &written) ||
        !FlushInstructionCache(pi.hProcess, remote, sizeof page) || !SetThreadContext(pi.hThread, &ctx)) {
        swprintf(err, errLen, TR(L"注入失败（错误 %lu）", L"Cannot load the TAS module (error %lu)"), GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 0;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    return 1;
}

// Output file of the video export, full path; asks for one when none is set. 0: cancelled.
static int export_target(wchar_t *out) {
    wchar_t in[MAX_PATH];
    GetDlgItemTextW(g_wnd, IDC_EXPORT_PATH, in, MAX_PATH);
    if (!in[0]) {
        if (!browse(IDC_EXPORT_PATH, 0)) return 0;
        GetDlgItemTextW(g_wnd, IDC_EXPORT_PATH, in, MAX_PATH);
    }
    if (!GetFullPathNameW(in, MAX_PATH, out, NULL)) { msg(MB_ICONERROR, TR(L"导出文件路径无效。", L"The export file path is invalid.")); return 0; }
    if (file_exists(out) &&
        MessageBoxW(g_wnd, TR(L"导出文件已存在，导出会覆盖它。确定吗？", L"The export file already exists and will be overwritten. Continue?"),
                    L"Louke", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 0;
    SetDlgItemTextW(g_wnd, IDC_EXPORT_PATH, out);
    save_settings();
    return 1;
}

static void start_game(int record, int exportMovie) {
    wchar_t game[MAX_PATH], movieIn[MAX_PATH], movie[MAX_PATH], err[512];
    GetDlgItemTextW(g_wnd, IDC_GAME, game, MAX_PATH);
    GetDlgItemTextW(g_wnd, IDC_MOVIE, movieIn, MAX_PATH);
    if (!file_exists(game)) { msg(MB_ICONERROR, TR(L"找不到游戏程序：\n%ls", L"Game program not found:\n%ls"), game); return; }
    if (!file_exists(g_dll)) { msg(MB_ICONERROR, TR(L"找不到 TAS 模块：\n%ls", L"TAS module not found:\n%ls"), g_dll); return; }
    if (!movieIn[0]) {
        msg(MB_ICONWARNING, TR(L"请先选择录像文件（录制新录像时可以输入新文件名）。",
                               L"Select a movie file first (for a new recording you can type a new file name)."));
        return;
    }
    if (!GetFullPathNameW(movieIn, MAX_PATH, movie, NULL)) { msg(MB_ICONERROR, TR(L"录像路径无效。", L"The movie path is invalid.")); return; }
    if (record && file_exists(movie) &&
        MessageBoxW(g_wnd, TR(L"录像文件已存在，录制新录像会覆盖它。确定吗？",
                              L"The movie file already exists. Recording a new movie will overwrite it. Continue?"),
                    L"Louke", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    if (!record && !file_exists(movie)) { msg(MB_ICONERROR, TR(L"找不到录像文件：\n%ls", L"Movie file not found:\n%ls"), movie); return; }
    SetDlgItemTextW(g_wnd, IDC_MOVIE, movie);
    save_settings();
    wchar_t exportPath[MAX_PATH] = L"";
    if (exportMovie && !export_target(exportPath)) return;

    wchar_t shm[64];
    swprintf(shm, 64, L"Local\\LOUKE_%lu_%lu", GetCurrentProcessId(), GetTickCount());
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(TasShared), shm);
    g_sh = g_map ? (TasShared *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TasShared)) : NULL;
    if (!g_sh) {
        msg(MB_ICONERROR, TR(L"创建共享内存失败。", L"Cannot create the shared memory."));
        if (g_map) CloseHandle(g_map);
        g_map = NULL;
        return;
    }
    memset((void *)g_sh, 0, sizeof *g_sh);
    g_sh->magic = TAS_SHARED_MAGIC;
    g_sh->version = TAS_SHARED_VERSION;
    g_sh->guiPid = GetCurrentProcessId();
    for (int a = 0; a < HKA_COUNT; a++) g_sh->hotkeys[a] = g_hotkeys[a];
    if (exportMovie) {
        wcsncpy(g_sh->exportPath, exportPath, 259);
        g_sh->exportQuality = (uint32_t)export_quality();
        g_sh->exportMovie = 1;
    }

    SetEnvironmentVariableW(L"LOUKE_MODE", record ? L"record" : L"play");
    SetEnvironmentVariableW(L"LOUKE_MOVIE", movie);
    SetEnvironmentVariableW(L"LOUKE_LOG", g_log);
    SetEnvironmentVariableW(L"LOUKE_SHM", shm);
    if (!launch(game, err, 512)) {
        UnmapViewOfFile(g_sh); CloseHandle(g_map); g_sh = NULL; g_map = NULL;
        msg(MB_ICONERROR, L"%ls", err);
        return;
    }
    update_controls();
}

static void stop_game(void) {
    if (!g_proc) return;
    if (g_sh && g_sh->heartbeat && (g_sh->exportState == 1 || g_sh->exportState == 2)) {
        // finish the video file first (otherwise it cannot be played)
        g_sh->cmdSeq[CMD_EXPORT_STOP]++;
        SetDlgItemTextW(g_wnd, IDC_EXPORT_STATUS, TR(L"正在完成视频文件…", L"Finishing the video file…"));
        UpdateWindow(g_wnd);
        for (int i = 0; i < 3000 && (g_sh->exportState == 1 || g_sh->exportState == 2) &&
                        WaitForSingleObject(g_proc, 0) == WAIT_TIMEOUT; i++) Sleep(10);
    }
    if (g_sh) {                                   // ask the DLL to save the movie first
        uint32_t want = ++g_sh->cmdSeq[CMD_SAVE];
        for (int i = 0; i < 150 && g_sh->saveAck != want; i++) Sleep(10);
    }
    TerminateProcess(g_proc, 0);
    WaitForSingleObject(g_proc, 3000);
    game_exited();
}

// ---------------------------------------------------------------- status display
static void set_text_if_changed(int id, const wchar_t *text) {
    wchar_t old[256];
    GetDlgItemTextW(g_wnd, id, old, 256);
    if (wcscmp(old, text)) SetDlgItemTextW(g_wnd, id, text);
}

static void refresh_status(void) {
    if (g_proc && WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) game_exited();
    wchar_t l1[256], l2[256];
    if (!g_proc) {
        wcscpy(l1, TR(L"状态：未运行", L"Status: not running"));
        l2[0] = 0;
    } else if (!g_sh || !g_sh->heartbeat) {
        wcscpy(l1, TR(L"状态：游戏启动中…", L"Status: starting the game…"));
        l2[0] = 0;
    } else {
        wchar_t game[48];
        const wchar_t *title = g_lang && g_sh->gameTitleEn[0] ? (const wchar_t *)g_sh->gameTitleEn : (const wchar_t *)g_sh->gameTitle;
        wcsncpy(game, title[0] ? title : TR(L"未识别（通用模式）", L"unknown (generic mode)"), 47);
        game[47] = 0;
        swprintf(l1, 256, TR(L"状态：%ls%ls%ls      游戏：%ls", L"Status: %ls%ls%ls    Game: %ls"),
                 g_sh->mode == TASMODE_PLAY ? TR(L"回放中（只读）", L"playing (read-only)") : TR(L"录制中", L"recording"),
                 g_sh->paused ? TR(L"  ·  已暂停", L"  ·  paused") : L"", g_sh->ffwd ? TR(L"  ·  快进中", L"  ·  fast-forward") : L"", game);
        swprintf(l2, 256, TR(L"当前帧 %u      录像长度 %u 帧      重录次数 %u", L"Frame %u      Movie length %u frames      Rerecords %u"),
                 g_sh->frame, g_sh->movieLen, g_sh->rerecords);
    }
    set_text_if_changed(IDC_STATUS1, l1);
    set_text_if_changed(IDC_STATUS2, l2);
    for (int k = 0; k < TAS_SLOTS; k++) {
        wchar_t t[32] = L"—";
        if (g_proc && g_sh && g_sh->heartbeat) {
            uint32_t f = g_sh->slotFrame[k];
            if (f == TAS_SLOT_EMPTY) wcscpy(t, TR(L"空", L"empty")); else swprintf(t, 32, TR(L"第 %u 帧", L"frame %u"), f);
        }
        set_text_if_changed(IDC_SLOT_FIRST + k, t);
    }
    // video export (the last state stays shown after the game ended)
    static int exState;
    static uint32_t exFrames, exErr, exHr;
    if (g_proc && g_sh && g_sh->heartbeat) {
        exState = (int)g_sh->exportState;
        exFrames = g_sh->exportFrames;
        exErr = g_sh->exportErrorCode;
        exHr = g_sh->exportErrorHr;
    }
    const wchar_t *why;
    switch (exErr) {
    case EXPERR_NO_MF:       why = TR(L"系统缺少 Media Foundation 组件", L"Media Foundation is not available"); break;
    case EXPERR_CREATE_FILE: why = TR(L"无法创建导出文件", L"cannot create the export file"); break;
    case EXPERR_ENCODER:     why = TR(L"无法启动视频编码器", L"cannot start the video encoder"); break;
    case EXPERR_WRITE:       why = TR(L"写入视频失败", L"writing the video failed"); break;
    case EXPERR_FINALIZE:    why = TR(L"完成视频文件失败", L"finishing the video file failed"); break;
    case EXPERR_THREAD:      why = TR(L"无法启动编码线程", L"cannot start the encoder thread"); break;
    default:                 why = TR(L"未知原因", L"unknown reason"); break;
    }
    wchar_t ex[256];
    switch (exState) {
    case 1:
        swprintf(ex, 256, TR(L"正在导出：已写入 %u 帧（%u 分 %02u 秒）", L"Exporting: %u frames written (%u:%02u)"),
                 exFrames, exFrames / 3600, exFrames / 60 % 60);
        break;
    case 2: swprintf(ex, 256, TR(L"正在完成视频文件…（%u 帧）", L"Finishing the video file… (%u frames)"), exFrames); break;
    case 3:
        swprintf(ex, 256, TR(L"导出完成：%u 帧（%u 分 %02u 秒）", L"Export finished: %u frames (%u:%02u)"),
                 exFrames, exFrames / 3600, exFrames / 60 % 60);
        break;
    case 4: swprintf(ex, 256, TR(L"导出失败：%ls（0x%08X）", L"Export failed: %ls (0x%08X)"), why, exHr); break;
    default: wcscpy(ex, TR(L"未在导出", L"Not exporting")); break;
    }
    set_text_if_changed(IDC_EXPORT_STATUS, ex);
    if (g_proc) {
        int live = g_sh && g_sh->heartbeat;
        EnableWindow(GetDlgItem(g_wnd, IDC_EXPORT_START), live && exState != 1 && exState != 2);
        EnableWindow(GetDlgItem(g_wnd, IDC_EXPORT_STOP), live && exState == 1);
    }
    if (g_proc && g_sh && g_sh->heartbeat) {
        int play = g_sh->mode == TASMODE_PLAY;
        CheckDlgButton(g_wnd, IDC_READONLY, play ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(g_wnd, IDC_READONLY), play);
    }
}

// ---------------------------------------------------------------- window
static void create_controls(void) {
    addT(L"STATIC", L"游戏程序", L"Game", SS_CENTERIMAGE, 12, 12, 70, 24, -1);
    add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 86, 12, 330, 24, IDC_GAME);
    addT(L"BUTTON", L"浏览…", L"Browse…", WS_TABSTOP, 422, 12, 80, 24, IDC_GAME_BROWSE);
    addT(L"STATIC", L"录像文件", L"Movie", SS_CENTERIMAGE, 12, 44, 70, 24, -1);
    add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 86, 44, 330, 24, IDC_MOVIE);
    addT(L"BUTTON", L"浏览…", L"Browse…", WS_TABSTOP, 422, 44, 80, 24, IDC_MOVIE_BROWSE);

    addT(L"BUTTON", L"录制新录像", L"Record movie", WS_TABSTOP, 12, 80, 118, 30, IDC_RECORD);
    addT(L"BUTTON", L"回放录像", L"Play movie", WS_TABSTOP, 138, 80, 118, 30, IDC_PLAY);
    addT(L"BUTTON", L"停止游戏", L"Stop game", WS_TABSTOP, 264, 80, 118, 30, IDC_STOP);
    addT(L"BUTTON", L"只读", L"Read-only", WS_TABSTOP | BS_AUTOCHECKBOX, 400, 80, 100, 30, IDC_READONLY);

    addT(L"BUTTON", L"状态", L"Status", BS_GROUPBOX, 12, 120, 490, 116, -1);
    add(L"STATIC", L"", 0, 26, 144, 466, 22, IDC_STATUS1);
    add(L"STATIC", L"", 0, 26, 168, 466, 22, IDC_STATUS2);
    addT(L"BUTTON", L"暂停 / 继续", L"Pause / Resume", WS_TABSTOP, 26, 196, 118, 30, IDC_PAUSE);
    addT(L"BUTTON", L"逐帧前进", L"Frame advance", WS_TABSTOP, 152, 196, 118, 30, IDC_ADVANCE);

    addT(L"BUTTON", L"快捷键（游戏窗口或本窗口在前台时有效）", L"Hotkeys (work while the game or this window is in front)",
         BS_GROUPBOX, 12, 246, 490, 232, -1);
    for (int a = 0; a < HKA_GENERAL; a++) {
        addT(L"STATIC", kGeneralLabels[0][a], kGeneralLabels[1][a], SS_CENTERIMAGE, 26, 272 + a * 32, 200, 24, -1);
        add(L"BUTTON", L"", WS_TABSTOP, 232, 272 + a * 32, 160, 26, IDC_HK_FIRST + a);
    }
    addT(L"BUTTON", L"保存快捷键", L"Save hotkeys", WS_TABSTOP, 400, 432, 92, 30, IDC_HK_APPLY);

    addT(L"BUTTON", L"即时存档（只在本次游戏运行期间保留）", L"Savestates (kept only while the game runs)",
         BS_GROUPBOX, 514, 12, 378, 466, -1);
    addT(L"STATIC", L"栏位", L"Slot", SS_CENTERIMAGE, 528, 36, 40, 22, -1);
    addT(L"STATIC", L"存档键", L"Save key", SS_CENTERIMAGE, 572, 36, 110, 22, -1);
    addT(L"STATIC", L"读档键", L"Load key", SS_CENTERIMAGE, 688, 36, 96, 22, -1);
    addT(L"STATIC", L"内容", L"Contents", SS_CENTERIMAGE, 790, 36, 90, 22, -1);
    for (int k = 0; k < TAS_SLOTS; k++) {
        int y = 62 + k * 40;
        wchar_t n[8];
        swprintf(n, 8, L"%d", k + 1);
        add(L"STATIC", n, SS_CENTERIMAGE, 536, y, 30, 26, -1);
        add(L"BUTTON", L"", WS_TABSTOP, 572, y, 110, 26, IDC_HK_FIRST + HKA_STATE_SAVE + k);
        add(L"BUTTON", L"", WS_TABSTOP, 688, y, 96, 26, IDC_HK_FIRST + HKA_STATE_LOAD + k);
        add(L"STATIC", L"", SS_CENTERIMAGE, 790, y, 96, 26, IDC_SLOT_FIRST + k);
    }

    addT(L"BUTTON", L"导出视频（MP4：H.264 + AAC，60 帧/秒，每个游戏帧一帧）",
         L"Video export (MP4: H.264 + AAC, 60 fps, one video frame per game frame)", BS_GROUPBOX, 12, 488, 880, 112, -1);
    addT(L"STATIC", L"导出文件", L"Export to", SS_CENTERIMAGE, 26, 514, 70, 24, -1);
    add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 100, 514, 480, 24, IDC_EXPORT_PATH);
    addT(L"BUTTON", L"浏览…", L"Browse…", WS_TABSTOP, 586, 514, 80, 24, IDC_EXPORT_BROWSE);
    addT(L"STATIC", L"画质", L"Quality", SS_CENTERIMAGE, 674, 514, 48, 24, -1);
    add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 724, 514, 154, 200, IDC_EXPORT_QUALITY);
    fill_quality(1);
    addT(L"BUTTON", L"导出整个录像", L"Export whole movie", WS_TABSTOP, 26, 552, 140, 30, IDC_EXPORT_MOVIE);
    addT(L"BUTTON", L"从当前帧开始导出", L"Export from this frame", WS_TABSTOP, 174, 552, 160, 30, IDC_EXPORT_START);
    addT(L"BUTTON", L"停止导出", L"Stop export", WS_TABSTOP, 342, 552, 100, 30, IDC_EXPORT_STOP);
    add(L"STATIC", L"", SS_CENTERIMAGE, 454, 552, 424, 30, IDC_EXPORT_STATUS);

    // the language choice keeps both names, so it can be found in either language
    add(L"STATIC", L"界面语言 / Language", SS_CENTERIMAGE | SS_RIGHT, 610, 610, 160, 24, -1);
    HWND lang = add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 776, 610, 116, 200, IDC_LANG);
    SendMessageW(lang, CB_ADDSTRING, 0, (LPARAM)L"中文");
    SendMessageW(lang, CB_ADDSTRING, 0, (LPARAM)L"English");
    SendMessageW(lang, CB_SETCURSEL, g_lang, 0);
}

// Switch the interface language: every text is set again; nothing else changes.
static void set_language(int lang) {
    if (lang == g_lang) return;
    g_lang = lang;
    for (int i = 0; i < g_ntexts; i++) SetWindowTextW(g_texts[i].h, g_texts[i].text[g_lang]);
    SetWindowTextW(g_wnd, TR(L"Louke TAS 控制台", L"Louke TAS Control Panel"));
    fill_quality(export_quality());
    int capturing = g_capture;
    for (int a = 0; a < HKA_COUNT; a++) show_hotkey(a);
    if (capturing >= 0) { g_capture = -1; begin_capture(capturing); }
    refresh_status();
    save_settings();
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE:
        g_wnd = h;
        create_controls();
        load_settings();
        update_controls();
        refresh_status();
        SetTimer(h, TIMER_ID, 100, NULL);
        return 0;
    case WM_TIMER:
        refresh_status();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_EXPORT_QUALITY) { if (HIWORD(wp) == CBN_SELCHANGE) save_settings(); return 0; }
        if (LOWORD(wp) == IDC_LANG) {
            if (HIWORD(wp) == CBN_SELCHANGE) set_language(SendDlgItemMessageW(h, IDC_LANG, CB_GETCURSEL, 0, 0) == 1);
            return 0;
        }
        if (HIWORD(wp) != BN_CLICKED) break;
        if (LOWORD(wp) >= IDC_HK_FIRST && LOWORD(wp) < IDC_HK_FIRST + HKA_COUNT) {
            begin_capture(LOWORD(wp) - IDC_HK_FIRST);
            return 0;
        }
        if (g_capture >= 0) { show_hotkey(g_capture); g_capture = -1; }
        switch (LOWORD(wp)) {
        case IDC_GAME_BROWSE: browse(IDC_GAME, 1); break;
        case IDC_MOVIE_BROWSE: browse(IDC_MOVIE, 0); break;
        case IDC_RECORD: start_game(1, 0); break;
        case IDC_PLAY: start_game(0, 0); break;
        case IDC_EXPORT_BROWSE: browse(IDC_EXPORT_PATH, 0); break;
        case IDC_EXPORT_MOVIE: start_game(0, 1); break;
        case IDC_EXPORT_START: {
            wchar_t p[MAX_PATH];
            if (!g_sh || !export_target(p)) break;
            wcsncpy(g_sh->exportPath, p, 259);
            g_sh->exportQuality = (uint32_t)export_quality();
            g_sh->cmdSeq[CMD_EXPORT_START]++;
            break;
        }
        case IDC_EXPORT_STOP: if (g_sh) g_sh->cmdSeq[CMD_EXPORT_STOP]++; break;
        case IDC_STOP: stop_game(); break;
        case IDC_PAUSE: if (g_sh) g_sh->cmdSeq[CMD_PAUSE]++; break;
        case IDC_ADVANCE: if (g_sh) g_sh->cmdSeq[CMD_ADVANCE]++; break;
        case IDC_READONLY:
            if (g_sh && g_sh->mode == TASMODE_PLAY && !IsDlgButtonChecked(h, IDC_READONLY)) g_sh->cmdSeq[CMD_TAKEOVER]++;
            break;
        case IDC_HK_APPLY: apply_hotkeys(); break;
        }
        return 0;
    case WM_CLOSE:
        if (g_proc) {
            if (MessageBoxW(h, TR(L"游戏还在运行。关闭控制窗口会结束游戏（会先保存录像）。确定吗？",
                                  L"The game is still running. Closing this window ends the game (the movie is saved first). Continue?"),
                            L"Louke", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            stop_game();
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    g_inst = inst;
    init_hotkey_names();
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    GetModuleFileNameW(NULL, g_dll, MAX_PATH);
    wchar_t *slash = wcsrchr(g_dll, L'\\');
    if (!slash) return 1;
    slash[1] = 0;
    wcscpy(g_ini, g_dll); wcscat(g_ini, L"louke.ini");
    wcscpy(g_log, g_dll); wcscat(g_log, L"louke.log");
    wcscat(g_dll, L"louke.dll");
    wchar_t lang[8];
    GetPrivateProfileStringW(L"ui", L"language", L"zh", lang, 8, g_ini);
    g_lang = !wcscmp(lang, L"en");

    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    g_font = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Louke";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, S(904), S(646)};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
    CreateWindowExW(WS_EX_CONTROLPARENT, L"Louke", TR(L"Louke TAS 控制台", L"Louke TAS Control Panel"), style, CW_USEDEFAULT, CW_USEDEFAULT,
                    r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
    ShowWindow(g_wnd, show);

    MSG msgv;
    while (GetMessageW(&msgv, NULL, 0, 0) > 0) {
        // While a hotkey is being captured, every key (Space, Enter, Tab, F10, ...) goes to the capture.
        if ((msgv.message == WM_KEYDOWN || msgv.message == WM_SYSKEYDOWN) && capture_key((UINT)msgv.wParam)) continue;
        if (IsDialogMessageW(g_wnd, &msgv)) continue;
        TranslateMessage(&msgv);
        DispatchMessageW(&msgv);
    }
    return 0;
}
