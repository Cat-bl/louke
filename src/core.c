// Game-independent TAS core (see tas.h for the module layout).
//
// Time   : imports of the game executable  QPC / QPF / GetTickCount / timeGetTime /
//          GetSystemTime(AsFileTime) -> virtual clock advancing exactly 1/60 s per frame, for the
//          game-loop thread (and the initial thread until the game thread is known).
// Input  : IDirectInputDevice8::GetDeviceState  keyboard (256 bytes) <- movie / recorded keys,
//          joystick (DIJOYSTATE/2) <- neutral; XInputGetState -> not connected.
// Threads: every thread created by the game is tracked; at each frame end the game thread waits
//          until all background work started in that frame is finished (deterministic loading).
#include "tas.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define VQPC_FREQ      10000000LL                // virtual QPC frequency (100 ns ticks)
#define VQPC_BASE      0x0000001000000000LL
#define VTICK_BASE     0x00100000u               // virtual GetTickCount start (ms)
#define VFILETIME_BASE 130305456000000000LL      // 2013-12-03 12:00:00 UTC
#define AUTOSAVE_FRAMES 300
#define FFWD_PRESENT_EVERY 8

// Hotkeys come from the control window; these are the defaults without it.
// Format: low byte = virtual key, next byte = modifiers (1 Shift, 2 Ctrl, 4 Alt).
// Savestates: Shift+F1..F10 save, F1..F10 load.
#define HK(vk, mods) ((uint32_t)(vk) | ((uint32_t)(mods) << 8))
static const uint32_t kDefaultHotkeys[HKA_COUNT] = {
    HK(VK_PAUSE, 0), HK(VK_SPACE, 0), HK(VK_TAB, 0), HK(VK_INSERT, 0), HK('S', 2),
    HK(VK_F1, 1), HK(VK_F2, 1), HK(VK_F3, 1), HK(VK_F4, 1), HK(VK_F5, 1),
    HK(VK_F6, 1), HK(VK_F7, 1), HK(VK_F8, 1), HK(VK_F9, 1), HK(VK_F10, 1),
    HK(VK_F1, 0), HK(VK_F2, 0), HK(VK_F3, 0), HK(VK_F4, 0), HK(VK_F5, 0),
    HK(VK_F6, 0), HK(VK_F7, 0), HK(VK_F8, 0), HK(VK_F9, 0), HK(VK_F10, 0),
};

enum { MODE_RECORD = 1, MODE_PLAY = 2 };

// ---------------------------------------------------------------- state
static const GameProfile *g_profile;
static TasShared *g_sh;
static int      g_mode;
static DWORD    g_initThread;
static volatile DWORD g_gameThread;
static wchar_t  g_moviePath[MAX_PATH];
static FILE    *g_log;

static uint32_t g_frame;               // index of the logic frame currently running
static uint8_t  g_frameKeys[256];      // keyboard state the game sees during g_frame
static int64_t  g_spinTicks;           // virtual 100 ns ticks added by busy-wait protection
static uint32_t g_queriesThisFrame;

typedef struct { uint8_t bits[32]; } FrameInput;
static FrameInput *g_movie;
static uint32_t g_movieLen, g_movieCap, g_rerecords;
static int      g_dirty;
static uint32_t g_lastSaveFrame;

static int      g_paused, g_ff;
static DWORD    g_advHoldStart;
static int64_t  g_realFreq, g_nextDeadline;
static HWND     g_gameWnd;
static uint32_t g_titleFrame = 0xFFFFFFFF;

static int      g_trace;
static int      g_diag;
static int      g_reqSave = -1, g_reqLoad = -1;   // savestate requests (slot), handled at the frame boundary
static volatile LONG g_asyncOutstanding;
static double   g_lastSyncMs;

// ---------------------------------------------------------------- log
void tas_log(const char *fmt, ...) {
    if (!g_log) return;
    char line[1024];
    int n = snprintf(line, sizeof line, "[f%06u t%05lu] ", g_frame, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    if (n > 0 && n < (int)sizeof line) vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    fprintf(g_log, "%s\n", line);   // one call per line: lines from different threads do not interleave
    fflush(g_log);
}
#define logf_ tas_log

// ---------------------------------------------------------------- patching helpers
static void **find_iat(HMODULE mod, const char *dll, const char *name, WORD ordinal) {
    uint8_t *base = (uint8_t *)mod;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA32 *ot = (IMAGE_THUNK_DATA32 *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA32 *ft = (IMAGE_THUNK_DATA32 *)(base + d->FirstThunk);
        for (; ot->u1.AddressOfData; ot++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL32(ot->u1.Ordinal)) {
                if (!name && IMAGE_ORDINAL32(ot->u1.Ordinal) == ordinal) return (void **)&ft->u1.Function;
            } else if (name) {
                IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + ot->u1.AddressOfData);
                if (!strcmp((char *)ibn->Name, name)) return (void **)&ft->u1.Function;
            }
        }
    }
    return NULL;
}

int tas_hook_iat(const char *dll, const char *name, WORD ordinal, void *fn, void **orig) {
    void **slot = find_iat(GetModuleHandleW(NULL), dll, name, ordinal);
    if (!slot) { logf_("NOTE: import %s!%s#%u not found", dll, name ? name : "", ordinal); return 0; }
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return 0;
    *orig = *slot;
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return 1;
}

int tas_patch(void *addr, const void *bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(addr, bytes, n);
    VirtualProtect(addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    return 1;
}

// ---------------------------------------------------------------- key names (DirectInput codes)
static const char *kDikNames[256];
static uint8_t kDikVk[256];            // DirectInput key code -> Windows virtual key (for recording)
static void init_dik_names(void) {
    static const struct { uint8_t c; const char *n; uint8_t vk; } t[] = {
        {0x01,"ESCAPE",VK_ESCAPE},{0x02,"1",'1'},{0x03,"2",'2'},{0x04,"3",'3'},{0x05,"4",'4'},
        {0x06,"5",'5'},{0x07,"6",'6'},{0x08,"7",'7'},{0x09,"8",'8'},{0x0A,"9",'9'},{0x0B,"0",'0'},
        {0x0C,"MINUS",VK_OEM_MINUS},{0x0D,"EQUALS",VK_OEM_PLUS},{0x0E,"BACK",VK_BACK},{0x0F,"TAB",VK_TAB},
        {0x10,"Q",'Q'},{0x11,"W",'W'},{0x12,"E",'E'},{0x13,"R",'R'},{0x14,"T",'T'},{0x15,"Y",'Y'},
        {0x16,"U",'U'},{0x17,"I",'I'},{0x18,"O",'O'},{0x19,"P",'P'},{0x1A,"LBRACKET",VK_OEM_4},
        {0x1B,"RBRACKET",VK_OEM_6},{0x1C,"RETURN",VK_RETURN},{0x1D,"LCONTROL",VK_LCONTROL},
        {0x1E,"A",'A'},{0x1F,"S",'S'},{0x20,"D",'D'},{0x21,"F",'F'},{0x22,"G",'G'},{0x23,"H",'H'},
        {0x24,"J",'J'},{0x25,"K",'K'},{0x26,"L",'L'},{0x27,"SEMICOLON",VK_OEM_1},
        {0x28,"APOSTROPHE",VK_OEM_7},{0x29,"GRAVE",VK_OEM_3},{0x2A,"LSHIFT",VK_LSHIFT},
        {0x2B,"BACKSLASH",VK_OEM_5},{0x2C,"Z",'Z'},{0x2D,"X",'X'},{0x2E,"C",'C'},{0x2F,"V",'V'},
        {0x30,"B",'B'},{0x31,"N",'N'},{0x32,"M",'M'},{0x33,"COMMA",VK_OEM_COMMA},
        {0x34,"PERIOD",VK_OEM_PERIOD},{0x35,"SLASH",VK_OEM_2},{0x36,"RSHIFT",VK_RSHIFT},
        {0x37,"MULTIPLY",VK_MULTIPLY},{0x38,"LMENU",VK_LMENU},{0x39,"SPACE",VK_SPACE},
        {0x3A,"CAPITAL",VK_CAPITAL},{0x3B,"F1",VK_F1},{0x3C,"F2",VK_F2},{0x3D,"F3",VK_F3},
        {0x3E,"F4",VK_F4},{0x3F,"F5",VK_F5},{0x40,"F6",VK_F6},{0x41,"F7",VK_F7},{0x42,"F8",VK_F8},
        {0x43,"F9",VK_F9},{0x44,"F10",VK_F10},{0x45,"NUMLOCK",VK_NUMLOCK},{0x46,"SCROLL",VK_SCROLL},
        {0x47,"NUMPAD7",VK_NUMPAD7},{0x48,"NUMPAD8",VK_NUMPAD8},{0x49,"NUMPAD9",VK_NUMPAD9},
        {0x4A,"SUBTRACT",VK_SUBTRACT},{0x4B,"NUMPAD4",VK_NUMPAD4},{0x4C,"NUMPAD5",VK_NUMPAD5},
        {0x4D,"NUMPAD6",VK_NUMPAD6},{0x4E,"ADD",VK_ADD},{0x4F,"NUMPAD1",VK_NUMPAD1},
        {0x50,"NUMPAD2",VK_NUMPAD2},{0x51,"NUMPAD3",VK_NUMPAD3},{0x52,"NUMPAD0",VK_NUMPAD0},
        {0x53,"DECIMAL",VK_DECIMAL},{0x57,"F11",VK_F11},{0x58,"F12",VK_F12},
        {0x9C,"NUMPADENTER",0},{0x9D,"RCONTROL",VK_RCONTROL},{0xB5,"DIVIDE",VK_DIVIDE},
        {0xB7,"SYSRQ",VK_SNAPSHOT},{0xB8,"RMENU",VK_RMENU},{0xC5,"PAUSE",VK_PAUSE},{0xC7,"HOME",VK_HOME},
        {0xC8,"UP",VK_UP},{0xC9,"PRIOR",VK_PRIOR},{0xCB,"LEFT",VK_LEFT},{0xCD,"RIGHT",VK_RIGHT},
        {0xCF,"END",VK_END},{0xD0,"DOWN",VK_DOWN},{0xD1,"NEXT",VK_NEXT},{0xD2,"INSERT",VK_INSERT},
        {0xD3,"DELETE",VK_DELETE},{0xDB,"LWIN",0},{0xDC,"RWIN",0},{0xDD,"APPS",0},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) { kDikNames[t[i].c] = t[i].n; kDikVk[t[i].c] = t[i].vk; }
}
static int dik_from_name(const char *s) {
    for (int i = 1; i < 256; i++) if (kDikNames[i] && !_stricmp(kDikNames[i], s)) return i;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { long v = strtol(s, NULL, 16); if (v > 0 && v < 256) return (int)v; }
    return -1;
}

// ---------------------------------------------------------------- movie
// Text format, one line per frame (key names or "."), editable in any text editor:
//   TASMOVIE 1 / game <profile id> / rerecords N / frames N / --- / frame lines
static void movie_reserve(uint32_t n) {
    if (n <= g_movieCap) return;
    uint32_t cap = g_movieCap ? g_movieCap : 4096;
    while (cap < n) cap *= 2;
    FrameInput *p = realloc(g_movie, (size_t)cap * sizeof *p);
    if (!p) { logf_("FATAL: out of memory for movie"); ExitProcess(3); }
    g_movie = p; g_movieCap = cap;
}
static void pack_keys(const uint8_t keys[256], FrameInput *fi) {
    memset(fi, 0, sizeof *fi);
    for (int i = 0; i < 256; i++) if (keys[i] & 0x80) fi->bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}
static void unpack_keys(const FrameInput *fi, uint8_t keys[256]) {
    for (int i = 0; i < 256; i++) keys[i] = (fi->bits[i >> 3] >> (i & 7)) & 1 ? 0x80 : 0;
}

static int movie_save(void) {
    wchar_t tmp[MAX_PATH + 8];
    swprintf(tmp, MAX_PATH + 8, L"%ls.tmp", g_moviePath);
    FILE *f = _wfopen(tmp, L"wb");
    if (!f) { logf_("ERROR: cannot write movie temp file"); return 0; }
    fprintf(f, "TASMOVIE 1\r\ngame %s\r\nrerecords %u\r\nframes %u\r\n---\r\n",
            g_profile ? g_profile->id : "generic", g_rerecords, g_movieLen);
    for (uint32_t fr = 0; fr < g_movieLen; fr++) {
        int any = 0;
        for (int k = 1; k < 256; k++) {
            if (!((g_movie[fr].bits[k >> 3] >> (k & 7)) & 1)) continue;
            if (any) fputc(' ', f);
            if (kDikNames[k]) fputs(kDikNames[k], f); else fprintf(f, "0x%02X", k);
            any = 1;
        }
        if (!any) fputc('.', f);
        fputs("\r\n", f);
    }
    int ok = fflush(f) == 0;
    fclose(f);
    if (!ok || !MoveFileExW(tmp, g_moviePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        logf_("ERROR: saving movie failed (%lu)", GetLastError());
        return 0;
    }
    g_dirty = 0;
    g_lastSaveFrame = g_frame;
    logf_("movie saved: %u frames, %u rerecords", g_movieLen, g_rerecords);
    return 1;
}

static int movie_load(void) {
    FILE *f = _wfopen(g_moviePath, L"rb");
    if (!f) { logf_("ERROR: cannot open movie"); return 0; }
    char line[4096];
    int inBody = 0, lineNo = 0;
    g_movieLen = 0;
    while (fgets(line, sizeof line, f)) {
        lineNo++;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
        if (!inBody) {
            if (!strcmp(line, "---")) inBody = 1;
            else if (!strncmp(line, "rerecords ", 10)) g_rerecords = (uint32_t)strtoul(line + 10, NULL, 10);
            else if (!strncmp(line, "game ", 5) && strcmp(line + 5, g_profile ? g_profile->id : "generic"))
                logf_("WARNING: movie was recorded for game '%s'", line + 5);
            continue;
        }
        movie_reserve(g_movieLen + 1);
        FrameInput *fi = &g_movie[g_movieLen++];
        memset(fi, 0, sizeof *fi);
        for (char *tok = strtok(line, " \t"); tok; tok = strtok(NULL, " \t")) {
            if (!strcmp(tok, ".")) continue;
            int k = dik_from_name(tok);
            if (k < 0) { logf_("WARNING: movie line %d: unknown key '%s' ignored", lineNo, tok); continue; }
            fi->bits[k >> 3] |= (uint8_t)(1u << (k & 7));
        }
    }
    fclose(f);
    if (!inBody) { logf_("ERROR: movie has no '---' line"); return 0; }
    logf_("movie loaded: %u frames, %u rerecords", g_movieLen, g_rerecords);
    return 1;
}

// ---------------------------------------------------------------- threads / status
uint32_t tas_frame(void) { return g_frame; }
DWORD tas_game_thread(void) { return g_gameThread; }
static int64_t vtime_base(void);
static int g_exportPending;
static void export_begin(int fast);
// The lockstep main thread hands out turns to the other game threads (see lockstep() below): the
// initial thread from the start, the game thread from the moment it is known.
static volatile DWORD g_mainThread;
static HANDLE g_turnDone;
static void main_handover(void);
void tas_set_game_thread(DWORD tid) {
    if (g_gameThread == tid) return;
    g_gameThread = tid;
    logf_("game thread is %lu", tid);
    if (tid == GetCurrentThreadId()) main_handover();
    if (g_exportPending && tid == GetCurrentThreadId()) { g_exportPending = 0; export_begin(1); }
}
int tas_is_game_thread(void) { return g_gameThread && GetCurrentThreadId() == g_gameThread; }
static int g_boundaryClaimed;
void tas_claim_frame_boundary(void) { g_boundaryClaimed = 1; }
int tas_frame_boundary_claimed(void) { return g_boundaryClaimed; }
// Virtual time is seen by the game thread and by every thread the game created (audio engines,
// loaders, ...). Only the window thread runs on real time once the game thread is known.
static int tracked_thread(void);
static int is_vtime_thread(void) {
    DWORD t = GetCurrentThreadId();
    if (t == (g_gameThread ? g_gameThread : g_initThread)) return 1;
    return tracked_thread();
}

// Hotkeys and recorded keys are accepted while the game or the control window is in front.
static int tas_foreground(void) {
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    if (!h) return 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId() || (g_sh && g_sh->guiPid && pid == g_sh->guiPid);
}

static uint32_t state_frame(int slot);
// Video export (avdump.c), started from the control window: for the whole movie from its first
// frame (as fast as possible, without live sound), or from the current frame on.
static int g_exportFast, g_exportPending;
static int64_t g_exportLastV;
int tas_export_fast(void) { return g_exportFast && avdump_active(); }
static void export_begin(int fast) {
    if (!g_sh || !g_sh->exportPath[0]) return;
    wchar_t path[260];
    memcpy(path, (const void *)g_sh->exportPath, sizeof path);
    path[259] = 0;
    if (avdump_start(path, (int)g_sh->exportQuality)) {
        g_exportFast = fast;
        g_exportLastV = vtime_base();
    }
}
static void export_end(void) {
    avdump_stop();
    g_exportFast = 0;
}

static void publish_status(void) {
    if (!g_sh) return;
    g_sh->exportState = (uint32_t)avdump_state();
    g_sh->exportFrames = avdump_frames();
    uint32_t hr = 0;
    g_sh->exportErrorCode = avdump_error(&hr);
    g_sh->exportErrorHr = hr;
    for (int k = 0; k < TAS_SLOTS; k++) g_sh->slotFrame[k] = state_frame(k);
    g_sh->frame = g_frame;
    g_sh->movieLen = g_movieLen;
    g_sh->rerecords = g_rerecords;
    g_sh->mode = g_mode == MODE_PLAY ? TASMODE_PLAY : TASMODE_RECORD;
    g_sh->paused = (uint32_t)g_paused;
    g_sh->ffwd = (uint32_t)g_ff;
    g_sh->heartbeat++;
}

static BOOL CALLBACK find_wnd_cb(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}

static void update_title(int force) {
    if (!force && g_titleFrame != 0xFFFFFFFF && g_frame - g_titleFrame < 15) return;
    if (!g_gameWnd || !IsWindow(g_gameWnd)) { g_gameWnd = NULL; EnumWindows(find_wnd_cb, (LPARAM)&g_gameWnd); }
    if (!g_gameWnd) return;
    wchar_t buf[256];
    swprintf(buf, 256, L"Louke | %ls | 帧 %u / %u | 重录 %u%ls%ls",
             g_mode == MODE_PLAY ? L"回放" : L"录制", g_frame, g_movieLen, g_rerecords,
             g_paused ? L" | 暂停" : L"", g_ff ? L" | 快进" : L"");
    DWORD_PTR r;
    SendMessageTimeoutW(g_gameWnd, WM_SETTEXT, 0, (LPARAM)buf, SMTO_ABORTIFHUNG, 50, &r);
    g_titleFrame = g_frame;
}

static int64_t real_qpc(void) { LARGE_INTEGER li; QueryPerformanceCounter(&li); return li.QuadPart; }

// Keep real time at 60 fps using an absolute cadence; resync when more than one frame late.
static void pace_60(void) {
    int64_t period = g_realFreq / 60;
    int64_t now = real_qpc();
    if (g_nextDeadline == 0 || now - g_nextDeadline > period) {
        g_nextDeadline = now + period;
        return;
    }
    for (;;) {
        int64_t remain = g_nextDeadline - real_qpc();
        if (remain <= 0) break;
        Sleep(remain > period / 8 ? 1 : 0);
    }
    g_nextDeadline += period;
}

// ---------------------------------------------------------------- hotkeys / control window commands
typedef struct { int pausePressed, advPressed, advDown, ffDown, takeoverPressed, savePressed, exportStart, exportStop; } Hotkeys;
static int g_hkPrev[HKA_COUNT];
static uint32_t g_cmdSeen[CMD_COUNT];

static uint32_t hotkey(int a) { return g_sh ? g_sh->hotkeys[a] : kDefaultHotkeys[a]; }
static UINT held_mods(void) {
    return ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1u : 0u) | ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 2u : 0u) |
           ((GetAsyncKeyState(VK_MENU) & 0x8000) ? 4u : 0u);
}
// The modifiers must match exactly, so that F1 and Shift+F1 can be different hotkeys.
static int hotkey_down(uint32_t hk) {
    UINT vk = hk & 0xFF, mods = (hk >> 8) & 0xFF;
    if (!vk || !(GetAsyncKeyState(vk) & 0x8000)) return 0;
    return held_mods() == mods;
}
static int hk_edge(int a, int fg, int *down) {
    int d = fg && hotkey_down(hotkey(a));
    int pressed = d && !g_hkPrev[a];
    g_hkPrev[a] = d;
    if (down) *down = d;
    return pressed;
}
static int cmd_pending(int c) {          // a button press in the control window
    if (!g_sh) return 0;
    uint32_t s = g_sh->cmdSeq[c];
    if (s == g_cmdSeen[c]) return 0;
    g_cmdSeen[c] = s;
    return 1;
}
static Hotkeys poll_hotkeys(void) {
    Hotkeys h;
    int fg = tas_foreground();
    h.pausePressed    = hk_edge(HKA_PAUSE, fg, NULL) | cmd_pending(CMD_PAUSE);
    h.advPressed      = hk_edge(HKA_ADVANCE, fg, &h.advDown) | cmd_pending(CMD_ADVANCE);
    hk_edge(HKA_FFWD, fg, &h.ffDown);
    h.takeoverPressed = hk_edge(HKA_TAKEOVER, fg, NULL) | cmd_pending(CMD_TAKEOVER);
    h.savePressed     = hk_edge(HKA_SAVE, fg, NULL) | cmd_pending(CMD_SAVE);
    h.exportStart     = cmd_pending(CMD_EXPORT_START);
    h.exportStop      = cmd_pending(CMD_EXPORT_STOP);
    for (int k = 0; k < TAS_SLOTS; k++) {
        if (hk_edge(HKA_STATE_SAVE + k, fg, NULL)) g_reqSave = k;
        if (hk_edge(HKA_STATE_LOAD + k, fg, NULL)) g_reqLoad = k;
    }
    return h;
}
static void save_requested(void) {
    movie_save();
    if (g_sh) g_sh->saveAck = g_cmdSeen[CMD_SAVE];
}

