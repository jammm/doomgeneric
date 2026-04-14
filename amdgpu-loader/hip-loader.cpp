//===-- HIP-based loader for AMDGPU doomgeneric --------------------------===//
//
// Based on nvidia-loader.cpp, ported from CUDA driver API to HIP.
// Loads an AMDGPU ELF binary and runs it on the GPU, servicing RPC requests
// (drawing, input, malloc/free, libc) from the host side via SDL2.
//
//===----------------------------------------------------------------------===//

#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Frontend/Offloading/Utility.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/WithColor.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include "win_compat.h"

#include <hip/hip_runtime.h>
#ifdef _WIN32
#include <intrin.h>
#endif

#include <shared/rpc.h>
#include <shared/rpc_opcodes.h>

// The internal rpc_server.h uses `rpc::Buffer` inside `namespace LIBC_NAMESPACE`
// which resolves to `__llvm_libc::rpc::Buffer`. We need to bring `::rpc` types
// into that namespace so lookup succeeds.
namespace __llvm_libc {
namespace rpc {
using ::rpc::Buffer;
using ::rpc::Port;
using ::rpc::Server;
using ::rpc::Client;
using ::rpc::Status;
using ::rpc::RPC_SUCCESS;
using ::rpc::RPC_ERROR;
using ::rpc::RPC_UNHANDLED_OPCODE;
using ::rpc::MAX_PORT_COUNT;
} // namespace rpc
} // namespace __llvm_libc

#include <shared/rpc_server.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "doomgeneric.h"
#include "doomkeys.h"

#ifdef _WIN32
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#else
#include <sched.h>
#include <unistd.h>
#endif

using namespace llvm;

static cl::OptionCategory loader_category("loader options");

static cl::opt<bool> help("h", cl::desc("Alias for -help"), cl::Hidden,
                          cl::cat(loader_category));

static cl::opt<unsigned>
    threads_x("threads-x", cl::desc("Number of threads in the 'x' dimension"),
              cl::init(1024), cl::cat(loader_category));
static cl::opt<unsigned>
    threads_y("threads-y", cl::desc("Number of threads in the 'y' dimension"),
              cl::init(1), cl::cat(loader_category));
static cl::opt<unsigned>
    threads_z("threads-z", cl::desc("Number of threads in the 'z' dimension"),
              cl::init(1), cl::cat(loader_category));
static cl::alias threads("threads", cl::aliasopt(threads_x),
                         cl::desc("Alias for --threads-x"),
                         cl::cat(loader_category));

static cl::opt<unsigned>
    blocks_x("blocks-x", cl::desc("Number of blocks in the 'x' dimension"),
             cl::init(1), cl::cat(loader_category));
static cl::opt<unsigned>
    blocks_y("blocks-y", cl::desc("Number of blocks in the 'y' dimension"),
             cl::init(1), cl::cat(loader_category));
static cl::opt<unsigned>
    blocks_z("blocks-z", cl::desc("Number of blocks in the 'z' dimension"),
             cl::init(1), cl::cat(loader_category));
static cl::alias blocks("blocks", cl::aliasopt(blocks_x),
                        cl::desc("Alias for --blocks-x"),
                        cl::cat(loader_category));

static cl::opt<bool>
    print_resource_usage("print-resource-usage",
                         cl::desc("Output resource usage of launched kernels"),
                         cl::init(false), cl::cat(loader_category));

static cl::opt<bool>
    no_parallelism("no-parallelism",
                   cl::desc("Allows only a single process to use the GPU at a "
                            "time. Useful to suppress out-of-resource errors"),
                   cl::init(false), cl::cat(loader_category));

static cl::opt<std::string> file(cl::Positional, cl::Required,
                                 cl::desc("<gpu executable>"),
                                 cl::cat(loader_category));
static cl::list<std::string> args(cl::ConsumeAfter,
                                  cl::desc("<program arguments>..."),
                                  cl::cat(loader_category));

struct LaunchParameters {
  uint32_t num_threads_x;
  uint32_t num_threads_y;
  uint32_t num_threads_z;
  uint32_t num_blocks_x;
  uint32_t num_blocks_y;
  uint32_t num_blocks_z;
};

struct begin_args_t {
  int argc;
  void *argv;
  void *envp;
};

struct start_args_t {
  int argc;
  void *argv;
  void *envp;
  void *ret;
};

struct end_args_t {
  int argc;
};

template <typename V, typename A> inline V align_up(V val, A align) {
  return ((val + V(align) - 1) / V(align)) * V(align);
}

template <typename Allocator>
void *copy_argument_vector(int argc, const char **argv, Allocator alloc) {
  size_t argv_size = sizeof(char *) * (argc + 1);
  size_t str_size = 0;
  for (int i = 0; i < argc; ++i)
    str_size += strlen(argv[i]) + 1;

  void *dev_argv = alloc(argv_size + str_size);
  if (!dev_argv)
    return nullptr;

  void *dev_str = reinterpret_cast<uint8_t *>(dev_argv) + argv_size;
  for (int i = 0; i < argc; ++i) {
    size_t size = strlen(argv[i]) + 1;
    std::memcpy(dev_str, argv[i], size);
    static_cast<void **>(dev_argv)[i] = dev_str;
    dev_str = reinterpret_cast<uint8_t *>(dev_str) + size;
  }

  reinterpret_cast<void **>(dev_argv)[argc] = nullptr;
  return dev_argv;
}

template <typename Allocator>
void *copy_environment(const char **envp, Allocator alloc) {
  int envc = 0;
  for (const char **env = envp; *env != 0; ++env)
    ++envc;

  return copy_argument_vector(envc, envp, alloc);
}

inline void handle_error_impl(const char *file, int32_t line, const char *msg) {
  fprintf(stderr, "%s:%d:0: Error: %s\n", file, line, msg);
  exit(EXIT_FAILURE);
}

#define handle_error(X) handle_error_impl(__FILE__, __LINE__, X)

[[noreturn]] void report_error(Error E) {
  outs().flush();
  logAllUnhandledErrors(std::move(E), WithColor::error(errs(), "loader"));
  exit(EXIT_FAILURE);
}

std::string get_main_executable(const char *name) {
  void *ptr = (void *)(intptr_t)&get_main_executable;
  auto cow_path = sys::fs::getMainExecutable(name, ptr);
  return sys::path::parent_path(cow_path).str();
}

static void handle_error_impl(const char *file, int32_t line, hipError_t err) {
  if (err == hipSuccess)
    return;

  fprintf(stderr, "%s:%d:0: Error: %s\n", file, line,
          hipGetErrorString(err));
  exit(1);
}

