// Game profile: THE KING OF FIGHTERS XIII, 2013 Steam edition (kofxiii.exe, 32-bit,
// TimeDateStamp 0x529D8F4E, fixed image base 0x400000). The generic core runs the game; this
// profile only adds what is specific to it:
//
// 1. While paused, the window thread may request the device (NgDevice singleton 0x7EA950 ->
//    DeviceImpl: +0xD request, +0xC acknowledge) and waits without pumping messages until the
//    game loop acknowledges. The acknowledgement is given here so the window stays responsive.
// 2. A 60 fps limiter sometimes patched into kofxiii.exe for normal windowed play (jmp at
//    0x53B410, the game's Present wrapper, into a cave at 0x71A2D0) is undone: the TAS core paces
//    frames itself and the limiter would wait for real time that the game never sees.
#include "tas.h"
#include <string.h>

#define KOF13_TIMESTAMP    0x529D8F4Eu
#define PRESENT_WRAPPER_VA 0x0053B410u
#define NGDEVICE_SINGLETON 0x007EA950u

static const uint8_t kWrapperOrig[6]    = {0x56, 0x8B, 0x71, 0x08, 0x8B, 0x06};  // push esi; mov esi,[ecx+8]; mov eax,[esi]
static const uint8_t kWrapperLimiter[6] = {0xE9, 0xBB, 0xEE, 0x1D, 0x00, 0x90};  // jmp 0x71A2D0; nop

static int kof13_match(HMODULE exe, const IMAGE_NT_HEADERS32 *nt) {
    return (uintptr_t)exe == 0x400000 && nt->FileHeader.TimeDateStamp == KOF13_TIMESTAMP;
}

static int kof13_install(HMODULE exe) {
    (void)exe;
    uint8_t *w = (uint8_t *)PRESENT_WRAPPER_VA;
    int limiter = !memcmp(w, kWrapperLimiter, 6);
    if (!limiter && memcmp(w, kWrapperOrig, 6)) {
        tas_log("kof13: unexpected bytes at the Present wrapper");
        return 0;
    }
    if (limiter) {
        tas_log("kof13: 60 fps limiter patch found in the executable; undone in memory");
        return tas_patch(w, kWrapperOrig, 6);
    }
    return 1;
}

static void kof13_pause_tick(void) {
    uint8_t *ng = *(uint8_t **)NGDEVICE_SINGLETON;
    uint8_t *dev = ng ? *(uint8_t **)(ng + 4) : NULL;
    if (dev && dev[0xD]) dev[0xC] = 1;
}

const GameProfile g_profile_kof13 = {
    "kof13",
    L"拳皇13（2013 Steam 版）",
    L"KOF XIII (2013 Steam)",
    kof13_match,
    kof13_install,
    kof13_pause_tick,
};
