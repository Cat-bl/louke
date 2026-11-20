// Direct3D 9 frame boundary: Direct3DCreate9 (game executable import) -> IDirect3D9::CreateDevice
// -> IDirect3DDevice9::Present. Each Present of the game thread ends one TAS frame; so does a
// failed TestCooperativeLevel (the device is lost: the game skips presenting that frame, but the
// frame still ends). A game profile may mark the frame boundary itself instead
// (tas_claim_frame_boundary) and then calls d3d9_capture before presenting.
// The game thread is the thread of the first Present, unless a game profile identified it earlier.
//
// Debug capture (testing determinism):
//   LOUKE_FRAMEHASH=<file>                 one back-buffer hash per frame
//   LOUKE_SHOTS="600,1200" LOUKE_SHOTDIR save the back buffer of those frames as BMP
#include "tas.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef void *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT (WINAPI *CreateDevice9_t)(void *, UINT, int, HWND, DWORD, void *, void **);
typedef HRESULT (WINAPI *Present9_t)(void *, const RECT *, const RECT *, HWND, const void *);

static Direct3DCreate9_t o_Direct3DCreate9;
static CreateDevice9_t o_CreateDevice;
static Present9_t o_Present;
static void **g_d3dVt, **g_devVt;
static void *g_dev;

// Resource creation (objects kept alive for savestates, see comobj.c) and Reset.
typedef HRESULT (WINAPI *Reset_t)(void *, void *);
typedef HRESULT (WINAPI *CreateTexture_t)(void *, UINT, UINT, UINT, DWORD, int, int, void **, HANDLE *);
typedef HRESULT (WINAPI *CreateVolumeTexture_t)(void *, UINT, UINT, UINT, UINT, DWORD, int, int, void **, HANDLE *);
typedef HRESULT (WINAPI *CreateCubeTexture_t)(void *, UINT, UINT, DWORD, int, int, void **, HANDLE *);
typedef HRESULT (WINAPI *CreateBuffer_t)(void *, UINT, DWORD, DWORD, int, void **, HANDLE *);
typedef HRESULT (WINAPI *CreateSurface_t)(void *, UINT, UINT, int, int, DWORD, BOOL, void **, HANDLE *);
typedef HRESULT (WINAPI *CreateOffscreen_t)(void *, UINT, UINT, int, int, void **, HANDLE *);
typedef HRESULT (WINAPI *Create1_t)(void *, const void *, void **);      // decl / shaders / swap chain
typedef HRESULT (WINAPI *CreateTyped_t)(void *, int, void **);           // state block / query
typedef HRESULT (WINAPI *EndStateBlock_t)(void *, void **);
static Reset_t o_Reset;
static CreateTexture_t o_CreateTexture;
static CreateVolumeTexture_t o_CreateVolumeTexture;
static CreateCubeTexture_t o_CreateCubeTexture;
static CreateBuffer_t o_CreateVertexBuffer, o_CreateIndexBuffer;
static CreateSurface_t o_CreateRenderTarget, o_CreateDepthStencil;
static CreateOffscreen_t o_CreateOffscreen;
static Create1_t o_CreateSwapChain, o_CreateVertexDecl, o_CreateVertexShader, o_CreatePixelShader;
static CreateTyped_t o_CreateStateBlock, o_CreateQuery;
static EndStateBlock_t o_EndStateBlock;

static int patch_slot(void **vt, int slot, void *fn, void **orig) {
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old)) return 0;
    *orig = vt[slot];
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
    return 1;
}

// ---------------------------------------------------------------- debug capture
static uint32_t g_shotFrames[256];
static int      g_shotCount;
static wchar_t  g_shotDir[MAX_PATH];
static FILE    *g_hashLog;

