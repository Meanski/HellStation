// PS2 audio: not yet. hs_audio_open() reports no device, so DirectSoundCreate
// fails and the game runs silent, as before. (The plan: an IOP module that
// keeps each voice's stream in SPU2 memory as ADPCM; docs/port/NEXT.md.)
#include "Redivivus/port/platform.h"

int hs_audio_open(void) { return 0; }
int hs_audio_voice_create(int, uint32_t) { return -1; }
void hs_audio_voice_destroy(int) {}
void hs_audio_voice_reset(int) {}
void hs_audio_voice_append(int, const int16_t*, uint32_t) {}
void hs_audio_voice_play(int, int) {}
uint64_t hs_audio_voice_played(int) { return 0; }
void hs_audio_voice_params(int, int, int, uint32_t) {}
