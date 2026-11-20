// Video export: the frames and the sound of the game, by game frames, into an MP4 file
// (H.264 video, AAC stereo 48 kHz) written with Media Foundation (part of Windows).
//
// One video frame per game frame, timed by the virtual clock; the sound of each frame is exactly
// what the game's sound buffers played during it (see dsound.c), mixed and converted to stereo.
// Nothing depends on how fast the game runs in real time, so loading stalls leave no trace and
// the export can run as fast as the computer allows.
//
// The game thread hands each finished frame (pixels + sound) to an encoder thread through a short
// queue; Media Foundation is loaded only when an export starts.
#define COBJMACROS
#include "tas.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE     48000
#define OUT_CH       2
#define QUEUE_FRAMES 6

typedef HRESULT (WINAPI *MFStartup_t)(ULONG, DWORD);
typedef HRESULT (WINAPI *MFShutdown_t)(void);
typedef HRESULT (WINAPI *MFCreateAttributes_t)(IMFAttributes **, UINT32);
typedef HRESULT (WINAPI *MFCreateMediaType_t)(IMFMediaType **);
typedef HRESULT (WINAPI *MFCreateMemoryBuffer_t)(DWORD, IMFMediaBuffer **);
typedef HRESULT (WINAPI *MFCreateSample_t)(IMFSample **);
typedef HRESULT (WINAPI *MFCreateSinkWriterFromURL_t)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **);
static MFStartup_t pMFStartup;
static MFShutdown_t pMFShutdown;
static MFCreateAttributes_t pMFCreateAttributes;
static MFCreateMediaType_t pMFCreateMediaType;
static MFCreateMemoryBuffer_t pMFCreateMemoryBuffer;
static MFCreateSample_t pMFCreateSample;
static MFCreateSinkWriterFromURL_t pMFCreateSinkWriterFromURL;

typedef struct {
    int stop;
    UINT w, h;
    uint8_t *pixels;                   // w * h * 4, top-down BGRX
    int16_t *pcm;                      // OUT_CH * samples
    DWORD samples;
    uint64_t aStart;                   // index of its first sound sample in the export
    int64_t time, dur;                 // 100 ns
} Item;

static CRITICAL_SECTION g_cs;
static int g_csInit;
static CONDITION_VARIABLE g_cvItem, g_cvSpace;
static Item *g_q[QUEUE_FRAMES];
static int g_qn, g_qhead;
static HANDLE g_thread;
static volatile LONG g_state;          // AVDUMP_IDLE / RUNNING / FINISHING / DONE / FAILED
static volatile LONG g_frames;
static wchar_t g_path[MAX_PATH];
static int g_quality;
static uint32_t g_errCode, g_errHr;

// per frame being assembled (game thread)
static uint8_t *g_pix;
static UINT g_pw, g_ph;
static int64_t g_time;                 // start of the frame being assembled in the export (100 ns)
static int64_t g_dur;                  // its duration
static uint32_t g_frameNo;             // frames finished
// Sound: every game buffer is a source whose samples follow each other without gaps in the
// output (a buffer that plays on continues exactly where it stopped the frame before), mixed in
// float stereo. g_mix[0] is output sample g_samplesOut; whatever lies beyond the frame's end
// stays for the next frame.
#define MAX_SRC 72                     // the game's buffers (64), then other components' (8)
typedef struct {
    uint32_t frame;                    // g_frameNo + 1 of the last frame it gave sound in
    uint8_t  fmt[40];                  // its format then
    int64_t  pos;                      // output sample its next sound goes to
    double   phase;                    // resampling: next output sample, in its samples after the last one taken
    float    last[OUT_CH];             // its last sample, for interpolation
} Src;
static Src g_src[MAX_SRC];
static float *g_mix;                   // OUT_CH floats per output sample; zero beyond g_mixLen
static DWORD g_mixCap, g_mixLen;
static float *g_tmp;                   // one source's sound of this frame, in stereo
static DWORD g_tmpCap;
static int64_t g_frameEnd;             // output sample at the end of the frame being assembled
static uint64_t g_samplesOut;          // sound samples written so far

static void fail(uint32_t code, HRESULT hr) {
    static const char *const kWhat[] = {"", "Media Foundation is not available", "cannot create the output file",
        "the video encoder could not be set up", "writing the video failed", "finishing the file failed",
        "cannot start the encoder thread"};
    g_errCode = code;
    g_errHr = (uint32_t)hr;
    tas_log("export: %s (0x%08lX)", code < sizeof kWhat / sizeof kWhat[0] ? kWhat[code] : "?", (unsigned long)hr);
    InterlockedExchange(&g_state, AVDUMP_FAILED);
    if (g_csInit) {                    // a game thread waiting for queue space goes on
        EnterCriticalSection(&g_cs);
        WakeAllConditionVariable(&g_cvSpace);
        LeaveCriticalSection(&g_cs);
    }
}

