// In-memory savestates of the memory a game owns (Hourglass-style).
//
// Saved:
//   - writable sections of the game executable and the game thread's implicit-TLS block
//   - every reservation of the heaps the executable creates (HeapCreate through its imports):
//     segments and large blocks, found from the blocks HeapAlloc / HeapReAlloc hand out
//   - reservations the executable makes itself (VirtualAlloc through its imports)
//   - the game thread's stack above the save point, its callee-saved registers, SEH chain head
//     and FPU / SSE control words
// Not saved: memory of system DLLs (Direct3D, sound, input, ntdll). Their objects stay as they are.
//
// Each state lives in a pagefile-backed section, outside the game's (2 GB) address space.
// Loading switches the game thread to a private stack, makes the tracked reservations and their
// commit state match the state (reservations made after the save are released), copies the data
// back and continues the game thread inside the ss_save call that made the state.
//
// The caller (core) brings all other game threads to a safe point first; see quiesce().
#include "tas.h"
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#define MAX_RESV    8192
#define MAX_HEAPS   32
#define VIEW_SIZE   (16u << 20)        // section window, multiple of the 64 KB granularity
#define PRIV_STACK  (1u << 20)

typedef struct { uintptr_t base, size; } Resv;
typedef struct {
    uintptr_t addr, size;
    DWORD     prot;
    uint8_t   inResv;                  // 1: part of a tracked reservation (commit state restored)
    uint8_t   hasData;                 // 0: committed but unreadable (guard / no access)
} Range;
typedef struct {
    int      used;
    HANDLE   sec;
    uint64_t bytes;
    Range   *ranges;
    int      nranges;
    Resv    *resv;
    int     *resvFirst;                // index of each reservation's first range
    int      nresv;
    HANDLE   heaps[MAX_HEAPS];
    int      nheaps;
    uintptr_t esp, seh, stackBase;
    uint16_t fpucw;
    uint32_t mxcsr;
} Slot;

// ---------------------------------------------------------------- tracking
static SRWLOCK g_lock = SRWLOCK_INIT;
static Resv    g_resv[MAX_RESV];       // sorted by base
static int     g_nresv;
static HANDLE  g_heaps[MAX_HEAPS];
static int     g_nheaps;
static int     g_overflowLogged;
static int     g_check;             // LOUKE_SSCHECK diagnostics

static uint8_t  *g_exe;
static DWORD    *g_tlsIndex;
static uint32_t  g_tlsSize, g_tlsRaw;
static const uint8_t *g_tlsTemplate;
static Slot      g_slots[TAS_STATE_SLOTS];

// shared with the assembly below
uintptr_t g_ssPointEsp, g_ssResumeEsp, g_ssPrivTop;
static uintptr_t g_privBottom, g_gameStackLimit;
static int g_saveSlot, g_restoreSlot;
static uint8_t g_release[MAX_RESV];    // load plan: current reservation is released

static int resv_find(uintptr_t a) {     // caller holds g_lock
    int lo = 0, hi = g_nresv - 1, r = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_resv[m].base <= a) { r = m; lo = m + 1; } else hi = m - 1;
    }
    return (r >= 0 && a - g_resv[r].base < g_resv[r].size) ? r : -1;
}

static uintptr_t resv_extent(uintptr_t base) {
    MEMORY_BASIC_INFORMATION m;
    uintptr_t a = base;
    while (VirtualQuery((void *)a, &m, sizeof m) && (uintptr_t)m.AllocationBase == base && m.State != MEM_FREE)
        a = (uintptr_t)m.BaseAddress + m.RegionSize;
    return a - base;
}

static void resv_insert(uintptr_t base, uintptr_t size) {   // caller holds g_lock exclusively
    if (resv_find(base) >= 0) return;
    if (g_nresv >= MAX_RESV) {
        if (!g_overflowLogged) { g_overflowLogged = 1; tas_log("ERROR: savestate reservation table full"); }
        return;
    }
    int pos = g_nresv;
    while (pos > 0 && g_resv[pos - 1].base > base) pos--;
    memmove(&g_resv[pos + 1], &g_resv[pos], (size_t)(g_nresv - pos) * sizeof(Resv));
    g_resv[pos].base = base;
    g_resv[pos].size = size;
    g_nresv++;
}

static void note_addr(const void *p) {
    uintptr_t a = (uintptr_t)p;
    AcquireSRWLockShared(&g_lock);
    int i = resv_find(a);
    ReleaseSRWLockShared(&g_lock);
    if (i >= 0) return;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m) || m.State == MEM_FREE) return;
    uintptr_t base = (uintptr_t)m.AllocationBase, size = resv_extent(base);
    if (!size) return;
    AcquireSRWLockExclusive(&g_lock);
    resv_insert(base, size);
    ReleaseSRWLockExclusive(&g_lock);
}

static int still_allocated(uintptr_t base) {
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery((void *)base, &m, sizeof m) && m.State != MEM_FREE && (uintptr_t)m.AllocationBase == base;
}

// Placeholders: address ranges a savestate needs back, reserved (no access) by us while the game
// does not use them, so that other code (Direct3D, the driver, ...) cannot take them.
// g_lock protects them as well.
#define GRAN 0x10000u
static Resv g_hold[MAX_RESV];
static int  g_nhold;