void print_kernel_resources(hipModule_t binary, const char *kernel_name) {
  hipFunction_t function;
  if (hipError_t err = hipModuleGetFunction(&function, binary, kernel_name))
    handle_error(err);
  // HIP doesn't have a direct cuFuncGetAttribute equivalent for num_regs
  // that works cross-platform, so just print the kernel name.
  printf("Executing kernel %s\n", kernel_name);
}

void *screen_buffer;       // host-side copy for SDL
void *screen_buffer_gpu;   // coherent buffer the GPU writes into directly
void *rpc_buffer_global;   // for debug dumping

static SDL_Window *window = nullptr;
static SDL_Renderer *renderer = nullptr;
static SDL_Texture *sdl_texture = nullptr;

static unsigned char convertToDoomKey(unsigned int key) {
  switch (key) {
  case SDLK_RETURN:
    key = KEY_ENTER;
    break;
  case SDLK_ESCAPE:
    key = KEY_ESCAPE;
    break;
  case SDLK_LEFT:
    key = KEY_LEFTARROW;
    break;
  case SDLK_RIGHT:
    key = KEY_RIGHTARROW;
    break;
  case SDLK_UP:
    key = KEY_UPARROW;
    break;
  case SDLK_DOWN:
    key = KEY_DOWNARROW;
    break;
  case SDLK_LCTRL:
  case SDLK_RCTRL:
    key = KEY_FIRE;
    break;
  case SDLK_SPACE:
    key = KEY_USE;
    break;
  case SDLK_LSHIFT:
  case SDLK_RSHIFT:
    key = KEY_RSHIFT;
    break;
  case SDLK_LALT:
  case SDLK_RALT:
    key = KEY_LALT;
    break;
  case SDLK_F2:
    key = KEY_F2;
    break;
  case SDLK_F3:
    key = KEY_F3;
    break;
  case SDLK_F4:
    key = KEY_F4;
    break;
  case SDLK_F5:
    key = KEY_F5;
    break;
  case SDLK_F6:
    key = KEY_F6;
    break;
  case SDLK_F7:
    key = KEY_F7;
    break;
  case SDLK_F8:
    key = KEY_F8;
    break;
  case SDLK_F9:
    key = KEY_F9;
    break;
  case SDLK_F10:
    key = KEY_F10;
    break;
  case SDLK_F11:
    key = KEY_F11;
    break;
  case SDLK_EQUALS:
  case SDLK_PLUS:
    key = KEY_EQUALS;
    break;
  case SDLK_MINUS:
    key = KEY_MINUS;
    break;
  default:
    key = tolower(key);
    break;
  }

  return key;
}

static void init_sdl_windows() {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    handle_error(SDL_GetError());

  window =
      SDL_CreateWindow("DOOM", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                       DOOMGENERIC_RESX, DOOMGENERIC_RESY, SDL_WINDOW_SHOWN);

  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  SDL_RenderClear(renderer);
  SDL_RenderPresent(renderer);

  sdl_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
                              SDL_TEXTUREACCESS_TARGET, DOOMGENERIC_RESX,
                              DOOMGENERIC_RESY);
}

#define KEYQUEUE_SIZE 16

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static void addKeyToQueue(int pressed, unsigned int keyCode) {
  unsigned char key = convertToDoomKey(keyCode);

  unsigned short keyData = (pressed << 8) | key;

  s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
  s_KeyQueueWriteIndex++;
  s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}

static uint32_t sdl_get_input() {
  if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex)
    return 0;

  uint32_t key = s_KeyQueue[s_KeyQueueReadIndex];
  s_KeyQueueReadIndex++;
  s_KeyQueueReadIndex %= KEYQUEUE_SIZE;
  return key;
}

static void sdl_draw(void *buffer_ptr) {
  static LARGE_INTEGER freq = {}, last_time = {};
  static int frame_count = 0;
  if (freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last_time);
  }
  frame_count++;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  double elapsed = (double)(now.QuadPart - last_time.QuadPart) / freq.QuadPart;
  if (elapsed >= 2.0) {
    fprintf(stderr, "[fps] %.1f fps (%.1f ms/frame)\n",
            frame_count / elapsed, elapsed * 1000.0 / frame_count);
    frame_count = 0;
    last_time = now;
  }

  // buffer_ptr is the device-visible address the GPU wrote to.  If we set up
  // a coherent host buffer (screen_buffer_gpu), read from the host pointer
  // instead -- plain memcpy, no HIP API call, no WDDM submission, no GPU
  // preemption.  Matches the HSA loader's zero-copy SDMA path.
  void *src = screen_buffer_gpu ? screen_buffer_gpu : nullptr;
  if (src) {
    memcpy(screen_buffer, src,
           DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(uint32_t));
  } else {
    if (hipError_t err =
            hipMemcpyDtoH(screen_buffer, reinterpret_cast<hipDeviceptr_t>(buffer_ptr),
                          DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(uint32_t)))
      handle_error(err);
  }

  SDL_UpdateTexture(sdl_texture, nullptr, screen_buffer,
                    DOOMGENERIC_RESX * sizeof(uint32_t));

  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, sdl_texture, nullptr, nullptr);
  SDL_RenderPresent(renderer);

  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) {
      puts("Quit requested");
      atexit(SDL_Quit);
      exit(1);
    }
    if (e.type == SDL_KEYDOWN)
      addKeyToQueue(1, e.key.keysym.sym);
    else if (e.type == SDL_KEYUP)
      addKeyToQueue(0, e.key.keysym.sym);
  }
}

// ---------------------------------------------------------------------------
// Host-side audio via SDL_mixer (SFX + Music)
// ---------------------------------------------------------------------------

#include <SDL_mixer.h>

#define SND_NUM_CHANNELS 16
#define SND_OUTPUT_RATE 44100

static std::unordered_map<int, Mix_Chunk *> sfx_cache;

