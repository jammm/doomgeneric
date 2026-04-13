//
// RPC-based sound module for GPU port.
// Forwards sound commands to the host via RPC opcodes; the host does the
// actual mixing and playback using SDL2's audio API.
//

#include "doomtype.h"
#include "doomfeatures.h"

#ifdef FEATURE_SOUND

#include <string.h>

#include "deh_str.h"
#include "doomgeneric.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#define RPC_NUM_CHANNELS 16

static boolean sound_initialized = false;
static boolean use_sfx_prefix;

// Track which lumpnums have already been sent to the host so we don't
// re-transfer the raw PCM data on every play.
#define MAX_CACHED_LUMPS 4096
static boolean sfx_sent[MAX_CACHED_LUMPS];
static uint32_t active_channel_mask;

int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;

static boolean RPC_InitSound(boolean _use_sfx_prefix)
{
    use_sfx_prefix = _use_sfx_prefix;
    sound_initialized = true;
    memset(sfx_sent, 0, sizeof(sfx_sent));
    active_channel_mask = 0;
    return true;
}

static void RPC_ShutdownSound(void)
{
    sound_initialized = false;
}

static int RPC_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    if (sfx->link != NULL)
        sfx = sfx->link;

    if (use_sfx_prefix)
        M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfx->name));
    else
        M_StringCopy(namebuf, DEH_String(sfx->name), sizeof(namebuf));

    return W_GetNumForName(namebuf);
}

static int RPC_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    int lumpnum;
    void *data = NULL;
    int datalen = 0;

    if (!sound_initialized || channel < 0 || channel >= RPC_NUM_CHANNELS)
        return -1;

    if (sfxinfo->link != NULL)
        sfxinfo = sfxinfo->link;

    lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0)
        return -1;

    if (lumpnum < MAX_CACHED_LUMPS && !sfx_sent[lumpnum]) {
        data = W_CacheLumpNum(lumpnum, PU_CACHE);
        datalen = W_LumpLength(lumpnum);
        sfx_sent[lumpnum] = true;
    }

    return DG_SndStart(lumpnum, data, datalen, channel, vol, sep);
}

static void RPC_StopSound(int channel)
{
    if (!sound_initialized)
        return;
    DG_SndStop(channel);
}

static void RPC_UpdateSoundParams(int channel, int vol, int sep)
{
    if (!sound_initialized)
        return;
    DG_SndUpdateParams(channel, vol, sep);
}

static void RPC_UpdateSound(void)
{
    if (!sound_initialized)
        return;
    active_channel_mask = DG_SndPoll();
}

static boolean RPC_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= RPC_NUM_CHANNELS)
        return false;
    return (active_channel_mask >> channel) & 1;
}

static void RPC_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds;
    (void)num_sounds;
}

static snddevice_t sound_rpc_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module =
{
    sound_rpc_devices,
    sizeof(sound_rpc_devices) / sizeof(*sound_rpc_devices),
    RPC_InitSound,
    RPC_ShutdownSound,
    RPC_GetSfxLumpNum,
    RPC_UpdateSound,
    RPC_UpdateSoundParams,
    RPC_StartSound,
    RPC_StopSound,
    RPC_SoundIsPlaying,
    RPC_PrecacheSounds,
};

#endif // FEATURE_SOUND