static void switch_to_record(const char *why, int countRerecord) {
    if (g_movieLen > g_frame) g_movieLen = g_frame;   // truncate: frames >= g_frame will be re-recorded
    g_mode = MODE_RECORD;
    if (countRerecord) g_rerecords++;
    g_dirty = 1;
    logf_("switched to RECORD at frame %u (%s), rerecords=%u", g_frame, why, g_rerecords);
    movie_save();
    update_title(1);
}

// ---------------------------------------------------------------- DirectInput 8
typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
typedef HRESULT (WINAPI *CreateDevice_t)(void *, REFGUID, void **, void *);
typedef HRESULT (WINAPI *GetDeviceState_t)(void *, DWORD, void *);

static DI8Create_t o_DirectInput8Create;

#define MAX_VT 4
typedef struct { void **vt; void *orig; } VtHook;
static VtHook g_cdVt[MAX_VT], g_gdsVt[MAX_VT];

static void *vt_lookup(VtHook *tab, void *obj) {
    void **vt = *(void ***)obj;
    for (int i = 0; i < MAX_VT; i++) if (tab[i].vt == vt) return tab[i].orig;
    return NULL;
}
static int vt_hook(VtHook *tab, void *obj, int slot, void *fn) {
    void **vt = *(void ***)obj;
    for (int i = 0; i < MAX_VT; i++) if (tab[i].vt == vt) return 1;
    for (int i = 0; i < MAX_VT; i++) {
        if (tab[i].vt) continue;
        DWORD old;
        if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old)) return 0;
        tab[i].vt = vt;
        tab[i].orig = vt[slot];
        vt[slot] = fn;
        VirtualProtect(&vt[slot], sizeof(void *), old, &old);
        return 1;
    }
    return 0;
}

int tas_diag(void) { return g_diag; }
void tas_diag_caller(const char *what, void *ret) {
    static struct { const char *what; void *ret; DWORD tid; } seen[256];
    static volatile LONG n;
    static CRITICAL_SECTION cs;
    static volatile LONG init;
    if (!g_diag) return;
    if (InterlockedCompareExchange(&init, 1, 0) == 0) { InitializeCriticalSection(&cs); init = 2; }
    while (init != 2) Sleep(0);
    DWORD tid = GetCurrentThreadId(), e = GetLastError();
    EnterCriticalSection(&cs);
    int found = 0;
    for (LONG i = 0; i < n; i++) if (seen[i].what == what && seen[i].ret == ret && seen[i].tid == tid) { found = 1; break; }
    if (!found && n < 256) {
        seen[n].what = what; seen[n].ret = ret; seen[n].tid = tid; n++;
        logf_("DIAG %s from %p, thread %lu%s", what, ret, tid,
              tid == g_gameThread ? " (game thread)" : tid == g_initThread ? " (window thread)" : "");
    }
    LeaveCriticalSection(&cs);
    SetLastError(e);
}

static HRESULT WINAPI h_GetDeviceState(void *self, DWORD cb, void *data) {
    tas_diag_caller(cb == 256 ? "keyboard GetDeviceState" : "joystick GetDeviceState", __builtin_return_address(0));
    GetDeviceState_t orig = (GetDeviceState_t)vt_lookup(g_gdsVt, self);
    if (!orig) return E_FAIL;
    if (!data) return orig(self, cb, data);
    if (cb == 256) {                                  // keyboard
        memcpy(data, g_frameKeys, 256);
        return S_OK;
    }
    if (cb == 0x50 || cb == 0x110) {                  // DIJOYSTATE / DIJOYSTATE2: neutral
        memset(data, 0, cb);
        memset((uint8_t *)data + 0x20, 0xFF, 16);     // rgdwPOV[4] = centered
        return S_OK;
    }
    return orig(self, cb, data);
}

static HRESULT WINAPI h_CreateDevice(void *self, REFGUID guid, void **dev, void *outer) {
    CreateDevice_t orig = (CreateDevice_t)vt_lookup(g_cdVt, self);
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, guid, dev, outer);
    if (SUCCEEDED(hr) && dev && *dev && !vt_hook(g_gdsVt, *dev, 9, (void *)h_GetDeviceState))
        logf_("ERROR: GetDeviceState hook failed");
    return hr;
}

static HRESULT WINAPI h_DirectInput8Create(HINSTANCE hi, DWORD ver, REFIID riid, LPVOID *out, void *outer) {
    HRESULT hr = o_DirectInput8Create(hi, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        if (!vt_hook(g_cdVt, *out, 3, (void *)h_CreateDevice)) logf_("ERROR: CreateDevice hook failed");
        else logf_("DirectInput8 hooked");
    }
    return hr;
}

// ---------------------------------------------------------------- XInput
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, void *);
static XInputGetState_t o_XInputGetState[3];
static DWORD WINAPI h_XInputGetState(DWORD idx, void *state) {
    (void)idx; (void)state;
    return ERROR_DEVICE_NOT_CONNECTED;
}

// Recording reads the system key state (works while the game or the control window is in front),
// mapped to DirectInput key codes. Keys bound to TAS hotkeys are never recorded.
// A key used by a hotkey is not recorded as a game key, except a key bound only with modifiers
// (Ctrl+S) while those modifiers are not held.
static int is_hotkey_vk(UINT vk, UINT mods) {
    for (int a = 0; a < HKA_COUNT; a++) {
        uint32_t hk = hotkey(a);
        if ((hk & 0xFF) == vk && !(((hk >> 8) & 0xFF) & ~mods)) return 1;
    }
    return 0;
}
static void read_real_keys(uint8_t out[256]) {
    memset(out, 0, 256);
    if (!tas_foreground()) return;
    UINT mods = held_mods();
    for (int d = 1; d < 256; d++) {
        UINT vk = kDikVk[d];
        if (vk && !is_hotkey_vk(vk, mods) && (GetAsyncKeyState((int)vk) & 0x8000)) out[d] = 0x80;
    }
}

// ---------------------------------------------------------------- virtual time
typedef BOOL (WINAPI *QPC_t)(LARGE_INTEGER *);
typedef DWORD (WINAPI *DWORDV_t)(void);
typedef void (WINAPI *FT_t)(FILETIME *);
typedef void (WINAPI *ST_t)(SYSTEMTIME *);
static QPC_t o_QPC, o_QPF;
static DWORDV_t o_GetTickCount, o_timeGetTime;
static FT_t o_GetSystemTimeAsFileTime;
static ST_t o_GetSystemTime;

static int64_t vtime_base(void) { return (int64_t)g_frame * VQPC_FREQ / 60 + g_spinTicks; }
int64_t tas_vtime(void) { return vtime_base(); }
static void background_time_query(void);
static int64_t vtime_100ns(void) {
    DWORD t = GetCurrentThreadId();
    if (t != (g_gameThread ? g_gameThread : g_initThread)) { background_time_query(); return vtime_base(); }
    if (++g_queriesThisFrame > 20000 && g_queriesThisFrame % 1000 == 0) {
        g_spinTicks += 10000;  // the game thread is busy-waiting on time: let 1 ms pass
        if (g_queriesThisFrame == 21000) logf_("WARNING: game thread busy-waits on time, advancing virtual clock");
    }
    return vtime_base();
}
static BOOL WINAPI h_QPC(LARGE_INTEGER *li) {
    if (!is_vtime_thread()) tas_diag_caller("real-time QueryPerformanceCounter", __builtin_return_address(0));
    if (!is_vtime_thread() || !li) return o_QPC(li);
    li->QuadPart = VQPC_BASE + vtime_100ns();
    return TRUE;
}
static BOOL WINAPI h_QPF(LARGE_INTEGER *li) {
    if (!is_vtime_thread() || !li) return o_QPF(li);
    li->QuadPart = VQPC_FREQ;
    return TRUE;
}
static DWORD WINAPI h_GetTickCount(void) {
    if (!is_vtime_thread()) tas_diag_caller("real-time GetTickCount", __builtin_return_address(0));
    if (!is_vtime_thread()) return o_GetTickCount();
    return VTICK_BASE + (DWORD)(vtime_100ns() / 10000);
}
static DWORD WINAPI h_timeGetTime(void) {
    if (!is_vtime_thread()) tas_diag_caller("real-time timeGetTime", __builtin_return_address(0));
    if (!is_vtime_thread()) return o_timeGetTime();
    return VTICK_BASE + (DWORD)(vtime_100ns() / 10000);
}
static void WINAPI h_GetSystemTimeAsFileTime(FILETIME *ft) {
    if (!is_vtime_thread()) tas_diag_caller("real-time GetSystemTimeAsFileTime", __builtin_return_address(0));
    if (!is_vtime_thread() || !ft) { o_GetSystemTimeAsFileTime(ft); return; }
    uint64_t v = (uint64_t)(VFILETIME_BASE + vtime_100ns());
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}
static void WINAPI h_GetSystemTime(SYSTEMTIME *st) {
    if (!is_vtime_thread()) tas_diag_caller("real-time GetSystemTime", __builtin_return_address(0));
    if (!is_vtime_thread() || !st) { o_GetSystemTime(st); return; }
    FILETIME ft;
    h_GetSystemTimeAsFileTime(&ft);
    FileTimeToSystemTime(&ft, st);
}

