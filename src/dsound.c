// DirectSound 8 (created with DirectSoundCreate8 or CoCreateInstance(CLSID_DirectSound8)).
//
// 1. Virtual audio clock: the play position of every secondary buffer the game creates is computed
//    from the virtual time, not read from the sound card. Audio engines decide how much to mix (and
//    when sounds end) from this position, so with a real-time position the game's state would
//    follow the wall clock.
// 2. Output (as Hourglass does it): the game's buffers never play themselves. At the end of each
//    frame the part of each playing buffer that the virtual play position passed during the frame
//    is copied into a queue; a feeder thread of our own streams the queues to output buffers in
//    real time. When frames come late (loading, pause, frame advance) the output plays silence
//    instead of the stale buffer contents; when they come early (fast-forward) old audio is dropped.
// 3. Savestates: the buffers are kept alive (comobj.c); a state records each buffer's virtual
//    position and whether it was playing.
#include "tas.h"
#include <string.h>

typedef HRESULT (WINAPI *DSCreate8_t)(const GUID *, void **, void *);
typedef HRESULT (WINAPI *CoCreateInstance_t)(REFCLSID, void *, DWORD, REFIID, void **);
typedef HRESULT (WINAPI *CreateSoundBuffer_t)(void *, const void *, void **, void *);
typedef HRESULT (WINAPI *DuplicateSoundBuffer_t)(void *, void *, void **);
typedef HRESULT (WINAPI *QI_t)(void *, REFIID, void **);
typedef HRESULT (WINAPI *GetPos_t)(void *, DWORD *, DWORD *);
typedef HRESULT (WINAPI *GetStatus_t)(void *, DWORD *);
typedef HRESULT (WINAPI *Play_t)(void *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *SetPos_t)(void *, DWORD);
typedef HRESULT (WINAPI *Stop_t)(void *);
typedef HRESULT (WINAPI *Lock_t)(void *, DWORD, DWORD, void **, DWORD *, void **, DWORD *, DWORD);
typedef HRESULT (WINAPI *Unlock_t)(void *, void *, DWORD, void *, DWORD);
typedef HRESULT (WINAPI *GetVolume_t)(void *, LONG *);
typedef HRESULT (WINAPI *SetVolume_t)(void *, LONG);
static DSCreate8_t o_DirectSoundCreate8;
static CoCreateInstance_t o_CoCreateInstance;
static CreateSoundBuffer_t o_CreateSoundBuffer;
static DuplicateSoundBuffer_t o_DuplicateSoundBuffer;
static void **g_dsVt;
// Devices the game created through its own imports. The device methods are shared with other
// DirectSound users in the process (video playback), whose buffers are left alone. Each device is
// kept alive for good: releasing a device destroys all its buffers, which states may still need.
static void *g_devs[8];
static int g_ndevs;
static int our_device(void *ds) {
    for (int i = 0; i < g_ndevs; i++) if (g_devs[i] == ds) return 1;
    return 0;
}

static const GUID kCLSID_DirectSound8 = {0x3901cc3f, 0x84b5, 0x4fa4, {0xba, 0x35, 0xaa, 0x81, 0x72, 0xb8, 0xa0, 0x9b}};
static const GUID kCLSID_DirectSound  = {0x47d4d946, 0x62e8, 0x11cf, {0x93, 0xbc, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

static void **vt_of(void *o) { return *(void ***)o; }
static int patch(void **vt, int slot, void *fn, void **orig) {
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old)) return 0;
    *orig = vt[slot];
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
    return 1;
}

// ---------------------------------------------------------------- virtual buffers
#define MAX_VBUF 64
typedef struct {
    void   *obj[2];                    // IDirectSoundBuffer and, if different, IDirectSoundBuffer8
    void   *ds;                        // the device that made it
    DWORD   size, bytesPerSec, align;
    int     playing, looping;
    DWORD   pos0;                      // position at vstart
    int64_t vstart;                    // virtual 100 ns time
    DWORD   lastPos;                   // position up to which its sound went to the output
    int     stream;                    // output stream, -1 none yet
    uint8_t fmt[64];                   // WAVEFORMATEX(TENSIBLE)
    uint8_t silence;                   // sample value of silence
} VBuf;
static VBuf g_vb[MAX_VBUF];
static int g_nvb;
static CRITICAL_SECTION g_cs;

// Buffer methods. The method tables are shared with sound buffers of other components (video
// playback), so every hook first checks whether the buffer is one of the game's.
#define MAX_BUFVT 4
static void **g_bufVt[MAX_BUFVT];
static int g_nbufVt;
static QI_t o_QueryInterface[MAX_BUFVT];
static GetPos_t o_GetCurrentPosition[MAX_BUFVT];
static GetStatus_t o_GetStatus[MAX_BUFVT];
static Play_t o_Play[MAX_BUFVT];
static SetPos_t o_SetCurrentPosition[MAX_BUFVT];
static Stop_t o_Stop[MAX_BUFVT];
static Lock_t o_Lock[MAX_BUFVT];
static Unlock_t o_Unlock[MAX_BUFVT];
static int vt_index(void *b) {
    void **vt = vt_of(b);
    for (int i = 0; i < g_nbufVt; i++) if (g_bufVt[i] == vt) return i;
    return 0;
}
static VBuf *vb_find(void *b) {
    for (int i = 0; i < g_nvb; i++) if (g_vb[i].obj[0] == b || g_vb[i].obj[1] == b) return &g_vb[i];
    return NULL;
}
static DWORD vb_pos(const VBuf *v, int64_t now) {
    if (!v->playing || !v->size) return v->pos0;
    uint64_t adv = (uint64_t)(now > v->vstart ? now - v->vstart : 0) * v->bytesPerSec / 10000000u;
    adv -= adv % v->align;
    uint64_t p = v->pos0 + adv;
    return v->looping ? (DWORD)(p % v->size) : (DWORD)(p < v->size ? p : v->size);
}
static void vb_rebase(VBuf *v, int64_t now) { v->pos0 = vb_pos(v, now); v->vstart = now; }