static int load_mf(void) {
    if (pMFCreateSinkWriterFromURL) return 1;
    HMODULE plat = LoadLibraryW(L"mfplat.dll"), rw = LoadLibraryW(L"mfreadwrite.dll");
    if (!plat || !rw) return 0;
    pMFStartup = (MFStartup_t)(void *)GetProcAddress(plat, "MFStartup");
    pMFShutdown = (MFShutdown_t)(void *)GetProcAddress(plat, "MFShutdown");
    pMFCreateAttributes = (MFCreateAttributes_t)(void *)GetProcAddress(plat, "MFCreateAttributes");
    pMFCreateMediaType = (MFCreateMediaType_t)(void *)GetProcAddress(plat, "MFCreateMediaType");
    pMFCreateMemoryBuffer = (MFCreateMemoryBuffer_t)(void *)GetProcAddress(plat, "MFCreateMemoryBuffer");
    pMFCreateSample = (MFCreateSample_t)(void *)GetProcAddress(plat, "MFCreateSample");
    pMFCreateSinkWriterFromURL = (MFCreateSinkWriterFromURL_t)(void *)GetProcAddress(rw, "MFCreateSinkWriterFromURL");
    return pMFStartup && pMFShutdown && pMFCreateAttributes && pMFCreateMediaType && pMFCreateMemoryBuffer &&
           pMFCreateSample && pMFCreateSinkWriterFromURL;
}

static UINT64 pack2(UINT32 hi, UINT32 lo) { return ((UINT64)hi << 32) | lo; }

static const UINT32 kBitrate[4] = {8000000, 16000000, 32000000, 60000000};

// Create the writer for frames of w x h. hw: allow hardware encoders.
static IMFSinkWriter *open_writer(UINT w, UINT h, int hw, DWORD *vs, DWORD *as) {
    IMFAttributes *attr = NULL;
    IMFSinkWriter *wr = NULL;
    IMFMediaType *t = NULL;
    HRESULT hr = pMFCreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUINT32(attr, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hw ? TRUE : FALSE);
    if (SUCCEEDED(hr)) hr = pMFCreateSinkWriterFromURL(g_path, NULL, attr, &wr);
    if (attr) IMFAttributes_Release(attr);
    if (FAILED(hr)) { fail(EXPERR_CREATE_FILE, hr); return NULL; }
    UINT32 rate = kBitrate[g_quality < 0 || g_quality > 3 ? 1 : g_quality];
    // video: H.264 out, RGB32 (top-down) in
    if (SUCCEEDED(hr = pMFCreateMediaType(&t))) {
        IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
        IMFMediaType_SetUINT32(t, &MF_MT_AVG_BITRATE, rate);
        IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        IMFMediaType_SetUINT32(t, &MF_MT_MPEG2_PROFILE, 100);      // eAVEncH264VProfile_High
        IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, pack2(w, h));
        IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE, pack2(60, 1));
        IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, pack2(1, 1));
        hr = IMFSinkWriter_AddStream(wr, t, vs);
        IMFMediaType_Release(t);
    }
    if (SUCCEEDED(hr) && SUCCEEDED(hr = pMFCreateMediaType(&t))) {
        IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
        IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        IMFMediaType_SetUINT32(t, &MF_MT_DEFAULT_STRIDE, w * 4);
        IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, pack2(w, h));
        IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE, pack2(60, 1));
        IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, pack2(1, 1));
        hr = IMFSinkWriter_SetInputMediaType(wr, *vs, t, NULL);
        IMFMediaType_Release(t);
    }
    // sound: AAC 192 kbit/s out, PCM 16-bit stereo in
    if (SUCCEEDED(hr) && SUCCEEDED(hr = pMFCreateMediaType(&t))) {
        IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, OUT_RATE);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, OUT_CH);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
        hr = IMFSinkWriter_AddStream(wr, t, as);
        IMFMediaType_Release(t);
    }
    if (SUCCEEDED(hr) && SUCCEEDED(hr = pMFCreateMediaType(&t))) {
        IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, OUT_RATE);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, OUT_CH);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, OUT_CH * 2);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, OUT_RATE * OUT_CH * 2);
        hr = IMFSinkWriter_SetInputMediaType(wr, *as, t, NULL);
        IMFMediaType_Release(t);
    }
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_BeginWriting(wr);
    if (FAILED(hr)) {
        IMFSinkWriter_Release(wr);
        DeleteFileW(g_path);
        if (!hw) fail(EXPERR_ENCODER, hr);
        return NULL;
    }
    tas_log("export: %ux%u, %u kbit/s, %s encoder", w, h, rate / 1000, hw ? "hardware or software" : "software");
    return wr;
}

