// Kernel handles the game creates (events, semaphores, mutexes, files, threads), kept valid for
// savestates.
//
// Savestates restore the game's memory, which holds handle values. A handle the game closes
// while a state still refers to it is not really closed, so the value stays valid (and is not
// reused for another object). A state records the state of each handle the game held: event
// signaled or not, semaphore count, file position; loading sets these back. Handles the game
// opened after the save are unknown to the restored game and are closed (unless another state
// still refers to them).
#include "tas.h"
#include <stdlib.h>
#include <string.h>

enum { HT_EVENT = 1, HT_SEMAPHORE, HT_MUTEX, HT_FILE, HT_THREAD };
typedef struct { HANDLE h; uint8_t type, zombie; } Entry;
typedef struct { HANDLE h; uint8_t type; int64_t value; } SavedHandle;
typedef struct { int used; SavedHandle *e; int n; } HandleSlot;

static CRITICAL_SECTION g_cs;
static Entry *g_e;
static int g_n, g_cap;
static HandleSlot g_hs[TAS_STATE_SLOTS];

typedef LONG (WINAPI *NtQuery_t)(HANDLE, int, void *, ULONG, ULONG *);
static NtQuery_t NtQueryEvent, NtQuerySemaphore, NtQueryMutant;

static int find(HANDLE h) {               // caller holds g_cs
    for (int i = g_n - 1; i >= 0; i--) if (g_e[i].h == h) return i;
    return -1;
}
static void track(HANDLE h, int type) {
    if (!h || h == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_cs);
    int i = find(h);
    if (i >= 0) {                          // value reused after a real close we did not see
        g_e[i].type = (uint8_t)type;
        g_e[i].zombie = 0;
    } else {
        if (g_n == g_cap) {
            int cap = g_cap ? g_cap * 2 : 256;
            Entry *n = realloc(g_e, (size_t)cap * sizeof(Entry));
            if (!n) { LeaveCriticalSection(&g_cs); return; }
            g_e = n;
            g_cap = cap;
        }
        g_e[g_n].h = h;
        g_e[g_n].type = (uint8_t)type;
        g_e[g_n].zombie = 0;
        g_n++;
    }
    LeaveCriticalSection(&g_cs);
}
void handles_track_thread(HANDLE h) { track(h, HT_THREAD); }

static int cmp_saved(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const SavedHandle *)a)->h, y = (uintptr_t)((const SavedHandle *)b)->h;
    return x < y ? -1 : x > y;
}
static SavedHandle *slot_find(const HandleSlot *s, HANDLE h) {
    SavedHandle key;
    key.h = h;
    return s->used ? bsearch(&key, s->e, (size_t)s->n, sizeof(SavedHandle), cmp_saved) : NULL;
}
static int referenced(HANDLE h) {
    for (int k = 0; k < TAS_STATE_SLOTS; k++) if (slot_find(&g_hs[k], h)) return 1;
    return 0;
}

// close zombies no state refers to any more
static void collect(void) {
    for (int i = 0; i < g_n; ) {
        if (g_e[i].zombie && !referenced(g_e[i].h)) {
            CloseHandle(g_e[i].h);
            g_e[i] = g_e[--g_n];
        } else i++;
    }
}