static void ext_seen(void *b, int idx);
static void ext_stop(void *b, int idx);
static HRESULT WINAPI h_GetCurrentPosition(void *b, DWORD *play, DWORD *write) {
    EnterCriticalSection(&g_cs);
    VBuf *v = vb_find(b);
    if (!v) {
        LeaveCriticalSection(&g_cs);
        int i = vt_index(b);
        if (avdump_active()) ext_seen(b, i);
        return o_GetCurrentPosition[i](b, play, write);
    }
    DWORD p = vb_pos(v, tas_vtime());
    if (!v->looping && v->playing && p >= v->size) { v->playing = 0; v->pos0 = 0; p = 0; }
    DWORD lead = v->bytesPerSec / 100;                         // write cursor ~10 ms ahead, like hardware
    lead -= lead % v->align;
    DWORD size = v->size;
    LeaveCriticalSection(&g_cs);
    if (play) *play = p;
    if (write) *write = size ? (p + lead) % size : 0;
    return S_OK;
}
static HRESULT WINAPI h_GetStatus(void *b, DWORD *st) {
    HRESULT hr = o_GetStatus[vt_index(b)](b, st);
    EnterCriticalSection(&g_cs);
    VBuf *v = vb_find(b);
    if (v && st && SUCCEEDED(hr)) {
        if (!v->looping && v->playing && vb_pos(v, tas_vtime()) >= v->size) { v->playing = 0; v->pos0 = 0; }
        *st = (*st & ~5u) | (v->playing ? 1u : 0u) | (v->playing && v->looping ? 4u : 0u);
    }
    LeaveCriticalSection(&g_cs);
    return hr;
}
static HRESULT WINAPI h_Play(void *b, DWORD r1, DWORD prio, DWORD flags) {
    EnterCriticalSection(&g_cs);
    VBuf *v = vb_find(b);
    if (v) {
        int64_t now = tas_vtime();
        if (!v->playing) { v->vstart = now; v->playing = 1; v->lastPos = v->pos0; } else vb_rebase(v, now);
        v->looping = (flags & 1) != 0;
    }
    LeaveCriticalSection(&g_cs);
    return v ? S_OK : o_Play[vt_index(b)](b, r1, prio, flags);
}
static HRESULT WINAPI h_SetCurrentPosition(void *b, DWORD pos) {
    EnterCriticalSection(&g_cs);
    VBuf *v = vb_find(b);
    if (v) {
        if (v->size) pos %= v->size;
        pos -= pos % v->align;
        v->pos0 = pos; v->vstart = tas_vtime(); v->lastPos = pos;
    }
    LeaveCriticalSection(&g_cs);
    return v ? S_OK : o_SetCurrentPosition[vt_index(b)](b, pos);
}
static HRESULT WINAPI h_Stop(void *b) {
    EnterCriticalSection(&g_cs);
    VBuf *v = vb_find(b);
    if (v) { vb_rebase(v, tas_vtime()); v->playing = 0; }
    LeaveCriticalSection(&g_cs);
    if (!v && avdump_active()) ext_stop(b, vt_index(b));
    return v ? S_OK : o_Stop[vt_index(b)](b);
}
static void hook_buffer_vt(void *b);
static HRESULT WINAPI h_QueryInterface(void *b, REFIID riid, void **out) {
    HRESULT hr = o_QueryInterface[vt_index(b)](b, riid, out);
    if (SUCCEEDED(hr) && out && *out && *out != b) {
        EnterCriticalSection(&g_cs);
        VBuf *v = vb_find(b);
        if (v && !v->obj[1]) v->obj[1] = *out;            // the IDirectSoundBuffer8 interface
        LeaveCriticalSection(&g_cs);
        if (v) hook_buffer_vt(*out);
    }
    return hr;
}