// ---------------------------------------------------------------- background thread sync
// Every thread the game creates (through the exe's CreateThread import, which _beginthreadex also
// uses) is tracked as RUNNING / WAITING (inside a wait, sleep, contended lock or alertable I/O
// wait) / SUSPENDED / DEAD. Once the game thread is known the other threads run in lockstep with
// it (see lockstep() below); at the end of each frame they finish everything they can do, so work
// started in a frame completes before the next one, independent of disk and scheduler speed.
enum { TS_FREE, TS_RUNNING, TS_WAITING, TS_SUSPENDED, TS_DEAD, TS_IDLE };

// Per-thread runtime data (such as the C runtime's per-thread block) lies in game memory, which
// savestates restore, but the TLS / FLS slots pointing to it belong to the threads. The values of
// the slots the game allocated are recorded whenever a thread enters a wait; a state keeps them
// for every thread, and after a load each thread puts its own values back when it continues.
#define MAX_TLSIDX 8
typedef struct { void *tls[MAX_TLSIDX], *fls[MAX_TLSIDX]; } TlsVals;
static DWORD g_tlsIdx[MAX_TLSIDX], g_flsIdx[MAX_TLSIDX];
static volatile LONG g_ntls, g_nfls;
static void tls_record(TlsVals *v) {
    DWORD e = GetLastError();
    for (LONG i = 0; i < g_ntls; i++) v->tls[i] = TlsGetValue(g_tlsIdx[i]);
    for (LONG i = 0; i < g_nfls; i++) v->fls[i] = FlsGetValue(g_flsIdx[i]);
    SetLastError(e);
}
static void tls_apply(const TlsVals *v) {
    DWORD e = GetLastError();
    for (LONG i = 0; i < g_ntls; i++) TlsSetValue(g_tlsIdx[i], v->tls[i]);
    for (LONG i = 0; i < g_nfls; i++) FlsSetValue(g_flsIdx[i], v->fls[i]);
    SetLastError(e);
}

typedef struct {
    volatile LONG state, prev;
    volatile DWORD tid;
    void *start;
    TlsVals vals;                      // slot values at the latest wait
    TlsVals want;                      // values to put back after a load
    volatile LONG rebind;
    HANDLE turnEv;                     // lockstep: set when the thread gets a turn
    volatile LONG polling;             // lockstep: inside a hooked wait, waiting for a turn
    volatile LONG runs;                // waits that let the thread go on
    volatile LONG yielded;             // gave its turn back without waiting for anything
    volatile LONG starting;            // created, has not reached its first turn yet
    void *waitFrom;                    // diagnostics: caller of the current wait
    volatile LONG hasDeadline;
    int64_t vdeadline;                 // virtual time at which the current wait times out
    uint32_t qframe, queries;          // time queries in the current frame (busy-wait detection)
    uint32_t polls;                    // zero-timeout polls in a row that found nothing
    // Thread wrapping: the real thread of a game thread never ends. When the game's thread ends it
    // waits, idle, for the next thread the game creates, and a savestate can bring back what it did.
    HANDLE hReal;                      // our own handle of the real thread
    LPTHREAD_START_ROUTINE fn;         // work of the next logical thread
    void *arg;
    volatile LONG work;                // a logical thread was assigned and has not started yet
    volatile LONG lsuspend;            // it was created suspended and not resumed yet
    volatile LONG lt;                  // logical thread on this real thread, -1 when idle
    uint32_t order;                    // creation order of that logical thread (turn order)
    DWORD exitCode, parkErr;
    uintptr_t stackBase, idleSeh, exitSeh, parkSeh;
    void *idleJb[5], *exitJb[5], *parkJb[5];
    volatile LONG exitJbValid;
    void *tlsBlock;                    // the thread's block of the exe's implicit TLS
    uint32_t tlsSize;
    const void *volatile restore;      // savestate load: go back to this SavedThread
    volatile LONG restored;            // ... done
    uint8_t *privTop;                  // top of a small private stack used while restoring
    int64_t turnTicks;                 // diagnostics (LOUKE_FRAMEPROF): real time in turns this frame
    // What the thread waits for while it waits for a turn, so that the main thread only gives turns
    // to threads whose wait may be satisfied (checked without changing any object).
    uint8_t wkind;                     // 0 unknown (always give a turn), 1 objects, 2 critical section, 3 time only
    uint8_t wall, walert;
    DWORD wn;
    HANDLE wh[MAXIMUM_WAIT_OBJECTS];
    int8_t wtype[MAXIMUM_WAIT_OBJECTS];
    LPCRITICAL_SECTION wcs;
} ThreadSlot;
#define MAX_THREADS 128
static ThreadSlot g_threads[MAX_THREADS];
static volatile LONG g_threadCount;
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static volatile LONG g_signalWakes;
static uint32_t g_pollsThisFrame;      // zero-timeout waits of the game thread in this frame

typedef HANDLE (WINAPI *CreateThread_t)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
typedef void (WINAPI *ExitThread_t)(DWORD);
typedef DWORD (WINAPI *ThreadOp_t)(HANDLE);
typedef DWORD (WINAPI *WFSO_t)(HANDLE, DWORD);
typedef DWORD (WINAPI *WFMO_t)(DWORD, const HANDLE *, BOOL, DWORD);
typedef DWORD (WINAPI *WFMOEx_t)(DWORD, const HANDLE *, BOOL, DWORD, BOOL);
typedef DWORD (WINAPI *SleepEx_t)(DWORD, BOOL);
typedef void (WINAPI *Sleep_t)(DWORD);
typedef BOOL (WINAPI *GetOverlappedResult_t)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
typedef void (WINAPI *EnterCS_t)(LPCRITICAL_SECTION);
typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *ReadFileEx_t)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
static CreateThread_t o_CreateThread;
static ExitThread_t o_ExitThread;
static ThreadOp_t o_ResumeThread, o_SuspendThread;
static WFSO_t o_WaitForSingleObject;
static WFMO_t o_WaitForMultipleObjects;
static WFMOEx_t o_WaitForMultipleObjectsEx;
static SleepEx_t o_SleepEx;
static Sleep_t o_Sleep;
static GetOverlappedResult_t o_GetOverlappedResult;
static EnterCS_t o_EnterCriticalSection;
static CreateFileW_t o_CreateFileW;
static CreateFileA_t o_CreateFileA;
static ReadFile_t o_ReadFile;
static ReadFileEx_t o_ReadFileEx;