static HRESULT write_sample(IMFSinkWriter *wr, DWORD stream, const void *data, DWORD bytes, int64_t time, int64_t dur,
                            UINT rowBytes, UINT rows, LONG srcPitch) {
    IMFMediaBuffer *buf = NULL;
    IMFSample *s = NULL;
    BYTE *p;
    HRESULT hr = pMFCreateMemoryBuffer(bytes, &buf);
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(buf, &p, NULL, NULL);
    if (SUCCEEDED(hr)) {
        if (rows) for (UINT y = 0; y < rows; y++) memcpy(p + (size_t)y * rowBytes, (const uint8_t *)data + (size_t)y * srcPitch, rowBytes);
        else memcpy(p, data, bytes);
        IMFMediaBuffer_Unlock(buf);
        hr = IMFMediaBuffer_SetCurrentLength(buf, bytes);
    }
    if (SUCCEEDED(hr)) hr = pMFCreateSample(&s);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(s, buf);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(s, time);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(s, dur);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_WriteSample(wr, stream, s);
    if (s) IMFSample_Release(s);
    if (buf) IMFMediaBuffer_Release(buf);
    return hr;
}

static Item *take(void) {
    EnterCriticalSection(&g_cs);
    while (!g_qn) SleepConditionVariableCS(&g_cvItem, &g_cs, INFINITE);
    Item *it = g_q[g_qhead];
    g_qhead = (g_qhead + 1) % QUEUE_FRAMES;
    g_qn--;
    WakeConditionVariable(&g_cvSpace);
    LeaveCriticalSection(&g_cs);
    return it;
}
static void free_item(Item *it) { if (it) { free(it->pixels); free(it->pcm); free(it); } }

static DWORD WINAPI encoder(void *arg) {
    (void)arg;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMFSinkWriter *wr = NULL;
    DWORD vs = 0, as = 0;
    UINT w = 0, h = 0;
    int started = SUCCEEDED(pMFStartup(MF_VERSION, MFSTARTUP_FULL));
    if (!started) fail(EXPERR_NO_MF, E_FAIL);
    for (;;) {
        Item *it = take();
        if (it->stop || g_state == AVDUMP_FAILED) { free_item(it); break; }
        if (!wr) {
            w = it->w & ~1u;           // the encoder wants even sizes
            h = it->h & ~1u;
            wr = open_writer(w, h, 1, &vs, &as);
            if (!wr && g_state != AVDUMP_FAILED) wr = open_writer(w, h, 0, &vs, &as);
            if (!wr) { free_item(it); continue; }
        }
        HRESULT hr = S_OK;
        if (it->pixels) {
            UINT cw = it->w < w ? it->w : w, ch = it->h < h ? it->h : h;
            if (cw == w && ch == h)
                hr = write_sample(wr, vs, it->pixels, w * h * 4, it->time, it->dur, w * 4, h, (LONG)it->w * 4);
            else {                     // the game changed resolution: keep the first size
                uint8_t *f = calloc((size_t)w * h, 4);
                if (f) {
                    for (UINT y = 0; y < ch; y++) memcpy(f + (size_t)y * w * 4, it->pixels + (size_t)y * it->w * 4, (size_t)cw * 4);
                    hr = write_sample(wr, vs, f, w * h * 4, it->time, it->dur, 0, 0, 0);
                    free(f);
                }
            }
        }
        if (SUCCEEDED(hr) && it->samples)
            hr = write_sample(wr, as, it->pcm, it->samples * OUT_CH * 2, (int64_t)(it->aStart * 10000000 / OUT_RATE),
                              (int64_t)((uint64_t)it->samples * 10000000 / OUT_RATE), 0, 0, 0);
        free_item(it);
        if (FAILED(hr)) { fail(EXPERR_WRITE, hr); IMFSinkWriter_Release(wr); wr = NULL; }
        else InterlockedIncrement(&g_frames);
    }
    EnterCriticalSection(&g_cs);       // frames still queued (after a failure) are dropped
    while (g_qn) { free_item(g_q[g_qhead]); g_qhead = (g_qhead + 1) % QUEUE_FRAMES; g_qn--; }
    WakeAllConditionVariable(&g_cvSpace);
    LeaveCriticalSection(&g_cs);
    if (wr) {
        HRESULT hr = IMFSinkWriter_Finalize(wr);
        IMFSinkWriter_Release(wr);
        if (FAILED(hr)) fail(EXPERR_FINALIZE, hr);
    }
    if (started) pMFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    if (g_state != AVDUMP_FAILED) {
        InterlockedExchange(&g_state, AVDUMP_DONE);
        tas_log("export finished: %ld frames", g_frames);
    }
    return 0;
}