static HRESULT WINAPI h_Unlock(void *b, void *p1, DWORD n1, void *p2, DWORD n2);
static void hook_buffer_vt(void *b) {
    void **vt = vt_of(b);
    EnterCriticalSection(&g_cs);
    int known = 0;
    for (int i = 0; i < g_nbufVt; i++) if (g_bufVt[i] == vt) known = 1;
    if (!known && g_nbufVt < MAX_BUFVT) {
        int i = g_nbufVt;
        if (patch(vt, 0, (void *)h_QueryInterface, (void **)&o_QueryInterface[i]) &&
            patch(vt, 4, (void *)h_GetCurrentPosition, (void **)&o_GetCurrentPosition[i]) &&
            patch(vt, 9, (void *)h_GetStatus, (void **)&o_GetStatus[i]) &&
            patch(vt, 12, (void *)h_Play, (void **)&o_Play[i]) &&
            patch(vt, 13, (void *)h_SetCurrentPosition, (void **)&o_SetCurrentPosition[i]) &&
            patch(vt, 18, (void *)h_Stop, (void **)&o_Stop[i]) &&
            patch(vt, 19, (void *)h_Unlock, (void **)&o_Unlock[i])) {
            o_Lock[i] = (Lock_t)vt[11];
            g_bufVt[i] = vt;
            g_nbufVt++;
        } else tas_log("ERROR: DirectSound buffer hooks failed");
    }
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------- output
// One output stream per game buffer that played: a queue filled at frame ends and an output buffer
// (on the game's device, same format) that the feeder thread keeps playing.
#define MAX_STREAM 8
typedef struct {
    void   *out;                       // our IDirectSoundBuffer, looping
    DWORD   size, align, bytesPerSec;
    uint8_t silence;
    uint8_t *q;                        // queue (ring) of sound waiting for the output
    DWORD   qcap, qhead, qlen;
    DWORD   lastPlay;                  // real play cursor at the previous feeder pass
    int64_t played, written, silenced; // byte counts since the start of the stream
    LONG    volume;
    int     sample;                    // 1: 16-bit integer, 2: 32-bit float, 0: other (no fades)
    int     faded;                     // the sound written so far ends in a fade-out
    DWORD   wleadMax;                  // largest distance seen from play to write cursor
} Stream;
static Stream g_st[MAX_STREAM];
static int g_nst;
static int g_soundLog;                 // LOUKE_SOUNDLOG=1: diagnostics
static uint8_t *g_shadow[64];          // per game buffer: its contents at the last capture
static uint32_t g_stale, g_dry, g_drop;
static CRITICAL_SECTION g_qcs;         // queues and output buffers
static HANDLE g_feeder;

static Lock_t lock_of(void *b) { return (Lock_t)vt_of(b)[11]; }
static Unlock_t unlock_of(void *b) { return (Unlock_t)vt_of(b)[19]; }

// Write n bytes (src NULL: silence) to the output at stream byte count pos.
static void out_write(Stream *s, int64_t pos, const uint8_t *src, DWORD n) {
    void *p1, *p2;
    DWORD n1, n2;
    if (!n || FAILED(lock_of(s->out)(s->out, (DWORD)(pos % s->size), n, &p1, &n1, &p2, &n2, 0))) return;
    if (src) { memcpy(p1, src, n1); if (p2) memcpy(p2, src + n1, n2); }
    else { memset(p1, s->silence, n1); if (p2) memset(p2, s->silence, n2); }
    unlock_of(s->out)(s->out, p1, n1, p2, n2);
}

// Gain ramp over n bytes of samples: from 1 to 0 (out) or 0 to 1 (in), per sample frame.
static void ramp(const Stream *s, uint8_t *p, DWORD n, int out) {
    DWORD frames = n / s->align, ch = s->sample == 1 ? s->align / 2 : s->align / 4;
    for (DWORD f = 0; f < frames; f++) {
        float g = (float)(out ? frames - f : f) / (float)frames;
        for (DWORD c = 0; c < ch; c++) {
            if (s->sample == 1) { int16_t *x = (int16_t *)p + f * ch + c; *x = (int16_t)(*x * g); }
            else { float *x = (float *)p + f * ch + c; *x *= g; }
        }
    }
}
// Fade out the sound already in the output between stream byte counts a and b.
static void out_fade(Stream *s, int64_t a, int64_t b) {
    void *p1, *p2;
    DWORD n1, n2, n = (DWORD)(b - a);
    n -= n % s->align;
    if (!n || !s->sample || FAILED(lock_of(s->out)(s->out, (DWORD)(a % s->size), n, &p1, &n1, &p2, &n2, 0))) return;
    uint8_t *t = HeapAlloc(GetProcessHeap(), 0, n);
    if (t) {
        memcpy(t, p1, n1);
        if (p2) memcpy(t + n1, p2, n2);
        ramp(s, t, n1 + (p2 ? n2 : 0), 1);
        memcpy(p1, t, n1);
        if (p2) memcpy(p2, t + n1, n2);
        HeapFree(GetProcessHeap(), 0, t);
    }
    unlock_of(s->out)(s->out, p1, n1, p2, n2);
}

// Feeder thread (real time, outside the game's lockstep): every 5 ms it moves queued sound to the
// outputs, keeping 40..120 ms ahead of the sound card, and keeps silence ahead of the sound. When
// the game falls behind real time (loading) the sound fades out over ~10 ms instead of breaking
// off, and fades back in when it continues.
static DWORD WINAPI feeder(void *arg) {
    (void)arg;
    uint8_t *tmp = HeapAlloc(GetProcessHeap(), 0, 1 << 16);
    for (;;) {
        Sleep(5);
        EnterCriticalSection(&g_qcs);
        for (int i = 0; i < g_nst; i++) {
            Stream *s = &g_st[i];
            DWORD play = 0, w;
            if (!s->out || FAILED(o_GetCurrentPosition[vt_index(s->out)](s->out, &play, &w))) continue;
            s->played += (play + s->size - s->lastPlay) % s->size;
            s->lastPlay = play;
            // The sound card has taken everything before its write cursor: sound goes after it.
            DWORD wl = (w + s->size - play) % s->size;
            int64_t wpos = s->played + wl;
            if (wl > s->wleadMax) {
                s->wleadMax = wl;
                if (g_soundLog) tas_log("SOUND write cursor %lu bytes (%.1f ms) ahead of the play cursor", wl, wl * 1000.0 / s->bytesPerSec);
            }
            DWORD guard = s->bytesPerSec * 3 / 1000, cushion = s->bytesPerSec * 50 / 1000, maxAhead = s->bytesPerSec * 150 / 1000;
            cushion -= cushion % s->align;
            if (s->qlen && s->written < wpos + guard) {         // ran dry: go on a little after the write cursor
                if (g_soundLog && ++g_dry <= 200) tas_log("SOUND ran dry (%ld bytes behind), frame %u", (long)(wpos - s->written), tas_frame());
                s->written = wpos + cushion;
                s->faded = 1;
            }
            while (s->qlen && s->written - wpos < (int64_t)maxAhead && tmp) {
                DWORD n = s->qlen;
                if (n > maxAhead - (DWORD)(s->written - wpos)) n = maxAhead - (DWORD)(s->written - wpos);
                if (n > s->qcap - s->qhead) n = s->qcap - s->qhead;
                if (n > (1u << 16)) n = 1u << 16;
                n -= n % s->align;
                if (!n) break;
                memcpy(tmp, s->q + s->qhead, n);
                if (s->faded && s->sample) {                    // continue after a fade-out
                    DWORD f = s->bytesPerSec / 100;
                    f -= f % s->align;
                    ramp(s, tmp, n < f ? n : f, 0);
                }
                s->faded = 0;
                out_write(s, s->written, tmp, n);
                s->qhead = (s->qhead + n) % s->qcap;
                s->qlen -= n;
                s->written += n;
            }
            if (!s->qlen && !s->faded) {
                if (s->written <= wpos) s->faded = 1;           // already ran out
                else if (s->written - wpos < (int64_t)(s->bytesPerSec * 12 / 1000)) {   // about to run out
                    out_fade(s, wpos, s->written);
                    s->faded = 1;
                }
            }
            if (s->silenced < s->written) s->silenced = s->written;
            int64_t target = wpos + s->bytesPerSec * 300 / 1000;       // silence after the last sound
            if (s->silenced < target) {
                DWORD n = (DWORD)(target - s->silenced);
                n -= n % s->align;
                out_write(s, s->silenced, NULL, n);
                s->silenced += n;
            }
        }
        LeaveCriticalSection(&g_qcs);
    }
    return 0;
}

static int stream_create(VBuf *v) {
    if (g_nst >= MAX_STREAM || !v->ds || !o_CreateSoundBuffer) return -1;
    typedef struct { DWORD size, flags, bytes, reserved; void *fmt; GUID alg; } BufferDesc;   // DSBUFFERDESC
    DWORD size = v->bytesPerSec - v->bytesPerSec % v->align;
    BufferDesc d = {sizeof d, 0x18080, size, 0, v->fmt, {0}};   // GETCURRENTPOSITION2 | GLOBALFOCUS | CTRLVOLUME
    void *out = NULL;
    if (FAILED(o_CreateSoundBuffer(v->ds, &d, &out, NULL)) || !out) { tas_log("WARNING: sound output buffer could not be created"); return -1; }
    uint8_t *q = HeapAlloc(GetProcessHeap(), 0, size);
    if (!q) { ((ULONG (WINAPI *)(void *))vt_of(out)[2])(out); return -1; }
    EnterCriticalSection(&g_qcs);
    Stream *s = &g_st[g_nst];
    memset(s, 0, sizeof *s);
    s->out = out; s->size = size; s->align = v->align; s->bytesPerSec = v->bytesPerSec; s->silence = v->silence;
    s->q = q; s->qcap = size - size % v->align;
    s->volume = 1;                     // unknown yet
    WORD tag = ((const WORD *)v->fmt)[0], bits = ((const WORD *)v->fmt)[7];
    if (tag == 0xFFFE) tag = (WORD)((const DWORD *)(v->fmt + 24))[0];     // WAVEFORMATEXTENSIBLE subformat
    s->sample = tag == 1 && bits == 16 ? 1 : tag == 3 && bits == 32 ? 2 : 0;
    s->faded = 1;
    out_write(s, 0, NULL, size);
    o_GetCurrentPosition[vt_index(out)](out, &s->lastPlay, &(DWORD){0});
    o_Play[vt_index(out)](out, 0, 0, 1);
    o_GetCurrentPosition[vt_index(out)](out, &s->lastPlay, &(DWORD){0});
    int idx = g_nst++;
    LeaveCriticalSection(&g_qcs);
    if (!g_feeder) g_feeder = CreateThread(NULL, 0, feeder, NULL, 0, NULL);
    tas_log("sound output %d: %lu bytes/s", idx, v->bytesPerSec);
    return idx;
}
static void stream_push(Stream *s, const uint8_t *p, DWORD n) {
    if (n > s->qcap) { p += n - s->qcap; n = s->qcap; }
    if (s->qlen + n > s->qcap) {               // too far ahead of real time (fast-forward): drop the oldest
        DWORD drop = s->qlen + n - s->qcap;
        if (g_soundLog && ++g_drop <= 50) tas_log("SOUND queue full, dropped %lu bytes, frame %u", drop, tas_frame());
        s->qhead = (s->qhead + drop) % s->qcap;
        s->qlen -= drop;
    }
    DWORD tail = (s->qhead + s->qlen) % s->qcap;
    DWORD a = n < s->qcap - tail ? n : s->qcap - tail;
    memcpy(s->q + tail, p, a);
    memcpy(s->q, p + a, n - a);
    s->qlen += n;
}
static void stream_clear(int i) {
    if (i < 0) return;
    EnterCriticalSection(&g_qcs);
    g_st[i].qlen = 0;
    LeaveCriticalSection(&g_qcs);
}

// ---------------------------------------------------------------- export: sound of other components
// Other components of the process play some sound themselves, in real time: the opening movie's
// player (DirectShow's DirectSound renderer) has its own device and buffers, which are not
// virtualised. While a video is exported, what such a buffer actually plays goes into the export:
// - every write into it is mirrored (the buffer itself may already hold newer sound where the
//   play position just passed);
// - whenever the player looks at its play position (about every 100 ms), we look too;
// - at each frame end the play position is advanced by the real time since that look (it plays
//   by the real clock), and the sound up to there is taken from the mirror. So the export gets
//   it frame by frame, as it is heard.
#define MAX_EXT 8
typedef struct {
    void    *b;
    uint8_t *base;                     // the buffer's memory
    DWORD    size, align, bytesPerSec;
    uint8_t  fmt[64];
    uint8_t *mirror;                   // what was last written at each position
    DWORD    lastPlay;                 // play position at the last look
    int      playing;                  // it was playing at the last look
    int64_t  playAbs;                  // bytes played up to the last look
    int64_t  lookT;                    // real QPC of the last look
    int64_t  capAbs;                   // bytes taken so far
    DWORD    capPos;                   // buffer position of the next byte to take
    LONG     volume;
    int64_t  lastUse;                  // real QPC of the last look or write
    uint8_t *pend;                     // taken, not yet given to the export
    DWORD    pendLen, pendCap;
} ExtBuf;
static ExtBuf g_ext[MAX_EXT];
static CRITICAL_SECTION g_extcs;
static int64_t g_qpf;

static int64_t real_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// A buffer of the game or one of our outputs? (Read without locks: entries are only ever added and
// are complete before the buffer is used.)
static int game_or_own(void *b) {
    for (int i = 0; i < MAX_VBUF; i++) if (g_vb[i].obj[0] == b || g_vb[i].obj[1] == b) return 1;
    for (int i = 0; i < MAX_STREAM; i++) if (g_st[i].out == b) return 1;
    return 0;
}

static ExtBuf *ext_find(void *b) {
    for (int i = 0; i < MAX_EXT; i++) if (g_ext[i].b == b) return &g_ext[i];
    return NULL;
}

// Take n bytes from the mirror at the take position.
static void ext_take(ExtBuf *e, DWORD n) {
    if (!n) return;
    DWORD cap = e->bytesPerSec * 2;
    if (!e->pend) { e->pend = HeapAlloc(GetProcessHeap(), 0, cap); e->pendCap = e->pend ? cap : 0; }
    if (!e->pend) return;
    DWORD keep = n > e->pendCap ? e->pendCap : n;
    DWORD skip = n - keep;                             // more than the queue holds: the newest part
    e->capPos = (e->capPos + skip) % e->size;
    if (e->pendLen + keep > e->pendCap) {              // nobody took it (no frames): keep the newest
        DWORD drop = e->pendLen + keep - e->pendCap;
        memmove(e->pend, e->pend + drop, e->pendLen - drop);
        e->pendLen -= drop;
    }
    DWORD first = e->size - e->capPos < keep ? e->size - e->capPos : keep;
    memcpy(e->pend + e->pendLen, e->mirror + e->capPos, first);
    if (keep > first) memcpy(e->pend + e->pendLen + first, e->mirror, keep - first);
    e->pendLen += keep;
    e->capPos = (e->capPos + keep) % e->size;
    e->capAbs += n;
}

// Look at the real play position; take what was played and not taken yet.
static void ext_look(ExtBuf *e, int idx) {
    DWORD play = 0, write = 0, st = 0;
    if (FAILED(o_GetCurrentPosition[idx](e->b, &play, &write)) || FAILED(o_GetStatus[idx](e->b, &st)) || play >= e->size) return;
    DWORD d = (play + e->size - e->lastPlay) % e->size;
    d -= d % e->align;
    int64_t now = real_now();
    if (!(st & 1) || d > e->size / 4 * 3) {            // stopped, or lost track: start again from here
        if (e->playing && !(st & 1)) {                 // it stopped: what it played until then
            e->playAbs += d;
            if (e->playAbs > e->capAbs) ext_take(e, (DWORD)(e->playAbs - e->capAbs));
        }
        e->lastPlay = play - play % e->align;
        e->capPos = e->lastPlay;
        e->capAbs = e->playAbs;
        e->playing = (st & 1) != 0;
        e->lookT = now;
        return;
    }
    e->lastPlay = (e->lastPlay + d) % e->size;
    e->playAbs += d;
    e->playing = 1;
    e->lookT = now;
    if (e->playAbs > e->capAbs) ext_take(e, (DWORD)(e->playAbs - e->capAbs));
}

// Start watching buffer b (or watch it anew: a buffer unused for a second may be another one).
static ExtBuf *ext_init(ExtBuf *e, void *b, int idx) {
    int64_t now = real_now();
    if (!e) {
        for (int i = 0; i < MAX_EXT && !e; i++) if (!g_ext[i].b || now - g_ext[i].lastUse > 2 * g_qpf) e = &g_ext[i];
        if (!e) return NULL;
    }
    e->b = NULL;
    uint8_t fmt[64] = {0};
    DWORD got = 0;
    typedef HRESULT (WINAPI *GetFormat_t)(void *, void *, DWORD, DWORD *);
    if (FAILED(((GetFormat_t)vt_of(b)[5])(b, fmt, sizeof fmt, &got))) return NULL;
    WORD align = ((const WORD *)fmt)[6];
    DWORD bps = ((const DWORD *)fmt)[2];
    void *p1, *p2;
    DWORD n1, n2;
    if (!align || !bps || FAILED(o_Lock[idx](b, 0, 0, &p1, &n1, &p2, &n2, 2 /* DSBLOCK_ENTIREBUFFER */)) || !p1) return NULL;
    if (e->size != n1 || !e->mirror) {
        if (e->mirror) HeapFree(GetProcessHeap(), 0, e->mirror);
        e->mirror = HeapAlloc(GetProcessHeap(), 0, n1);
    }
    if (e->mirror) memcpy(e->mirror, p1, n1);       // what it holds now (written before we looked)
    o_Unlock[idx](b, p1, 0, NULL, 0);
    if (!e->mirror) { e->size = 0; return NULL; }
    e->base = p1;
    e->size = n1;
    e->align = align;
    e->bytesPerSec = bps;
    memcpy(e->fmt, fmt, sizeof fmt);
    DWORD play = 0, write, st = 0;
    if (FAILED(o_GetCurrentPosition[idx](b, &play, &write)) || play >= n1) play = 0;
    o_GetStatus[idx](b, &st);
    e->lastPlay = play - play % align;
    e->capPos = e->lastPlay;
    e->playAbs = e->capAbs = 0;
    e->playing = (st & 1) != 0;
    e->lookT = now;
    e->volume = 0;
    e->pendLen = 0;
    e->lastUse = now;
    e->b = b;
    tas_log("video export also takes the sound of another component: buffer %p, %lu bytes, %lu bytes/s",
            b, (unsigned long)n1, (unsigned long)bps);
    return e;
}

static void ext_seen(void *b, int idx) {
    if (game_or_own(b)) return;
    EnterCriticalSection(&g_extcs);
    ExtBuf *e = ext_find(b);
    if (!e || real_now() - e->lastUse > g_qpf) e = ext_init(e, b, idx);
    if (e) {
        ext_look(e, idx);
        e->lastUse = real_now();
    }
    LeaveCriticalSection(&g_extcs);
}

static HRESULT WINAPI h_Unlock(void *b, void *p1, DWORD n1, void *p2, DWORD n2) {
    int idx = vt_index(b);
    if (avdump_active() && !game_or_own(b)) {
        EnterCriticalSection(&g_extcs);
        ExtBuf *e = ext_find(b);
        if (e && real_now() - e->lastUse <= g_qpf) {
            ext_look(e, idx);                          // before the mirror takes the new sound
            uintptr_t off = (uintptr_t)p1 - (uintptr_t)e->base;
            if (p1 && off < e->size && n1 <= e->size - off) memcpy(e->mirror + off, p1, n1);
            if (p2 && (uint8_t *)p2 == e->base && n2 <= e->size) memcpy(e->mirror, p2, n2);
            LONG vol;
            if (SUCCEEDED(((GetVolume_t)vt_of(b)[6])(b, &vol))) e->volume = vol;
            e->lastUse = real_now();
        }
        LeaveCriticalSection(&g_extcs);
    }
    return o_Unlock[idx](b, p1, n1, p2, n2);
}

// Its player stops it: take what it played until now.
static void ext_stop(void *b, int idx) {
    if (game_or_own(b)) return;
    EnterCriticalSection(&g_extcs);
    ExtBuf *e = ext_find(b);
    if (e && real_now() - e->lastUse <= g_qpf) {
        ext_look(e, idx);
        e->playing = 0;
    }
    LeaveCriticalSection(&g_extcs);
}

// Frame end: the sound the other components played until now goes to the export.
static void ext_flush(void) {
    int64_t now = real_now();
    EnterCriticalSection(&g_extcs);
    for (int i = 0; i < MAX_EXT; i++) {
        ExtBuf *e = &g_ext[i];
        if (!e->b) continue;
        if (e->playing && now - e->lookT < g_qpf / 5) {    // it plays on by the real clock since the last look
            int64_t est = e->playAbs + (now - e->lookT) * e->bytesPerSec / g_qpf;
            est -= est % e->align;
            if (est > e->capAbs) ext_take(e, (DWORD)(est - e->capAbs));
        }
        if (e->pendLen) avdump_audio(MAX_VBUF + i, e->fmt, e->pend, e->pendLen, NULL, 0, e->volume, 1);
        e->pendLen = 0;
    }
    LeaveCriticalSection(&g_extcs);
}

// Is another component playing sound in real time now (a movie)? The export then runs in real time.
int dsound_realtime_media(void) {
    int64_t now = real_now();
    int r = 0;
    EnterCriticalSection(&g_extcs);
    for (int i = 0; i < MAX_EXT && !r; i++) r = g_ext[i].b && g_ext[i].playing && now - g_ext[i].lastUse < g_qpf * 3 / 10;
    LeaveCriticalSection(&g_extcs);
    return r;
}

// Nothing to do while the TAS is paused: no frames, so the outputs play silence.
void dsound_pause(int paused) { (void)paused; }

// Called right after the frame counter advanced and before the game's threads run for the new
// frame: the sound between each playing buffer's previous and current virtual position is final
// (the mixer only writes behind the play position) and goes to the output queue.
void dsound_frame(void) {
    EnterCriticalSection(&g_cs);
    int64_t now = tas_vtime();
    for (int i = 0; i < g_nvb; i++) {
        VBuf *v = &g_vb[i];
        if (!v->playing || !v->size) continue;
        DWORD cur = vb_pos(v, now), n;
        if (v->looping) n = (cur + v->size - v->lastPos) % v->size;
        else n = cur > v->lastPos ? cur - v->lastPos : 0;
        if (n > v->bytesPerSec / 4) { v->lastPos = cur; continue; }   // a jump (time advanced at once)
        if (!n) continue;
        if (v->stream < 0) v->stream = stream_create(v);
        if (v->stream < 0) { v->lastPos = cur; continue; }
        void *b = v->obj[0], *p1, *p2;
        DWORD n1, n2;
        if (SUCCEEDED(lock_of(b)(b, v->lastPos, n, &p1, &n1, &p2, &n2, 0))) {
            Stream *s = &g_st[v->stream];
            if (g_soundLog && i < 64) {        // did the mixer write this part since we last took it?
                if (!g_shadow[i]) g_shadow[i] = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, v->size);
                uint8_t *sh = g_shadow[i];
                if (sh) {
                    int same = !memcmp(sh + v->lastPos, p1, n1) && (!p2 || !memcmp(sh, p2, n2));
                    int zero = 1;
                    for (DWORD k = 0; k < n1 && zero; k++) zero = !((uint8_t *)p1)[k];
                    if (same && !zero && ++g_stale <= 100)
                        tas_log("SOUND stale data at frame %u: pos %lu..%lu (buffer %lu)", tas_frame(), v->lastPos, cur, v->size);
                    memcpy(sh + v->lastPos, p1, n1);
                    if (p2) memcpy(sh, p2, n2);
                }
            }
            LONG vol = 0;
            int volOk = SUCCEEDED(((GetVolume_t)vt_of(b)[6])(b, &vol));
            avdump_audio(i, v->fmt, p1, n1, p2, n2, volOk ? vol : 0, 0);
            EnterCriticalSection(&g_qcs);
            if (!tas_export_fast()) {
                stream_push(s, p1, n1);
                if (p2 && n2) stream_push(s, p2, n2);
            }
            if (volOk && vol != s->volume) {
                s->volume = vol;
                ((SetVolume_t)vt_of(s->out)[15])(s->out, vol);
            }
            LeaveCriticalSection(&g_qcs);
            unlock_of(b)(b, p1, 0, p2, 0);
        }
        v->lastPos = cur;
    }
    LeaveCriticalSection(&g_cs);
    if (avdump_active()) ext_flush();
}