static void hold_add(uintptr_t a, uintptr_t b) {          // [a, b) is free: reserve what fits
    a = (a + GRAN - 1) & ~(uintptr_t)(GRAN - 1);
    if (a >= b || g_nhold >= MAX_RESV) return;
    if (VirtualAlloc((void *)a, b - a, MEM_RESERVE, PAGE_NOACCESS) != (void *)a) return;
    g_hold[g_nhold].base = a;
    g_hold[g_nhold].size = b - a;
    g_nhold++;
}
// Reserve the free parts of [a, b) that saved reservations of any used slot (except skip) cover.
static void hold_range(uintptr_t a, uintptr_t b, int skip) {
    for (int k = 0; k < TAS_STATE_SLOTS; k++) {
        Slot *s = &g_slots[k];
        if (!s->used || k == skip) continue;
        for (int i = 0; i < s->nresv; i++) {
            uintptr_t x = s->resv[i].base > a ? s->resv[i].base : a;
            uintptr_t y = s->resv[i].base + s->resv[i].size < b ? s->resv[i].base + s->resv[i].size : b;
            MEMORY_BASIC_INFORMATION m;
            while (x < y && VirtualQuery((void *)x, &m, sizeof m)) {
                uintptr_t e = (uintptr_t)m.BaseAddress + m.RegionSize;
                if (e > y) e = y;
                if (m.State == MEM_FREE) hold_add(x, e);
                x = e;
            }
        }
    }
}
static void hold_all(int skip) {
    for (int k = 0; k < TAS_STATE_SLOTS; k++) {
        if (!g_slots[k].used || k == skip) continue;
        for (int i = 0; i < g_slots[k].nresv; i++)
            hold_range(g_slots[k].resv[i].base, g_slots[k].resv[i].base + g_slots[k].resv[i].size, skip);
    }
}
static int hold_overlaps_slot(const Resv *h, const Slot *s) {
    for (int i = 0; i < s->nresv; i++)
        if (h->base < s->resv[i].base + s->resv[i].size && s->resv[i].base < h->base + h->size) return 1;
    return 0;
}
// Release placeholders that no slot needs any more (keep = 0) or that overlap slot s (keep = 1).
static void hold_release(const Slot *only) {
    int n = 0;
    for (int i = 0; i < g_nhold; i++) {
        int needed = 0;
        if (only) needed = !hold_overlaps_slot(&g_hold[i], only);
        else for (int k = 0; k < TAS_STATE_SLOTS && !needed; k++)
            needed = g_slots[k].used && hold_overlaps_slot(&g_hold[i], &g_slots[k]);
        if (needed) g_hold[n++] = g_hold[i];
        else VirtualFree((void *)g_hold[i].base, 0, MEM_RELEASE);
    }
    g_nhold = n;
}
static int hold_index(uintptr_t allocBase) {
    for (int i = 0; i < g_nhold; i++) if (g_hold[i].base == allocBase) return i;
    return -1;
}