static void parse_capture_env(void) {
    wchar_t buf[4096];
    if (GetEnvironmentVariableW(L"LOUKE_FRAMEHASH", buf, MAX_PATH)) {
        g_hashLog = _wfopen(buf, L"w");
        tas_log("frame hash log %s", g_hashLog ? "enabled" : "FAILED");
    }
    if (!GetEnvironmentVariableW(L"LOUKE_SHOTS", buf, 4096)) return;
    if (!GetEnvironmentVariableW(L"LOUKE_SHOTDIR", g_shotDir, MAX_PATH)) return;
    for (wchar_t *p = buf; *p && g_shotCount < 256;) {
        wchar_t *end;
        unsigned long v = wcstoul(p, &end, 10);
        if (end == p) { p++; continue; }
        g_shotFrames[g_shotCount++] = (uint32_t)v;
        p = end;
    }
    tas_log("screenshots requested: %d", g_shotCount);
}

static int want_shot(uint32_t f) {
    for (int i = 0; i < g_shotCount; i++) if (g_shotFrames[i] == f) return 1;
    return 0;
}

static void capture_frame(void *dev, uint32_t frame, int save, int hash) {
    typedef HRESULT (WINAPI *GetBackBuffer_t)(void *, UINT, UINT, int, void **);
    typedef HRESULT (WINAPI *GetRTData_t)(void *, void *, void *);
    typedef ULONG (WINAPI *Release_t)(void *);
    typedef HRESULT (WINAPI *GetDesc_t)(void *, void *);
    typedef HRESULT (WINAPI *LockRect_t)(void *, void *, const RECT *, DWORD);
    typedef HRESULT (WINAPI *UnlockRect_t)(void *);
    void **dvt = *(void ***)dev;
    void *bb = NULL, *sys = NULL;
    CreateOffscreen_t createOffscreen = o_CreateOffscreen ? o_CreateOffscreen : (CreateOffscreen_t)dvt[36];
    if (FAILED(((GetBackBuffer_t)dvt[18])(dev, 0, 0, 0, &bb))) { tas_log("shot: GetBackBuffer failed"); return; }
    void **bvt = *(void ***)bb;
    uint32_t desc[8] = {0};
    ((GetDesc_t)bvt[12])(bb, desc);
    UINT fmt = desc[0], w = desc[6], h = desc[7];
    if ((fmt != 21 && fmt != 22) ||
        FAILED(createOffscreen(dev, w, h, (int)fmt, 2 /*SYSTEMMEM*/, &sys, NULL))) {
        tas_log("shot: unsupported format %u or surface creation failed", fmt);
        ((Release_t)bvt[2])(bb);
        return;
    }
    void **svt = *(void ***)sys;
    if (SUCCEEDED(((GetRTData_t)dvt[32])(dev, bb, sys))) {
        struct { INT pitch; uint8_t *bits; } lr;
        HRESULT lhr = ((LockRect_t)svt[13])(sys, &lr, NULL, 0x10 /*READONLY*/);
        if (FAILED(lhr)) tas_log("shot: LockRect failed %08lX", (unsigned long)lhr);
        else if (hash) {
            uint64_t hv = 0xcbf29ce484222325ULL;
            for (UINT y = 0; y < h; y++) {
                const uint64_t *q = (const uint64_t *)(lr.bits + (size_t)y * lr.pitch);
                for (UINT x = 0; x < w / 2; x++) hv = (hv ^ (q[x] & 0x00FFFFFF00FFFFFFULL)) * 0x100000001b3ULL;
            }
            fprintf(g_hashLog, "%u %016llx\n", frame, (unsigned long long)hv);
            if (frame % 60 == 0) fflush(g_hashLog);
        }
        if (SUCCEEDED(lhr) && save) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\frame_%06u.bmp", g_shotDir, frame);
            for (int k = 2; k < 10 && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; k++)   // frame shown again after a load
                swprintf(path, MAX_PATH, L"%ls\\frame_%06u_%d.bmp", g_shotDir, frame, k);
            FILE *f = _wfopen(path, L"wb");
            if (!f) tas_log("shot: cannot create file (errno %d)", errno);
            else {
                uint32_t row = (w * 3 + 3) & ~3u, img = row * h;
                uint8_t hdr[54] = {'B', 'M'};
                *(uint32_t *)(hdr + 2) = 54 + img; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
                *(int32_t *)(hdr + 18) = (int32_t)w; *(int32_t *)(hdr + 22) = (int32_t)h;
                *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24; *(uint32_t *)(hdr + 34) = img;
                fwrite(hdr, 1, 54, f);
                uint8_t *line = calloc(1, row);
                for (int y = (int)h - 1; y >= 0 && line; y--) {
                    uint8_t *src = lr.bits + (size_t)y * lr.pitch;
                    for (UINT x = 0; x < w; x++) memcpy(line + x * 3, src + x * 4, 3);
                    fwrite(line, 1, row, f);
                }
                free(line);
                fclose(f);
                tas_log("shot saved (%ux%u)", w, h);
            }
        }
        if (SUCCEEDED(lhr)) ((UnlockRect_t)svt[14])(sys);
    } else tas_log("shot: GetRenderTargetData failed");
    ((Release_t)svt[2])(sys);
    ((Release_t)bvt[2])(bb);
}