// ---------------------------------------------------------------- savestates
static void buf_released(void *b) {
    EnterCriticalSection(&g_cs);
    VBuf *v = vb_find(b);
    if (v && v->playing) { vb_rebase(v, tas_vtime()); v->playing = 0; }
    if (v) stream_clear(v->stream);
    LeaveCriticalSection(&g_cs);
}
static void buf_save(void *b, uint32_t extra[2]) {
    EnterCriticalSection(&g_cs);
    VBuf *v = vb_find(b);
    if (v) {
        extra[0] = (v->playing ? 1u : 0u) | (v->looping ? 4u : 0u);
        extra[1] = vb_pos(v, tas_vtime());
    }
    LeaveCriticalSection(&g_cs);
}
static void buf_load(void *b, const uint32_t *extra) {
    if (!g_nbufVt) return;
    EnterCriticalSection(&g_cs);
    VBuf *v = vb_find(b);
    if (v) {
        v->playing = extra && (extra[0] & 1);
        v->looping = extra && (extra[0] & 4);
        v->pos0 = extra ? extra[1] : 0;
        v->vstart = tas_vtime();
        v->lastPos = v->pos0;
        stream_clear(v->stream);       // sound of the abandoned timeline
    }
    LeaveCriticalSection(&g_cs);
}
static const ObjKindOps kBufferOps = {buf_released, buf_save, buf_load};