typedef BOOL (WINAPI *CloseHandle_t)(HANDLE);
static CloseHandle_t o_CloseHandle;
static BOOL WINAPI h_CloseHandle(HANDLE h) {
    tas_thread_handle_closed(h);
    EnterCriticalSection(&g_cs);
    int i = find(h);
    if (i >= 0) {
        if (g_e[i].zombie) {               // closed twice: what the game would have seen
            LeaveCriticalSection(&g_cs);
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        if (referenced(h)) {
            g_e[i].zombie = 1;
            LeaveCriticalSection(&g_cs);
            return TRUE;
        }
        g_e[i] = g_e[--g_n];
    }
    LeaveCriticalSection(&g_cs);
    return o_CloseHandle(h);
}

typedef HANDLE (WINAPI *CreateEventA_t)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
typedef HANDLE (WINAPI *CreateEventW_t)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
typedef HANDLE (WINAPI *CreateMutexA_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
typedef HANDLE (WINAPI *CreateMutexW_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
typedef HANDLE (WINAPI *CreateSemA_t)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR);
typedef HANDLE (WINAPI *CreateSemW_t)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCWSTR);
static CreateEventA_t o_CreateEventA;
static CreateEventW_t o_CreateEventW;
static CreateMutexA_t o_CreateMutexA;
static CreateMutexW_t o_CreateMutexW;
static CreateSemA_t o_CreateSemaphoreA;
static CreateSemW_t o_CreateSemaphoreW;
static HANDLE WINAPI h_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL m, BOOL st, LPCSTR n) {
    HANDLE h = o_CreateEventA(sa, m, st, n); track(h, HT_EVENT); return h;
}
static HANDLE WINAPI h_CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL m, BOOL st, LPCWSTR n) {
    HANDLE h = o_CreateEventW(sa, m, st, n); track(h, HT_EVENT); return h;
}
static HANDLE WINAPI h_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR n) {
    HANDLE h = o_CreateMutexA(sa, own, n); track(h, HT_MUTEX); return h;
}
static HANDLE WINAPI h_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCWSTR n) {
    HANDLE h = o_CreateMutexW(sa, own, n); track(h, HT_MUTEX); return h;
}
static HANDLE WINAPI h_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG c, LONG m, LPCSTR n) {
    HANDLE h = o_CreateSemaphoreA(sa, c, m, n); track(h, HT_SEMAPHORE); return h;
}
static HANDLE WINAPI h_CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG c, LONG m, LPCWSTR n) {
    HANDLE h = o_CreateSemaphoreW(sa, c, m, n); track(h, HT_SEMAPHORE); return h;
}
void handles_track_file(HANDLE h) { track(h, HT_FILE); }

// ---------------------------------------------------------------- object state
static int64_t query_state(HANDLE h, int type) {
    LONG info[2] = {0, 0};
    switch (type) {
    case HT_EVENT:                         // EventBasicInformation: type, state
        if (NtQueryEvent && NtQueryEvent(h, 0, info, sizeof info, NULL) >= 0) return info[1];
        return -1;
    case HT_SEMAPHORE:                     // SemaphoreBasicInformation: current, maximum
        if (NtQuerySemaphore && NtQuerySemaphore(h, 0, info, sizeof info, NULL) >= 0) return info[0];
        return -1;
    case HT_MUTEX:                         // MutantBasicInformation: count, owned by caller, abandoned
        if (NtQueryMutant && NtQueryMutant(h, 0, info, sizeof info, NULL) >= 0) return info[0];
        return -1;
    case HT_FILE: {
        LARGE_INTEGER zero = {{0, 0}}, pos;
        if (GetFileType(h) == FILE_TYPE_DISK && SetFilePointerEx(h, zero, &pos, FILE_CURRENT)) return pos.QuadPart;
        return -1;
    }
    default:
        return 0;
    }
}

// Kind of a handle the game created (0: unknown), and whether a wait on it would be satisfied now
// (1 yes, 0 no, -1 unknown). Neither changes anything (no event is reset, no count taken).
int handles_type(HANDLE h) {
    EnterCriticalSection(&g_cs);
    int i = find(h);
    int t = i >= 0 ? g_e[i].type : 0;
    LeaveCriticalSection(&g_cs);
    return t;
}
int handles_ready(HANDLE h, int type) {
    int64_t v;
    switch (type) {
    case HT_EVENT:     v = query_state(h, HT_EVENT); return v < 0 ? -1 : v != 0;
    case HT_SEMAPHORE: v = query_state(h, HT_SEMAPHORE); return v < 0 ? -1 : v > 0;
    case HT_MUTEX:     v = query_state(h, HT_MUTEX); return v > 0 ? 1 : -1;   // owned: maybe by the waiter
    case HT_THREAD:    return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;    // not consumed by a wait
    default:           return -1;
    }
}