// Video export: the back buffer read into a system-memory surface kept between frames.
static void *g_exportSurf;
static UINT g_exportW, g_exportH, g_exportFmt;
static void export_frame(void *dev) {
    typedef HRESULT (WINAPI *GetBackBuffer_t)(void *, UINT, UINT, int, void **);
    typedef HRESULT (WINAPI *GetRTData_t)(void *, void *, void *);
    typedef ULONG (WINAPI *Release_t)(void *);
    typedef HRESULT (WINAPI *GetDesc_t)(void *, void *);
    typedef HRESULT (WINAPI *LockRect_t)(void *, void *, const RECT *, DWORD);
    typedef HRESULT (WINAPI *UnlockRect_t)(void *);
    void **dvt = *(void ***)dev;
    void *bb = NULL;
    if (FAILED(((GetBackBuffer_t)dvt[18])(dev, 0, 0, 0, &bb))) return;
    void **bvt = *(void ***)bb;
    uint32_t desc[8] = {0};
    ((GetDesc_t)bvt[12])(bb, desc);
    UINT fmt = desc[0], w = desc[6], h = desc[7];
    if (fmt != 21 && fmt != 22) { ((Release_t)bvt[2])(bb); return; }
    if (g_exportSurf && (w != g_exportW || h != g_exportH || fmt != g_exportFmt)) {
        ((Release_t)(*(void ***)g_exportSurf)[2])(g_exportSurf);
        g_exportSurf = NULL;
    }
    CreateOffscreen_t createOffscreen = o_CreateOffscreen ? o_CreateOffscreen : (CreateOffscreen_t)dvt[36];
    if (!g_exportSurf && FAILED(createOffscreen(dev, w, h, (int)fmt, 2 /*SYSTEMMEM*/, &g_exportSurf, NULL))) {
        g_exportSurf = NULL;
        ((Release_t)bvt[2])(bb);
        return;
    }
    g_exportW = w; g_exportH = h; g_exportFmt = fmt;
    void **svt = *(void ***)g_exportSurf;
    if (SUCCEEDED(((GetRTData_t)dvt[32])(dev, bb, g_exportSurf))) {
        struct { INT pitch; uint8_t *bits; } lr;
        if (SUCCEEDED(((LockRect_t)svt[13])(g_exportSurf, &lr, NULL, 0x10 /*READONLY*/))) {
            avdump_video(lr.bits, lr.pitch, w, h);
            ((UnlockRect_t)svt[14])(g_exportSurf);
        }
    }
    ((Release_t)bvt[2])(bb);
}

// ---------------------------------------------------------------- hooks
void d3d9_capture(void *dev) {
    if (!tas_is_game_thread()) return;
    int ws = g_shotCount && want_shot(tas_frame());
    if (ws || g_hashLog) capture_frame(dev, tas_frame(), ws, g_hashLog != NULL);
    if (avdump_active()) export_frame(dev);
}