static void enqueue(Item *it) {
    EnterCriticalSection(&g_cs);
    while (g_qn == QUEUE_FRAMES && g_state != AVDUMP_FAILED) SleepConditionVariableCS(&g_cvSpace, &g_cs, INFINITE);
    if (g_qn == QUEUE_FRAMES) { LeaveCriticalSection(&g_cs); free_item(it); return; }   // the encoder gave up
    g_q[(g_qhead + g_qn) % QUEUE_FRAMES] = it;
    g_qn++;
    WakeConditionVariable(&g_cvItem);
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------- API (game thread)
int avdump_start(const wchar_t *path, int quality) {
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        InitializeConditionVariable(&g_cvItem);
        InitializeConditionVariable(&g_cvSpace);
        g_csInit = 1;
    }
    if (g_state == AVDUMP_RUNNING || g_state == AVDUMP_FINISHING) return 0;
    if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
    while (g_qn) { free_item(g_q[g_qhead]); g_qhead = (g_qhead + 1) % QUEUE_FRAMES; g_qn--; }
    g_errCode = EXPERR_NONE;
    g_errHr = 0;
    if (!load_mf()) { fail(EXPERR_NO_MF, E_FAIL); return 0; }
    wcsncpy(g_path, path, MAX_PATH - 1);
    g_path[MAX_PATH - 1] = 0;
    g_quality = quality;
    g_frames = 0;
    g_time = 0;
    g_dur = 10000000 / 60;
    g_frameNo = 0;
    g_frameEnd = 0;
    g_samplesOut = 0;
    memset(g_src, 0, sizeof g_src);
    if (g_mix) memset(g_mix, 0, (size_t)g_mixCap * OUT_CH * sizeof(float));
    g_mixLen = 0;
    free(g_pix);
    g_pix = NULL;
    InterlockedExchange(&g_state, AVDUMP_RUNNING);
    g_thread = CreateThread(NULL, 0, encoder, NULL, 0, NULL);
    if (!g_thread) { fail(EXPERR_THREAD, E_FAIL); return 0; }
    char u[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, g_path, -1, u, sizeof u, NULL, NULL);
    tas_log("export started: %s", u);
    return 1;
}
void avdump_stop(void) {
    if (g_state != AVDUMP_RUNNING) return;
    InterlockedExchange(&g_state, AVDUMP_FINISHING);
    Item *it = calloc(1, sizeof *it);
    if (it) { it->stop = 1; enqueue(it); }
}
void avdump_finish(DWORD ms) {
    avdump_stop();
    if (g_thread && g_state == AVDUMP_FINISHING) WaitForSingleObject(g_thread, ms);
}
int avdump_active(void) { return g_state == AVDUMP_RUNNING; }
int avdump_state(void) { return g_state; }
uint32_t avdump_frames(void) { return (uint32_t)g_frames; }
uint32_t avdump_error(uint32_t *hr) { if (hr) *hr = g_errHr; return g_errCode; }

void avdump_video(const uint8_t *bgrx, int pitch, UINT w, UINT h) {
    if (g_state != AVDUMP_RUNNING || !bgrx) return;
    if (w != g_pw || h != g_ph || !g_pix) {
        free(g_pix);
        g_pix = malloc((size_t)w * h * 4);
        g_pw = w; g_ph = h;
        if (!g_pix) return;
    }
    for (UINT y = 0; y < h; y++) memcpy(g_pix + (size_t)y * w * 4, bgrx + (size_t)y * pitch, (size_t)w * 4);
}

// The frame that just ended lasted dur (100 ns); its sound comes next (avdump_audio), then avdump_frame.
void avdump_frame_begin(int64_t dur) {
    if (g_state != AVDUMP_RUNNING) return;
    if (dur <= 0) dur = 10000000 / 60;
    g_dur = dur;
    g_frameEnd = (g_time + dur) * OUT_RATE / 10000000;
}

