#ifndef DOOM_GENERIC
#define DOOM_GENERIC

#include <stdint.h>
#include <stdlib.h>

#ifndef DOOMGENERIC_RESX
#define DOOMGENERIC_RESX 2 * 640
#endif // DOOMGENERIC_RESX

#ifndef DOOMGENERIC_RESY
#define DOOMGENERIC_RESY 2 * 400
#endif // DOOMGENERIC_RESY

#define DOOM_DRAW_BUFFER ('d' << 24 | 0)
#define DOOM_GET_INPUT   ('d' << 24 | 1)
#define DOOM_SND_START   ('d' << 24 | 2)
#define DOOM_SND_STOP    ('d' << 24 | 3)
#define DOOM_SND_UPDATE  ('d' << 24 | 4)
#define DOOM_SND_POLL    ('d' << 24 | 5)
#define DOOM_MUS_REGISTER   ('d' << 24 | 6)
#define DOOM_MUS_UNREGISTER ('d' << 24 | 7)
#define DOOM_MUS_PLAY       ('d' << 24 | 8)
#define DOOM_MUS_STOP       ('d' << 24 | 9)
#define DOOM_MUS_VOLUME     ('d' << 24 | 10)
#define DOOM_MUS_PAUSE      ('d' << 24 | 11)
#define DOOM_MUS_RESUME     ('d' << 24 | 12)
#define DOOM_MUS_IS_PLAYING ('d' << 24 | 13)

#ifdef CMAP256

typedef uint8_t pixel_t;

#else // CMAP256

typedef uint32_t pixel_t;

#endif // CMAP256

#ifdef __cplusplus
extern "C" {
#endif

extern pixel_t *DG_ScreenBuffer;

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick();

// Implement below functions for your platform
void DG_Init();
void DG_DrawFrame();
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs();
int DG_GetKey(int *pressed, unsigned char *key);
void DG_SetWindowTitle(const char *title);

#ifdef FEATURE_SOUND
int DG_SndStart(int lumpnum, void *data, int datalen,
                int channel, int vol, int sep);
void DG_SndStop(int channel);
void DG_SndUpdateParams(int channel, int vol, int sep);
uint32_t DG_SndPoll(void);

uint32_t DG_MusRegister(void *data, int len);
void DG_MusUnregister(uint32_t handle);
void DG_MusPlay(uint32_t handle, int looping);
void DG_MusStop(void);
void DG_MusSetVolume(int volume);
void DG_MusPause(void);
void DG_MusResume(void);
int DG_MusIsPlaying(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // DOOM_GENERIC