static void resv_drop_if_released(uintptr_t base) {
    if (still_allocated(base)) return;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_nresv; i++) {
        if (g_resv[i].base != base) continue;
        uintptr_t end = base + g_resv[i].size;
        memmove(&g_resv[i], &g_resv[i + 1], (size_t)(g_nresv - i - 1) * sizeof(Resv));
        g_nresv--;
        hold_range(base, end, -1);
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static void prune_released(void) {
    AcquireSRWLockExclusive(&g_lock);
    int n = 0;
    for (int i = 0; i < g_nresv; i++) {
        if (still_allocated(g_resv[i].base)) g_resv[n++] = g_resv[i];
        else hold_range(g_resv[i].base, g_resv[i].base + g_resv[i].size, -1);
    }
    g_nresv = n;
    ReleaseSRWLockExclusive(&g_lock);
}

// Find every reservation of the game heaps, including ones the heap uses internally and never hands
// out blocks from (seen: one reservation outside every block returned by HeapAlloc).
static void discover_heap_memory(void) {
    for (int h = 0; h < g_nheaps; h++) {
        PROCESS_HEAP_ENTRY e;
        memset(&e, 0, sizeof e);
        if (!HeapLock(g_heaps[h])) continue;
        while (HeapWalk(g_heaps[h], &e)) {
            AcquireSRWLockShared(&g_lock);
            int found = resv_find((uintptr_t)e.lpData) >= 0;
            ReleaseSRWLockShared(&g_lock);
            if (found) continue;
            if (g_check) tas_log("savestate check: heap %p entry %p (%lu bytes, flags %04X) was untracked",
                                 g_heaps[h], e.lpData, e.cbData, e.wFlags);
            note_addr(e.lpData);
        }
        HeapUnlock(g_heaps[h]);
    }
}

// A block near the start of a reservation may be a large block (its own reservation).
static uintptr_t resv_start_of(const void *p) {
    uintptr_t a = (uintptr_t)p, base = 0;
    AcquireSRWLockShared(&g_lock);
    int i = resv_find(a);
    if (i >= 0 && a - g_resv[i].base < 0x100) base = g_resv[i].base;
    ReleaseSRWLockShared(&g_lock);
    return base;
}

static int is_game_heap(HANDLE h) {
    for (int i = 0; i < g_nheaps; i++) if (g_heaps[i] == h) return 1;
    return 0;
}

typedef HANDLE (WINAPI *HeapCreate_t)(DWORD, SIZE_T, SIZE_T);
typedef BOOL   (WINAPI *HeapDestroy_t)(HANDLE);
typedef LPVOID (WINAPI *HeapAlloc_t)(HANDLE, DWORD, SIZE_T);
typedef LPVOID (WINAPI *HeapReAlloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef BOOL   (WINAPI *HeapFree_t)(HANDLE, DWORD, LPVOID);
typedef LPVOID (WINAPI *VirtualAlloc_t)(LPVOID, SIZE_T, DWORD, DWORD);
typedef BOOL   (WINAPI *VirtualFree_t)(LPVOID, SIZE_T, DWORD);
static HeapCreate_t o_HeapCreate;
static HeapDestroy_t o_HeapDestroy;
static HeapAlloc_t o_HeapAlloc;
static HeapReAlloc_t o_HeapReAlloc;
static HeapFree_t o_HeapFree;
static VirtualAlloc_t o_VirtualAlloc;
static VirtualFree_t o_VirtualFree;

static HANDLE WINAPI h_HeapCreate(DWORD fl, SIZE_T init, SIZE_T max) {
    HANDLE h = o_HeapCreate(fl, init, max);
    if (h) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_nheaps < MAX_HEAPS) g_heaps[g_nheaps++] = h;
        ReleaseSRWLockExclusive(&g_lock);
        note_addr(h);
        tas_log("savestate: tracking heap %p", h);
    }
    return h;
}
static BOOL WINAPI h_HeapDestroy(HANDLE h) {
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_nheaps; i++) if (g_heaps[i] == h) { g_heaps[i] = g_heaps[--g_nheaps]; break; }
    ReleaseSRWLockExclusive(&g_lock);
    BOOL r = o_HeapDestroy(h);
    prune_released();
    return r;
}
static LPVOID WINAPI h_HeapAlloc(HANDLE h, DWORD fl, SIZE_T n) {
    LPVOID p = o_HeapAlloc(h, fl, n);
    if (p && is_game_heap(h)) note_addr(p);
    return p;
}
static LPVOID WINAPI h_HeapReAlloc(HANDLE h, DWORD fl, LPVOID old, SIZE_T n) {
    int game = is_game_heap(h);
    uintptr_t oldBase = game && old ? resv_start_of(old) : 0;
    LPVOID p = o_HeapReAlloc(h, fl, old, n);
    if (p && game) {
        note_addr(p);
        if (oldBase && p != old) resv_drop_if_released(oldBase);
    }
    return p;
}
static BOOL WINAPI h_HeapFree(HANDLE h, DWORD fl, LPVOID p) {
    uintptr_t base = p && is_game_heap(h) ? resv_start_of(p) : 0;
    BOOL r = o_HeapFree(h, fl, p);
    if (base) resv_drop_if_released(base);
    return r;
}
// ---------------------------------------------------------------- deterministic addresses
// Memory the executable asks VirtualAlloc for without naming an address is placed, by first fit in
// 64 KB units, in one region reserved at a fixed address before the game starts. Given the same
// sequence of requests the game gets the same addresses on every run, whatever else the process
// allocates, so containers ordered or hashed by address (Lua tables keyed by objects, for one)
// behave the same in every playback. The region's map lives in its first unit: savestates restore
// it together with the rest of the region (one tracked reservation).
#define ARENA_UNIT  0x10000u
#define ARENA_SIZE  (256u << 20)
#define ARENA_UNITS (ARENA_SIZE / ARENA_UNIT)
typedef struct {
    uint32_t used, peak;               // units in use / most units ever in use
    uint16_t len[ARENA_UNITS];         // length of the run starting at a unit, 0 elsewhere
    uint32_t busy[ARENA_UNITS / 32];
} ArenaMap;
static uintptr_t g_arena;
static ArenaMap *g_amap;
static SRWLOCK   g_arenaLock = SRWLOCK_INIT;

static int arena_busy(uint32_t u) { return (g_amap->busy[u >> 5] >> (u & 31)) & 1; }
static void arena_mark(uint32_t u, uint32_t n, int on) {
    for (uint32_t i = u; i < u + n; i++) {
        if (on) g_amap->busy[i >> 5] |= 1u << (i & 31);
        else g_amap->busy[i >> 5] &= ~(1u << (i & 31));
    }
}
static int in_arena(const void *p) { return g_amap && (uintptr_t)p - g_arena < ARENA_SIZE; }