static int ensure(float **p, DWORD *cap, uint64_t n) {
    if (n <= *cap) return 1;
    if (n > 1u << 24) return 0;
    DWORD c = *cap ? *cap : 4096;
    while (c < n) c *= 2;
    float *m = realloc(*p, (size_t)c * OUT_CH * sizeof(float));
    if (!m) return 0;
    memset(m + (size_t)*cap * OUT_CH, 0, (size_t)(c - *cap) * OUT_CH * sizeof(float));
    *p = m;
    *cap = c;
    return 1;
}

// Sound of the frame that just ended from source src: its format (WAVEFORMATEX at fmt), the part
// of the buffer it played (two pieces when it wrapped around) and its volume (1/100 dB).
// realtime: a component that plays by the real clock (a movie): its sound does not arrive exactly
// with the frames, so it is placed 30 ms late and only placed anew when it ran out of that margin.
void avdump_audio(int src, const void *fmt, const void *p1, DWORD n1, const void *p2, DWORD n2, LONG volume, int realtime) {
    if (g_state != AVDUMP_RUNNING || src < 0 || src >= MAX_SRC || !p1) return;
    const WORD *f16 = fmt;
    WORD tag = f16[0], ch = f16[1], align = f16[6], bits = f16[7];
    DWORD rate = ((const DWORD *)fmt)[1], mask = 0;
    if (tag == 0xFFFE) { mask = ((const DWORD *)fmt)[5]; tag = (WORD)((const DWORD *)fmt)[6]; }
    int isFloat = tag == 3 && bits == 32, isInt = tag == 1 && (bits == 16 || bits == 8 || bits == 24 || bits == 32);
    if ((!isFloat && !isInt) || !ch || !align || !rate || (DWORD)ch * (bits / 8) > align) return;
    DWORD k1 = n1 / align, k2 = p2 ? n2 / align : 0, k = k1 + k2;
    if (!k || !ensure(&g_tmp, &g_tmpCap, k)) return;
    if (!mask) mask = ch == 1 ? 0x4 : ch == 2 ? 0x3 : ch == 4 ? 0x33 : ch == 6 ? 0x3F : ch == 8 ? 0x63F : 0x3;
    // gains of each source channel into left / right (ITU downmix), with the buffer's volume
    float vol = volume <= -10000 ? 0.0f : volume >= 0 ? 1.0f : powf(10.0f, (float)volume / 2000.0f);
    float gl[32], gr[32];
    DWORD bit = 1;
    for (WORD c = 0; c < ch && c < 32; c++) {
        while (bit && !(mask & bit)) bit <<= 1;
        DWORD sp = bit;
        if (bit) bit <<= 1;
        gl[c] = gr[c] = 0;
        if (ch == 1) { gl[c] = gr[c] = vol; continue; }
        switch (sp) {
        case 0x1: case 0x10: case 0x200: case 0x40: gl[c] = sp == 0x1 ? vol : 0.7071f * vol; break;   // FL, BL, SL, FLC
        case 0x2: case 0x20: case 0x400: case 0x80: gr[c] = sp == 0x2 ? vol : 0.7071f * vol; break;   // FR, BR, SR, FRC
        case 0x4: case 0x100: gl[c] = gr[c] = 0.7071f * vol; break;                                  // FC, BC
        default: break;                                                                             // LFE, top
        }
    }
    for (DWORD i = 0; i < k; i++) {
        const uint8_t *s = i < k1 ? (const uint8_t *)p1 + (size_t)i * align : (const uint8_t *)p2 + (size_t)(i - k1) * align;
        float l = 0, r = 0;
        for (WORD c = 0; c < ch && c < 32; c++) {
            float v;
            if (isFloat) { float f; memcpy(&f, s + c * 4, 4); v = f; }
            else if (bits == 16) { int16_t x; memcpy(&x, s + c * 2, 2); v = x / 32768.0f; }
            else if (bits == 8) v = (s[c] - 128) / 128.0f;
            else if (bits == 24) { const uint8_t *b = s + c * 3; v = (int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24) / 2147483648.0f; }
            else { int32_t x; memcpy(&x, s + c * 4, 4); v = x / 2147483648.0f; }
            l += v * gl[c];
            r += v * gr[c];
        }
        g_tmp[i * 2] = l;
        g_tmp[i * 2 + 1] = r;
    }
    // place it: right after its sound of the frame before, or (new, or more than 2 ms off) so that
    // it ends with this frame
    Src *s = &g_src[src];
    int fresh = !s->frame || memcmp(s->fmt, fmt, sizeof s->fmt) ||
                (realtime ? g_frameNo - (s->frame - 1) > 30 : s->frame != g_frameNo);
    if (fresh) { s->phase = 0; s->last[0] = g_tmp[0]; s->last[1] = g_tmp[1]; }
    double step = (double)rate / OUT_RATE, ph = s->phase;
    DWORD outN = ph > (double)(k - 1) ? 0 : (DWORD)floor(((double)(k - 1) - ph) / step) + 1;
    int64_t at = g_frameEnd - (int64_t)outN;
    if (realtime) {
        int64_t lat = OUT_RATE * 30 / 1000;
        if (fresh || s->pos < (int64_t)g_samplesOut || s->pos > at + 4 * lat) s->pos = at + lat;
    } else if (fresh || llabs(s->pos - at) > OUT_RATE / 500) s->pos = at;
    int64_t base = s->pos - (int64_t)g_samplesOut;
    if (base + (int64_t)outN > 0 && ensure(&g_mix, &g_mixCap, (uint64_t)(base + (int64_t)outN))) {
        for (DWORD j = 0; j < outN; j++) {
            int64_t o = base + j;
            if (o < 0) continue;
            double x = ph + j * step;
            long idx = (long)floor(x);
            float f = (float)(x - idx), a0, a1, b0, b1;
            if (idx < 0) { a0 = s->last[0]; a1 = s->last[1]; } else { a0 = g_tmp[idx * 2]; a1 = g_tmp[idx * 2 + 1]; }
            if (f > 0 && idx + 1 < (long)k) { b0 = g_tmp[(idx + 1) * 2]; b1 = g_tmp[(idx + 1) * 2 + 1]; }
            else { b0 = a0; b1 = a1; }
            g_mix[o * 2] += a0 + (b0 - a0) * f;
            g_mix[o * 2 + 1] += a1 + (b1 - a1) * f;
        }
        if (base + (int64_t)outN > (int64_t)g_mixLen) g_mixLen = (DWORD)(base + (int64_t)outN);
    }
    s->pos += outN;
    s->phase = ph + outN * step - k;
    s->last[0] = g_tmp[(k - 1) * 2];
    s->last[1] = g_tmp[(k - 1) * 2 + 1];
    s->frame = g_frameNo + 1;
    memcpy(s->fmt, fmt, sizeof s->fmt);
}