static ThreadSlot *my_slot(void) {
    DWORD e = GetLastError();
    ThreadSlot *s = (ThreadSlot *)TlsGetValue(g_tls);
    SetLastError(e);
    return s;
}
static ThreadSlot *slot_by_tid(DWORD tid) {
    LONG n = g_threadCount;
    if (n > MAX_THREADS) n = MAX_THREADS;
    for (LONG i = 0; i < n; i++) if (tid && g_threads[i].tid == tid) return &g_threads[i];
    return NULL;
}
static int tracked_thread(void) { return g_tls != TLS_OUT_OF_INDEXES && my_slot() != NULL; }
static LONG thread_slots(void) { return g_threadCount < MAX_THREADS ? g_threadCount : MAX_THREADS; }
static inline uintptr_t teb_read(uint32_t off) {
    uintptr_t v;
    __asm__ volatile("movl %%fs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}
static inline void teb_write(uint32_t off, uintptr_t v) {
    __asm__ volatile("movl %0, %%fs:(%1)" :: "r"(v), "r"(off) : "memory");
}

// Logical threads: what the game sees as threads (the handles CreateThread gave it). Each runs on a
// real thread that never ends; a wait on the handle of a logical thread that has ended is satisfied
// and GetExitCodeThread reports its exit code, although its real thread lives on.
typedef struct { HANDLE h; DWORD tid; int slot; uint8_t used, exited, closed; LONG suspend; DWORD exitCode; } LThread;
#define MAX_LT 1024
static LThread g_lt[MAX_LT];
static int g_nlt;
static uint32_t g_createSeq;
static CRITICAL_SECTION g_ltLock;
static volatile LONG g_ltReady;
static int lt_find(HANDLE h) {         // caller holds g_ltLock
    if (!h) return -1;
    for (int i = 0; i < g_nlt; i++) if (g_lt[i].used && !g_lt[i].closed && g_lt[i].h == h) return i;
    return -1;
}
static int lt_alloc(void) {            // caller holds g_ltLock
    for (int i = 0; i < g_nlt; i++) if (!g_lt[i].used) return i;
    return g_nlt < MAX_LT ? g_nlt++ : -1;
}
void tas_thread_handle_closed(HANDLE h) {
    if (!g_ltReady) return;
    EnterCriticalSection(&g_ltLock);
    int i = lt_find(h);
    if (i >= 0) { g_lt[i].closed = 1; if (g_lt[i].exited) g_lt[i].used = 0; }
    LeaveCriticalSection(&g_ltLock);
}
static int has_lt(DWORD n, const HANDLE *hs) {
    if (!g_ltReady || !g_nlt) return 0;
    int any = 0;
    EnterCriticalSection(&g_ltLock);
    for (DWORD i = 0; i < n && !any; i++) any = lt_find(hs[i]) >= 0;
    LeaveCriticalSection(&g_ltLock);
    return any;
}
// One wait call on objects (timeout t); ended logical threads count as signaled.
static DWORD xwait(DWORD n, const HANDLE *hs, BOOL all, DWORD t, BOOL alertable) {
    if (!g_ltReady || !g_nlt || n > MAXIMUM_WAIT_OBJECTS) return o_WaitForMultipleObjectsEx(n, hs, all, t, alertable);
    HANDLE tmp[MAXIMUM_WAIT_OBJECTS];
    DWORD map[MAXIMUM_WAIT_OBJECTS], m = 0;
    int any = 0, ended = -1;
    EnterCriticalSection(&g_ltLock);
    for (DWORD i = 0; i < n; i++) {
        int l = lt_find(hs[i]);
        if (l >= 0) {
            any = 1;
            if (g_lt[l].exited) {
                if (!all) { ended = (int)i; break; }
                continue;
            }
        }
        tmp[m] = hs[i];
        map[m++] = i;
    }
    LeaveCriticalSection(&g_ltLock);
    if (!any) return o_WaitForMultipleObjectsEx(n, hs, all, t, alertable);
    if (ended >= 0) {
        if (ended > 0) {
            DWORD r = o_WaitForMultipleObjectsEx((DWORD)ended, hs, FALSE, 0, alertable);
            if (r != WAIT_TIMEOUT) return r;
        }
        return WAIT_OBJECT_0 + (DWORD)ended;
    }
    if (!m) return WAIT_OBJECT_0;
    DWORD r = o_WaitForMultipleObjectsEx(m, tmp, all, t, alertable);
    if (r < WAIT_OBJECT_0 + m) return all ? WAIT_OBJECT_0 : WAIT_OBJECT_0 + map[r - WAIT_OBJECT_0];
    if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + m) return all ? WAIT_ABANDONED_0 : WAIT_ABANDONED_0 + map[r - WAIT_ABANDONED_0];
    return r;
}
// A wait of any length: on a logical thread's handle it checks every 10 ms.
static DWORD real_wait(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable) {
    if (ms == 0 || !has_lt(n, hs)) return xwait(n, hs, all, ms, alertable);
    int64_t start = real_qpc();
    for (;;) {
        int64_t el = (real_qpc() - start) * 1000 / g_realFreq;
        DWORD left = ms == INFINITE ? INFINITE : el >= (int64_t)ms ? 0 : ms - (DWORD)el;
        DWORD slice = left > 10 ? 10 : left;
        DWORD r = xwait(n, hs, all, slice, alertable);
        if (r != WAIT_TIMEOUT || slice == left) return r;
    }
}

// Lockstep: once the game thread is known, the other game threads take turns. A thread that waits
// (in one of the hooked wait functions) gives its turn back; the game thread hands out turns, one
// thread at a time in creation order, and only while it is blocked itself or at the end of a frame.
// In its turn a thread checks its wait without blocking; if the wait is satisfied it runs until it
// waits again. Nothing depends on how fast threads wake up: the same game input gives the same
// order of everything the threads do, which is what makes playback reproducible.
static int lockstep(void) { return g_mainThread != 0 && g_turnDone != NULL; }
static int is_main(void) { return g_mainThread && GetCurrentThreadId() == g_mainThread; }
static void rebind_check(ThreadSlot *s) {
    if (s && s->rebind) { tls_apply(&s->want); s->vals = s->want; s->rebind = 0; }
}
// Background thread: give the turn back and wait for the next one. This is also where a savestate
// finds every background thread, and where a load puts each one back (see restore_now).
static void restore_now(ThreadSlot *s) __attribute__((noreturn));
static void turn_wait(ThreadSlot *s) {
    s->parkErr = GetLastError();
    tls_record(&s->vals);
    s->parkSeh = teb_read(0);
    int landed = 0;
    if (__builtin_setjmp(s->parkJb)) { s = my_slot(); landed = 1; }   // brought back by a savestate load
    InterlockedExchange(&s->polling, 1);
    InterlockedExchange(&s->state, TS_WAITING);
    InterlockedExchange(&s->starting, 0);
    if (landed) InterlockedExchange(&s->restored, 1);
    SetEvent(g_turnDone);
    for (;;) {
        o_WaitForSingleObject(s->turnEv, INFINITE);
        if (s->restore) restore_now(s);
        if (s->state == TS_RUNNING) break;             // a turn
    }
    InterlockedExchange(&s->polling, 0);
    rebind_check(s);
    SetLastError(s->parkErr);
}

// Async reads (ReadFileEx): the completion routine runs as an APC in an alertable wait of the thread
// that started the read. Each read is issued with an OVERLAPPED of our own; before any alertable
// wait is checked, every outstanding read is allowed to finish in the kernel, and the game's
// completion routines then run in the order the reads were issued (the kernel would run them in the
// order they finished, which depends on the disk). Neither the readiness nor the order of
// completions depends on timing.
#define MAX_PENDING 1024
typedef struct {
    OVERLAPPED ov;                     // ours, given to the kernel
    LPOVERLAPPED gameOv;
    LPOVERLAPPED_COMPLETION_ROUTINE cb;
    uint32_t seq;
    DWORD tid, err, bytes;             // err / bytes: as passed to our completion APC
    uint8_t used, delivered, apcDone;
} AsyncRead;
static CRITICAL_SECTION g_pendLock;
static AsyncRead g_pend[MAX_PENDING];
static uint32_t g_readSeq;
static void async_settle(void) {
    if (!g_asyncOutstanding) return;
    int64_t start = real_qpc();
    for (;;) {
        int pending = 0;
        EnterCriticalSection(&g_pendLock);
        for (int i = 0; i < MAX_PENDING && !pending; i++)
            if (g_pend[i].used && !g_pend[i].delivered && !HasOverlappedIoCompleted(&g_pend[i].ov)) pending = 1;
        LeaveCriticalSection(&g_pendLock);
        if (!pending) break;
        Sleep(0);
        if (real_qpc() - start > g_realFreq * 3) {
            static int logged;
            if (!logged) { logged = 1; logf_("WARNING: an async read did not finish within 3 s"); }
            break;
        }
    }
}

// Game thread: wait until thread t gives its turn back.
static int g_frameProf;                // LOUKE_FRAMEPROF=1: where the real time of slow frames goes
static int64_t g_mainWaitTicks, g_settleTicks;
static int g_profRounds[2], g_profMaxRounds;  // rounds in the two frame-end turns; most in any frame
static char g_profRoundMs[160];
static int g_inSettle;                  // 1 / 2: inside the first / second frame-end turn
static void turn_finish(ThreadSlot *t) {
    int64_t start = real_qpc();
    while (t->state == TS_RUNNING && is_main()) {
        o_WaitForSingleObject(g_turnDone, 20);
        if (t->state == TS_RUNNING && real_qpc() - start > g_realFreq * 5) {
            static int logged;
            if (!logged) { logged = 1; logf_("WARNING: thread %lu (start %p) kept its turn for 5 s; it now runs freely", t->tid, t->start); }
            break;
        }
    }
    if (g_frameProf) t->turnTicks += real_qpc() - start;
}
static void frame_prof(int64_t frameTicks) {
    if (!g_frameProf) return;
    double ms = (double)frameTicks * 1000.0 / (double)g_realFreq;
    if (ms > 25) {
        char top[256];
        size_t tl = 0;
        top[0] = 0;
        LONG n = thread_slots();
        for (int k = 0; k < 4; k++) {               // the four threads that used most time
            LONG best = -1;
            for (LONG i = 0; i < n; i++)
                if (g_threads[i].turnTicks > 0 && (best < 0 || g_threads[i].turnTicks > g_threads[best].turnTicks)) best = i;
            if (best < 0) break;
            tl += snprintf(top + tl, sizeof top - tl, " t%ld(%p) %.0f ms", best, g_threads[best].start,
                           (double)g_threads[best].turnTicks * 1000.0 / (double)g_realFreq);
            g_threads[best].turnTicks = -g_threads[best].turnTicks;
            if (tl >= sizeof top) break;
        }
        logf_("PROF frame %u: %.0f ms, frame-end turns %.0f ms (rounds %d+%d:%s), game thread waiting %.0f ms;%s", g_frame, ms,
              (double)g_settleTicks * 1000.0 / (double)g_realFreq, g_profRounds[0], g_profRounds[1], g_profRoundMs,
              (double)g_mainWaitTicks * 1000.0 / (double)g_realFreq, top);
    }
    if (g_profRounds[0] + g_profRounds[1] > g_profMaxRounds) {
        g_profMaxRounds = g_profRounds[0] + g_profRounds[1];
        logf_("PROF most frame-end rounds so far: %d (frame %u, %.0f ms)", g_profMaxRounds, g_frame, ms);
    }
    if (g_frameProf) {
        static uint32_t hist[8];
        int r = g_profRounds[0] + g_profRounds[1], k = r <= 2 ? 0 : r <= 4 ? 1 : r <= 6 ? 2 : r <= 8 ? 3 : r <= 12 ? 4 : r <= 16 ? 5 : r <= 32 ? 6 : 7;
        hist[k]++;
        if (g_frame % 1000 == 0) logf_("PROF rounds per frame: <=2 %u, <=4 %u, <=6 %u, <=8 %u, <=12 %u, <=16 %u, <=32 %u, more %u",
                                       hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
    }
    g_profRounds[0] = g_profRounds[1] = 0;
    g_profRoundMs[0] = 0;
    LONG n = thread_slots();
    for (LONG i = 0; i < n; i++) g_threads[i].turnTicks = 0;
    g_mainWaitTicks = g_settleTicks = 0;
}
enum { WK_ANY, WK_OBJECTS, WK_CS, WK_TIME };
// Main thread: could the wait of thread t (waiting for a turn) be satisfied now? Answers without
// changing anything; when unsure it answers yes (the thread then checks for itself in its turn).
static int async_ready(DWORD tid) {
    int r = 0;
    EnterCriticalSection(&g_pendLock);
    for (int i = 0; i < MAX_PENDING && !r; i++)
        r = g_pend[i].used && !g_pend[i].delivered && g_pend[i].tid == tid && HasOverlappedIoCompleted(&g_pend[i].ov);
    LeaveCriticalSection(&g_pendLock);
    return r;
}
static int obj_ready(HANDLE h, int type) {
    if (g_ltReady) {
        EnterCriticalSection(&g_ltLock);
        int l = lt_find(h), ended = l >= 0 && g_lt[l].exited;
        LeaveCriticalSection(&g_ltLock);
        if (l >= 0) return ended;
    }
    return handles_ready(h, type);
}
static int may_go(ThreadSlot *t) {
    if (t->hasDeadline && vtime_base() >= t->vdeadline) return 1;
    switch (t->wkind) {
    case WK_OBJECTS: {
        if (t->walert && async_ready(t->tid)) return 1;
        int yes = 0, no = 0;
        for (DWORD i = 0; i < t->wn; i++) {
            int r = obj_ready(t->wh[i], t->wtype[i]);
            if (r) yes = 1; else no = 1;
        }
        return t->wall ? !no : yes;
    }
    case WK_CS: {
        DWORD owner = (DWORD)(uintptr_t)t->wcs->OwningThread;
        return owner == 0 || owner == t->tid;
    }
    case WK_TIME:
        return t->walert && async_ready(t->tid);
    default:
        return 1;
    }
}

// Game thread: a thread created recently first reaches its first turn.
static void starting_settle(void) {
    int64_t start = real_qpc();
    for (;;) {
        int busy = 0;
        LONG n = thread_slots();
        for (LONG i = 0; i < n && !busy; i++) {
            ThreadSlot *t = &g_threads[i];
            busy = t->starting && t->tid != g_mainThread && t->state != TS_SUSPENDED && t->state != TS_DEAD;
        }
        if (!busy) return;
        Sleep(0);
        if (real_qpc() - start > g_realFreq * 3) {
            static int logged;
            if (!logged) { logged = 1; logf_("WARNING: a new thread did not start within 3 s"); }
            return;
        }
    }
}
// Game thread: hand out turns until no waiting thread can go on. Returns 1 if any thread ran.
static int g_schedLog;                 // LOUKE_SCHEDLOG=1: log which threads ran in each turn
static int64_t g_profRoundT0;
static int background_turn(void) {
    if (!lockstep() || !is_main()) return 0;
    int any = 0;
    char trace[256];
    size_t tl = 0;
    trace[0] = 0;
    for (int round = 0; round < 64; round++) {
        int64_t roundStart = g_frameProf ? real_qpc() : 0;
        if (round && g_frameProf && g_inSettle) {
            size_t l = strlen(g_profRoundMs);
            if (l < sizeof g_profRoundMs - 8) snprintf(g_profRoundMs + l, sizeof g_profRoundMs - l, " %.0f", (double)(real_qpc() - g_profRoundT0) * 1000.0 / (double)g_realFreq);
        }
        if (g_frameProf) g_profRoundT0 = roundStart;
        if (g_frameProf && g_inSettle) g_profRounds[g_inSettle - 1]++;
        starting_settle();
        async_settle();
        int progress = 0;
        if (g_schedLog && round && tl < sizeof trace - 8) tl += snprintf(trace + tl, sizeof trace - tl, " |");
        LONG n = thread_slots(), ord[MAX_THREADS];
        for (LONG i = 0; i < n; i++) {                   // in creation order of the logical threads
            LONG k = i;
            while (k > 0 && g_threads[ord[k - 1]].order > g_threads[i].order) { ord[k] = ord[k - 1]; k--; }
            ord[k] = i;
        }
        for (LONG j = 0; j < n; j++) {
            LONG i = ord[j];
            ThreadSlot *t = &g_threads[i];
            if (!is_main()) return any;                  // the main role moved to the game thread
            if (t->tid == g_mainThread || t->state != TS_WAITING || !t->polling) continue;
            LONG yielded = t->yielded;
            if (yielded && round >= 16) continue;         // a thread that only yields gets 16 turns
            if (!yielded && !may_go(t)) continue;         // its wait cannot be satisfied yet
            LONG runs = t->runs;
            InterlockedExchange(&t->state, TS_RUNNING);
            SetEvent(t->turnEv);
            turn_finish(t);
            if (g_schedLog >= 2 && g_frame < 6)
                logf_("GRANT f=%u r%d t%ld %s wait@%p", g_frame, round, i, t->runs != runs ? "ran" : "-", t->waitFrom);
            if (t->runs != runs) {
                progress = 1;
                if (g_schedLog && tl < sizeof trace - 8) tl += snprintf(trace + tl, sizeof trace - tl, " %ld", i);
            }
        }
        if (!progress) {
            if (g_schedLog && any) logf_("TURN f=%u%s", g_frame, trace);
            return any;
        }
        any = 1;
    }
    if (g_schedLog) logf_("TURN f=%u%s ...", g_frame, trace);
    static int logged;
    if (!logged) { logged = 1; logf_("WARNING: background threads kept running for 64 rounds"); }
    return any;
}

// The game thread takes over the main role (called on the game thread). The initial thread, which
// handed it its turn, stops handing out turns and continues as the window thread, outside the lockstep.
static void main_handover(void) {
    if (!lockstep() || g_mainThread == GetCurrentThreadId()) return;
    ThreadSlot *s = my_slot();
    g_mainThread = GetCurrentThreadId();
    if (s) s->state = TS_RUNNING;
    SetEvent(g_turnDone);
    logf_("lockstep main thread is now the game thread");
}

// Without lockstep: wait until every other thread is idle.
static int background_idle(void) {
    if (g_asyncOutstanding) return 0;
    LONG n = thread_slots();
    for (LONG i = 0; i < n; i++)
        if (g_threads[i].state == TS_RUNNING && g_threads[i].tid != g_gameThread && g_threads[i].tid != g_mainThread) return 0;
    return 1;
}
static void wait_background(void) {
    int64_t start = real_qpc(), limit = g_realFreq * 3;
    for (;;) {
        if (background_idle()) {
            LONG w0 = g_signalWakes;
            Sleep(1);
            if (background_idle() && g_signalWakes == w0) break;
        } else {
            Sleep(0);
        }
        if (real_qpc() - start > limit) {
            LONG n = thread_slots();
            for (LONG i = 0; i < n; i++)
                if (g_threads[i].state == TS_RUNNING && g_threads[i].tid != g_gameThread)
                    logf_("WARNING: thread %lu (start %p) still busy after 3 s", g_threads[i].tid, g_threads[i].start);
            if (g_asyncOutstanding) logf_("WARNING: %ld async reads still outstanding", g_asyncOutstanding);
            break;
        }
    }
}
// Let all other game threads finish what they can do now.
static void settle_background(void) {
    int64_t start = real_qpc();
    if (lockstep()) background_turn();
    else wait_background();
    g_settleTicks += real_qpc() - start;
    g_lastSyncMs = (double)(real_qpc() - start) * 1000.0 / (double)g_realFreq;
}

// Earliest virtual deadline of a thread waiting for its turn (INT64_MAX: none).
static int64_t earliest_deadline(void) {
    int64_t d = INT64_MAX;
    LONG n = thread_slots();
    for (LONG i = 0; i < n; i++) {
        ThreadSlot *t = &g_threads[i];
        if (t->state == TS_WAITING && t->polling && t->hasDeadline && t->vdeadline < d) d = t->vdeadline;
    }
    return d;
}
// The game thread is blocked and no other game thread can go on: let virtual time pass up to the
// next timeout of a waiting thread (but not beyond limit). Returns 1 if time advanced.
static int advance_to_deadline(int64_t limit) {
    int64_t d = earliest_deadline(), now = vtime_base();
    if (d == INT64_MAX || d > limit) return 0;
    if (d > now) g_spinTicks += d - now;
    static int logged;
    if (!logged) { logged = 1; logf_("note: the main thread waited for a timed wait of another thread; virtual time advanced"); }
    return 1;
}

// Background thread, lockstep: wait until poll() is satisfied or the virtual deadline passes.
// ms == 0 is a poll; every 64th unsuccessful poll in a row gives the turn back (a polling loop
// waiting for another thread must let it run).
typedef DWORD (*Poll_t)(void *ctx);
typedef struct { DWORD n; const HANDLE *hs; BOOL all, alertable; } WaitCtx;
static void describe_wait(ThreadSlot *s, int kind, void *ctx) {
    s->wkind = WK_ANY;
    if (kind == WK_OBJECTS) {
        const WaitCtx *c = ctx;
        if (c->n > MAXIMUM_WAIT_OBJECTS) return;
        for (DWORD i = 0; i < c->n; i++) { s->wh[i] = c->hs[i]; s->wtype[i] = (int8_t)handles_type(c->hs[i]); }
        s->wn = c->n;
        s->wall = c->all != 0;
        s->walert = c->alertable != 0;
        if (!c->n) kind = WK_TIME;
    } else if (kind == WK_CS) {
        s->wcs = ctx;
    } else if (kind == WK_TIME) {
        s->walert = 0;
    }
    s->wkind = (uint8_t)kind;
}
static DWORD bg_wait(ThreadSlot *s, Poll_t poll, void *ctx, DWORD ms, int kind) {
    DWORD r;
    int described = 0;
    if (ms != 0 && ms != INFINITE) { s->vdeadline = vtime_base() + (int64_t)ms * 10000; s->hasDeadline = 1; }
    for (;;) {
        r = poll(ctx);
        if (r != WAIT_TIMEOUT) { s->polls = 0; break; }
        if (ms == 0) {
            if (++s->polls % 64 == 0) { s->yielded = 1; turn_wait(s); s->yielded = 0; }
            break;
        }
        if (s->hasDeadline && vtime_base() >= s->vdeadline) break;
        if (!described) { describe_wait(s, kind, ctx); described = 1; }
        turn_wait(s);
    }
    s->wkind = WK_ANY;
    s->hasDeadline = 0;
    InterlockedIncrement(&s->runs);
    return r;
}
static DWORD poll_objects(void *p) {
    WaitCtx *c = p;
    if (c->alertable) async_settle();
    if (c->n) return xwait(c->n, c->hs, c->all, 0, c->alertable);
    return o_SleepEx(0, c->alertable) == WAIT_IO_COMPLETION ? WAIT_IO_COMPLETION : WAIT_TIMEOUT;
}
static DWORD poll_cs(void *p) { return TryEnterCriticalSection((LPCRITICAL_SECTION)p) ? WAIT_OBJECT_0 : WAIT_TIMEOUT; }
static DWORD poll_never(void *p) { (void)p; return WAIT_TIMEOUT; }
static int bg_lockstep(ThreadSlot *s) { return s && lockstep() && s->tid != g_mainThread && s->turnEv; }

// A background thread asking for the time very often within one frame is waiting for time to
// pass: let it wait for a millisecond of virtual time.
static void background_time_query(void) {
    ThreadSlot *s = my_slot();
    if (!bg_lockstep(s)) return;
    if (s->qframe != g_frame) { s->qframe = g_frame; s->queries = 0; }
    if (++s->queries < 20000) return;
    s->queries = 0;
    bg_wait(s, poll_never, NULL, 1, WK_TIME);
}

// The game thread polls (zero timeout): every 256th poll in a frame gives the other threads a turn,
// so a polling loop that waits for them cannot hang.
static void game_poll(void) {
    if (++g_pollsThisFrame % 256 == 0) background_turn();
}
// The game thread waits: the other game threads take their turns; if none of them can go on, virtual
// time passes up to the next timeout among them (within the wait's own timeout). Only when nothing
// in the game's threads can satisfy the wait does the game thread wait for real (for the window
// thread or the system).
static DWORD game_wait(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable) {
    if (!lockstep() || !is_main()) return real_wait(n, hs, all, ms, alertable);
    if (alertable) async_settle();
    DWORD r = xwait(n, hs, all, 0, alertable);
    if (r != WAIT_TIMEOUT) return r;
    if (ms == 0) { game_poll(); return r; }
    while (background_turn()) {
        r = xwait(n, hs, all, 0, alertable);
        if (r != WAIT_TIMEOUT) return r;
    }
    // Nothing the other game threads can do now satisfies it: wait for real, for the window thread
    // or the system, letting game threads woken meanwhile take their turns.
    int64_t start = real_qpc();
    for (;;) {
        int64_t el = (real_qpc() - start) * 1000 / g_realFreq;
        if (ms != INFINITE && el >= (int64_t)ms) return WAIT_TIMEOUT;
        DWORD slice = ms == INFINITE || ms - (DWORD)el > 5 ? 5 : ms - (DWORD)el;
        r = xwait(n, hs, all, slice, alertable);
        if (r != WAIT_TIMEOUT) return r;
        if (!is_main()) return real_wait(n, hs, all, ms == INFINITE ? INFINITE : ms - (DWORD)el, alertable);
        int ran = background_turn();
        if (!ran && ms == INFINITE && el > 20) ran = advance_to_deadline(INT64_MAX);
        if (ran) {
            r = xwait(n, hs, all, 0, alertable);
            if (r != WAIT_TIMEOUT) return r;
        }
    }
}
static void game_enter_cs(LPCRITICAL_SECTION cs) {
    int64_t start = real_qpc();
    while (lockstep() && is_main()) {
        if (TryEnterCriticalSection(cs)) return;
        if (background_turn()) continue;
        if ((real_qpc() - start) * 1000 / g_realFreq > 20 && advance_to_deadline(INT64_MAX)) continue;
        o_Sleep(1);                    // held by a thread outside the lockstep
    }
    o_EnterCriticalSection(cs);
}

// Without lockstep (before the game thread is known) waits are real; a thread that comes back from
// such a wait after lockstep started joins it.
static void wait_enter(ThreadSlot *s) {
    if (!s) return;
    tls_record(&s->vals);
    s->state = TS_WAITING;
}
static void wait_leave(ThreadSlot *s, int signaled) {
    if (!s) return;
    rebind_check(s);
    if (signaled) InterlockedIncrement(&g_signalWakes);
    if (bg_lockstep(s)) { turn_wait(s); InterlockedIncrement(&s->runs); }
    s->state = TS_RUNNING;
}

// Savestates keep, for every real game thread, what it was doing: idle, or parked in turn_wait with
// its stack and registers (see save_threads / restore_now).
enum { ST_IDLE, ST_PARKED, ST_OTHER, ST_MAIN };
typedef struct {
    uint8_t kind;
    LONG lt, yielded, hasDeadline, exitJbValid;
    uint32_t order, polls, qframe, queries, stackSize, tlsSize;
    int64_t vdeadline;
    DWORD parkErr;
    void *jb[5], *exitJb[5];
    uintptr_t seh, exitSeh, esp;
    TlsVals vals;
    uint8_t *stack;
    uint8_t tls[64];
    // what the thread waits for (see may_go): the wait it goes back to, not the one it left
    uint8_t wkind, wall, walert;
    DWORD wn;
    HANDLE wh[MAXIMUM_WAIT_OBJECTS];
    int8_t wtype[MAXIMUM_WAIT_OBJECTS];
    LPCRITICAL_SECTION wcs;
} SavedThread;
static const SavedThread g_idleThread = {ST_IDLE};

// Runs on the thread's private stack: puts its stack, registers and per-thread data back as saved
// and continues in the turn_wait that was waiting at the save (or idle).
__attribute__((used, noreturn)) static void __cdecl restore_body(ThreadSlot *s) {
    const SavedThread *t = s->restore;
    if (t->kind == ST_PARKED) {
        memcpy((void *)t->esp, t->stack, t->stackSize);
        if (s->tlsBlock && t->tlsSize) memcpy(s->tlsBlock, t->tls, t->tlsSize);
        tls_apply(&t->vals);
        s->vals = t->vals;
        s->lt = t->lt; s->order = t->order; s->parkErr = t->parkErr;
        s->yielded = t->yielded; s->hasDeadline = t->hasDeadline; s->vdeadline = t->vdeadline;
        s->polls = t->polls; s->qframe = t->qframe; s->queries = t->queries;
        s->wkind = t->wkind; s->wall = t->wall; s->walert = t->walert; s->wn = t->wn; s->wcs = t->wcs;
        memcpy(s->wh, t->wh, sizeof s->wh);
        memcpy(s->wtype, t->wtype, sizeof s->wtype);
        s->work = 0; s->lsuspend = 0;
        memcpy(s->exitJb, t->exitJb, sizeof s->exitJb);
        s->exitSeh = t->exitSeh;
        s->exitJbValid = t->exitJbValid;
        memcpy(s->parkJb, t->jb, sizeof s->parkJb);
        teb_write(0, t->seh);
        s->restore = NULL;
        __builtin_longjmp(s->parkJb, 1);
    }
    TlsVals zero;
    memset(&zero, 0, sizeof zero);
    tls_apply(&zero);
    s->vals = zero;
    ss_tls_reset();
    s->lt = -1; s->work = 0; s->lsuspend = 0; s->exitJbValid = 0;
    s->yielded = 0; s->hasDeadline = 0; s->polls = 0;
    s->wkind = WK_ANY;
    teb_write(0, s->idleSeh);
    s->restore = NULL;
    __builtin_longjmp(s->idleJb, 1);
}
static void restore_now(ThreadSlot *s) {
    __asm__ volatile("movl %0, %%esp\n\t"
                     "pushl %1\n\t"
                     "call _restore_body\n\t" :: "r"(s->privTop), "r"(s) : "memory");
    __builtin_unreachable();
}

// The game's thread ended (returned or called ExitThread): the real thread becomes idle.
static void thread_exited(ThreadSlot *s, DWORD code) {
    EnterCriticalSection(&g_ltLock);
    if (s->lt >= 0) {
        LThread *l = &g_lt[s->lt];
        l->exited = 1;
        l->exitCode = code;
        if (l->closed) l->used = 0;
    }
    s->lt = -1;
    LeaveCriticalSection(&g_ltLock);
    s->exitJbValid = 0;
    TlsVals zero;
    memset(&zero, 0, sizeof zero);
    tls_apply(&zero);                  // a new thread starts with empty TLS / FLS slots
    s->vals = zero;
    ss_tls_reset();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    InterlockedIncrement(&s->runs);
    if (g_trace) logf_("TRACE thread %lu ends (exit code %lu), stays idle", s->tid, code);
}
static DWORD WINAPI thread_start(void *p) {
    ThreadSlot *s = p;
    s->tid = GetCurrentThreadId();
    s->stackBase = teb_read(4);
    uint32_t sz = 0;
    s->tlsBlock = ss_tls_block(&sz);
    s->tlsSize = sz;
    TlsSetValue(g_tls, s);
    s->idleSeh = teb_read(0);
    int landed = 0;
    if (__builtin_setjmp(s->idleJb)) { s = my_slot(); landed = 1; }   // sent back to idle by a savestate load
    for (;;) {
        EnterCriticalSection(&g_ltLock);
        if (!s->work) { s->state = TS_IDLE; s->starting = 0; }
        LeaveCriticalSection(&g_ltLock);
        if (landed) { landed = 0; InterlockedExchange(&s->restored, 1); }
        if (g_turnDone) SetEvent(g_turnDone);
        while (!(s->work && !s->lsuspend) && !s->restore) o_WaitForSingleObject(s->turnEv, INFINITE);
        if (s->restore) restore_now(s);
        s->work = 0;
        LPTHREAD_START_ROUTINE fn = s->fn;
        void *arg = s->arg;
        if (g_trace) logf_("TRACE thread %lu begins, start=%p", s->tid, (void *)fn);
        if (bg_lockstep(s)) { turn_wait(s); InterlockedIncrement(&s->runs); }   // starts in its first turn
        else s->starting = 0;
        s->state = TS_RUNNING;
        DWORD r = 0;
        s->exitSeh = teb_read(0);
        if (__builtin_setjmp(s->exitJb) == 0) {
            s->exitJbValid = 1;
            r = fn(arg);
        } else {
            s = my_slot();
            teb_write(0, s->exitSeh);
            r = s->exitCode;
        }
        thread_exited(s, r);
    }
}
static HANDLE WINAPI h_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T st, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD fl, LPDWORD tid) {
    LONG suspend = (fl & CREATE_SUSPENDED) != 0;
    HANDLE h = NULL;
    DWORD t = 0;
    EnterCriticalSection(&g_ltLock);
    int l = lt_alloc();
    ThreadSlot *s = NULL;
    LONG n = thread_slots();
    for (LONG i = 0; l >= 0 && i < n && !s; i++) {
        ThreadSlot *c = &g_threads[i];
        if (c->state == TS_IDLE && !c->work && c->lt < 0 && c->hReal && !c->restore) s = c;
    }
    if (s && DuplicateHandle(GetCurrentProcess(), s->hReal, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        // an idle real thread runs the new thread
        t = s->tid;
        g_lt[l] = (LThread){h, t, (int)(s - g_threads), 1, 0, 0, suspend, 0};
        s->fn = fn; s->arg = arg; s->start = (void *)fn;
        s->lt = l; s->order = g_createSeq++;
        s->lsuspend = suspend;
        s->state = suspend ? TS_SUSPENDED : TS_RUNNING;
        s->starting = !suspend;
        s->work = 1;
        LeaveCriticalSection(&g_ltLock);
        if (!suspend) SetEvent(s->turnEv);
        handles_track_thread(h);
        if (tid) *tid = t;
        if (g_trace) logf_("TRACE CreateThread start=%p -> idle thread %lu", (void *)fn, t);
        return h;
    }
    LONG i = l >= 0 ? InterlockedIncrement(&g_threadCount) - 1 : MAX_THREADS;
    if (i >= MAX_THREADS) {
        if (l >= 0) g_lt[l].used = 0;
        LeaveCriticalSection(&g_ltLock);
        static int logged;
        if (!logged) { logged = 1; logf_("WARNING: too many game threads; further ones are not managed"); }
        return o_CreateThread(sa, st, fn, arg, fl, tid);
    }
    s = &g_threads[i];
    s->turnEv = CreateEventW(NULL, FALSE, FALSE, NULL);
    uint8_t *ps = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    s->privTop = ps ? ps + 0x10000 - 64 : NULL;
    s->fn = fn; s->arg = arg; s->start = (void *)fn;
    s->lt = l; s->order = g_createSeq++;
    s->lsuspend = suspend;
    s->prev = TS_RUNNING;
    s->state = suspend ? TS_SUSPENDED : TS_RUNNING;
    s->starting = !suspend;
    s->work = 1;
    g_lt[l] = (LThread){NULL, 0, (int)i, 1, 0, 0, suspend, 0};
    h = o_CreateThread(sa, st, thread_start, s, fl & ~(DWORD)CREATE_SUSPENDED, &t);
    if (!h) {
        g_lt[l].used = 0;
        s->state = TS_DEAD; s->starting = 0; s->work = 0; s->lt = -1;
        LeaveCriticalSection(&g_ltLock);
        return NULL;
    }
    g_lt[l].h = h;
    g_lt[l].tid = t;
    if (!s->tid) s->tid = t;
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &s->hReal, 0, FALSE, DUPLICATE_SAME_ACCESS);
    LeaveCriticalSection(&g_ltLock);
    handles_track_thread(h);
    if (tid) *tid = t;
    if (g_trace) logf_("TRACE CreateThread start=%p -> new thread %lu", (void *)fn, t);
    return h;
}
static void WINAPI h_ExitThread(DWORD code) {
    ThreadSlot *s = my_slot();
    if (s && s->exitJbValid) {         // the game's thread ends; its real thread goes idle
        s->exitCode = code;
        __builtin_longjmp(s->exitJb, 1);
    }
    if (s) { s->state = TS_DEAD; SetEvent(g_turnDone); }
    o_ExitThread(code);
}
static DWORD WINAPI h_SuspendThread(HANDLE h) {
    if (g_ltReady) {                   // a thread created suspended that has not started yet
        EnterCriticalSection(&g_ltLock);
        int l = lt_find(h);
        if (l >= 0 && !g_lt[l].exited && g_lt[l].suspend > 0) {
            DWORD prev = (DWORD)g_lt[l].suspend++;
            LeaveCriticalSection(&g_ltLock);
            return prev;
        }
        LeaveCriticalSection(&g_ltLock);
    }
    DWORD r = o_SuspendThread(h);
    ThreadSlot *s = slot_by_tid(GetThreadId(h));
    if (s && r == 0) { s->prev = s->state; s->state = TS_SUSPENDED; if (g_turnDone) SetEvent(g_turnDone); }
    return r;
}
static DWORD WINAPI h_ResumeThread(HANDLE h) {
    if (g_ltReady) {
        EnterCriticalSection(&g_ltLock);
        int l = lt_find(h);
        if (l >= 0 && !g_lt[l].exited && g_lt[l].suspend > 0) {
            DWORD prev = (DWORD)g_lt[l].suspend--;
            ThreadSlot *s = &g_threads[g_lt[l].slot];
            int start = g_lt[l].suspend == 0 && s->lt == l && s->lsuspend;
            if (start) { s->lsuspend = 0; s->starting = 1; s->state = TS_RUNNING; }
            LeaveCriticalSection(&g_ltLock);
            if (start) SetEvent(s->turnEv);
            return prev;
        }
        LeaveCriticalSection(&g_ltLock);
    }
    DWORD r = o_ResumeThread(h);
    ThreadSlot *s = slot_by_tid(GetThreadId(h));
    if (s && r == 1 && s->state == TS_SUSPENDED) s->state = s->prev;
    return r;
}
typedef BOOL (WINAPI *GetExitCodeThread_t)(HANDLE, LPDWORD);
static GetExitCodeThread_t o_GetExitCodeThread;
static BOOL WINAPI h_GetExitCodeThread(HANDLE h, LPDWORD code) {
    if (g_ltReady && code) {
        EnterCriticalSection(&g_ltLock);
        int l = lt_find(h);
        if (l >= 0) *code = g_lt[l].exited ? g_lt[l].exitCode : STILL_ACTIVE;
        LeaveCriticalSection(&g_ltLock);
        if (l >= 0) return TRUE;
    }
    return o_GetExitCodeThread(h, code);
}
static void diag_wait_failed(const char *fn, HANDLE h, void *from) {
    static volatile LONG n;
    DWORD e = GetLastError();
    if (InterlockedIncrement(&n) <= 20) logf_("WARNING: %s(%p) failed, error %lu, called from %p", fn, h, e, from);
    SetLastError(e);
}
// One wait on objects (n = 0: sleep) by any game thread.
static DWORD any_wait(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable) {
    ThreadSlot *s = my_slot();
    if (is_main()) {
        if (s) tls_record(&s->vals);
        int64_t t0 = g_frameProf ? real_qpc() : 0;
        DWORD r;
        if (n) r = game_wait(n, hs, all, ms, alertable);
        else {
            if (alertable && lockstep()) async_settle();
            if (ms == 0) { r = o_SleepEx(0, alertable); game_poll(); }
            else {
                // Before the first frame there is no frame clock: when the main thread only sleeps
                // (for example polling a flag another thread sets) and every other game thread
                // waits for a timeout, let virtual time pass up to the next timeout, as a wait does.
                static int64_t idleSince;
                if (background_turn() || g_gameThread || !lockstep()) idleSince = 0;
                else if (!idleSince) idleSince = real_qpc();
                else if ((real_qpc() - idleSince) * 1000 / g_realFreq > 20 && advance_to_deadline(INT64_MAX)) {
                    idleSince = 0;
                    background_turn();
                }
                r = o_SleepEx(ms, alertable);
            }
        }
        if (g_frameProf) g_mainWaitTicks += real_qpc() - t0;
        return r;
    }
    if (bg_lockstep(s)) {
        WaitCtx c = {n, hs, all, alertable};
        tls_record(&s->vals);
        return bg_wait(s, poll_objects, &c, ms, WK_OBJECTS);
    }
    wait_enter(s);
    DWORD r;
    int64_t start = real_qpc();
    for (;;) {
        // in slices, so that a thread waiting when lockstep starts joins it within 10 ms
        int64_t el = (real_qpc() - start) * 1000 / g_realFreq;
        DWORD left = ms == INFINITE ? INFINITE : el >= (int64_t)ms ? 0 : ms - (DWORD)el;
        DWORD slice = s && left > 10 ? 10 : left;
        r = n ? (s ? xwait(n, hs, all, slice, alertable) : real_wait(n, hs, all, slice, alertable)) : o_SleepEx(slice, alertable);
        if (n ? r != WAIT_TIMEOUT : r == WAIT_IO_COMPLETION) break;
        if (slice == left) break;
        if (bg_lockstep(s)) {
            WaitCtx c = {n, hs, all, alertable};
            el = (real_qpc() - start) * 1000 / g_realFreq;
            left = ms == INFINITE ? INFINITE : el >= (int64_t)ms ? 1 : ms - (DWORD)el;
            turn_wait(s);              // join the lockstep, then wait in it
            return bg_wait(s, poll_objects, &c, left, WK_OBJECTS);
        }
    }
    wait_leave(s, n ? r != WAIT_TIMEOUT : r == WAIT_IO_COMPLETION);
    return r;
}
static void note_wait(void *from) { ThreadSlot *s = g_schedLog ? my_slot() : NULL; if (s) s->waitFrom = from; }
static DWORD WINAPI h_WaitForSingleObject(HANDLE h, DWORD ms) {
    note_wait(__builtin_return_address(0));
    DWORD r = any_wait(1, &h, FALSE, ms, FALSE);
    if (r == WAIT_FAILED) diag_wait_failed("WaitForSingleObject", h, __builtin_return_address(0));
    return r;
}
static DWORD WINAPI h_WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    note_wait(__builtin_return_address(0));
    DWORD r = any_wait(n, hs, all, ms, FALSE);
    if (r == WAIT_FAILED) diag_wait_failed("WaitForMultipleObjects", n ? hs[0] : NULL, __builtin_return_address(0));
    return r;
}
static DWORD WINAPI h_WaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable) {
    note_wait(__builtin_return_address(0));
    DWORD r = any_wait(n, hs, all, ms, alertable);
    if (r == WAIT_FAILED) diag_wait_failed("WaitForMultipleObjectsEx", n ? hs[0] : NULL, __builtin_return_address(0));
    return r;
}
// Savestates park the window (initial) thread while they read or write game memory: it waits here,
// inside one of its own Sleep calls, where it holds no locks.
static HANDLE g_parkedEv, g_unparkEv;
static volatile LONG g_parkReq;
static TlsVals g_initVals, g_initWant;
static volatile LONG g_initRebind;
static void park_check(void) {
    if (!g_parkReq || GetCurrentThreadId() != g_initThread) return;
    tls_record(&g_initVals);
    DWORD e = GetLastError();
    while (g_parkReq) {
        SetEvent(g_parkedEv);
        WaitForSingleObject(g_unparkEv, 50);
    }
    SetLastError(e);
    if (g_initRebind) { tls_apply(&g_initWant); g_initRebind = 0; }
}