// Build a WAV in memory from raw PCM so Mix_LoadWAV_RW can parse it.
static Mix_Chunk *make_wav_chunk(const int16_t *pcm, int num_samples,
                                 int sample_rate) {
  int data_bytes = num_samples * 2; // 16-bit mono
  int wav_size = 44 + data_bytes;
  uint8_t *wav = (uint8_t *)malloc(wav_size);

  auto w16 = [&](int off, uint16_t v) {
    wav[off] = v & 0xFF; wav[off + 1] = v >> 8;
  };
  auto w32 = [&](int off, uint32_t v) {
    wav[off] = v & 0xFF; wav[off+1] = (v>>8)&0xFF;
    wav[off+2] = (v>>16)&0xFF; wav[off+3] = (v>>24)&0xFF;
  };

  memcpy(wav, "RIFF", 4);        w32(4, wav_size - 8);
  memcpy(wav + 8, "WAVE", 4);
  memcpy(wav + 12, "fmt ", 4);   w32(16, 16);
  w16(20, 1);                    // PCM
  w16(22, 1);                    // mono
  w32(24, sample_rate);
  w32(28, sample_rate * 2);      // bytes/sec
  w16(32, 2);                    // block align
  w16(34, 16);                   // bits/sample
  memcpy(wav + 36, "data", 4);   w32(40, data_bytes);
  memcpy(wav + 44, pcm, data_bytes);

  SDL_RWops *rw = SDL_RWFromMem(wav, wav_size);
  Mix_Chunk *chunk = Mix_LoadWAV_RW(rw, 1);
  free(wav);
  return chunk;
}

static void cache_dmx_sound(int lumpnum, const uint8_t *data, int datalen) {
  if (sfx_cache.count(lumpnum))
    return;

  if (datalen < 8 || data[0] != 0x03 || data[1] != 0x00) {
    fprintf(stderr, "[snd] Invalid DMX header for lump %d\n", lumpnum);
    return;
  }

  int sample_rate = data[2] | (data[3] << 8);
  int num_samples = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24);

  if (num_samples <= 0 || 8 + num_samples > datalen)
    num_samples = datalen - 8;
  if (num_samples <= 0)
    return;

  int pad = (num_samples > 32) ? 16 : 0;
  const uint8_t *pcm = data + 8 + pad;
  int actual_samples = num_samples - 2 * pad;
  if (actual_samples <= 0) {
    pcm = data + 8;
    actual_samples = num_samples;
  }

  // Convert unsigned 8-bit to signed 16-bit
  int16_t *converted = (int16_t *)malloc(actual_samples * sizeof(int16_t));
  for (int i = 0; i < actual_samples; i++)
    converted[i] = ((int)pcm[i] - 128) << 8;

  if (sample_rate <= 0) sample_rate = 11025;
  Mix_Chunk *chunk = make_wav_chunk(converted, actual_samples, sample_rate);
  free(converted);

  if (chunk)
    sfx_cache[lumpnum] = chunk;
}

static void snd_start(int lumpnum, int channel, int vol, int sep) {
  if (channel < 0 || channel >= SND_NUM_CHANNELS)
    return;
  auto it = sfx_cache.find(lumpnum);
  if (it == sfx_cache.end())
    return;

  Mix_PlayChannel(channel, it->second, 0);
  Mix_Volume(channel, vol);
  // sep: 0 = full left, 127 = center, 254 = full right
  Uint8 left  = (Uint8)((254 - sep) * 255 / 254);
  Uint8 right = (Uint8)(sep * 255 / 254);
  Mix_SetPanning(channel, left, right);
}

static void snd_stop(int channel) {
  if (channel < 0 || channel >= SND_NUM_CHANNELS)
    return;
  Mix_HaltChannel(channel);
}

static void snd_update(int channel, int vol, int sep) {
  if (channel < 0 || channel >= SND_NUM_CHANNELS)
    return;
  Mix_Volume(channel, vol);
  Uint8 left  = (Uint8)((254 - sep) * 255 / 254);
  Uint8 right = (Uint8)(sep * 255 / 254);
  Mix_SetPanning(channel, left, right);
}

static uint32_t snd_poll() {
  uint32_t mask = 0;
  for (int i = 0; i < SND_NUM_CHANNELS; i++)
    if (Mix_Playing(i))
      mask |= (1u << i);
  return mask;
}

static void init_sdl_audio() {
  if (Mix_OpenAudio(SND_OUTPUT_RATE, MIX_DEFAULT_FORMAT, 2, 1024) < 0) {
    fprintf(stderr, "[snd] Mix_OpenAudio failed: %s\n", Mix_GetError());
    return;
  }
  Mix_AllocateChannels(SND_NUM_CHANNELS);
  fprintf(stderr, "[snd] SDL_mixer audio opened: %d Hz, %d channels\n",
          SND_OUTPUT_RATE, SND_NUM_CHANNELS);
}

// ---------------------------------------------------------------------------
// Host-side music via SDL_mixer (MUS-to-MIDI + Mix_LoadMUS)
// ---------------------------------------------------------------------------

// Self-contained MUS-to-MIDI converter.
struct MidiWriter {
  std::vector<uint8_t> buf;
  unsigned int tracksize = 0;
  unsigned int queuedtime = 0;
  uint8_t channelvelocities[16];
  int channel_map[16];

  void init() {
    buf.clear(); tracksize = 0; queuedtime = 0;
    memset(channelvelocities, 127, sizeof(channelvelocities));
    for (int i = 0; i < 16; i++) channel_map[i] = -1;
  }
  void write(const void *data, size_t len) {
    auto p = reinterpret_cast<const uint8_t *>(data);
    buf.insert(buf.end(), p, p + len);
  }
  void write8(uint8_t v) { buf.push_back(v); }
  void writeTime(unsigned int time) {
    unsigned int buffer = time & 0x7F;
    while ((time >>= 7) != 0) { buffer <<= 8; buffer |= ((time & 0x7F) | 0x80); }
    for (;;) {
      write8(buffer & 0xFF); tracksize++;
      if (buffer & 0x80) buffer >>= 8;
      else { queuedtime = 0; return; }
    }
  }
  void writeEvent2(uint8_t s, uint8_t d1) {
    writeTime(queuedtime); write8(s); write8(d1); tracksize += 2;
  }
  void writeEvent3(uint8_t s, uint8_t d1, uint8_t d2) {
    writeTime(queuedtime); write8(s); write8(d1); write8(d2); tracksize += 3;
  }
  int getMIDIChannel(int mus_ch) {
    if (mus_ch == 15) return 9;
    if (channel_map[mus_ch] == -1) {
      int max = -1;
      for (int i = 0; i < 16; i++)
        if (channel_map[i] > max) max = channel_map[i];
      int result = max + 1;
      if (result == 9) result++;
      channel_map[mus_ch] = result;
      writeEvent3(0xB0 | result, 0x7B, 0);
    }
    return channel_map[mus_ch];
  }
};

static const uint8_t mus_ctrl_map[] = {
    0x00, 0x20, 0x01, 0x07, 0x0A, 0x0B, 0x5B, 0x5D,
    0x40, 0x43, 0x78, 0x7B, 0x7E, 0x7F, 0x79
};