typedef HRESULT (WINAPI *TCL_t)(void *);
static TCL_t o_TestCooperativeLevel;
static HRESULT WINAPI h_TestCooperativeLevel(void *dev) {
    HRESULT hr = o_TestCooperativeLevel(dev);
    if (FAILED(hr) && tas_is_game_thread() && !tas_frame_boundary_claimed()) tas_frame_end();
    return hr;
}

static HRESULT WINAPI h_Present(void *dev, const RECT *src, const RECT *dst, HWND wnd, const void *dirty) {
    if (!tas_game_thread()) tas_set_game_thread(GetCurrentThreadId());
    if (!tas_is_game_thread() || tas_frame_boundary_claimed()) return o_Present(dev, src, dst, wnd, dirty);
    d3d9_capture(dev);
    HRESULT hr = tas_skip_present() ? S_OK : o_Present(dev, src, dst, wnd, dirty);
    tas_frame_end();
    return hr;
}

// ---------------------------------------------------------------- resources and savestates
#define POOL_DEFAULT 0
static HRESULT track(HRESULT hr, void **pp, int defaultPool) {
    if (SUCCEEDED(hr) && pp && *pp) tas_obj_track(*pp, defaultPool ? OBJ_D3D_DEFAULT : OBJ_D3D);
    return hr;
}
static HRESULT WINAPI h_CreateTexture(void *d, UINT w, UINT h, UINT l, DWORD u, int f, int pool, void **pp, HANDLE *sh) {
    return track(o_CreateTexture(d, w, h, l, u, f, pool, pp, sh), pp, pool == POOL_DEFAULT);
}
static HRESULT WINAPI h_CreateVolumeTexture(void *d, UINT w, UINT h, UINT dp, UINT l, DWORD u, int f, int pool, void **pp, HANDLE *sh) {
    return track(o_CreateVolumeTexture(d, w, h, dp, l, u, f, pool, pp, sh), pp, pool == POOL_DEFAULT);
}
static HRESULT WINAPI h_CreateCubeTexture(void *d, UINT e, UINT l, DWORD u, int f, int pool, void **pp, HANDLE *sh) {
    return track(o_CreateCubeTexture(d, e, l, u, f, pool, pp, sh), pp, pool == POOL_DEFAULT);
}
static HRESULT WINAPI h_CreateVertexBuffer(void *d, UINT n, DWORD u, DWORD fvf, int pool, void **pp, HANDLE *sh) {
    return track(o_CreateVertexBuffer(d, n, u, fvf, pool, pp, sh), pp, pool == POOL_DEFAULT);
}
static HRESULT WINAPI h_CreateIndexBuffer(void *d, UINT n, DWORD u, DWORD fmt, int pool, void **pp, HANDLE *sh) {
    return track(o_CreateIndexBuffer(d, n, u, fmt, pool, pp, sh), pp, pool == POOL_DEFAULT);
}
static HRESULT WINAPI h_CreateRenderTarget(void *d, UINT w, UINT h, int f, int ms, DWORD q, BOOL lock, void **pp, HANDLE *sh) {
    return track(o_CreateRenderTarget(d, w, h, f, ms, q, lock, pp, sh), pp, 1);
}
static HRESULT WINAPI h_CreateDepthStencil(void *d, UINT w, UINT h, int f, int ms, DWORD q, BOOL discard, void **pp, HANDLE *sh) {
    return track(o_CreateDepthStencil(d, w, h, f, ms, q, discard, pp, sh), pp, 1);
}
static HRESULT WINAPI h_CreateOffscreen(void *d, UINT w, UINT h, int f, int pool, void **pp, HANDLE *sh) {
    return track(o_CreateOffscreen(d, w, h, f, pool, pp, sh), pp, pool == POOL_DEFAULT);
}
static HRESULT WINAPI h_CreateSwapChain(void *d, const void *a, void **pp) { return track(o_CreateSwapChain(d, a, pp), pp, 1); }
static HRESULT WINAPI h_CreateVertexDecl(void *d, const void *a, void **pp) { return track(o_CreateVertexDecl(d, a, pp), pp, 0); }
static HRESULT WINAPI h_CreateVertexShader(void *d, const void *a, void **pp) { return track(o_CreateVertexShader(d, a, pp), pp, 0); }
static HRESULT WINAPI h_CreatePixelShader(void *d, const void *a, void **pp) { return track(o_CreatePixelShader(d, a, pp), pp, 0); }
static HRESULT WINAPI h_CreateStateBlock(void *d, int t, void **pp) { return track(o_CreateStateBlock(d, t, pp), pp, 1); }
static HRESULT WINAPI h_CreateQuery(void *d, int t, void **pp) { return track(o_CreateQuery(d, t, pp), pp, 0); }
static HRESULT WINAPI h_EndStateBlock(void *d, void **pp) { return track(o_EndStateBlock(d, pp), pp, 1); }

