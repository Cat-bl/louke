// Game profile: THE KING OF FIGHTERS XIII, 2013 Steam edition (kofxiii.exe, 32-bit,
// TimeDateStamp 0x529D8F4E, fixed image base 0x400000).
//
// 1. Frame boundary: DeviceImpl::Present (0x53B410, thiscall, ecx = DeviceImpl*) runs
//    TestCooperativeLevel and, only if that succeeds, IDirect3DDevice9::Present. It is replaced
//    with kof13_present, which ends the TAS frame even when the device skipped presenting, so the
//    virtual clock always advances. This also overwrites the 60 fps limiter patched into
//    kofxiii.exe for normal windowed play (jmp at 0x53B410 into a cave at 0x71A2D0): the TAS core
//    paces frames itself and fakes the time it reads.
// 2. The game loop runs in application::Renderer::run (vtable slot 0x799F78 -> 0x494BA0, thiscall
//    with one stack argument). Wrapping it identifies the game thread before it runs any game code,
//    so its very first time reads are already virtual.
// 3. While paused, the window thread may request the device (NgDevice singleton 0x7EA950 ->
//    DeviceImpl: +0xD request, +0xC acknowledge) and waits without pumping messages until the
//    game loop acknowledges. The acknowledgement is given here so the window stays responsive.
#include "tas.h"
#include <string.h>

#define KOF13_TIMESTAMP    0x529D8F4Eu
#define PRESENT_WRAPPER_VA 0x0053B410u
#define RENDERER_RUN_SLOT  0x00799F78u
#define RENDERER_RUN_ORIG  0x00494BA0u
#define NGDEVICE_SINGLETON 0x007EA950u

static const uint8_t kWrapperOrig[6]    = {0x56, 0x8B, 0x71, 0x08, 0x8B, 0x06};  // push esi; mov esi,[ecx+8]; mov eax,[esi]
static const uint8_t kWrapperLimiter[6] = {0xE9, 0xBB, 0xEE, 0x1D, 0x00, 0x90};  // jmp 0x71A2D0; nop

static int kof13_match(HMODULE exe, const IMAGE_NT_HEADERS32 *nt) {
    return (uintptr_t)exe == 0x400000 && nt->FileHeader.TimeDateStamp == KOF13_TIMESTAMP;
}

static void __attribute__((thiscall)) kof13_present(void *self) {
    void *dev = *(void **)((uint8_t *)self + 8);
    void **vt = *(void ***)dev;
    typedef HRESULT (WINAPI *TCL_t)(void *);
    typedef HRESULT (WINAPI *Present_t)(void *, const RECT *, const RECT *, HWND, const void *);
    if (!tas_skip_present() && SUCCEEDED(((TCL_t)vt[3])(dev))) {
        d3d9_capture(dev);
        ((Present_t)vt[17])(dev, NULL, NULL, NULL, NULL);
    }
    if (tas_is_game_thread()) tas_frame_end();
}

typedef uint32_t (__attribute__((thiscall)) *RendererRun_t)(void *, uint32_t);
static uint32_t __attribute__((thiscall)) renderer_run(void *self, uint32_t arg) {
    tas_set_game_thread(GetCurrentThreadId());
    return ((RendererRun_t)RENDERER_RUN_ORIG)(self, arg);
}

static int kof13_install(HMODULE exe) {
    (void)exe;
    uint8_t *w = (uint8_t *)PRESENT_WRAPPER_VA;
    if (memcmp(w, kWrapperLimiter, 6) && memcmp(w, kWrapperOrig, 6)) {
        tas_log("kof13: unexpected bytes at the Present wrapper");
        return 0;
    }
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    int32_t rel = (int32_t)((uintptr_t)kof13_present - (PRESENT_WRAPPER_VA + 5));
    memcpy(jmp + 1, &rel, 4);
    if (!tas_patch(w, jmp, 6)) return 0;
    tas_claim_frame_boundary();
    void **slot = (void **)RENDERER_RUN_SLOT;
    if ((uintptr_t)*slot != RENDERER_RUN_ORIG) { tas_log("kof13: unexpected Renderer vtable slot"); return 0; }
    void *fn = (void *)renderer_run;
    return tas_patch(slot, &fn, sizeof fn);
}

static void kof13_pause_tick(void) {
    uint8_t *ng = *(uint8_t **)NGDEVICE_SINGLETON;
    uint8_t *dev = ng ? *(uint8_t **)(ng + 4) : NULL;
    if (dev && dev[0xD]) dev[0xC] = 1;
}

const GameProfile g_profile_kof13 = {
    "kof13",
    L"拳皇13（2013 Steam 版）",
    kof13_match,
    kof13_install,
    kof13_pause_tick,
};