// The frame is finished: its picture (or the previous one) and its sound go to the encoder.
void avdump_frame(void) {
    if (g_state != AVDUMP_RUNNING) return;
    Item *it = calloc(1, sizeof *it);
    if (!it) return;
    it->time = g_time;
    it->dur = g_dur;
    if (g_pix) {                       // a frame without a new picture repeats the last one
        it->w = g_pw; it->h = g_ph;
        it->pixels = malloc((size_t)g_pw * g_ph * 4);
        if (it->pixels) memcpy(it->pixels, g_pix, (size_t)g_pw * g_ph * 4);
    }
    g_time += g_dur;
    int64_t n = g_frameEnd - (int64_t)g_samplesOut;
    if (n > 0) {
        it->pcm = malloc((size_t)n * OUT_CH * 2);
        if (it->pcm) {
            for (int64_t i = 0; i < n * OUT_CH; i++) {
                float v = i < (int64_t)g_mixLen * OUT_CH ? g_mix[i] : 0.0f;
                int x = (int)(v * 32767.0f + (v >= 0 ? 0.5f : -0.5f));
                it->pcm[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
            }
            it->samples = (DWORD)n;
            it->aStart = g_samplesOut;
        }
        g_samplesOut += (uint64_t)n;
        if ((int64_t)g_mixLen > n) {   // sound beyond the frame's end stays for the next frame
            DWORD rest = g_mixLen - (DWORD)n;
            memmove(g_mix, g_mix + n * OUT_CH, (size_t)rest * OUT_CH * sizeof(float));
            memset(g_mix + (size_t)rest * OUT_CH, 0, (size_t)n * OUT_CH * sizeof(float));
            g_mixLen = rest;
        } else if (g_mixLen) {
            memset(g_mix, 0, (size_t)g_mixLen * OUT_CH * sizeof(float));
            g_mixLen = 0;
        }
    }
    g_frameNo++;
    if (it->pixels || it->pcm) enqueue(it); else free_item(it);
}