// ---------------------------------------------------------------- creation
static HRESULT WINAPI h_CreateSoundBuffer(void *ds, const void *desc, void **pp, void *outer) {
    HRESULT hr = o_CreateSoundBuffer(ds, desc, pp, outer);
    if (FAILED(hr) || !pp || !*pp || !our_device(ds)) return hr;
    const DWORD *d = (const DWORD *)desc;              // DSBUFFERDESC: size, flags, bytes, reserved, format
    int primary = d && (d[1] & 1);                     // DSBCAPS_PRIMARYBUFFER
    tas_obj_track(*pp, primary ? OBJ_DSPRIMARY : OBJ_DSBUFFER);
    if (primary || !d) return hr;
    typedef struct { WORD tag, channels; DWORD rate, nAvgBytesPerSec; WORD nBlockAlign, bits; } WaveFormat;   // WAVEFORMATEX
    const WaveFormat *wf = (const WaveFormat *)(uintptr_t)d[4];
    EnterCriticalSection(&g_cs);
    if (g_nvb < MAX_VBUF && wf && wf->nAvgBytesPerSec) {
        VBuf *v = &g_vb[g_nvb++];
        memset(v, 0, sizeof *v);
        v->obj[0] = *pp;
        v->ds = ds;
        v->stream = -1;
        v->size = d[2];
        v->bytesPerSec = wf->nAvgBytesPerSec;
        v->align = wf->nBlockAlign ? wf->nBlockAlign : 1;
        size_t fs = wf->tag == 1 ? 16 : 18 + ((const WORD *)wf)[8];          // PCM: no cbSize
        if (fs > sizeof v->fmt) fs = sizeof v->fmt;
        memcpy(v->fmt, wf, fs);
        if (fs == 16) ((WORD *)v->fmt)[8] = 0;
        v->silence = wf->bits == 8 ? 0x80 : 0;
    }
    LeaveCriticalSection(&g_cs);
    hook_buffer_vt(*pp);
    return hr;
}
static HRESULT WINAPI h_DuplicateSoundBuffer(void *ds, void *orig, void **pp) {
    HRESULT hr = o_DuplicateSoundBuffer(ds, orig, pp);
    if (FAILED(hr) || !pp || !*pp || !our_device(ds)) return hr;
    tas_obj_track(*pp, OBJ_DSBUFFER);
    EnterCriticalSection(&g_cs);
    VBuf *src = vb_find(orig);
    if (src && g_nvb < MAX_VBUF) {
        VBuf *v = &g_vb[g_nvb++];
        *v = *src;
        v->obj[0] = *pp;
        v->obj[1] = NULL;
        v->playing = 0;
        v->pos0 = 0;
        v->lastPos = 0;
        v->stream = -1;
    }
    LeaveCriticalSection(&g_cs);
    return hr;
}

