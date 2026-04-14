#include <stdio.h>
#include <time.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "m_argv.h"

#include <gpuintrin.h>
#include <shared/rpc.h>

#define NS_IN_MS 1000000L

[[gnu::visibility("protected")]] extern rpc::Client
    client asm("__llvm_rpc_client");

// Host-allocated coherent buffer for zero-copy screen output.
// Set by the loader before _begin runs; if non-null, DG_Init overrides
// the malloc'd DG_ScreenBuffer so frame data goes straight to host memory.
[[gnu::visibility("protected")]] void *__dg_screen_buffer = nullptr;

void DG_Init() {
  if (__dg_screen_buffer)
    DG_ScreenBuffer = reinterpret_cast<pixel_t *>(__dg_screen_buffer);
}

void DG_DrawFrame() {
  auto port = client.open<DOOM_DRAW_BUFFER>();
  port.send([&](rpc::Buffer *buffer, uint32_t) {
    buffer->data[0] = reinterpret_cast<uintptr_t>(DG_ScreenBuffer);
  });
}

void DG_SleepMs(uint32_t ms) {
  struct timespec tim;
  tim.tv_sec = ms / 1000;
  tim.tv_nsec = (NS_IN_MS * ms) % (NS_IN_MS * 1000L);
  nanosleep(&tim, NULL);
}

uint32_t DG_GetTicksMs() {
  struct timespec tim;
  clock_gettime(CLOCK_MONOTONIC, &tim);
  return (uint32_t)(tim.tv_sec * 1000 + tim.tv_nsec / NS_IN_MS);
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
  uint32_t key = 0;
  {
    auto port = client.open<DOOM_GET_INPUT>();
    port.send_and_recv(
        [](rpc::Buffer *, uint32_t) {},
        [&](rpc::Buffer *buffer, uint32_t) { key = buffer->data[0]; });
  }
  if (key == 0)
    return 0;

  *pressed = key >> 8;
  *doomKey = key & 0xFF;

  return 1;
}

void DG_SetWindowTitle(const char *title) {}

#ifdef FEATURE_SOUND
int DG_SndStart(int lumpnum, void *data, int datalen,
                int channel, int vol, int sep) {
  auto port = client.open<DOOM_SND_START>();
  port.send([&](rpc::Buffer *buffer, uint32_t) {
    buffer->data[0] = static_cast<uint64_t>(lumpnum);
    buffer->data[1] = static_cast<uint64_t>(channel);
    buffer->data[2] = static_cast<uint64_t>(vol);
    buffer->data[3] = static_cast<uint64_t>(sep);
    buffer->data[4] = static_cast<uint64_t>(datalen);
  });
  if (datalen > 0 && data) {
    uint64_t len = static_cast<uint64_t>(datalen);
    port.send_n(&data, &len);
  }
  return channel;
}

void DG_SndStop(int channel) {
  auto port = client.open<DOOM_SND_STOP>();
  port.send([&](rpc::Buffer *buffer, uint32_t) {
    buffer->data[0] = static_cast<uint64_t>(channel);
  });
}

void DG_SndUpdateParams(int channel, int vol, int sep) {
  auto port = client.open<DOOM_SND_UPDATE>();
  port.send([&](rpc::Buffer *buffer, uint32_t) {
    buffer->data[0] = static_cast<uint64_t>(channel);
    buffer->data[1] = static_cast<uint64_t>(vol);
    buffer->data[2] = static_cast<uint64_t>(sep);
  });
}

uint32_t DG_SndPoll(void) {
  uint32_t mask = 0;
  {
    auto port = client.open<DOOM_SND_POLL>();
    port.send_and_recv(
        [](rpc::Buffer *, uint32_t) {},
        [&](rpc::Buffer *buffer, uint32_t) { mask = (uint32_t)buffer->data[0]; });
  }
  return mask;
}

uint32_t DG_MusRegister(void *data, int len) {
  uint32_t handle = 0;
  {
    auto port = client.open<DOOM_MUS_REGISTER>();
    port.send([&](rpc::Buffer *buffer, uint32_t) {
      buffer->data[0] = static_cast<uint64_t>(len);
    });
    uint64_t slen = static_cast<uint64_t>(len);
    port.send_n(&data, &slen);
    port.recv([&](rpc::Buffer *buffer, uint32_t) {
      handle = static_cast<uint32_t>(buffer->data[0]);
    });
  }
  return handle;
}

void DG_MusUnregister(uint32_t handle) {
  auto port = client.open<DOOM_MUS_UNREGISTER>();
  port.send([&](rpc::Buffer *buffer, uint32_t) {
    buffer->data[0] = static_cast<uint64_t>(handle);
  });
}

void DG_MusPlay(uint32_t handle, int looping) {
  auto port = client.open<DOOM_MUS_PLAY>();
  port.send([&](rpc::Buffer *buffer, uint32_t) {
    buffer->data[0] = static_cast<uint64_t>(handle);
    buffer->data[1] = static_cast<uint64_t>(looping);
  });
}

void DG_MusStop(void) {
  auto port = client.open<DOOM_MUS_STOP>();
  port.send([](rpc::Buffer *, uint32_t) {});
}

void DG_MusSetVolume(int volume) {
  auto port = client.open<DOOM_MUS_VOLUME>();
  port.send([&](rpc::Buffer *buffer, uint32_t) {
    buffer->data[0] = static_cast<uint64_t>(volume);
  });
}

void DG_MusPause(void) {
  auto port = client.open<DOOM_MUS_PAUSE>();
  port.send([](rpc::Buffer *, uint32_t) {});
}

void DG_MusResume(void) {
  auto port = client.open<DOOM_MUS_RESUME>();
  port.send([](rpc::Buffer *, uint32_t) {});
}

int DG_MusIsPlaying(void) {
  int playing = 0;
  {
    auto port = client.open<DOOM_MUS_IS_PLAYING>();
    port.send_and_recv(
        [](rpc::Buffer *, uint32_t) {},
        [&](rpc::Buffer *buffer, uint32_t) {
          playing = static_cast<int>(buffer->data[0]);
        });
  }
  return playing;
}
#endif // FEATURE_SOUND

int main(int argc, char **argv, char **envp) {
  if (__gpu_thread_id(0) == 0)
    doomgeneric_Create(argc, argv);
  __gpu_sync_threads();

#ifdef SHOWFPS
  uint32_t time = DG_GetTicksMs();
  uint32_t last_tick = 0;
#endif
  for (int i = 0;; ++i) {
    doomgeneric_Tick();

#ifdef SHOWFPS
    if (__gpu_thread_id(0) == 0) {
      int interval = 10;
      if (i % interval == 0) {
        uint32_t new_time = DG_GetTicksMs();
        uint32_t diff = (new_time - time);
        if (diff > 2000) {
          float fps = (float)(i - last_tick) / (diff / 1000.0f);
          last_tick = i;
          time = new_time;
          printf("fps %f\n", fps);
        }
      }
    }
#endif
  }

  return 0;
}