static void arena_init(void) {
    static const uintptr_t bases[] = {0x20000000, 0x30000000, 0x40000000, 0x50000000};
    for (size_t i = 0; i < sizeof bases / sizeof bases[0] && !g_arena; i++)
        if (VirtualAlloc((void *)bases[i], ARENA_SIZE, MEM_RESERVE, PAGE_NOACCESS) == (void *)bases[i]) g_arena = bases[i];
    if (!g_arena) { tas_log("WARNING: no fixed address range is free; the game's memory addresses will differ between runs"); return; }
    g_amap = VirtualAlloc((void *)g_arena, sizeof(ArenaMap), MEM_COMMIT, PAGE_READWRITE);
    if (!g_amap) { VirtualFree((void *)g_arena, 0, MEM_RELEASE); g_arena = 0; return; }
    g_amap->len[0] = 1;                // unit 0 holds the map
    arena_mark(0, 1, 1);
    g_amap->used = g_amap->peak = 1;
    tas_log("game memory region at %p (%u MB)", (void *)g_arena, ARENA_SIZE >> 20);
}
static void *arena_alloc(SIZE_T n, DWORD type, DWORD prot) {
    uint32_t units = (uint32_t)((n + ARENA_UNIT - 1) / ARENA_UNIT), run = 0, start = 0;
    if (!units || units >= ARENA_UNITS) return NULL;
    void *p = NULL;
    AcquireSRWLockExclusive(&g_arenaLock);
    for (uint32_t u = 1; u < ARENA_UNITS && run < units; u++) {
        if (arena_busy(u)) { run = 0; continue; }
        if (!run) start = u;
        run++;
    }
    if (run == units) {
        arena_mark(start, units, 1);
        g_amap->len[start] = (uint16_t)units;
        g_amap->used += units;
        if (g_amap->used > g_amap->peak) {
            if (g_amap->used / 256 != g_amap->peak / 256)
                tas_log("game memory region: %u MB in use", (g_amap->used * ARENA_UNIT) >> 20);
            g_amap->peak = g_amap->used;
        }
        p = (void *)(g_arena + (uintptr_t)start * ARENA_UNIT);
    }
    ReleaseSRWLockExclusive(&g_arenaLock);
    if (!p) {
        static int logged;
        if (!logged) { logged = 1; tas_log("WARNING: the game memory region is full; further addresses differ between runs"); }
        return NULL;
    }
    if ((type & MEM_COMMIT) && !VirtualAlloc(p, n, MEM_COMMIT, prot)) {
        DWORD e = GetLastError();
        AcquireSRWLockExclusive(&g_arenaLock);
        arena_mark(start, units, 0);
        g_amap->len[start] = 0;
        g_amap->used -= units;
        ReleaseSRWLockExclusive(&g_arenaLock);
        SetLastError(e);
        return NULL;
    }
    return p;
}
static BOOL arena_free(void *p) {
    uint32_t u = (uint32_t)(((uintptr_t)p - g_arena) / ARENA_UNIT), n = 0;
    AcquireSRWLockExclusive(&g_arenaLock);
    if ((uintptr_t)p % ARENA_UNIT == 0 && u && (n = g_amap->len[u]) != 0) {
        VirtualFree(p, (SIZE_T)n * ARENA_UNIT, MEM_DECOMMIT);
        arena_mark(u, n, 0);
        g_amap->len[u] = 0;
        g_amap->used -= n;
    }
    ReleaseSRWLockExclusive(&g_arenaLock);
    if (!n) SetLastError(ERROR_INVALID_ADDRESS);
    return n != 0;
}

static LPVOID WINAPI h_VirtualAlloc(LPVOID a, SIZE_T n, DWORD type, DWORD prot) {
    LPVOID p = NULL;
    if (!a && g_amap && (type & (MEM_RESERVE | MEM_COMMIT)) && !(type & ~(MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN)))
        p = arena_alloc(n, type, prot);
    if (!p) p = o_VirtualAlloc(a, n, type, prot);
    if (p) note_addr(p);
    return p;
}
static BOOL WINAPI h_VirtualFree(LPVOID a, SIZE_T n, DWORD type) {
    if (in_arena(a) && (type & MEM_RELEASE)) return arena_free(a);
    BOOL r = o_VirtualFree(a, n, type);
    if (r && (type & MEM_RELEASE)) resv_drop_if_released((uintptr_t)a);
    return r;
}

int ss_install(HMODULE exe) {
    g_exe = (uint8_t *)exe;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    IMAGE_DATA_DIRECTORY td = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (td.VirtualAddress) {
        IMAGE_TLS_DIRECTORY32 *t = (IMAGE_TLS_DIRECTORY32 *)(g_exe + td.VirtualAddress);
        g_tlsIndex = (DWORD *)(uintptr_t)t->AddressOfIndex;
        g_tlsSize = t->EndAddressOfRawData - t->StartAddressOfRawData + t->SizeOfZeroFill;
        g_tlsRaw = t->EndAddressOfRawData - t->StartAddressOfRawData;
        g_tlsTemplate = (const uint8_t *)(uintptr_t)t->StartAddressOfRawData;
    }
    wchar_t env[8];
    g_check = GetEnvironmentVariableW(L"LOUKE_SSCHECK", env, 8) != 0;
    uint8_t *stack = VirtualAlloc(NULL, PRIV_STACK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stack) return 0;
    g_privBottom = (uintptr_t)stack;
    g_ssPrivTop = (uintptr_t)stack + PRIV_STACK - 64;
    arena_init();
    if (g_arena) note_addr((void *)g_arena);
    int ok = 1;
    ok &= tas_hook_iat("KERNEL32.dll", "HeapCreate", 0, (void *)h_HeapCreate, (void **)&o_HeapCreate);
    ok &= tas_hook_iat("KERNEL32.dll", "HeapAlloc", 0, (void *)h_HeapAlloc, (void **)&o_HeapAlloc);
    ok &= tas_hook_iat("KERNEL32.dll", "HeapFree", 0, (void *)h_HeapFree, (void **)&o_HeapFree);
    // optional: a game that does not import them never calls them
    tas_hook_iat("KERNEL32.dll", "HeapDestroy", 0, (void *)h_HeapDestroy, (void **)&o_HeapDestroy);
    tas_hook_iat("KERNEL32.dll", "HeapReAlloc", 0, (void *)h_HeapReAlloc, (void **)&o_HeapReAlloc);
    tas_hook_iat("KERNEL32.dll", "VirtualAlloc", 0, (void *)h_VirtualAlloc, (void **)&o_VirtualAlloc);
    tas_hook_iat("KERNEL32.dll", "VirtualFree", 0, (void *)h_VirtualFree, (void **)&o_VirtualFree);
    if (!ok) tas_log("WARNING: savestates: the game does not use HeapCreate/HeapAlloc/HeapFree; its heap memory is not tracked");
    return 1;
}