static bool mus_to_midi(const uint8_t *mus, int muslen,
                        std::vector<uint8_t> &out) {
  if (muslen < 16 || memcmp(mus, "MUS\x1a", 4) != 0) return false;
  int scorestart = mus[6] | (mus[7] << 8);
  if (scorestart >= muslen) return false;

  MidiWriter w; w.init();
  static const uint8_t hdr[] = {
      'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,0x46,
      'M','T','r','k', 0,0,0,0
  };
  w.write(hdr, sizeof(hdr));

  int pos = scorestart;
  bool hitend = false;
  while (!hitend && pos < muslen) {
    while (!hitend && pos < muslen) {
      uint8_t desc = mus[pos++];
      int ch = w.getMIDIChannel(desc & 0x0F);
      switch (desc & 0x70) {
      case 0x00: { if (pos>=muslen) return false; w.writeEvent3(0x80|ch, mus[pos++]&0x7F, 0); break; }
      case 0x10: {
        if (pos>=muslen) return false;
        uint8_t key = mus[pos++];
        if (key & 0x80) { if (pos>=muslen) return false; w.channelvelocities[ch] = mus[pos++]&0x7F; }
        w.writeEvent3(0x90|ch, key&0x7F, w.channelvelocities[ch]); break;
      }
      case 0x20: { if (pos>=muslen) return false; short wh=(short)(mus[pos++])*64; w.writeEvent3(0xE0|ch, wh&0x7F, (wh>>7)&0x7F); break; }
      case 0x30: {
        if (pos>=muslen) return false; uint8_t c=mus[pos++];
        if (c<10||c>14) return false;
        w.writeEvent3(0xB0|ch, mus_ctrl_map[c], 0); break;
      }
      case 0x40: {
        if (pos+1>=muslen) return false;
        uint8_t c=mus[pos++], v=mus[pos++];
        if (c==0) { w.writeEvent2(0xC0|ch, v&0x7F); }
        else { if (c<1||c>9) return false; uint8_t vv=v; if(vv&0x80) vv=0x7F; w.writeEvent3(0xB0|ch, mus_ctrl_map[c], vv); }
        break;
      }
      case 0x60: hitend=true; break;
      default: return false;
      }
      if (desc & 0x80) break;
    }
    if (!hitend && pos < muslen) {
      unsigned int delay = 0;
      for (;;) { if (pos>=muslen) return false; uint8_t b=mus[pos++]; delay=delay*128+(b&0x7F); if (!(b&0x80)) break; }
      w.queuedtime += delay;
    }
  }
  w.writeTime(w.queuedtime);
  w.write8(0xFF); w.write8(0x2F); w.write8(0x00);
  w.tracksize += 3;
  uint32_t ts = w.tracksize;
  w.buf[18]=(ts>>24)&0xFF; w.buf[19]=(ts>>16)&0xFF;
  w.buf[20]=(ts>>8)&0xFF;  w.buf[21]=ts&0xFF;
  out = std::move(w.buf);
  return true;
}

static std::string get_temp_midi_path() {
  const char *tmp = getenv("TMPDIR");
  if (!tmp) tmp = getenv("TEMP");
  if (!tmp) tmp = "/tmp";
  return std::string(tmp) + "/doom_gpu_music.mid";
}

static Mix_Music *current_music = nullptr;
static std::string current_midi_path;
static uint32_t mus_next_handle = 1;

static uint32_t mus_register(const uint8_t *data, int len) {
  if (current_music) {
    Mix_HaltMusic();
    Mix_FreeMusic(current_music);
    current_music = nullptr;
  }

  std::vector<uint8_t> midi;
  if (!mus_to_midi(data, len, midi)) {
    fprintf(stderr, "[mus] MUS-to-MIDI conversion failed (len=%d)\n", len);
    return 0;
  }

  current_midi_path = get_temp_midi_path();
  FILE *f = fopen(current_midi_path.c_str(), "wb");
  if (!f) {
    fprintf(stderr, "[mus] Failed to write temp MIDI: %s\n",
            current_midi_path.c_str());
    return 0;
  }
  fwrite(midi.data(), 1, midi.size(), f);
  fclose(f);

  current_music = Mix_LoadMUS(current_midi_path.c_str());
  if (!current_music) {
    fprintf(stderr, "[mus] Mix_LoadMUS failed: %s\n", Mix_GetError());
    return 0;
  }

  fprintf(stderr, "[mus] Registered song (%d bytes MUS -> %zu bytes MIDI)\n",
          len, midi.size());
  return mus_next_handle++;
}

static void mus_unregister(uint32_t handle) {
  if (current_music) {
    Mix_HaltMusic();
    Mix_FreeMusic(current_music);
    current_music = nullptr;
  }
}

static void mus_play(uint32_t handle, int looping) {
  if (!current_music) return;
  Mix_PlayMusic(current_music, looping ? -1 : 1);
  fprintf(stderr, "[mus] Playing music (loop=%d)\n", looping);
}

static void mus_stop() {
  Mix_HaltMusic();
}

static void mus_set_volume(int vol) {
  // Doom volume: 0-127, SDL_mixer: 0-128
  Mix_VolumeMusic(vol);
}

static void mus_pause() {
  Mix_PauseMusic();
}

static void mus_resume() {
  Mix_ResumeMusic();
}

static int mus_is_playing() {
  return Mix_PlayingMusic() && !Mix_PausedMusic() ? 1 : 0;
}