static DWORD WINAPI h_SleepEx(DWORD ms, BOOL alertable) {
    park_check();
    note_wait(__builtin_return_address(0));
    DWORD r = any_wait(0, NULL, FALSE, ms, alertable);
    return r == WAIT_IO_COMPLETION ? WAIT_IO_COMPLETION : 0;
}
static void WINAPI h_Sleep(DWORD ms) {
    park_check();
    note_wait(__builtin_return_address(0));
    any_wait(0, NULL, FALSE, ms, FALSE);
}
static BOOL WINAPI h_GetOverlappedResult(HANDLE f, LPOVERLAPPED ov, LPDWORD n, BOOL wait) {
    if (!wait) return o_GetOverlappedResult(f, ov, n, wait);
    ThreadSlot *s = my_slot();
    if (is_main()) { background_turn(); return o_GetOverlappedResult(f, ov, n, wait); }
    if (bg_lockstep(s)) return o_GetOverlappedResult(f, ov, n, wait);   // keeps its turn: the I/O finishes by itself
    wait_enter(s);
    BOOL r = o_GetOverlappedResult(f, ov, n, wait);
    wait_leave(s, 1);
    return r;
}
static void WINAPI h_EnterCriticalSection(LPCRITICAL_SECTION cs) {
    if (TryEnterCriticalSection(cs)) return;
    ThreadSlot *s = my_slot();
    if (is_main()) {
        int64_t t0 = g_frameProf ? real_qpc() : 0;
        game_enter_cs(cs);
        if (g_frameProf) g_mainWaitTicks += real_qpc() - t0;
        return;
    }
    if (bg_lockstep(s)) { if (g_schedLog) s->waitFrom = __builtin_return_address(0); bg_wait(s, poll_cs, cs, INFINITE, WK_CS); return; }
    wait_enter(s);
    o_EnterCriticalSection(cs);
    wait_leave(s, 1);
}

