//
// Stub music module for GPU port.
// Music requires MIDI playback (SDL_mixer or OPL emulation) which is not
// yet implemented. Init returns false so i_sound.c gracefully disables music.
//

#include "doomtype.h"
#include "doomfeatures.h"

#ifdef FEATURE_SOUND

#include "i_sound.h"

static boolean RPC_InitMusic(void)
{
    return false;
}

static void RPC_ShutdownMusic(void) {}
static void RPC_SetMusicVolume(int volume) { (void)volume; }
static void RPC_PauseMusic(void) {}
static void RPC_ResumeMusic(void) {}

static void *RPC_RegisterSong(void *data, int len)
{
    (void)data;
    (void)len;
    return NULL;
}

static void RPC_UnRegisterSong(void *handle) { (void)handle; }

static void RPC_PlaySong(void *handle, boolean looping)
{
    (void)handle;
    (void)looping;
}

static void RPC_StopSong(void) {}

static boolean RPC_MusicIsPlaying(void)
{
    return false;
}

static void RPC_PollMusic(void) {}

static snddevice_t music_rpc_devices[] =
{
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32,
};

music_module_t DG_music_module =
{
    music_rpc_devices,
    sizeof(music_rpc_devices) / sizeof(*music_rpc_devices),
    RPC_InitMusic,
    RPC_ShutdownMusic,
    RPC_SetMusicVolume,
    RPC_PauseMusic,
    RPC_ResumeMusic,
    RPC_RegisterSong,
    RPC_UnRegisterSong,
    RPC_PlaySong,
    RPC_StopSong,
    RPC_MusicIsPlaying,
    RPC_PollMusic,
};

#endif // FEATURE_SOUND