// Device state each savestate re-applies: everything a D3DSBT_ALL state block records, plus the
// render targets and the depth buffer (which state blocks leave out). The state block and the
// surfaces also keep the objects bound at the save alive.
typedef struct { int used; void *sb, *rt[4], *ds; } DevSlot;
static DevSlot g_devSlots[TAS_STATE_SLOTS];

static void com_rel(void *o) { if (o) ((ULONG (WINAPI *)(void *))(*(void ***)o)[2])(o); }
static void devslot_clear(DevSlot *d) {
    com_rel(d->sb);
    for (int i = 0; i < 4; i++) com_rel(d->rt[i]);
    com_rel(d->ds);
    memset(d, 0, sizeof *d);
}

int d3d9_state_save(int slot) {
    DevSlot *d = &g_devSlots[slot];
    devslot_clear(d);
    if (g_dev) {
        typedef HRESULT (WINAPI *GetSurf_t)(void *, DWORD, void **);
        typedef HRESULT (WINAPI *GetDS_t)(void *, void **);
        void **vt = *(void ***)g_dev;
        if (FAILED(o_CreateStateBlock(g_dev, 1 /*D3DSBT_ALL*/, &d->sb))) { tas_log("savestate: CreateStateBlock failed"); return 0; }
        for (DWORD i = 0; i < 4; i++) if (FAILED(((GetSurf_t)vt[38])(g_dev, i, &d->rt[i]))) d->rt[i] = NULL;
        if (FAILED(((GetDS_t)vt[40])(g_dev, &d->ds))) d->ds = NULL;
    }
    d->used = 1;
    return 1;
}

int d3d9_state_valid(int slot) { return g_devSlots[slot].used; }

void d3d9_state_load(int slot) {
    DevSlot *d = &g_devSlots[slot];
    if (!g_dev || !d->sb) return;
    typedef HRESULT (WINAPI *SetRT_t)(void *, DWORD, void *);
    typedef HRESULT (WINAPI *SetDS_t)(void *, void *);
    typedef HRESULT (WINAPI *Apply_t)(void *);
    void **vt = *(void ***)g_dev;
    for (DWORD i = 0; i < 4; i++) if (i || d->rt[0]) ((SetRT_t)vt[37])(g_dev, i, d->rt[i]);
    ((SetDS_t)vt[39])(g_dev, d->ds);
    ((Apply_t)(*(void ***)d->sb)[5])(d->sb);   // after the render targets: setting them resets the viewport
}

static void rehook(void);
static HRESULT WINAPI h_Reset(void *dev, void *pp) {
    int lost = 0;
    for (int k = 0; k < TAS_STATE_SLOTS; k++) if (g_devSlots[k].used) { devslot_clear(&g_devSlots[k]); lost++; }
    tas_obj_drop_unheld(OBJ_D3D_DEFAULT);
    if (lost) tas_log("Direct3D device reset: %d savestate(s) can no longer be loaded", lost);
    HRESULT hr = o_Reset(dev, pp);
    rehook();                          // Reset rewrites the device's method table
    return hr;
}