template <uint32_t num_lanes, typename Alloc, typename Free>
static uint32_t handle_server(rpc::Server &server, uint32_t index,
                              Alloc &&alloc, Free &&free) {
  auto port = server.try_open(num_lanes, index);
  if (!port)
    return 0;
  index = port->get_index() + 1;

  int status = rpc::RPC_SUCCESS;
  switch (port->get_opcode()) {
  case LIBC_MALLOC: {
    port->recv_and_send([&](rpc::Buffer *buffer, uint32_t) {
      buffer->data[0] = reinterpret_cast<uintptr_t>(alloc(buffer->data[0]));
    });
    break;
  }
  case LIBC_FREE: {
    port->recv([&](rpc::Buffer *buffer, uint32_t) {
      free(reinterpret_cast<void *>(buffer->data[0]));
    });
    break;
  }
  case DOOM_DRAW_BUFFER: {
    port->recv([&](rpc::Buffer *buffer, uint32_t) {
      sdl_draw(reinterpret_cast<void *>(buffer->data[0]));
    });
    break;
  }
  case DOOM_GET_INPUT: {
    port->recv_and_send([&](rpc::Buffer *buffer, uint32_t) {
      buffer->data[0] = sdl_get_input();
    });
    break;
  }
  case DOOM_SND_START: {
    int snd_lumpnum = 0, snd_channel = 0, snd_vol = 0, snd_sep = 0;
    int snd_datalen = 0;
    port->recv([&](rpc::Buffer *buffer, uint32_t) {
      snd_lumpnum = static_cast<int>(buffer->data[0]);
      snd_channel = static_cast<int>(buffer->data[1]);
      snd_vol     = static_cast<int>(buffer->data[2]);
      snd_sep     = static_cast<int>(buffer->data[3]);
      snd_datalen = static_cast<int>(buffer->data[4]);
    });
    if (snd_datalen > 0) {
      uint64_t sizes[num_lanes] = {0};
      void *bufs[num_lanes] = {nullptr};
      auto temp_alloc = [](uint64_t sz) -> void * { return malloc(sz); };
      port->recv_n(bufs, sizes, temp_alloc);
      if (bufs[0] && sizes[0] > 0)
        cache_dmx_sound(snd_lumpnum, (const uint8_t *)bufs[0], (int)sizes[0]);
      for (uint32_t i = 0; i < num_lanes; i++)
        ::free(bufs[i]);
    }
    snd_start(snd_lumpnum, snd_channel, snd_vol, snd_sep);
    break;
  }
  case DOOM_SND_STOP: {
    port->recv([&](rpc::Buffer *buffer, uint32_t) {
      snd_stop(static_cast<int>(buffer->data[0]));
    });
    break;
  }
  case DOOM_SND_UPDATE: {
    port->recv([&](rpc::Buffer *buffer, uint32_t) {
      snd_update(static_cast<int>(buffer->data[0]),
                 static_cast<int>(buffer->data[1]),
                 static_cast<int>(buffer->data[2]));
    });
    break;
  }
  case DOOM_SND_POLL: {
    port->recv_and_send([&](rpc::Buffer *buffer, uint32_t) {
      buffer->data[0] = snd_poll();
    });
    break;
  }
  case DOOM_MUS_REGISTER: {
    int mus_datalen = 0;
    port->recv([&](rpc::Buffer *buffer, uint32_t) {
      mus_datalen = static_cast<int>(buffer->data[0]);
    });
    uint64_t sizes[num_lanes] = {0};
    void *bufs[num_lanes] = {nullptr};
    auto temp_alloc = [](uint64_t sz) -> void * { return malloc(sz); };
    port->recv_n(bufs, sizes, temp_alloc);
    uint32_t h = 0;
    if (bufs[0] && sizes[0] > 0)
      h = mus_register((const uint8_t *)bufs[0], (int)sizes[0]);
    for (uint32_t i = 0; i < num_lanes; i++)
      ::free(bufs[i]);
    port->send([&](rpc::Buffer *buffer, uint32_t) {
      buffer->data[0] = static_cast<uint64_t>(h);
    });
    break;
  }
  case DOOM_MUS_UNREGISTER: {
    port->recv([&](rpc::Buffer *buffer, uint32_t) {
      mus_unregister(static_cast<uint32_t>(buffer->data[0]));
    });
    break;
  }
  case DOOM_MUS_PLAY: {
    port->recv([&](rpc::Buffer *buffer, uint32_t) {
      mus_play(static_cast<uint32_t>(buffer->data[0]),
               static_cast<int>(buffer->data[1]));
    });
    break;
  }
  case DOOM_MUS_STOP: {
    port->recv([&](rpc::Buffer *buffer, uint32_t) { mus_stop(); });
    break;
  }
  case DOOM_MUS_VOLUME: {
    port->recv([&](rpc::Buffer *buffer, uint32_t) {
      mus_set_volume(static_cast<int>(buffer->data[0]));
    });
    break;
  }
  case DOOM_MUS_PAUSE: {
    port->recv([&](rpc::Buffer *buffer, uint32_t) { mus_pause(); });
    break;
  }
  case DOOM_MUS_RESUME: {
    port->recv([&](rpc::Buffer *buffer, uint32_t) { mus_resume(); });
    break;
  }
  case DOOM_MUS_IS_PLAYING: {
    port->recv_and_send([&](rpc::Buffer *buffer, uint32_t) {
      buffer->data[0] = static_cast<uint64_t>(mus_is_playing());
    });
    break;
  }
  case LIBC_EXIT: {
    port->recv_and_send([](rpc::Buffer *, uint32_t) {});
    port->recv([](rpc::Buffer *buffer, uint32_t) {
      int exit_status = 0;
      __builtin_memcpy(&exit_status, buffer->data, sizeof(int));
      fprintf(stderr, "[rpc] GPU requested exit with status %d\n", exit_status);
      exit(exit_status);
    });
    break;
  }
  case LIBC_READ_FROM_STREAM: {
    uint64_t sizes[num_lanes] = {0};
    void *data[num_lanes] = {nullptr};
    port->recv([&](rpc::Buffer *buffer, uint32_t id) {
      uint64_t read_size = buffer->data[0];
      FILE *f = reinterpret_cast<FILE *>(buffer->data[1] & ~0x3ull);
      uintptr_t tag = buffer->data[1] & 0x3;
      if (tag == 1) f = stdin;
      else if (tag == 2) f = stdout;
      else if (tag == 3) f = stderr;
      data[id] = malloc(read_size);
      sizes[id] = fread(data[id], 1, read_size, f);
    });
    port->send_n(data, sizes);
    port->send([&](rpc::Buffer *buffer, uint32_t id) {
      __builtin_memcpy(buffer->data, &sizes[id], sizeof(uint64_t));
    });
    for (uint32_t i = 0; i < num_lanes; i++)
      ::free(data[i]);
    break;
  }
  case LIBC_OPEN_FILE: {
    uint64_t sizes[num_lanes] = {0};
    void *paths[num_lanes] = {nullptr};
    auto temp_alloc = [](uint64_t sz) -> void * { return malloc(sz); };
    port->recv_n(paths, sizes, temp_alloc);
    port->recv_and_send([&](rpc::Buffer *buffer, uint32_t id) {
      const char *path = paths[id] ? reinterpret_cast<char *>(paths[id]) : "(null)";
      const char *mode = reinterpret_cast<char *>(buffer->data);
      FILE *file = fopen(path, mode);
      buffer->data[0] = reinterpret_cast<uintptr_t>(file);
    });
    for (uint32_t i = 0; i < num_lanes; i++)
      ::free(paths[i]);
    break;
  }
  case LIBC_FSEEK: {
    port->recv_and_send([](rpc::Buffer *buffer, uint32_t) {
      FILE *f = reinterpret_cast<FILE *>(buffer->data[0] & ~0x3ull);
      uintptr_t tag = buffer->data[0] & 0x3;
      if (tag == 1) f = stdin;
      else if (tag == 2) f = stdout;
      else if (tag == 3) f = stderr;
      int64_t offset = static_cast<int64_t>(buffer->data[1]);
      int whence = static_cast<int>(buffer->data[2]);
      int result = fseek(f, offset, whence);
      buffer->data[0] = result;
    });
    break;
  }
  case LIBC_FTELL: {
    port->recv_and_send([](rpc::Buffer *buffer, uint32_t) {
      FILE *f = reinterpret_cast<FILE *>(buffer->data[0] & ~0x3ull);
      uintptr_t tag = buffer->data[0] & 0x3;
      if (tag == 1) f = stdin;
      else if (tag == 2) f = stdout;
      else if (tag == 3) f = stderr;
#ifdef _WIN32
      buffer->data[0] = static_cast<uint64_t>(_ftelli64(f));
#else
      buffer->data[0] = static_cast<uint64_t>(ftell(f));
#endif
    });
    break;
  }
  case LIBC_ABORT: {
    fprintf(stderr, "[rpc] GPU called abort!\n");
    port->recv([](rpc::Buffer *, uint32_t) {});
    abort();
    break;
  }
  default: {
    uint32_t op = port->get_opcode();
    status = __llvm_libc::shared::handle_libc_opcodes(*port, num_lanes);
    if (op >= LIBC_PRINTF_TO_STDOUT && op <= LIBC_PRINTF_TO_STREAM_PACKED)
      fflush(stdout);
    break;
  }
  }

  if (status != rpc::RPC_SUCCESS) {
    fprintf(stderr, "[rpc] FATAL: opcode 0x%08x failed with status %d\n",
            port->get_opcode(), status);
    handle_error("Error handling RPC server");
  }

  return index;
}