// Runs as the APC of one of our reads. The APCs of all finished reads of a thread run one after the
// other in the same alertable wait; the last of them delivers, in issue order, every finished read.
static VOID CALLBACK read_done(DWORD err, DWORD bytes, LPOVERLAPPED ov) {
    DWORD tid = GetCurrentThreadId();
    AsyncRead *me = (AsyncRead *)ov;
    int queued = 0;
    EnterCriticalSection(&g_pendLock);
    me->apcDone = 1;
    me->err = err;
    me->bytes = bytes;
    for (int i = 0; i < MAX_PENDING && !queued; i++) {
        AsyncRead *a = &g_pend[i];
        queued = a->used && !a->delivered && !a->apcDone && a->tid == tid && HasOverlappedIoCompleted(&a->ov);
    }
    LeaveCriticalSection(&g_pendLock);
    if (queued) return;
    ThreadSlot *s = my_slot();
    LONG prev = s ? s->state : 0;
    rebind_check(s);
    InterlockedIncrement(&g_signalWakes);
    if (s && !lockstep()) s->state = TS_RUNNING;
    for (;;) {
        AsyncRead *next = NULL;
        EnterCriticalSection(&g_pendLock);
        for (int i = 0; i < MAX_PENDING; i++) {
            AsyncRead *a = &g_pend[i];
            if (a->used && !a->delivered && a->apcDone && a->tid == tid && (!next || a->seq - next->seq > 0x80000000u))
                next = a;
        }
        LPOVERLAPPED gameOv = NULL;
        LPOVERLAPPED_COMPLETION_ROUTINE cb = NULL;
        DWORD e = 0, n = 0;
        if (next) {
            next->delivered = 1;
            gameOv = next->gameOv;
            cb = next->cb;
            gameOv->Internal = next->ov.Internal;
            gameOv->InternalHigh = next->bytes;
            n = next->bytes;
            e = next->err;
        }
        // a record is free once its own APC ran and the game's routine was called
        for (int i = 0; i < MAX_PENDING; i++)
            if (g_pend[i].used && g_pend[i].delivered && g_pend[i].apcDone) g_pend[i].used = 0;
        LeaveCriticalSection(&g_pendLock);
        if (!next) break;
        if (g_trace) logf_("TRACE ReadFileEx done %p err=%lu bytes=%lu", (void *)gameOv, e, n);
        InterlockedDecrement(&g_asyncOutstanding);
        if (cb) cb(e, n, gameOv);
    }
    if (s && !lockstep()) s->state = prev;
}
static BOOL WINAPI h_ReadFileEx(HANDLE f, LPVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE cb) {
    AsyncRead *a = NULL;
    EnterCriticalSection(&g_pendLock);
    for (int i = 0; i < MAX_PENDING; i++) if (!g_pend[i].used) { a = &g_pend[i]; break; }
    if (a) {
        memset(a, 0, sizeof *a);
        a->used = 1;
        a->ov.Internal = STATUS_PENDING;
        a->ov.Offset = ov->Offset;
        a->ov.OffsetHigh = ov->OffsetHigh;
        a->gameOv = ov;
        a->cb = cb;
        a->seq = g_readSeq++;
        a->tid = GetCurrentThreadId();
    }
    LeaveCriticalSection(&g_pendLock);
    if (!a) { logf_("WARNING: ReadFileEx table full"); return o_ReadFileEx(f, buf, n, ov, cb); }
    InterlockedIncrement(&g_asyncOutstanding);
    ov->Internal = STATUS_PENDING;
    BOOL r = o_ReadFileEx(f, buf, n, &a->ov, read_done);
    if (g_trace) logf_("TRACE ReadFileEx %p %lu bytes ov=%p -> %d", f, n, (void *)ov, r);
    if (!r) {
        EnterCriticalSection(&g_pendLock); a->used = 0; LeaveCriticalSection(&g_pendLock);
        InterlockedDecrement(&g_asyncOutstanding);
    }
    return r;
}

// Debug I/O trace (LOUKE_TRACEIO=1): file opens and synchronous reads.
static HANDLE WINAPI h_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE h = o_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
    handles_track_file(h);
    if (!g_trace) return h;
    char u[MAX_PATH * 3] = "?";
    if (name) WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, NULL, NULL);
    logf_("TRACE open %s -> %p%s", u, h, (flags & FILE_FLAG_OVERLAPPED) ? " (overlapped)" : "");
    return h;
}
static HANDLE WINAPI h_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE h = o_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
    handles_track_file(h);
    if (g_trace) logf_("TRACE openA %s -> %p", name ? name : "?", h);
    return h;
}
static BOOL WINAPI h_ReadFile(HANDLE f, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    BOOL r = o_ReadFile(f, buf, n, got, ov);
    logf_("TRACE ReadFile %p %lu bytes%s -> %d", f, n, ov ? " overlapped" : "", r);
    return r;
}

typedef BOOL (WINAPI *Handle1_t)(HANDLE);
static Handle1_t o_SetEvent, o_ResetEvent;
static BOOL WINAPI h_SetEvent(HANDLE h) {
    BOOL r = o_SetEvent(h);
    if (!r) diag_wait_failed("SetEvent", h, __builtin_return_address(0));
    return r;
}
static BOOL WINAPI h_ResetEvent(HANDLE h) {
    BOOL r = o_ResetEvent(h);
    if (!r) diag_wait_failed("ResetEvent", h, __builtin_return_address(0));
    return r;
}