static void set_state(HANDLE h, int type, int64_t want) {
    if (want < 0) return;
    int64_t cur = query_state(h, type);
    if (cur < 0 || cur == want) return;
    switch (type) {
    case HT_EVENT:
        if (want) SetEvent(h); else ResetEvent(h);
        break;
    case HT_SEMAPHORE:
        if (want > cur) ReleaseSemaphore(h, (LONG)(want - cur), NULL);
        else for (int64_t k = cur; k > want; k--) WaitForSingleObject(h, 0);
        break;
    case HT_MUTEX:                         // ownership belongs to threads; cannot be set from here
        tas_log("savestate: mutex %p was %s at the save and is %s now", h,
                want > 0 ? "free" : "owned", cur > 0 ? "free" : "owned");
        break;
    case HT_FILE: {
        LARGE_INTEGER p;
        p.QuadPart = want;
        SetFilePointerEx(h, p, NULL, FILE_BEGIN);
        break;
    }
    }
}

int handles_save(int slot) {
    HandleSlot *s = &g_hs[slot];
    EnterCriticalSection(&g_cs);
    free(s->e);
    memset(s, 0, sizeof *s);
    s->e = malloc((size_t)(g_n ? g_n : 1) * sizeof(SavedHandle));
    if (!s->e) { collect(); LeaveCriticalSection(&g_cs); return 0; }
    for (int i = 0; i < g_n; i++) {
        if (g_e[i].zombie) continue;
        SavedHandle *e = &s->e[s->n++];
        e->h = g_e[i].h;
        e->type = g_e[i].type;
        e->value = query_state(e->h, e->type);
    }
    qsort(s->e, (size_t)s->n, sizeof(SavedHandle), cmp_saved);
    s->used = 1;
    collect();                             // zombies only the overwritten state needed
    tas_log("savestate %d: %d handles held by the game, %d kept in total", slot + 1, s->n, g_n);
    LeaveCriticalSection(&g_cs);
    return 1;
}

void handles_load(int slot) {
    HandleSlot *s = &g_hs[slot];
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_n; i++) {
        SavedHandle *e = slot_find(s, g_e[i].h);
        if (e && e->type == g_e[i].type) {
            g_e[i].zombie = 0;             // open again, as far as the game knows
            set_state(e->h, e->type, e->value);
        } else {
            g_e[i].zombie = 1;             // opened after the save: unknown to the restored game
        }
    }
    collect();
    LeaveCriticalSection(&g_cs);
}

void handles_install(void) {
    InitializeCriticalSection(&g_cs);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    NtQueryEvent = (NtQuery_t)(void *)GetProcAddress(nt, "NtQueryEvent");
    NtQuerySemaphore = (NtQuery_t)(void *)GetProcAddress(nt, "NtQuerySemaphore");
    NtQueryMutant = (NtQuery_t)(void *)GetProcAddress(nt, "NtQueryMutant");
    tas_hook_iat("KERNEL32.dll", "CloseHandle", 0, (void *)h_CloseHandle, (void **)&o_CloseHandle);
    tas_hook_iat("KERNEL32.dll", "CreateEventA", 0, (void *)h_CreateEventA, (void **)&o_CreateEventA);
    tas_hook_iat("KERNEL32.dll", "CreateEventW", 0, (void *)h_CreateEventW, (void **)&o_CreateEventW);
    tas_hook_iat("KERNEL32.dll", "CreateMutexA", 0, (void *)h_CreateMutexA, (void **)&o_CreateMutexA);
    tas_hook_iat("KERNEL32.dll", "CreateMutexW", 0, (void *)h_CreateMutexW, (void **)&o_CreateMutexW);
    tas_hook_iat("KERNEL32.dll", "CreateSemaphoreA", 0, (void *)h_CreateSemaphoreA, (void **)&o_CreateSemaphoreA);
    tas_hook_iat("KERNEL32.dll", "CreateSemaphoreW", 0, (void *)h_CreateSemaphoreW, (void **)&o_CreateSemaphoreW);
}