template <typename args_t>
hipError_t launch_kernel(hipModule_t binary, hipStream_t stream,
                         rpc::Server &server, const LaunchParameters &params,
                         const char *kernel_name, args_t kernel_args,
                         bool print_resource_usage,
                         bool long_running = false) {
  fprintf(stderr, "[kernel] Getting function '%s'...\n", kernel_name);
  hipFunction_t function;
  if (hipError_t err = hipModuleGetFunction(&function, binary, kernel_name))
    handle_error(err);
  fprintf(stderr, "[kernel] Got function %p, launching (%u,%u,%u) blocks x (%u,%u,%u) threads\n",
          (void *)function,
          params.num_blocks_x, params.num_blocks_y, params.num_blocks_z,
          params.num_threads_x, params.num_threads_y, params.num_threads_z);

  uint64_t args_size = sizeof(args_t);
  void *args_config[] = {HIP_LAUNCH_PARAM_BUFFER_POINTER, &kernel_args,
                         HIP_LAUNCH_PARAM_BUFFER_SIZE, &args_size,
                         HIP_LAUNCH_PARAM_END};

  // Synchronous allocation -- matches HSA's hsa_amd_memory_pool_allocate.
  // The old hipMallocAsync + hipStreamQuery loop went through WDDM command
  // submission for every allocation, adding ~ms of overhead per malloc.
  auto malloc_handler = [&](size_t size) -> void * {
    void *dev_ptr = nullptr;
    if (hipError_t err = hipMalloc(&dev_ptr, size))
      return nullptr;
    if (hipError_t err = hipMemset(dev_ptr, 0, size))
      handle_error(err);
    return dev_ptr;
  };
  auto free_handler = [&](void *data) {
    hipFree(data);
  };

  if (print_resource_usage)
    print_kernel_resources(binary, kernel_name);

  fprintf(stderr, "[kernel] hipModuleLaunchKernel...\n");
  if (hipError_t err = hipModuleLaunchKernel(
          function, params.num_blocks_x, params.num_blocks_y,
          params.num_blocks_z, params.num_threads_x, params.num_threads_y,
          params.num_threads_z, 0, stream, nullptr, args_config))
    handle_error(err);
  fprintf(stderr, "[kernel] Launched, entering RPC loop...\n");

  if (long_running) {
    // For long-running kernels (like _start's infinite game loop), avoid
    // calling hipStreamQuery on every iteration.  Each call goes through
    // HIP->PAL->WDDM and costs ~25us on Windows.  Instead, poll
    // infrequently and yield when no RPC work is pending to reduce
    // coherent-memory cache-line contention on the PCIe bus.
    uint64_t iter = 0;
    constexpr uint64_t QUERY_INTERVAL = 100000;
    for (;;) {
      uint32_t index = 0;
      if (handle_server<32>(server, index, malloc_handler, free_handler) == 0) {
        // No RPC work pending.  Pause briefly to avoid hammering the
        // coherent shared memory, which creates PCIe traffic that can
        // starve the GPU.  ~40 _mm_pause iterations ≈ 5-8µs, matching
        // the HSA loader's hsa_signal_wait timeout of 8192 cycles.
        for (int p = 0; p < 40; p++)
          _mm_pause();
      }
      if (++iter % QUERY_INTERVAL == 0) {
        if (hipStreamQuery(stream) != hipErrorNotReady)
          break;
      }
    }
  } else {
    while (hipStreamQuery(stream) == hipErrorNotReady) {
      uint32_t index = 0;
      handle_server<32>(server, index, malloc_handler, free_handler);
    }
  }

  return hipSuccess;
}