typedef DWORD (WINAPI *TlsAlloc_t)(void);
typedef BOOL (WINAPI *TlsFree_t)(DWORD);
typedef FARPROC (WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
static TlsAlloc_t o_TlsAlloc;
static TlsFree_t o_TlsFree;
static GetProcAddress_t o_GetProcAddress;
static void idx_add(DWORD *tab, volatile LONG *n, DWORD idx) {
    if (*n < MAX_TLSIDX) { tab[*n] = idx; InterlockedIncrement(n); }
    else logf_("WARNING: more than %d TLS/FLS indices; per-thread data is not kept for savestates", MAX_TLSIDX);
}
static void idx_remove(DWORD *tab, volatile LONG *n, DWORD idx) {
    for (LONG i = 0; i < *n; i++) if (tab[i] == idx) { tab[i] = tab[*n - 1]; InterlockedDecrement(n); break; }
}
static DWORD WINAPI h_TlsAlloc(void) {
    DWORD i = o_TlsAlloc();
    if (i != TLS_OUT_OF_INDEXES) idx_add(g_tlsIdx, &g_ntls, i);
    return i;
}
static BOOL WINAPI h_TlsFree(DWORD i) { idx_remove(g_tlsIdx, &g_ntls, i); return o_TlsFree(i); }
static DWORD WINAPI h_FlsAlloc(PFLS_CALLBACK_FUNCTION cb) {
    DWORD i = FlsAlloc(cb);
    if (i != FLS_OUT_OF_INDEXES) idx_add(g_flsIdx, &g_nfls, i);
    return i;
}
static BOOL WINAPI h_FlsFree(DWORD i) { idx_remove(g_flsIdx, &g_nfls, i); return FlsFree(i); }
static FARPROC WINAPI h_GetProcAddress(HMODULE m, LPCSTR name) {
    if ((uintptr_t)name > 0xFFFF) {
        if (!strcmp(name, "FlsAlloc")) return (FARPROC)(void *)h_FlsAlloc;
        if (!strcmp(name, "FlsFree")) return (FARPROC)(void *)h_FlsFree;
    }
    return o_GetProcAddress(m, name);
}

static void install_thread_sync(void) {
    g_tls = TlsAlloc();
    InitializeCriticalSection(&g_pendLock);
    InitializeCriticalSection(&g_ltLock);
    g_ltReady = 1;
    g_parkedEv = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_unparkEv = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_turnDone = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_mainThread = GetCurrentThreadId();   // the initial thread hands out turns until the game thread is known
    // Each hook is optional: a game that does not import a function simply never calls it.
    tas_hook_iat("KERNEL32.dll", "CreateThread", 0, (void *)h_CreateThread, (void **)&o_CreateThread);
    tas_hook_iat("KERNEL32.dll", "ExitThread", 0, (void *)h_ExitThread, (void **)&o_ExitThread);
    tas_hook_iat("KERNEL32.dll", "SuspendThread", 0, (void *)h_SuspendThread, (void **)&o_SuspendThread);
    tas_hook_iat("KERNEL32.dll", "ResumeThread", 0, (void *)h_ResumeThread, (void **)&o_ResumeThread);
    tas_hook_iat("KERNEL32.dll", "GetExitCodeThread", 0, (void *)h_GetExitCodeThread, (void **)&o_GetExitCodeThread);
    tas_hook_iat("KERNEL32.dll", "WaitForSingleObject", 0, (void *)h_WaitForSingleObject, (void **)&o_WaitForSingleObject);
    tas_hook_iat("KERNEL32.dll", "WaitForMultipleObjects", 0, (void *)h_WaitForMultipleObjects, (void **)&o_WaitForMultipleObjects);
    tas_hook_iat("KERNEL32.dll", "WaitForMultipleObjectsEx", 0, (void *)h_WaitForMultipleObjectsEx, (void **)&o_WaitForMultipleObjectsEx);
    tas_hook_iat("KERNEL32.dll", "SleepEx", 0, (void *)h_SleepEx, (void **)&o_SleepEx);
    tas_hook_iat("KERNEL32.dll", "Sleep", 0, (void *)h_Sleep, (void **)&o_Sleep);
    tas_hook_iat("KERNEL32.dll", "GetOverlappedResult", 0, (void *)h_GetOverlappedResult, (void **)&o_GetOverlappedResult);
    tas_hook_iat("KERNEL32.dll", "EnterCriticalSection", 0, (void *)h_EnterCriticalSection, (void **)&o_EnterCriticalSection);
    tas_hook_iat("KERNEL32.dll", "ReadFileEx", 0, (void *)h_ReadFileEx, (void **)&o_ReadFileEx);
    tas_hook_iat("KERNEL32.dll", "TlsAlloc", 0, (void *)h_TlsAlloc, (void **)&o_TlsAlloc);
    tas_hook_iat("KERNEL32.dll", "TlsFree", 0, (void *)h_TlsFree, (void **)&o_TlsFree);
    tas_hook_iat("KERNEL32.dll", "GetProcAddress", 0, (void *)h_GetProcAddress, (void **)&o_GetProcAddress);
    tas_hook_iat("KERNEL32.dll", "SetEvent", 0, (void *)h_SetEvent, (void **)&o_SetEvent);
    tas_hook_iat("KERNEL32.dll", "ResetEvent", 0, (void *)h_ResetEvent, (void **)&o_ResetEvent);
    tas_hook_iat("KERNEL32.dll", "CreateFileW", 0, (void *)h_CreateFileW, (void **)&o_CreateFileW);
    tas_hook_iat("KERNEL32.dll", "CreateFileA", 0, (void *)h_CreateFileA, (void **)&o_CreateFileA);
    wchar_t buf[8];
    if (GetEnvironmentVariableW(L"LOUKE_TRACEIO", buf, 8)) {
        tas_hook_iat("KERNEL32.dll", "ReadFile", 0, (void *)h_ReadFile, (void **)&o_ReadFile);
        g_trace = 1;
        logf_("I/O trace enabled");
    }
}

// ---------------------------------------------------------------- savestates
// Memory and the game thread's context are handled by savestate.c; each state also keeps the
// frame, the virtual clock and the movie up to that frame. Loading (Hourglass semantics):
//   recording (read-write): the movie becomes the state's movie, +1 rerecord
//   playback (read-only):   the movie is kept and plays on from the state's frame
typedef struct { int used; uint32_t frame; int64_t spinTicks; FrameInput *movie; LONG threads;
                 SavedThread *th; LThread *lt; int nlt; uint32_t createSeq;
                 TlsVals gameVals, initVals; } StateInfo;
static StateInfo g_states[TAS_STATE_SLOTS];
static int g_loadedSlot = -1;
static HANDLE g_qThreads[MAX_THREADS];
static int g_qCount;

static void resume_background(void) {
    for (int i = 0; i < g_qCount; i++) { ResumeThread(g_qThreads[i]); CloseHandle(g_qThreads[i]); }
    g_qCount = 0;
}
static void unquiesce(void) {
    resume_background();
    if (g_parkReq) { g_parkReq = 0; SetEvent(g_unparkEv); }
}

// Every real game thread at the save: idle, or parked in turn_wait (stack + registers kept).
static void free_threads(StateInfo *s) {
    if (s->th) for (int i = 0; i < MAX_THREADS; i++) free(s->th[i].stack);
    free(s->th);
    free(s->lt);
    s->th = NULL;
    s->lt = NULL;
}
static int save_threads(StateInfo *s, int slot) {
    free_threads(s);
    s->th = calloc(MAX_THREADS, sizeof(SavedThread));
    s->lt = malloc(sizeof(LThread) * (size_t)(g_nlt ? g_nlt : 1));
    if (!s->th || !s->lt) return 0;
    memcpy(s->lt, g_lt, sizeof(LThread) * (size_t)g_nlt);
    s->nlt = g_nlt;
    s->createSeq = g_createSeq;
    s->threads = thread_slots();
    for (LONG i = 0; i < s->threads; i++) {
        ThreadSlot *t = &g_threads[i];
        SavedThread *v = &s->th[i];
        if (t->tid == g_gameThread) { v->kind = ST_MAIN; continue; }
        if (t->state == TS_IDLE || t->state == TS_DEAD || !t->tid) { v->kind = ST_IDLE; continue; }
        uintptr_t esp = (uintptr_t)t->parkJb[2] - 256;
        if (t->state == TS_WAITING && t->polling && t->privTop && esp < t->stackBase && t->stackBase - esp < (16u << 20)) {
            v->stackSize = (uint32_t)(t->stackBase - esp);
            v->stack = malloc(v->stackSize);
            if (v->stack) {
                memcpy(v->stack, (void *)esp, v->stackSize);
                v->kind = ST_PARKED;
                v->esp = esp;
                v->lt = t->lt; v->order = t->order; v->parkErr = t->parkErr;
                v->yielded = t->yielded; v->hasDeadline = t->hasDeadline; v->vdeadline = t->vdeadline;
                v->polls = t->polls; v->qframe = t->qframe; v->queries = t->queries;
                v->wkind = t->wkind; v->wall = t->wall; v->walert = t->walert; v->wn = t->wn; v->wcs = t->wcs;
                memcpy(v->wh, t->wh, sizeof v->wh);
                memcpy(v->wtype, t->wtype, sizeof v->wtype);
                memcpy(v->jb, t->parkJb, sizeof v->jb);
                memcpy(v->exitJb, t->exitJb, sizeof v->exitJb);
                v->seh = t->parkSeh;
                v->exitSeh = t->exitSeh;
                v->exitJbValid = t->exitJbValid;
                v->vals = t->vals;
                if (t->tlsBlock && t->tlsSize <= sizeof v->tls) { v->tlsSize = t->tlsSize; memcpy(v->tls, t->tlsBlock, t->tlsSize); }
                continue;
            }
        }
        v->kind = ST_OTHER;
        logf_("WARNING: state %d: thread %lu (start %p) is not at a safe point; it is not saved", slot + 1, t->tid, t->start);
    }
    return 1;
}
// Every real game thread goes back to what it did at the save (threads made since then go idle).
static void load_threads(StateInfo *s, int slot) {
    LONG n = thread_slots();
    int pending[MAX_THREADS], np = 0;
    for (LONG i = 0; i < n; i++) {
        ThreadSlot *t = &g_threads[i];
        if (!t->tid || t->tid == g_gameThread || t->state == TS_DEAD) continue;
        const SavedThread *v = i < s->threads ? &s->th[i] : &g_idleThread;
        if (v->kind == ST_OTHER || v->kind == ST_MAIN) {
            logf_("WARNING: state %d: thread %lu (start %p) was not saved and goes on as it is", slot + 1, t->tid, t->start);
            continue;
        }
        if (!t->privTop || !((t->state == TS_WAITING && t->polling) || t->state == TS_IDLE)) {
            logf_("WARNING: state %d: thread %lu (start %p) is not at a safe point and goes on as it is", slot + 1, t->tid, t->start);
            continue;
        }
        t->restored = 0;
        t->restore = v;
        pending[np++] = (int)i;
    }
    EnterCriticalSection(&g_ltLock);
    memcpy(g_lt, s->lt, sizeof(LThread) * (size_t)s->nlt);
    g_nlt = s->nlt;
    g_createSeq = s->createSeq;
    LeaveCriticalSection(&g_ltLock);
    resume_background();               // the window thread stays parked
    for (int k = 0; k < np; k++) SetEvent(g_threads[pending[k]].turnEv);
    int64_t start = real_qpc();
    for (int k = 0; k < np; k++) {
        ThreadSlot *t = &g_threads[pending[k]];
        while (!t->restored) {
            o_WaitForSingleObject(g_turnDone, 20);
            if (real_qpc() - start > g_realFreq * 3) {
                logf_("WARNING: state %d: thread %lu did not go back to its saved state", slot + 1, t->tid);
                break;
            }
        }
    }
}

// Bring every other game thread to a safe point: background threads idle in a wait and suspended,
// the window thread parked in its Sleep.
static int quiesce(void) {
    for (int attempt = 0; attempt < 5; attempt++) {
        settle_background();
        if (g_initThread && g_initThread != g_gameThread) {
            ResetEvent(g_parkedEv);
            g_parkReq = 1;
            if (WaitForSingleObject(g_parkedEv, 2000) != WAIT_OBJECT_0) {
                unquiesce();
                logf_("savestate: the window thread did not reach a safe point");
                return 0;
            }
        }
        LONG n = g_threadCount;
        if (n > MAX_THREADS) n = MAX_THREADS;
        int busy = g_asyncOutstanding != 0;
        for (LONG i = 0; i < n; i++) {
            ThreadSlot *t = &g_threads[i];
            if (!t->tid || t->tid == g_gameThread || t->state == TS_DEAD || t->state == TS_FREE) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, t->tid);
            if (!h) continue;
            if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
            g_qThreads[g_qCount++] = h;
            if (t->state == TS_RUNNING) busy = 1;
        }
        if (!busy) return 1;
        unquiesce();
    }
    logf_("savestate: background threads did not become idle");
    return 0;
}

static void state_save(int slot) {
    if (!tas_is_game_thread() || !quiesce()) return;
    int r = ss_save(slot);
    if (r == 1 && (!tas_obj_save(slot) || !d3d9_state_save(slot) || !handles_save(slot))) {
        g_states[slot].used = 0;
        logf_("ERROR: state %d: saving Direct3D / DirectSound objects failed", slot + 1);
    } else if (r == 1) {
        StateInfo *s = &g_states[slot];
        FrameInput *m = realloc(s->movie, (size_t)(g_frame ? g_frame : 1) * sizeof(FrameInput));
        if (m) {
            uint32_t n = g_frame < g_movieLen ? g_frame : g_movieLen;
            memcpy(m, g_movie, (size_t)n * sizeof(FrameInput));
            memset(m + n, 0, (size_t)(g_frame - n) * sizeof(FrameInput));
            s->movie = m;
            s->frame = g_frame;
            s->spinTicks = g_spinTicks;
            tls_record(&s->gameVals);
            s->initVals = g_initVals;
            s->used = save_threads(s, slot);
            if (!s->used) logf_("ERROR: state %d: out of memory", slot + 1);
            logf_("state %d saved at frame %u (%ld game threads created so far, %ld TLS / %ld FLS slots)",
                  slot + 1, g_frame, s->threads, g_ntls, g_nfls);
        } else {
            s->used = 0;
            logf_("ERROR: state %d: out of memory", slot + 1);
        }
    } else if (r == 2) {
        tls_apply(&g_states[g_loadedSlot].gameVals);
        logf_("state %d loaded: frame %u, %s, rerecords %u", g_loadedSlot + 1, g_frame,
              g_mode == MODE_PLAY ? "playback" : "recording", g_rerecords);
    }
    unquiesce();
    update_title(1);
    publish_status();
}

static void state_load(int slot) {
    if (!tas_is_game_thread()) return;
    StateInfo *s = &g_states[slot];
    if (!s->used || !ss_slot_used(slot)) { logf_("state %d is empty", slot + 1); return; }
    if (!tas_obj_slot_valid(slot) || !d3d9_state_valid(slot)) {
        logf_("state %d can no longer be loaded (Direct3D device was reset)", slot + 1);
        return;
    }
    if (!quiesce()) return;
    if (!ss_load_check(slot)) { unquiesce(); logf_("state %d cannot be loaded now", slot + 1); return; }
    if (g_mode == MODE_RECORD) {
        movie_reserve(s->frame + 1);
        memcpy(g_movie, s->movie, (size_t)s->frame * sizeof(FrameInput));
        g_movieLen = s->frame;
        g_rerecords++;
        g_dirty = 1;
    } else if (s->frame > g_movieLen || memcmp(g_movie, s->movie, (size_t)s->frame * sizeof(FrameInput))) {
        logf_("WARNING: state %d is not on the movie being played", slot + 1);
    }
    load_threads(s, slot);
    g_initWant = s->initVals;
    g_initRebind = 1;
    g_frame = s->frame;
    g_spinTicks = s->spinTicks;
    g_queriesThisFrame = 0;
    g_nextDeadline = 0;
    g_titleFrame = 0xFFFFFFFF;
    g_loadedSlot = slot;
    d3d9_state_load(slot);       // device state first: it rebinds what the state had bound
    tas_obj_load(slot);
    handles_load(slot);
    ss_load_commit(slot);        // continues in the state_save that made the state
}

// Requests are handled one at a time; a load never returns here.
static uint32_t state_frame(int slot) { return g_states[slot].used ? g_states[slot].frame : TAS_SLOT_EMPTY; }

static void states_service(void) {
    if (g_reqSave >= 0) { int s = g_reqSave; g_reqSave = -1; state_save(s); }
    if (g_reqLoad >= 0) { int l = g_reqLoad; g_reqLoad = -1; state_load(l); }
}

// Development test (LOUKE_STATETEST="s1@600,l1@900"): save / load a slot at a frame, once each.
typedef struct { int load, slot, done; uint32_t frame; } TestEvent;
static TestEvent g_test[32];
static int g_ntest;
static void parse_state_test(void) {
    char buf[512];
    if (!GetEnvironmentVariableA("LOUKE_STATETEST", buf, sizeof buf)) return;
    for (char *t = strtok(buf, ","); t && g_ntest < 32; t = strtok(NULL, ",")) {
        TestEvent *e = &g_test[g_ntest];
        unsigned slot, frame;
        if ((t[0] != 's' && t[0] != 'l') || sscanf(t + 1, "%u@%u", &slot, &frame) != 2 || slot < 1 || slot > TAS_STATE_SLOTS) continue;
        e->load = t[0] == 'l';
        e->slot = (int)slot - 1;
        e->frame = frame;
        g_ntest++;
    }
    logf_("state test: %d events", g_ntest);
}
static void state_test_events(void) {
    for (int i = 0; i < g_ntest; i++) {
        if (g_test[i].done || g_test[i].frame != g_frame) continue;
        g_test[i].done = 1;
        if (g_test[i].load) g_reqLoad = g_test[i].slot; else g_reqSave = g_test[i].slot;
        return;
    }
}