// ---------------------------------------------------------------- thread context (TEB / FPU)
static inline uintptr_t teb_get(uint32_t off) {
    uintptr_t v;
    __asm__ volatile("movl %%fs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}
static inline void teb_set(uint32_t off, uintptr_t v) {
    __asm__ volatile("movl %0, %%fs:(%1)" :: "r"(v), "r"(off) : "memory");
}

// The calling thread's block of the executable's implicit TLS (__declspec(thread)), or NULL.
void *ss_tls_block(uint32_t *size) {
    *size = 0;
    if (!g_tlsIndex || !g_tlsSize) return NULL;
    uintptr_t *arr = (uintptr_t *)teb_get(0x2C);
    if (!arr || !arr[*g_tlsIndex]) return NULL;
    *size = g_tlsSize;
    return (void *)arr[*g_tlsIndex];
}
// Give the calling thread's block its initial contents, as for a new thread.
void ss_tls_reset(void) {
    uint32_t n;
    uint8_t *b = ss_tls_block(&n);
    if (!b) return;
    memcpy(b, g_tlsTemplate, g_tlsRaw);
    memset(b + g_tlsRaw, 0, n - g_tlsRaw);
}
static inline uint16_t fpu_cw(void) { uint16_t v; __asm__ volatile("fnstcw %0" : "=m"(v)); return v; }
static inline void fpu_set_cw(uint16_t v) { __asm__ volatile("fldcw %0" :: "m"(v)); }
static inline uint32_t sse_csr(void) { uint32_t v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static inline void sse_set_csr(uint32_t v) { __asm__ volatile("ldmxcsr %0" :: "m"(v)); }

// ss_point: save callee-saved registers, remember esp, call ss_save_body (eax: 1 saved, 0 failed).
// ss_resume: continue at the ss_point whose esp is g_ssResumeEsp, returning 2.
// ss_switch: move to the private stack and run ss_restore_body (does not return).
int  ss_point(void);
void ss_resume(void) __attribute__((noreturn));
void ss_switch(void) __attribute__((noreturn));
__asm__(
    ".text\n"
    ".globl _ss_point\n"
    "_ss_point:\n"
    "  pushl %ebp\n"
    "  pushl %ebx\n"
    "  pushl %esi\n"
    "  pushl %edi\n"
    "  movl %esp, _g_ssPointEsp\n"
    "  call _ss_save_body\n"
    "  popl %edi\n"
    "  popl %esi\n"
    "  popl %ebx\n"
    "  popl %ebp\n"
    "  ret\n"
    ".globl _ss_resume\n"
    "_ss_resume:\n"
    "  movl _g_ssResumeEsp, %esp\n"
    "  popl %edi\n"
    "  popl %esi\n"
    "  popl %ebx\n"
    "  popl %ebp\n"
    "  movl $2, %eax\n"
    "  ret\n"
    ".globl _ss_switch\n"
    "_ss_switch:\n"
    "  movl _g_ssPrivTop, %esp\n"
    "  call _ss_restore_body\n"
    "  int3\n");

// ---------------------------------------------------------------- section windows
typedef struct { HANDLE sec; uint64_t size, off; uint8_t *view; uint32_t len; DWORD access; } Win;

static uint8_t *win_at(Win *w, uint64_t pos, uint32_t *avail) {
    if (!w->view || pos < w->off || pos >= w->off + w->len) {
        if (w->view) UnmapViewOfFile(w->view);
        w->off = pos & ~(uint64_t)(VIEW_SIZE - 1);
        uint64_t rest = w->size - w->off;
        w->len = rest < VIEW_SIZE ? (uint32_t)rest : VIEW_SIZE;
        w->view = MapViewOfFile(w->sec, w->access, (DWORD)(w->off >> 32), (DWORD)w->off, w->len);
        if (!w->view) return NULL;
    }
    *avail = (uint32_t)(w->off + w->len - pos);
    return w->view + (pos - w->off);
}
static int win_copy(Win *w, uint64_t pos, uint8_t *mem, uintptr_t n, int toSection) {
    while (n) {
        uint32_t avail;
        uint8_t *p = win_at(w, pos, &avail);
        if (!p) return 0;
        uint32_t c = n < avail ? (uint32_t)n : avail;
        if (toSection) memcpy(p, mem, c); else memcpy(mem, p, c);
        pos += c; mem += c; n -= c;
    }
    return 1;
}
static void win_close(Win *w) { if (w->view) UnmapViewOfFile(w->view); w->view = NULL; }

// ---------------------------------------------------------------- save
static int readable(DWORD prot) {
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    return (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
static int writable(DWORD prot) {
    return !(prot & (PAGE_GUARD | PAGE_NOACCESS)) &&
           (prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

typedef struct { Range *r; int n, cap; uint64_t bytes; } RangeList;
static int range_add(RangeList *l, uintptr_t a, uintptr_t n, DWORD prot, int inResv) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 1024;
        Range *nr = realloc(l->r, (size_t)cap * sizeof(Range));
        if (!nr) return 0;
        l->r = nr; l->cap = cap;
    }
    Range *r = &l->r[l->n++];
    r->addr = a; r->size = n; r->prot = prot; r->inResv = (uint8_t)inResv; r->hasData = (uint8_t)readable(prot);
    if (r->hasData) l->bytes += n;
    return 1;
}
// committed regions of [a, end) (fixed memory) or of the whole reservation at a (inResv)
static int add_committed(RangeList *l, uintptr_t a, uintptr_t end, int inResv) {
    MEMORY_BASIC_INFORMATION m;
    uintptr_t base = a;
    while (a < end && VirtualQuery((void *)a, &m, sizeof m)) {
        if (inResv && (uintptr_t)m.AllocationBase != base) break;
        uintptr_t rs = (uintptr_t)m.BaseAddress, re = rs + m.RegionSize;
        if (re > end) re = end;
        if (m.State == MEM_COMMIT && !range_add(l, a, re - a, m.Protect, inResv)) return 0;
        (void)rs;
        a = re;
    }
    return 1;
}

static void slot_clear(Slot *s) {
    if (s->sec) CloseHandle(s->sec);
    free(s->ranges);
    free(s->resv);
    free(s->resvFirst);
    memset(s, 0, sizeof *s);
}

static int64_t qpc_now(void) { LARGE_INTEGER li; QueryPerformanceCounter(&li); return li.QuadPart; }
static double ms_since(int64_t t0) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    return (double)(qpc_now() - t0) * 1000.0 / (double)f.QuadPart;
}

// Diagnostics (LOUKE_SSCHECK=1): every heap entry must lie in a tracked reservation; list the
// process's threads with the module of their start address.
static void verify_tracking(void) {
    for (int h = 0; h < g_nheaps; h++) {
        PROCESS_HEAP_ENTRY e;
        memset(&e, 0, sizeof e);
        unsigned long entries = 0, miss = 0;
        uintptr_t firstMiss = 0;
        if (!HeapLock(g_heaps[h])) continue;
        while (HeapWalk(g_heaps[h], &e)) {
            entries++;
            if (resv_find((uintptr_t)e.lpData) < 0) { if (!miss) firstMiss = (uintptr_t)e.lpData; miss++; }
        }
        HeapUnlock(g_heaps[h]);
        tas_log("savestate check: heap %p: %lu entries, %lu outside tracked memory (first %p)",
                g_heaps[h], entries, miss, (void *)firstMiss);
    }
    typedef LONG (WINAPI *NtQIT_t)(HANDLE, int, void *, ULONG, ULONG *);
    NtQIT_t q = (NtQIT_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        void *start = NULL;
        HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (th && q) q(th, 9, &start, sizeof start, NULL);
        if (th) CloseHandle(th);
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)start, &mod))
            GetModuleFileNameA(mod, name, sizeof name);
        const char *base = strrchr(name, '\\');
        tas_log("savestate check: thread %lu start %p (%s)", te.th32ThreadID, start, base ? base + 1 : name);
    }
    CloseHandle(snap);
}

__attribute__((used)) int ss_save_body(void);
int ss_save_body(void) {
    int64_t t0 = qpc_now();
    Slot *s = &g_slots[g_saveSlot];
    slot_clear(s);
    prune_released();
    discover_heap_memory();
    if (g_check) verify_tracking();

    RangeList l = {0};
    uint64_t exeBytes = 0, stackBytes = 0;
    // writable sections of the executable
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sh[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uintptr_t a = (uintptr_t)g_exe + sh[i].VirtualAddress;
        uintptr_t n = (sh[i].Misc.VirtualSize + 0xFFF) & ~0xFFFu;
        uint64_t b0 = l.bytes;
        if (!add_committed(&l, a, a + n, 0)) goto fail;
        exeBytes += l.bytes - b0;
    }
    // implicit TLS block of the game thread
    if (g_tlsIndex && g_tlsSize) {
        uintptr_t *arr = (uintptr_t *)teb_get(0x2C);
        if (arr && arr[*g_tlsIndex] && !range_add(&l, arr[*g_tlsIndex], g_tlsSize, PAGE_READWRITE, 0)) goto fail;
    }
    // stack above the save point
    s->stackBase = teb_get(4);
    if (!range_add(&l, g_ssPointEsp, s->stackBase - g_ssPointEsp, PAGE_READWRITE, 0)) goto fail;
    stackBytes = s->stackBase - g_ssPointEsp;
    // tracked reservations
    AcquireSRWLockShared(&g_lock);
    s->nresv = g_nresv;
    s->resv = malloc((size_t)(g_nresv ? g_nresv : 1) * sizeof(Resv));
    s->resvFirst = malloc((size_t)(g_nresv ? g_nresv : 1) * sizeof(int));
    if (s->resv) memcpy(s->resv, g_resv, (size_t)g_nresv * sizeof(Resv));
    memcpy(s->heaps, g_heaps, sizeof g_heaps);
    s->nheaps = g_nheaps;
    ReleaseSRWLockShared(&g_lock);
    if (!s->resv || !s->resvFirst) goto fail;
    for (int i = 0; i < s->nresv; i++) {
        s->resvFirst[i] = l.n;
        if (!add_committed(&l, s->resv[i].base, s->resv[i].base + s->resv[i].size, 1)) goto fail;
    }

    // copy into a pagefile-backed section
    s->bytes = l.bytes;
    uint64_t secSize = l.bytes ? l.bytes : 1;
    s->sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT,
                                (DWORD)(secSize >> 32), (DWORD)secSize, NULL);
    if (!s->sec) { tas_log("savestate: cannot create a %.0f MB section (%lu)", secSize / 1048576.0, GetLastError()); goto fail; }
    Win w = {s->sec, secSize, 0, NULL, 0, FILE_MAP_WRITE};
    uint64_t pos = 0;
    for (int i = 0; i < l.n; i++) {
        if (!l.r[i].hasData) continue;
        if (!win_copy(&w, pos, (uint8_t *)l.r[i].addr, l.r[i].size, 1)) {
            win_close(&w);
            tas_log("savestate: cannot map the section (%lu)", GetLastError());
            goto fail;
        }
        pos += l.r[i].size;
    }
    win_close(&w);

    s->ranges = l.r;
    s->nranges = l.n;
    s->esp = g_ssPointEsp;
    s->seh = teb_get(0);
    s->fpucw = fpu_cw();
    s->mxcsr = sse_csr();
    s->used = 1;
    AcquireSRWLockExclusive(&g_lock);
    hold_release(NULL);                // placeholders only the overwritten state needed
    ReleaseSRWLockExclusive(&g_lock);
    tas_log("savestate %d saved: %.1f MB (exe %.1f MB, stack %.1f KB, %d reservations, %d ranges) in %.0f ms",
            g_saveSlot + 1, l.bytes / 1048576.0, exeBytes / 1048576.0, stackBytes / 1024.0,
            s->nresv, l.n, ms_since(t0));
    return 1;
fail:
    free(l.r);
    slot_clear(s);
    tas_log("savestate %d: saving failed", g_saveSlot + 1);
    return 0;
}