int load(int argc, const char **argv, const char **envp, void *image,
         size_t size, const LaunchParameters &params,
         bool print_resource_usage) {
  fprintf(stderr, "[loader] hipInit...\n");
  if (hipError_t err = hipInit(0))
    handle_error(err);

  fprintf(stderr, "[loader] hipDeviceGet...\n");
  uint32_t device_id = 0;
  hipDevice_t device;
  if (hipError_t err = hipDeviceGet(&device, device_id))
    handle_error(err);

  fprintf(stderr, "[loader] hipDevicePrimaryCtxRetain...\n");
  hipCtx_t context;
  if (hipError_t err = hipDevicePrimaryCtxRetain(&context, device))
    handle_error(err);
  if (hipError_t err = hipCtxSetCurrent(context))
    handle_error(err);

  size_t cur_stack = 0;
  hipDeviceGetLimit(&cur_stack, hipLimitStackSize);
  fprintf(stderr, "[loader] default stack per thread: %zu bytes\n", cur_stack);
  hipDeviceSetLimit(hipLimitStackSize, 16 * 1024);
  hipDeviceGetLimit(&cur_stack, hipLimitStackSize);
  fprintf(stderr, "[loader] stack per thread set to: %zu bytes\n", cur_stack);

  fprintf(stderr, "[loader] hipStreamCreate...\n");
  hipStream_t stream;
  if (hipError_t err =
          hipStreamCreateWithFlags(&stream, hipStreamNonBlocking))
    handle_error(err);

  fprintf(stderr, "[loader] hipModuleLoadData (image=%p, size=%zu)...\n", image, size);
  hipModule_t binary;
  if (hipError_t err = hipModuleLoadData(&binary, image))
    handle_error(err);
  fprintf(stderr, "[loader] Module loaded successfully\n");

  auto allocator = [&](uint64_t size) -> void * {
    void *dev_ptr;
    if (hipError_t err = hipHostMalloc(&dev_ptr, size,
                                       hipHostMallocMapped | hipHostMallocCoherent))
      handle_error(err);
    return dev_ptr;
  };

  fprintf(stderr, "[loader] Copying argv...\n");
  void *dev_argv = copy_argument_vector(argc, argv, allocator);
  if (!dev_argv)
    handle_error("Failed to allocate device argv");

  fprintf(stderr, "[loader] Copying envp...\n");
  void *dev_envp = copy_environment(envp, allocator);
  if (!dev_envp)
    handle_error("Failed to allocate device environment");

  hipDeviceptr_t dev_ret;
  if (hipError_t err = hipMalloc(reinterpret_cast<void **>(&dev_ret), sizeof(int)))
    handle_error(err);
  if (hipError_t err = hipMemsetD32(dev_ret, 0, 1))
    handle_error(err);

  screen_buffer =
      malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(uint32_t));

  uint32_t warp_size = 32;
  void *rpc_buffer = nullptr;
  size_t rpc_size = rpc::Server::allocation_size(warp_size, rpc::MAX_PORT_COUNT);
  fprintf(stderr, "[loader] Allocating RPC buffer: %zu bytes\n", rpc_size);

  // Try coherent first (matches Linux HSA fine-grained memory). If coherent
  // doesn't provide GPU-visible host-mapped memory, fall back to WC which
  // guarantees visibility but makes CPU reads uncached (slow polling).
  hipError_t alloc_err = hipHostMalloc(
      &rpc_buffer, rpc_size,
      hipHostMallocMapped | hipHostMallocCoherent);
  if (alloc_err != hipSuccess) {
    fprintf(stderr, "[loader] Coherent alloc failed (%s), falling back to WriteCombined\n",
            hipGetErrorString(alloc_err));
    if (hipError_t err = hipHostMalloc(&rpc_buffer, rpc_size,
                                       hipHostMallocMapped | hipHostMallocWriteCombined))
      handle_error(err);
  }
  memset(rpc_buffer, 0, rpc_size);
  rpc_buffer_global = rpc_buffer;

  void *rpc_buffer_dev = nullptr;
  if (hipError_t err = hipHostGetDevicePointer(&rpc_buffer_dev, rpc_buffer, 0))
    handle_error(err);
  fprintf(stderr, "[loader] RPC buffer: host=%p dev=%p %s\n",
          rpc_buffer, rpc_buffer_dev,
          rpc_buffer == rpc_buffer_dev ? "(SAME)" : "(DIFFERENT!)");

  unsigned int flags = 0;
  hipError_t flagErr = hipHostGetFlags(&flags, rpc_buffer);
  fprintf(stderr, "[loader] RPC buffer flags: 0x%x (err=%d) WC=%d coherent=%d mapped=%d\n",
          flags, flagErr,
          !!(flags & hipHostMallocWriteCombined),
          !!(flags & hipHostMallocCoherent),
          !!(flags & hipHostMallocMapped));
  fflush(stderr);

  // Server runs on host -> uses host pointer
  // Client runs on GPU -> must use device pointer if different
  rpc::Server server(rpc::MAX_PORT_COUNT, rpc_buffer);
  rpc::Client client(rpc::MAX_PORT_COUNT, rpc_buffer_dev);
  fprintf(stderr, "[loader] sizeof(rpc::Client) = %zu\n", sizeof(rpc::Client));

  fprintf(stderr, "[loader] Setting up RPC...\n");
  hipDeviceptr_t rpc_client_dev = 0;
  size_t client_ptr_size = sizeof(void *);
  if (hipError_t err = hipModuleGetGlobal(&rpc_client_dev, &client_ptr_size,
                                          binary, "__llvm_rpc_client"))
    handle_error(err);
  fprintf(stderr, "[loader] RPC client global at %p (size %zu)\n",
          (void *)rpc_client_dev, client_ptr_size);

  if (hipError_t err =
          hipMemcpyHtoD(rpc_client_dev, &client, sizeof(rpc::Client)))
    handle_error(err);

  // Query the GPU's wall-clock frequency (used by __builtin_readsteadycounter /
  // wall_clock64) and copy it to the GPU for libc's clock_gettime.
  hipDeviceptr_t freq_dev = 0;
  size_t freq_size = 0;
  hipError_t freq_err = hipModuleGetGlobal(&freq_dev, &freq_size, binary,
                                            "__llvm_libc_clock_freq");
  if (freq_err == hipSuccess) {
    int wall_clock_khz = 0;
    hipDeviceGetAttribute(&wall_clock_khz, hipDeviceAttributeWallClockRate,
                          device);

    // hipDeviceAttributeWallClockRate reports 100 MHz on RDNA 4, but the
    // actual s_sendmsg_rtn MSG_RTN_GET_REALTIME counter ticks much slower.
    // Measure it empirically: launch a trivial kernel that reads the counter
    // twice with a host-timed gap, then compute the real frequency.
    // For now, allow override via environment variable.
    uint64_t clock_hz = wall_clock_khz > 0
        ? (uint64_t)wall_clock_khz * 1000ULL
        : 100000000ULL;

    const char *freq_override = getenv("GPU_CLOCK_FREQ_HZ");
    if (freq_override) {
      clock_hz = strtoull(freq_override, nullptr, 10);
      fprintf(stderr, "[loader] GPU_CLOCK_FREQ_HZ override: %llu Hz\n",
              (unsigned long long)clock_hz);
    }

    fprintf(stderr, "[loader] wall clock: %d kHz, using %llu Hz\n",
            wall_clock_khz, (unsigned long long)clock_hz);
    if (hipError_t err = hipMemcpyHtoD(freq_dev, &clock_hz, sizeof(uint64_t)))
      fprintf(stderr, "[loader] warning: failed to set clock_freq: %s\n",
              hipGetErrorString(err));
  }

  // Allocate a coherent host buffer for the screen so the GPU can write
  // directly into host-visible memory.  The host reads it with memcpy in
  // sdl_draw, completely bypassing the HIP/ROCclr/PAL/WDDM command path.
  {
    size_t screen_size = DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(uint32_t);
    hipError_t scr_err = hipHostMalloc(
        &screen_buffer_gpu, screen_size,
        hipHostMallocMapped | hipHostMallocCoherent);
    if (scr_err == hipSuccess) {
      memset(screen_buffer_gpu, 0, screen_size);
      void *screen_buf_dev = nullptr;
      hipHostGetDevicePointer(&screen_buf_dev, screen_buffer_gpu, 0);
      fprintf(stderr, "[loader] Screen buffer: host=%p dev=%p %s\n",
              screen_buffer_gpu, screen_buf_dev,
              screen_buffer_gpu == screen_buf_dev ? "(SAME)" : "(DIFFERENT)");

      hipDeviceptr_t screen_global = 0;
      size_t screen_global_size = 0;
      hipError_t glob_err = hipModuleGetGlobal(
          &screen_global, &screen_global_size, binary, "__dg_screen_buffer");
      if (glob_err == hipSuccess) {
        hipMemcpyHtoD(screen_global, &screen_buf_dev, sizeof(void *));
        fprintf(stderr, "[loader] Set __dg_screen_buffer = %p\n", screen_buf_dev);
      } else {
        fprintf(stderr, "[loader] warning: __dg_screen_buffer not found in "
                        "binary, falling back to hipMemcpyDtoH\n");
        hipHostFree(screen_buffer_gpu);
        screen_buffer_gpu = nullptr;
      }
    } else {
      fprintf(stderr, "[loader] warning: coherent screen alloc failed (%s), "
                      "falling back to hipMemcpyDtoH\n",
              hipGetErrorString(scr_err));
      screen_buffer_gpu = nullptr;
    }
  }

  fprintf(stderr, "[loader] Launching _begin kernel...\n");
  LaunchParameters single_threaded_params = {1, 1, 1, 1, 1, 1};
  begin_args_t init_args = {argc, dev_argv, dev_envp};
  if (hipError_t err =
          launch_kernel(binary, stream, server, single_threaded_params,
                        "_begin", init_args, print_resource_usage))
    handle_error(err);
  fprintf(stderr, "[loader] _begin completed\n");

  fprintf(stderr, "[loader] Launching _start kernel (threads=%u)...\n",
          params.num_threads_x);
  start_args_t start_args = {argc, dev_argv, dev_envp,
                             reinterpret_cast<void *>(dev_ret)};
  if (hipError_t err = launch_kernel(binary, stream, server, params, "_start",
                                     start_args, print_resource_usage,
                                     /*long_running=*/true))
    handle_error(err);
  fprintf(stderr, "[loader] _start completed\n");

  int host_ret = 0;
  if (hipError_t err = hipMemcpyDtoH(&host_ret, dev_ret, sizeof(int)))
    handle_error(err);

  if (hipError_t err = hipStreamSynchronize(stream))
    handle_error(err);

  end_args_t fini_args = {host_ret};
  if (hipError_t err =
          launch_kernel(binary, stream, server, single_threaded_params, "_end",
                        fini_args, print_resource_usage))
    handle_error(err);

  if (hipError_t err = hipFree(reinterpret_cast<void *>(dev_ret)))
    handle_error(err);
  if (hipError_t err = hipFree(dev_argv))
    handle_error(err);
  if (hipError_t err = hipFree(rpc_buffer))
    handle_error(err);

  if (hipError_t err = hipModuleUnload(binary))
    handle_error(err);
  if (hipError_t err = hipDevicePrimaryCtxRelease(device))
    handle_error(err);
  return host_ret;
}