// ---------------------------------------------------------------- frame boundary
static void select_input(void) {
    if (g_mode == MODE_PLAY && g_frame >= g_movieLen) switch_to_record("movie end", 0);
    if (g_mode == MODE_PLAY) {
        unpack_keys(&g_movie[g_frame], g_frameKeys);
        return;
    }
    read_real_keys(g_frameKeys);
    movie_reserve(g_frame + 1);
    if (g_movieLen > g_frame) g_movieLen = g_frame;
    pack_keys(g_frameKeys, &g_movie[g_frame]);
    g_movieLen = g_frame + 1;
    g_dirty = 1;
}

static void pause_loop(void) {
    if (g_dirty) movie_save();
    update_title(1);
    dsound_pause(1);
    for (;;) {
        if (g_profile && g_profile->pause_tick) g_profile->pause_tick();
        publish_status();
        Hotkeys h = poll_hotkeys();
        if (h.savePressed) save_requested();
        if (h.exportStart) export_begin(0);
        if (h.exportStop) export_end();
        if (h.takeoverPressed && g_mode == MODE_PLAY) switch_to_record("takeover", 1);
        if (h.pausePressed) { g_paused = 0; g_advHoldStart = 0; g_nextDeadline = 0; break; }
        if (h.advPressed) { g_advHoldStart = GetTickCount(); g_nextDeadline = 0; break; }
        if (h.advDown && g_advHoldStart && GetTickCount() - g_advHoldStart > 350) { pace_60(); break; }
        if (!h.advDown) g_advHoldStart = 0;
        states_service();
        if (!g_paused) break;          // continued here by loading, while running, a state saved when paused
        Sleep(2);
    }
    dsound_pause(0);
}

int tas_skip_present(void) { return g_ff && !avdump_active() && (g_frame % FFWD_PRESENT_EVERY) != 0; }

// Debug (LOUKE_DATAHASH=<file>, LOUKE_DATADUMP=<dir>, LOUKE_DATADUMP_RANGE=a-b): per-frame hash of
// the executable's writable sections, and raw copies of them for a range of frames, to find where
// two runs of a movie start to differ.
static void data_debug(void) {
    static int init;
    static FILE *hashf;
    static wchar_t dumpDir[MAX_PATH];
    static unsigned lo, hi;
    static uint8_t *sec[8];
    static size_t secLen[8];
    static int nsec;
    if (!init) {
        init = 1;
        wchar_t b[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOUKE_DATAHASH", b, MAX_PATH)) hashf = _wfopen(b, L"w");
        if (GetEnvironmentVariableW(L"LOUKE_DATADUMP", dumpDir, MAX_PATH) &&
            GetEnvironmentVariableW(L"LOUKE_DATADUMP_RANGE", b, MAX_PATH)) swscanf(b, L"%u-%u", &lo, &hi);
        uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
        IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections && nsec < 8; i++)
            if (sh[i].Characteristics & IMAGE_SCN_MEM_WRITE) { sec[nsec] = base + sh[i].VirtualAddress; secLen[nsec++] = sh[i].Misc.VirtualSize; }
    }
    if (hashf) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (int i = 0; i < nsec; i++) for (size_t k = 0; k < secLen[i]; k++) h = (h ^ sec[i][k]) * 0x100000001b3ULL;
        fprintf(hashf, "%u %016llx\n", g_frame, (unsigned long long)h);
        if (g_frame % 60 == 0) fflush(hashf);
    }
    if (dumpDir[0] && g_frame >= lo && g_frame <= hi) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\data_%06u.bin", dumpDir, g_frame);
        FILE *f = _wfopen(path, L"wb");
        if (f) { for (int i = 0; i < nsec; i++) fwrite(sec[i], 1, secLen[i], f); fclose(f); }
    }
}

void tas_frame_end(void) {
    static int64_t profLast;
    g_inSettle = 1;
    settle_background();   // finish all background work started in this frame
    g_inSettle = 0;
    if (g_frameProf) { int64_t now = real_qpc(); if (profLast) frame_prof(now - profLast); profLast = now; }
    data_debug();
    if (g_trace) {
        static int64_t last;
        int64_t now = real_qpc();
        logf_("FRAME end, real %.2f ms, sync %.2f ms",
              last ? (double)(now - last) * 1000.0 / (double)g_realFreq : 0.0, g_lastSyncMs);
        last = now;
    }
    g_frame++;
    g_queriesThisFrame = 0;
    g_pollsThisFrame = 0;
    int exporting = avdump_active();   // the export gets the frame: its picture (taken at Present) and its sound
    int64_t exportNow = vtime_base();
    if (exporting) avdump_frame_begin(exportNow - g_exportLastV);
    dsound_frame();        // the sound of the frame that ended goes out, before the mixer refills it
    if (exporting) { avdump_frame(); g_exportLastV = exportNow; }
    g_inSettle = 2;
    settle_background();   // waits whose virtual timeout expired with this frame go on now
    g_inSettle = 0;
    d3d9_frame();
    tas_obj_gc_step(32);

    Hotkeys h = poll_hotkeys();
    g_ff = h.ffDown && !g_paused;
    if (h.savePressed) save_requested();
    if (h.exportStart) export_begin(0);
    if (h.exportStop) export_end();
    if (h.takeoverPressed && g_mode == MODE_PLAY) switch_to_record("takeover", 1);
    if (h.pausePressed || h.advPressed) { g_paused = 1; g_advHoldStart = 0; }
    state_test_events();
    states_service();              // a load continues in the states_service call that saved the state

    if (g_mode == MODE_PLAY && g_frame >= g_movieLen) {
        logf_("movie end reached");
        if (g_exportFast) export_end();    // the whole movie is exported
        switch_to_record("movie end", 0);
        g_paused = 1;
        g_advHoldStart = 0;
    }

    if (g_paused) pause_loop();
    else if (!g_ff && (!tas_export_fast() || dsound_realtime_media())) pace_60();   // a movie plays in real time
    else g_nextDeadline = 0;

    select_input();
    if (g_mode == MODE_RECORD && g_dirty && g_frame - g_lastSaveFrame >= AUTOSAVE_FRAMES) movie_save();
    update_title(0);
    publish_status();
}

// ---------------------------------------------------------------- crash diagnostics
// Logs access violations (module + offset, accessed address, likely return addresses on the stack).
static const char *addr_module(uintptr_t a, uintptr_t *off) {
    static char name[MAX_PATH];
    HMODULE m = NULL;
    *off = a;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)a, &m)) return "?";
    GetModuleFileNameA(m, name, sizeof name);
    *off = a - (uintptr_t)m;
    const char *b = strrchr(name, '\\');
    return b ? b + 1 : name;
}
static LONG CALLBACK crash_log(EXCEPTION_POINTERS *ep) {
    static volatile LONG count;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&count) > 8) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress, off;
    const char *mod = addr_module(ip, &off);
    logf_("EXCEPTION %08lX at %p (%s+%lX), %s %p, thread %lu", code, (void *)ip, mod, (unsigned long)off,
          ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
          (void *)ep->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
    CONTEXT *c = ep->ContextRecord;
    logf_("  eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX",
          c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    uintptr_t *sp = (uintptr_t *)c->Esp;
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; i++) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(&sp[i], &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) break;
        uintptr_t v = sp[i];
        if (!VirtualQuery((void *)v, &m, sizeof m) || m.State != MEM_COMMIT || m.Type != MEM_IMAGE ||
            !(m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
        mod = addr_module(v, &off);
        logf_("  [esp+%03X] %p %s+%lX", i * 4, (void *)v, mod, (unsigned long)off);
        shown++;
    }
    if (g_log) fflush(g_log);
    return EXCEPTION_CONTINUE_SEARCH;
}

// The game ending itself: log where it decided to.
typedef void (WINAPI *ExitProcess_t)(UINT);
typedef BOOL (WINAPI *TerminateProcess_t)(HANDLE, UINT);
static ExitProcess_t o_ExitProcess;
static TerminateProcess_t o_TerminateProcess;
static void log_caller(const char *what, UINT code, void *ret) {
    uintptr_t off;
    const char *mod = addr_module((uintptr_t)ret, &off);
    logf_("game called %s(%d) from %p (%s+%lX)", what, (int)code, ret, mod, (unsigned long)off);
    uintptr_t *sp = (uintptr_t *)__builtin_frame_address(0);
    int shown = 0;
    for (int i = 0; i < 256 && shown < 12; i++) {
        uintptr_t v = sp[i];
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)v, &m, sizeof m) || m.State != MEM_COMMIT || m.Type != MEM_IMAGE ||
            !(m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
        mod = addr_module(v, &off);
        logf_("  [+%03X] %p %s+%lX", i * 4, (void *)v, mod, (unsigned long)off);
        shown++;
    }
    if (g_log) fflush(g_log);
}
static void WINAPI h_ExitProcess(UINT code) {
    log_caller("ExitProcess", code, __builtin_return_address(0));
    avdump_finish(30000);              // a video being exported is finished, or it could not be played
    o_ExitProcess(code);
}
static BOOL WINAPI h_TerminateProcess(HANDLE p, UINT code) {
    if (GetProcessId(p) == GetCurrentProcessId()) {
        log_caller("TerminateProcess", code, __builtin_return_address(0));
        avdump_finish(30000);
    }
    return o_TerminateProcess(p, code);
}

// ---------------------------------------------------------------- init
static int init(void) {
    wchar_t buf[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOUKE_LOG", buf, MAX_PATH)) g_log = _wfopen(buf, L"w");
    g_diag = GetEnvironmentVariableW(L"LOUKE_DIAG", buf, MAX_PATH) != 0;
    if (GetEnvironmentVariableW(L"LOUKE_SCHEDLOG", buf, MAX_PATH)) g_schedLog = _wtoi(buf) > 1 ? 2 : 1;
    g_frameProf = GetEnvironmentVariableW(L"LOUKE_FRAMEPROF", buf, MAX_PATH) != 0;
    init_dik_names();
    if (!GetEnvironmentVariableW(L"LOUKE_MODE", buf, MAX_PATH)) { logf_("ERROR: LOUKE_MODE not set"); return 0; }
    g_mode = !wcscmp(buf, L"play") ? MODE_PLAY : !wcscmp(buf, L"record") ? MODE_RECORD : 0;
    if (!g_mode) { logf_("ERROR: bad mode"); return 0; }
    if (!GetEnvironmentVariableW(L"LOUKE_MOVIE", g_moviePath, MAX_PATH)) { logf_("ERROR: LOUKE_MOVIE not set"); return 0; }
    logf_("Louke core start, mode=%s", g_mode == MODE_PLAY ? "play" : "record");
    if (GetEnvironmentVariableW(L"LOUKE_SHM", buf, MAX_PATH)) {
        HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, buf);
        if (m) g_sh = (TasShared *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TasShared));
        if (g_sh && (g_sh->magic != TAS_SHARED_MAGIC || g_sh->version != TAS_SHARED_VERSION)) g_sh = NULL;
        if (g_sh) for (int c = 0; c < CMD_COUNT; c++) g_cmdSeen[c] = g_sh->cmdSeq[c];
        logf_("control window link %s", g_sh ? "ok" : "FAILED");
        if (g_sh && g_sh->exportMovie && g_sh->exportPath[0]) g_exportPending = 1;   // starts with the game thread
    }

    HMODULE exe = GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    g_profile = games_find(exe, nt);
    logf_("game profile: %s (exe timestamp %08lX)", g_profile ? g_profile->id : "none (generic core)", nt->FileHeader.TimeDateStamp);
    if (g_sh && g_profile) {
        wcsncpy(g_sh->gameTitle, g_profile->title, 47);
        wcsncpy(g_sh->gameTitleEn, g_profile->titleEn, 47);
    }

    if (g_mode == MODE_PLAY && !movie_load()) return 0;

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_realFreq = f.QuadPart;
    g_initThread = GetCurrentThreadId();

    tas_hook_iat("KERNEL32.dll", "QueryPerformanceCounter", 0, (void *)h_QPC, (void **)&o_QPC);
    tas_hook_iat("KERNEL32.dll", "QueryPerformanceFrequency", 0, (void *)h_QPF, (void **)&o_QPF);
    tas_hook_iat("KERNEL32.dll", "GetTickCount", 0, (void *)h_GetTickCount, (void **)&o_GetTickCount);
    tas_hook_iat("KERNEL32.dll", "GetSystemTimeAsFileTime", 0, (void *)h_GetSystemTimeAsFileTime, (void **)&o_GetSystemTimeAsFileTime);
    tas_hook_iat("KERNEL32.dll", "GetSystemTime", 0, (void *)h_GetSystemTime, (void **)&o_GetSystemTime);
    tas_hook_iat("WINMM.dll", "timeGetTime", 0, (void *)h_timeGetTime, (void **)&o_timeGetTime);
    tas_hook_iat("DINPUT8.dll", "DirectInput8Create", 0, (void *)h_DirectInput8Create, (void **)&o_DirectInput8Create);
    static const char *xinputDlls[3] = {"XINPUT1_3.dll", "XINPUT1_4.dll", "XINPUT9_1_0.dll"};
    for (int i = 0; i < 3; i++) {
        if (!tas_hook_iat(xinputDlls[i], NULL, 2, (void *)h_XInputGetState, (void **)&o_XInputGetState[i]))
            tas_hook_iat(xinputDlls[i], "XInputGetState", 0, (void *)h_XInputGetState, (void **)&o_XInputGetState[i]);
    }
    handles_install();
    install_thread_sync();
    tas_obj_init();
    if (!ss_install(exe)) logf_("WARNING: savestates unavailable");
    dsound_install();
    parse_state_test();
    if (!d3d9_install()) logf_("WARNING: the game does not import Direct3DCreate9; no frame boundary yet");
    if (g_profile && g_profile->install && !g_profile->install(exe)) {
        logf_("ERROR: game profile '%s' could not be installed", g_profile->id);
        return 0;
    }

    g_frame = 0;
    if (g_mode == MODE_PLAY) {
        if (g_movieLen == 0) { g_mode = MODE_RECORD; logf_("empty movie: recording from frame 0"); }
        else unpack_keys(&g_movie[0], g_frameKeys);
    }
    if (g_mode == MODE_RECORD) {
        memset(g_frameKeys, 0, sizeof g_frameKeys);
        movie_reserve(1);
        pack_keys(g_frameKeys, &g_movie[0]);
        g_movieLen = 1;
        g_dirty = 1;
    }
    AddVectoredExceptionHandler(1, crash_log);
    tas_hook_iat("KERNEL32.dll", "ExitProcess", 0, (void *)h_ExitProcess, (void **)&o_ExitProcess);
    tas_hook_iat("KERNEL32.dll", "TerminateProcess", 0, (void *)h_TerminateProcess, (void **)&o_TerminateProcess);
    logf_("hooks installed, initial thread %lu", g_initThread);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        if (!init()) { if (g_log) fclose(g_log); g_log = NULL; return FALSE; }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_dirty) movie_save();
    }
    return TRUE;
}
