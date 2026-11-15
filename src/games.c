// Registered game profiles. To support another game, add a game_<name>.c with a GameProfile and
// list it here; games without a profile run with the generic core.
#include "tas.h"

extern const GameProfile g_profile_kof13;

static const GameProfile *const kProfiles[] = {
    &g_profile_kof13,
};

const GameProfile *games_find(HMODULE exe, const IMAGE_NT_HEADERS32 *nt) {
    for (size_t i = 0; i < sizeof kProfiles / sizeof kProfiles[0]; i++)
        if (kProfiles[i]->match(exe, nt)) return kProfiles[i];
    return NULL;
}