#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  void *addr = ep->ExceptionRecord->ExceptionAddress;
  fprintf(stderr, "\n[CRASH] Exception 0x%08lx at %p\n", code, addr);
  fflush(stderr);

  // Walk the stack
  HANDLE process = GetCurrentProcess();
  SymInitialize(process, NULL, TRUE);
  CONTEXT *ctx = ep->ContextRecord;
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = ctx->Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx->Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx->Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  fprintf(stderr, "[CRASH] Stack trace:\n");
  for (int i = 0; i < 30; i++) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(),
                     &frame, ctx, NULL, SymFunctionTableAccess64,
                     SymGetModuleBase64, NULL))
      break;
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(process, frame.AddrPC.Offset, &disp, sym))
      fprintf(stderr, "  [%2d] %s + 0x%llx\n", i, sym->Name, (unsigned long long)disp);
    else
      fprintf(stderr, "  [%2d] 0x%llx\n", i, (unsigned long long)frame.AddrPC.Offset);
  }
  fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
  timeBeginPeriod(1);
  AddVectoredExceptionHandler(1, crash_handler);
#endif
  const char **envp = const_cast<const char **>((const char **)_environ);
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  cl::HideUnrelatedOptions(loader_category);
  cl::ParseCommandLineOptions(
      argc, argv,
      "A utility used to launch DOOM built for an AMDGPU target via HIP.\n");

  if (help) {
    cl::PrintHelpMessage();
    return EXIT_SUCCESS;
  }

  fprintf(stderr, "[main] Loading GPU image from '%s'...\n", file.c_str());
  ErrorOr<std::unique_ptr<MemoryBuffer>> image_or_err =
      MemoryBuffer::getFileOrSTDIN(file);
  if (std::error_code ec = image_or_err.getError())
    report_error(errorCodeToError(ec));
  MemoryBufferRef image = **image_or_err;
  fprintf(stderr, "[main] Image loaded: %zu bytes\n", image.getBufferSize());

  SmallVector<const char *> new_argv = {file.c_str()};
  llvm::transform(args, std::back_inserter(new_argv),
                  [](const std::string &arg) { return arg.c_str(); });

  fprintf(stderr, "[main] Initializing SDL...\n");
  init_sdl_windows();
  init_sdl_audio();
  fprintf(stderr, "[main] SDL initialized\n");

  LaunchParameters params{threads_x, threads_y, threads_z,
                          blocks_x,  blocks_y,  blocks_z};
  fprintf(stderr, "[main] Calling load()...\n");
  int ret = load(new_argv.size(), new_argv.data(), envp,
                 const_cast<char *>(image.getBufferStart()),
                 image.getBufferSize(), params, print_resource_usage);

  return ret;
}
