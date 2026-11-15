// Keeping COM objects of system components (Direct3D resources, DirectSound buffers) alive for
// savestates.
//
// Savestates restore the game's memory, which holds pointers to such objects, but the objects
// live inside the components. Every object the game creates gets one extra reference of ours,
// so it survives the game releasing it while a state still refers to it. A state records how many
// references the game held on each object; loading sets the counts back (objects released after
// the save come back, objects created after the save are let go). Objects that neither the game
// nor any state refers to are released step by step, a few per frame.
#include "tas.h"
#include <stdlib.h>
#include <string.h>

typedef struct { void *obj; uint32_t kind; uint8_t zombie; } Obj;
typedef struct { void *obj; ULONG refs; uint32_t extra[2]; } SavedObj;
typedef struct { int used; SavedObj *o; int n; } ObjSlot;

static CRITICAL_SECTION g_cs;
static Obj    *g_obj;
static int     g_n, g_cap, g_gcPos, g_young;
static ObjSlot g_os[TAS_STATE_SLOTS];
static const ObjKindOps *g_ops[8];

static ULONG com_addref(void *o) { return ((ULONG (WINAPI *)(void *))(*(void ***)o)[1])(o); }
static ULONG com_release(void *o) { return ((ULONG (WINAPI *)(void *))(*(void ***)o)[2])(o); }
static ULONG game_refs(void *o) { ULONG n = com_addref(o); com_release(o); return n >= 2 ? n - 2 : 0; }

void tas_obj_init(void) { InitializeCriticalSection(&g_cs); }
void tas_obj_register_kind(uint32_t kind, const ObjKindOps *ops) { if (kind < 8) g_ops[kind & 7] = ops; }

void tas_obj_track(void *o, uint32_t kind) {
    if (!o) return;
    com_addref(o);
    EnterCriticalSection(&g_cs);
    if (g_n == g_cap) {
        int cap = g_cap ? g_cap * 2 : 1024;
        Obj *n = realloc(g_obj, (size_t)cap * sizeof(Obj));
        if (!n) { LeaveCriticalSection(&g_cs); com_release(o); return; }
        g_obj = n;
        g_cap = cap;
    }
    g_obj[g_n].obj = o;
    g_obj[g_n].kind = kind;
    g_obj[g_n].zombie = 0;
    g_n++;
    LeaveCriticalSection(&g_cs);
}

static int cmp_saved(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const SavedObj *)a)->obj, y = (uintptr_t)((const SavedObj *)b)->obj;
    return x < y ? -1 : x > y;
}
static SavedObj *slot_find(const ObjSlot *s, void *o) {
    SavedObj key;
    key.obj = o;
    return s->used ? bsearch(&key, s->o, (size_t)s->n, sizeof(SavedObj), cmp_saved) : NULL;
}
static int referenced(void *o) {
    for (int k = 0; k < TAS_STATE_SLOTS; k++) if (slot_find(&g_os[k], o)) return 1;
    return 0;
}

// Object i: release it if nobody needs it; returns 1 if it was removed (g_obj[i] is then another one).
static int collect(int i) {
    Obj *e = &g_obj[i];
    if (game_refs(e->obj)) { e->zombie = 0; return 0; }
    if (referenced(e->obj)) {
        if (!e->zombie) {
            e->zombie = 1;
            const ObjKindOps *ops = g_ops[e->kind & 7];
            if (ops && ops->released) ops->released(e->obj);
        }
        return 0;
    }
    com_release(e->obj);
    g_obj[i] = g_obj[--g_n];
    return 1;
}

void tas_obj_gc_step(int count) {
    EnterCriticalSection(&g_cs);
    // objects created since the last step first: temporaries (e.g. D3DX staging textures) go at once
    for (int i = g_n - 1; i >= g_young && i >= 0; i--) collect(i);
    g_young = g_n;
    for (int k = 0; k < count && g_n; k++) {
        if (g_gcPos >= g_n) g_gcPos = 0;
        if (!collect(g_gcPos)) g_gcPos++;
    }
    LeaveCriticalSection(&g_cs);
}
static void gc_all(void) {
    for (int i = 0; i < g_n; ) if (!collect(i)) i++;
}

static void slot_clear(ObjSlot *s) { free(s->o); memset(s, 0, sizeof *s); }

int tas_obj_save(int slot) {
    ObjSlot *s = &g_os[slot];
    EnterCriticalSection(&g_cs);
    slot_clear(s);
    s->o = malloc((size_t)(g_n ? g_n : 1) * sizeof(SavedObj));
    if (!s->o) { gc_all(); LeaveCriticalSection(&g_cs); return 0; }
    for (int i = 0; i < g_n; i++) {
        ULONG r = game_refs(g_obj[i].obj);
        if (!r) continue;
        SavedObj *so = &s->o[s->n++];
        memset(so, 0, sizeof *so);
        so->obj = g_obj[i].obj;
        so->refs = r;
        const ObjKindOps *ops = g_ops[g_obj[i].kind & 7];
        if (ops && ops->save) ops->save(so->obj, so->extra);
    }
    qsort(s->o, (size_t)s->n, sizeof(SavedObj), cmp_saved);
    s->used = 1;
    gc_all();                          // objects only the overwritten state kept alive
    tas_log("savestate %d: %d objects held by the game, %d kept in total", slot + 1, s->n, g_n);
    LeaveCriticalSection(&g_cs);
    return 1;
}

int tas_obj_slot_valid(int slot) { return g_os[slot].used; }

void tas_obj_load(int slot) {
    ObjSlot *s = &g_os[slot];
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_n; i++) {
        SavedObj *so = slot_find(s, g_obj[i].obj);
        ULONG target = so ? so->refs : 0, cur = game_refs(g_obj[i].obj);
        for (; cur < target; cur++) com_addref(g_obj[i].obj);
        for (; cur > target; cur--) com_release(g_obj[i].obj);
        g_obj[i].zombie = 0;
        const ObjKindOps *ops = g_ops[g_obj[i].kind & 7];
        if (ops && ops->load) ops->load(g_obj[i].obj, so ? so->extra : NULL);
    }
    gc_all();
    LeaveCriticalSection(&g_cs);
}

// Before the component needs objects of a kind gone (Direct3D Reset: default-pool resources):
// let go of every such object the game no longer holds; states that still refer to one become
// unusable. Returns the number of states invalidated.
int tas_obj_drop_unheld(uint32_t kind) {
    int lost = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_n; ) {
        if (g_obj[i].kind != kind || game_refs(g_obj[i].obj)) { i++; continue; }
        for (int k = 0; k < TAS_STATE_SLOTS; k++)
            if (slot_find(&g_os[k], g_obj[i].obj)) { slot_clear(&g_os[k]); lost++; }
        com_release(g_obj[i].obj);
        g_obj[i] = g_obj[--g_n];
    }
    LeaveCriticalSection(&g_cs);
    return lost;
}
