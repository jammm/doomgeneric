//
// RPC-based music module for GPU port.
// Sends raw MUS lump data to the host, which converts it to MIDI and
// plays it via SDL_mixer (native MIDI on Windows, Timidity on Linux).
//

#include "doomtype.h"
#include "doomfeatures.h"

#ifdef FEATURE_SOUND

#include "doomgeneric.h"
#include "i_sound.h"

static boolean music_initialized = false;
static uint32_t current_handle = 0;

static boolean RPC_InitMusic(void)
{
    music_initialized = true;
    return true;
}

static void RPC_ShutdownMusic(void)
{
    if (current_handle) {
        DG_MusStop();
        DG_MusUnregister(current_handle);
        current_handle = 0;
    }
    music_initialized = false;
}

static void RPC_SetMusicVolume(int volume)
{
    if (!music_initialized)
        return;
    DG_MusSetVolume(volume);
}

static void RPC_PauseMusic(void)
{
    if (!music_initialized)
        return;
    DG_MusPause();
}

static void RPC_ResumeMusic(void)
{
    if (!music_initialized)
        return;
    DG_MusResume();
}

static void *RPC_RegisterSong(void *data, int len)
{
    uint32_t handle;
    if (!music_initialized)
        return NULL;
    handle = DG_MusRegister(data, len);
    if (handle == 0)
        return NULL;
    return (void *)(uintptr_t)handle;
}

static void RPC_UnRegisterSong(void *handle)
{
    uint32_t h;
    if (!music_initialized || !handle)
        return;
    h = (uint32_t)(uintptr_t)handle;
    if (h == current_handle)
        current_handle = 0;
    DG_MusUnregister(h);
}

static void RPC_PlaySong(void *handle, boolean looping)
{
    if (!music_initialized || !handle)
        return;
    current_handle = (uint32_t)(uintptr_t)handle;
    DG_MusPlay(current_handle, looping);
}

static void RPC_StopSong(void)
{
    if (!music_initialized)
        return;
    DG_MusStop();
}

static boolean RPC_MusicIsPlaying(void)
{
    if (!music_initialized)
        return false;
    return DG_MusIsPlaying() != 0;
}

static void RPC_PollMusic(void)
{
    // Looping is handled on the host side.
}

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
