// Shared-memory protocol between louke.exe (control window) and louke.dll (in the game).
// The control window creates the mapping and passes its name in LOUKE_SHM.
#pragma once
#include <stdint.h>

#define TAS_SHARED_MAGIC   0x4B554F4Cu   /* "LOUK" */
#define TAS_SHARED_VERSION 5u
#define TAS_SLOTS          10            // savestate slots

enum {                      // configurable hotkeys
    HKA_PAUSE,              // pause / resume
    HKA_ADVANCE,            // frame advance (hold to repeat)
    HKA_FFWD,               // fast-forward while held
    HKA_TAKEOVER,           // playback -> record from the current frame (clear read-only)
    HKA_SAVE,               // save the movie now
    HKA_STATE_SAVE,         // save state to slot 1 .. TAS_SLOTS (TAS_SLOTS entries)
    HKA_STATE_LOAD = HKA_STATE_SAVE + TAS_SLOTS,   // load state from slot 1 .. TAS_SLOTS
    HKA_COUNT = HKA_STATE_LOAD + TAS_SLOTS
};

enum {                      // commands sent by the control window buttons
    CMD_PAUSE,
    CMD_ADVANCE,
    CMD_TAKEOVER,
    CMD_SAVE,
    CMD_EXPORT_START,       // start exporting video to exportPath (exportQuality)
    CMD_EXPORT_STOP,        // finish the export file
    CMD_COUNT
};

enum { TASMODE_NONE, TASMODE_RECORD, TASMODE_PLAY };

enum {                      // why a video export failed (the control window shows it in its language)
    EXPERR_NONE,
    EXPERR_NO_MF,           // Media Foundation is not available
    EXPERR_CREATE_FILE,     // the output file cannot be created
    EXPERR_ENCODER,         // the encoder cannot be set up
    EXPERR_WRITE,           // writing failed
    EXPERR_FINALIZE,        // finishing the file failed
    EXPERR_THREAD,          // the encoder thread cannot be started
};

#define TAS_SLOT_EMPTY 0xFFFFFFFFu

typedef struct {
    uint32_t magic, version;
    // control window -> DLL
    uint32_t guiPid;
    volatile uint32_t hotkeys[HKA_COUNT];    // low byte: virtual key, next byte: modifiers (1 Shift, 2 Ctrl, 4 Alt)
    volatile uint32_t cmdSeq[CMD_COUNT];     // incremented for each button press
    wchar_t exportPath[260];                 // video export: output file
    volatile uint32_t exportQuality;         // 0..3
    volatile uint32_t exportMovie;           // 1: export the whole movie from its first frame, as fast as possible
    // DLL -> control window
    volatile uint32_t frame, movieLen, rerecords, mode, paused, ffwd;
    volatile uint32_t saveAck;               // = cmdSeq[CMD_SAVE] after that save request was handled
    volatile uint32_t heartbeat;
    volatile uint32_t slotFrame[TAS_SLOTS];  // frame of each savestate, TAS_SLOT_EMPTY if none
    wchar_t gameTitle[48];                   // recognised game profile, or "" for the generic core (Chinese)
    wchar_t gameTitleEn[48];                 // the same in English
    volatile uint32_t exportState;           // 0 idle, 1 exporting, 2 finishing the file, 3 done, 4 failed
    volatile uint32_t exportFrames;          // frames written so far
    volatile uint32_t exportErrorCode;       // EXPERR_*
    volatile uint32_t exportErrorHr;         // HRESULT of the failure
} TasShared;