int ss_save(int slot) {
    if (slot < 0 || slot >= TAS_STATE_SLOTS) return 0;
    g_saveSlot = slot;
    return ss_point();
}

int ss_slot_used(int slot) { return slot >= 0 && slot < TAS_STATE_SLOTS && g_slots[slot].used; }

// ---------------------------------------------------------------- load
static int saved_index(const Slot *s, uintptr_t base, uintptr_t size) {
    for (int i = 0; i < s->nresv; i++) if (s->resv[i].base == base && s->resv[i].size == size) return i;
    return -1;
}
static int current_index_of(uintptr_t allocBase) {
    for (int i = 0; i < g_nresv; i++) if (g_resv[i].base == allocBase) return i;
    return -1;
}

// Decide what loading needs, without changing anything. Threads must already be quiet.
int ss_load_check(int slot) {
    if (!ss_slot_used(slot)) return 0;
    Slot *s = &g_slots[slot];
    prune_released();
    discover_heap_memory();
    AcquireSRWLockShared(&g_lock);
    int ok = 1;
    for (int i = 0; i < g_nresv; i++) {
        int keep = saved_index(s, g_resv[i].base, g_resv[i].size) >= 0;
        int heapBase = 0;
        for (int h = 0; h < g_nheaps; h++) if ((uintptr_t)g_heaps[h] == g_resv[i].base) heapBase = 1;
        g_release[i] = (uint8_t)(!keep && !heapBase);   // heaps created after the save stay alive
    }
    for (int i = 0; i < s->nresv && ok; i++) {
        uintptr_t base = s->resv[i].base, end = base + s->resv[i].size;
        int cur = current_index_of(base);
        if (cur >= 0 && g_resv[cur].size == s->resv[i].size) continue;
        // the address range must be free once the reservations made after the save are released
        MEMORY_BASIC_INFORMATION m;
        for (uintptr_t a = base; a < end && ok; ) {
            if (!VirtualQuery((void *)a, &m, sizeof m)) { ok = 0; break; }
            if (m.State != MEM_FREE && hold_index((uintptr_t)m.AllocationBase) < 0) {
                int c = current_index_of((uintptr_t)m.AllocationBase);
                if (c < 0 || !g_release[c]) {
                    tas_log("savestate %d: address %p is now used by other memory (allocation %p, region %p+%lX, "
                            "state %lX type %lX protect %lX)", slot + 1, (void *)a, m.AllocationBase,
                            m.BaseAddress, (unsigned long)m.RegionSize, m.State, m.Type, m.Protect);
                    ok = 0;
                }
            }
            a = (uintptr_t)m.BaseAddress + m.RegionSize;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

static void restore_fail(const char *what, uintptr_t a) {
    // Memory is already partly overwritten; continuing would run the game on mixed state.
    tas_log("FATAL: savestate load failed while restoring (%s at %p, error %lu)", what, (void *)a, GetLastError());
    ExitProcess(0x5A5A);
}

__attribute__((used)) void ss_restore_body(void);
void ss_restore_body(void) {
    Slot *s = &g_slots[g_restoreSlot];
    int64_t t0 = qpc_now();
    g_gameStackLimit = teb_get(8);
    teb_set(0, 0xFFFFFFFFu);
    teb_set(4, g_ssPrivTop + 64);
    teb_set(8, g_privBottom);

    // 1. release reservations made after the save
    for (int i = 0; i < g_nresv; i++)
        if (g_release[i] && !VirtualFree((void *)g_resv[i].base, 0, MEM_RELEASE)) restore_fail("release", g_resv[i].base);
    AcquireSRWLockExclusive(&g_lock);
    hold_release(s);                   // placeholders kept for this state
    ReleaseSRWLockExclusive(&g_lock);
    // 2. re-reserve reservations released after the save
    for (int i = 0; i < s->nresv; i++) {
        if (still_allocated(s->resv[i].base)) continue;
        void *p = VirtualAlloc((void *)s->resv[i].base, s->resv[i].size, MEM_RESERVE, PAGE_NOACCESS);
        if (p != (void *)s->resv[i].base) restore_fail("reserve", s->resv[i].base);
    }
    // 3. decommit pages that were not committed at the save
    for (int i = 0; i < s->nresv; i++) {
        uintptr_t base = s->resv[i].base, end = base + s->resv[i].size;
        int r = s->resvFirst[i], rEnd = i + 1 < s->nresv ? s->resvFirst[i + 1] : s->nranges;
        MEMORY_BASIC_INFORMATION m;
        for (uintptr_t a = base; a < end && VirtualQuery((void *)a, &m, sizeof m); ) {
            uintptr_t re = (uintptr_t)m.BaseAddress + m.RegionSize;
            if (re > end) re = end;
            if (m.State == MEM_COMMIT) {
                uintptr_t c = a;
                while (c < re) {
                    while (r < rEnd && s->ranges[r].addr + s->ranges[r].size <= c) r++;
                    uintptr_t keepStart = r < rEnd ? s->ranges[r].addr : re;
                    if (keepStart > c) {
                        uintptr_t d = keepStart < re ? keepStart : re;
                        if (!VirtualFree((void *)c, d - c, MEM_DECOMMIT)) restore_fail("decommit", c);
                        c = d;
                    } else {
                        uintptr_t ke = s->ranges[r].addr + s->ranges[r].size;
                        c = ke < re ? ke : re;
                    }
                }
            }
            a = re;
        }
    }
    // 4. commit and copy
    Win w = {s->sec, s->bytes ? s->bytes : 1, 0, NULL, 0, FILE_MAP_READ};
    uint64_t pos = 0;
    for (int i = 0; i < s->nranges; i++) {
        Range *r = &s->ranges[i];
        if (r->inResv && !VirtualAlloc((void *)r->addr, r->size, MEM_COMMIT, r->hasData ? PAGE_READWRITE : r->prot))
            restore_fail("commit", r->addr);
        if (!r->hasData) continue;
        DWORD old;
        int tmp = !writable(r->prot) || r->inResv;
        if (tmp && !VirtualProtect((void *)r->addr, r->size, PAGE_READWRITE, &old)) restore_fail("protect", r->addr);
        if (!win_copy(&w, pos, (uint8_t *)r->addr, r->size, 0)) restore_fail("map", r->addr);
        if (tmp && r->prot != PAGE_READWRITE && !VirtualProtect((void *)r->addr, r->size, r->prot, &old))
            restore_fail("protect", r->addr);
        pos += r->size;
    }
    win_close(&w);
    // 5. tracking now describes the state again
    AcquireSRWLockExclusive(&g_lock);
    int n = 0;
    for (int i = 0; i < g_nresv; i++) if (!g_release[i]) g_resv[n++] = g_resv[i];
    g_nresv = n;
    for (int i = 0; i < s->nresv; i++) resv_insert(s->resv[i].base, s->resv[i].size);
    for (int i = 0; i < s->nheaps; i++) {
        int have = 0;
        for (int h = 0; h < g_nheaps; h++) if (g_heaps[h] == s->heaps[i]) have = 1;
        if (!have && g_nheaps < MAX_HEAPS) g_heaps[g_nheaps++] = s->heaps[i];
    }
    hold_all(-1);                      // memory released above that other states need
    ReleaseSRWLockExclusive(&g_lock);

    tas_log("savestate %d loaded: %.1f MB in %.0f ms", g_restoreSlot + 1, s->bytes / 1048576.0, ms_since(t0));
    teb_set(4, s->stackBase);
    teb_set(8, g_gameStackLimit);
    teb_set(0, s->seh);
    fpu_set_cw(s->fpucw);
    sse_set_csr(s->mxcsr);
    g_ssResumeEsp = s->esp;
    ss_resume();
}

void ss_load_commit(int slot) {
    g_restoreSlot = slot;
    ss_switch();
}
