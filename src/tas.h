// Internal interface of the TAS DLL.
//
//   core.c        game-independent core: movie, virtual time, input, background-thread sync,
//                 pause / frame advance / fast-forward, link to the control window
//   d3d9.c        Direct3D 9 frame boundary (IDirect3DDevice9::Present) + debug capture
//   games.c       list of game profiles
//   game_*.c      optional per-game profile (patches and workarounds for one game)
//
// A game without a profile runs with the core alone. Other graphics APIs (D3D8, DXGI, OpenGL)
// can be added as further modules that call tas_frame_end() after each presented frame.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "shared.h"

// ---- helpers provided by the core
void tas_log(const char *fmt, ...);
// Replace an import of the game's main executable; *orig receives the previous pointer.
int  tas_hook_iat(const char *dll, const char *name, WORD ordinal, void *fn, void **orig);
// Write bytes to code or read-only data (VirtualProtect + instruction cache flush).
int  tas_patch(void *addr, const void *bytes, size_t n);

// ---- frame boundary (called by a graphics module or a game profile)
void     tas_frame_end(void);      // after a frame was presented: pause, pacing, next input
int      tas_skip_present(void);   // non-zero while fast-forwarding frames that need not be shown
uint32_t tas_frame(void);          // index of the frame currently being computed
DWORD    tas_game_thread(void);    // thread that runs the game loop (0 until known)
void     tas_set_game_thread(DWORD tid);
int      tas_is_game_thread(void);
// A game profile whose own code marks the frame boundary calls this from install(); graphics
// modules then only present (and capture) and leave tas_frame_end() to the profile.
void     tas_claim_frame_boundary(void);
int      tas_frame_boundary_claimed(void);

// ---- graphics / sound modules
int  d3d9_install(void);           // hooks Direct3DCreate9 in the game executable's imports
void d3d9_capture(void *dev);      // debug hash / screenshot of the back buffer about to be shown
void d3d9_frame(void);             // once per frame
int  d3d9_state_save(int slot);    // device state of a savestate
int  d3d9_state_valid(int slot);
void d3d9_state_load(int slot);
int  dsound_install(void);         // hooks DirectSoundCreate8 / CoCreateInstance(CLSID_DirectSound8)
void dsound_pause(int paused);     // real playback stops while the TAS is paused
void dsound_frame(void);
int  dsound_realtime_media(void);   // another component plays sound by the real clock (a movie)           // once per frame: keep real playback near the virtual position
int64_t tas_vtime(void);           // current virtual time in 100 ns units
// Diagnostics (LOUKE_DIAG=1): report each distinct caller of an interesting API once.
int  tas_diag(void);
void tas_diag_caller(const char *what, void *ret);

// ---- kernel handles kept valid for savestates (handles.c)
void handles_install(void);
void handles_track_thread(HANDLE h);
void handles_track_file(HANDLE h);
int  handles_save(int slot);
void handles_load(int slot);

// ---- COM objects of system components kept alive for savestates (comobj.c)
typedef struct {
    void (*released)(void *obj);                       // the game let go of it, a state still keeps it
    void (*save)(void *obj, uint32_t extra[2]);        // extra data of a state
    void (*load)(void *obj, const uint32_t *extra);    // extra is NULL if the state did not hold it
} ObjKindOps;
enum { OBJ_D3D = 1, OBJ_D3D_DEFAULT = 2, OBJ_DSBUFFER = 3, OBJ_DSPRIMARY = 4 };
void tas_obj_init(void);
void tas_obj_register_kind(uint32_t kind, const ObjKindOps *ops);
void tas_obj_track(void *obj, uint32_t kind);      // a new object the game created
void tas_obj_gc_step(int count);
int  tas_obj_save(int slot);
int  tas_obj_slot_valid(int slot);
void tas_obj_load(int slot);
int  tas_obj_drop_unheld(uint32_t kind);

// ---- savestates (savestate.c): the game's own memory and the game thread's context.
// The core calls these on the game thread at a frame boundary with every other game thread at a
// safe point, and keeps the movie / virtual-time part of each state itself.
#define TAS_STATE_SLOTS TAS_SLOTS
int  ss_install(HMODULE exe);      // memory tracking hooks
int  ss_save(int slot);            // 1 saved, 0 failed, 2 = execution resumed here by a load
int  ss_load_check(int slot);      // 1 if the state can be loaded now; changes nothing
void ss_load_commit(int slot) __attribute__((noreturn));  // continues in the ss_save that made it
int  ss_slot_used(int slot);
void *ss_tls_block(uint32_t *size); // the calling thread's block of the exe's implicit TLS
void ss_tls_reset(void);            // ... back to its initial contents
void tas_thread_handle_closed(HANDLE h);   // handles.c: the game closed a handle
int  handles_type(HANDLE h);                // kind of a game handle, 0 unknown
int  handles_ready(HANDLE h, int type);     // a wait would be satisfied: 1 / 0 / -1 unknown

// Video export (avdump.c): MP4, H.264 + AAC, one video frame per game frame.
enum { AVDUMP_IDLE, AVDUMP_RUNNING, AVDUMP_FINISHING, AVDUMP_DONE, AVDUMP_FAILED };
int  avdump_start(const wchar_t *path, int quality);   // quality 0..3
void avdump_stop(void);                                // the file is finished in the background
void avdump_finish(DWORD ms);                          // stop and wait (up to ms) until the file is finished
int  avdump_active(void);
int  avdump_state(void);
uint32_t avdump_frames(void);
const char *avdump_error(void);
void avdump_video(const uint8_t *bgrx, int pitch, UINT w, UINT h);   // picture of the current frame
void avdump_frame_begin(int64_t duration100ns);                      // a frame ended: its sound comes next
void avdump_audio(int src, const void *fmt, const void *p1, DWORD n1, const void *p2, DWORD n2, LONG volume, int realtime);
void avdump_frame(void);                                             // the frame is finished
int  tas_export_fast(void);        // exporting a whole movie: no pacing, no live sound

// ---- game profiles
typedef struct {
    const char    *id;             // written into movies, e.g. "kof13"
    const wchar_t *title;          // shown in the control window
    int  (*match)(HMODULE exe, const IMAGE_NT_HEADERS32 *nt);
    int  (*install)(HMODULE exe);  // runs before the game's entry point; 0 = refuse to run
    void (*pause_tick)(void);      // called repeatedly while the game is paused at a frame boundary
} GameProfile;

const GameProfile *games_find(HMODULE exe, const IMAGE_NT_HEADERS32 *nt);