typedef struct { int slot; void *fn; void **orig; } DevHook;
static const DevHook kDevHooks[] = {
    {3, (void *)h_TestCooperativeLevel, (void **)&o_TestCooperativeLevel},
    {16, (void *)h_Reset, (void **)&o_Reset},
    {13, (void *)h_CreateSwapChain, (void **)&o_CreateSwapChain},
    {23, (void *)h_CreateTexture, (void **)&o_CreateTexture},
    {24, (void *)h_CreateVolumeTexture, (void **)&o_CreateVolumeTexture},
    {25, (void *)h_CreateCubeTexture, (void **)&o_CreateCubeTexture},
    {26, (void *)h_CreateVertexBuffer, (void **)&o_CreateVertexBuffer},
    {27, (void *)h_CreateIndexBuffer, (void **)&o_CreateIndexBuffer},
    {28, (void *)h_CreateRenderTarget, (void **)&o_CreateRenderTarget},
    {29, (void *)h_CreateDepthStencil, (void **)&o_CreateDepthStencil},
    {36, (void *)h_CreateOffscreen, (void **)&o_CreateOffscreen},
    {59, (void *)h_CreateStateBlock, (void **)&o_CreateStateBlock},
    {61, (void *)h_EndStateBlock, (void **)&o_EndStateBlock},
    {86, (void *)h_CreateVertexDecl, (void **)&o_CreateVertexDecl},
    {91, (void *)h_CreateVertexShader, (void **)&o_CreateVertexShader},
    {106, (void *)h_CreatePixelShader, (void **)&o_CreatePixelShader},
    {118, (void *)h_CreateQuery, (void **)&o_CreateQuery},
};

// The device's method table is a per-device copy that the runtime rewrites when the device state
// changes (seen on Reset). Install the hooks again whenever that happened, with the new methods as
// the ones to call.
static void rehook(void) {
    static int logged;
    if (!g_devVt) return;
    int n = 0;
    if (g_devVt[17] != (void *)h_Present) { patch_slot(g_devVt, 17, (void *)h_Present, (void **)&o_Present); n++; }
    for (size_t i = 0; i < sizeof kDevHooks / sizeof kDevHooks[0]; i++)
        if (g_devVt[kDevHooks[i].slot] != kDevHooks[i].fn) { patch_slot(g_devVt, kDevHooks[i].slot, kDevHooks[i].fn, kDevHooks[i].orig); n++; }
    if (n && !logged) { logged = 1; tas_log("Direct3D device methods were rewritten by the runtime; hooks installed again"); }
}
void d3d9_frame(void) { rehook(); }

static HRESULT WINAPI h_CreateDevice(void *self, UINT adapter, int type, HWND focus, DWORD flags, void *pp, void **out) {
    HRESULT hr = o_CreateDevice(self, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out && !g_devVt) {
        void **vt = *(void ***)*out;
        g_dev = *out;
        if (patch_slot(vt, 17, (void *)h_Present, (void **)&o_Present)) { g_devVt = vt; tas_log("Direct3D 9 Present hooked"); }
        else tas_log("ERROR: Present hook failed");
        for (size_t i = 0; i < sizeof kDevHooks / sizeof kDevHooks[0]; i++)
            if (!patch_slot(vt, kDevHooks[i].slot, kDevHooks[i].fn, kDevHooks[i].orig))
                tas_log("ERROR: Direct3D device method %d could not be hooked", kDevHooks[i].slot);
    }
    return hr;
}

static void *WINAPI h_Direct3DCreate9(UINT sdk) {
    void *d3d = o_Direct3DCreate9(sdk);
    if (d3d && !g_d3dVt) {
        void **vt = *(void ***)d3d;
        if (patch_slot(vt, 16, (void *)h_CreateDevice, (void **)&o_CreateDevice)) g_d3dVt = vt;
        else tas_log("ERROR: CreateDevice hook failed");
    }
    return d3d;
}

int d3d9_install(void) {
    parse_capture_env();
    return tas_hook_iat("d3d9.dll", "Direct3DCreate9", 0, (void *)h_Direct3DCreate9, (void **)&o_Direct3DCreate9);
}