static void hook_device(void *ds) {
    EnterCriticalSection(&g_cs);
    if (!our_device(ds) && g_ndevs < 8) {
        g_devs[g_ndevs++] = ds;
        ((ULONG (WINAPI *)(void *))vt_of(ds)[1])(ds);       // AddRef, never released
    }
    LeaveCriticalSection(&g_cs);
    if (g_dsVt) return;
    void **vt = vt_of(ds);
    if (patch(vt, 3, (void *)h_CreateSoundBuffer, (void **)&o_CreateSoundBuffer) &&
        patch(vt, 5, (void *)h_DuplicateSoundBuffer, (void **)&o_DuplicateSoundBuffer)) {
        g_dsVt = vt;
        tas_log("DirectSound hooked");
    } else tas_log("ERROR: DirectSound hook failed");
}

static int sound_disabled(void) {               // test switch: behave as without a sound device
    wchar_t env[4];
    return GetEnvironmentVariableW(L"LOUKE_NOSOUND", env, 4) != 0;
}

static HRESULT WINAPI h_DirectSoundCreate8(const GUID *dev, void **out, void *outer) {
    if (sound_disabled()) { if (out) *out = NULL; return (HRESULT)0x88780078; }   // DSERR_NODRIVER
    HRESULT hr = o_DirectSoundCreate8(dev, out, outer);
    if (SUCCEEDED(hr) && out && *out) hook_device(*out);
    return hr;
}
static HRESULT WINAPI h_CoCreateInstance(REFCLSID clsid, void *outer, DWORD ctx, REFIID iid, void **out) {
    int ds = clsid && (IsEqualGUID(clsid, &kCLSID_DirectSound8) || IsEqualGUID(clsid, &kCLSID_DirectSound));
    if (ds && sound_disabled()) { if (out) *out = NULL; return REGDB_E_CLASSNOTREG; }
    HRESULT hr = o_CoCreateInstance(clsid, outer, ctx, iid, out);
    if (ds && SUCCEEDED(hr) && out && *out) hook_device(*out);
    return hr;
}

int dsound_install(void) {
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_qcs);
    InitializeCriticalSection(&g_extcs);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    wchar_t env[4];
    g_soundLog = GetEnvironmentVariableW(L"LOUKE_SOUNDLOG", env, 4) != 0;
    tas_obj_register_kind(OBJ_DSBUFFER, &kBufferOps);
    int a = tas_hook_iat("DSOUND.dll", NULL, 11, (void *)h_DirectSoundCreate8, (void **)&o_DirectSoundCreate8) ||
            tas_hook_iat("DSOUND.dll", "DirectSoundCreate8", 0, (void *)h_DirectSoundCreate8, (void **)&o_DirectSoundCreate8);
    int b = tas_hook_iat("ole32.dll", "CoCreateInstance", 0, (void *)h_CoCreateInstance, (void **)&o_CoCreateInstance);
    return a || b;
}
