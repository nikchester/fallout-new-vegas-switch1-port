// Bink video stub: no movies. BinkOpen fails (returns 0), which the game treats as "no video" and skips the movie;
// the rest only exists so every Bink import resolves.
#include "pal.h"

namespace {
#define A(i) harg(s, (i))
#define RET(v, n) return hret(s, (uint32_t)(v), (n))

void b_BinkOpen(FnvCpu& s) { RET(0, 2); }
void b_BinkSetSoundSystem(FnvCpu& s) { RET(1, 2); }
void b_BinkOpenDirectSound(FnvCpu& s) { RET(0, 1); }
void b_void1(FnvCpu& s) { RET(0, 1); }
void b_void2(FnvCpu& s) { RET(0, 2); }
void b_BinkCopyToBufferRect(FnvCpu& s) { RET(0, 11); }

const PalEntry kBink[] = {
    {"BinkOpen", b_BinkOpen}, {"BinkSetSoundSystem", b_BinkSetSoundSystem}, {"BinkOpenDirectSound", b_BinkOpenDirectSound},
    {"BinkClose", b_void1}, {"BinkDoFrame", b_void1}, {"BinkNextFrame", b_void1}, {"BinkWait", b_void1},
    {"BinkPause", b_void2}, {"BinkCopyToBufferRect", b_BinkCopyToBufferRect},
    {nullptr, nullptr}};
} // namespace

const PalEntry* pal_bink_entries() { return kBink; }
