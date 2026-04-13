# Windows AMDGPU Port Changes

This document describes the changes made to port doomgeneric's GPU build
to Windows using HIP on AMD RDNA GPUs.

## Build System

### Top-level CMake (`CMakeLists.txt`)
New top-level CMake project that configures the Windows cross-compilation
toolchain (clang-cl, lld-link) and discovers ROCm/HIP, LLVM, SDL2, zlib,
and zstd dependencies. Adds the `doomgeneric` (GPU binary) and
`amdgpu-loader` subdirectories.

### GPU binary (`doomgeneric/CMakeLists.txt`)
Custom cross-compilation CMake file that compiles all DOOM C sources with
`--target=amdgcn-amd-amdhsa` using the ROCm clang toolchain. Builds the
GPU libc as an ExternalProject, then links the GPU ELF binary.
Compiles with `-O3 -flto` for maximum performance.

### HIP loader (`amdgpu-loader/CMakeLists.txt`)
Builds `hip-loader.exe` which loads the GPU ELF binary via HIP and
services the GPU's libc RPC calls from the host side.

### Helper files
- `cmake/amdgpu-toolchain.cmake` -- CMake toolchain file for AMDGPU cross-compilation
- `cmake/gpu_compat.h` -- Compatibility shims for GPU libc build on Windows
- `amdgpu-loader/win_compat.h` -- POSIX compatibility (nanosleep, clock_gettime) for Windows
- `rebuild.bat` -- Convenience script for clean rebuild with correct DLL copies

## Game Code Changes

### `doomgeneric/i_video.c` -- On-screen FPS overlay
Draws an FPS counter in the top-right corner of the screen using the
in-game HUD font (`hu_font` / STCFN patches). Controlled by the
`fps_overlay` variable:
- `0` -- off
- `1` -- FPS only (default)
- `2` -- advanced: FPS, frame time (ms), and game tick time (ms)

The counter draws into `I_VideoBuffer` (320x200 game resolution) so it
scales automatically with `fb_scaling`. `I_FPS_Drawer` is called from
`D_Display` just before `I_FinishUpdate`. Tick timing hooks
(`I_FPS_TickStart`/`I_FPS_TickEnd`) wrap `TryRunTics` in
`doomgeneric_Tick`.

### `doomgeneric/i_system.c` -- Fix I_Error hang
The original code had `while (true) {}` in `I_Error` when `ORIGCODE` is
undefined, causing the GPU kernel to spin forever on any error. Changed to
call `exit(-1)` unconditionally so errors are reported cleanly via the
libc RPC exit handler.

### `doomgeneric/i_video.c` -- 2D-parallel framebuffer conversion
The original `I_FinishUpdate` parallelized only the X dimension (320
pixels across threads) while iterating all Y rows sequentially on every
thread. At 1280x800 with scaling factor 4, this meant 800 sequential
`cmap_to_fb` calls with only 320 pixels of parallelism each.

**New approach:** Distributes entire output rows across threads. Each
thread processes `ceil(total_rows / num_threads)` complete rows
independently using a new `cmap_to_fb_single` function, with a single
barrier before the final `DG_DrawFrame` call.

Performance impact at 1280x800 (256 threads):
- Before: ~13 fps (each thread iterates all 800 rows, parallelizing only 320 px)
- After: ~60 fps (each thread handles ~3 rows, no inter-thread sync during conversion)

### `doomgeneric/doomgeneric_gpu.cc` -- Zero-copy screen buffer
Added `__dg_screen_buffer` symbol that the host loader sets to a
host-visible coherent GPU allocation. When set, `DG_Init` overrides
`DG_ScreenBuffer` to point directly at this shared memory, eliminating
a per-frame GPU-to-host copy.

## Host Loader (`amdgpu-loader/hip-loader.cpp`)

The HIP-based host loader that:
1. Initializes HIP and loads the GPU ELF binary via `hipModuleLoadData`
2. Sets up shared coherent memory for the RPC ring buffer and screen buffer
3. Copies `argv`, `envp`, the RPC client struct, clock frequency, and
   screen buffer pointer into GPU-visible symbols
4. Dispatches the three GPU kernels (`_begin`, `_start`, `_end`) via
   `hipModuleLaunchKernel`
5. Services GPU libc RPC in a polling loop (malloc, free, file I/O, printf,
   system, exit, etc.) while the kernel runs
6. Renders frames via SDL2 when the GPU issues `DOOM_DRAW_BUFFER` RPC calls

The default thread count is 256 (`--threads-x`), matching the optimal
configuration found during performance tuning.

### Key optimizations
- **Synchronous malloc:** Uses `hipMalloc` instead of `hipMallocAsync` to
  avoid WDDM command submission overhead per allocation
- **Infrequent stream query:** For the long-running `_start` kernel, checks
  `hipStreamQuery` only every 100,000 iterations to avoid the ~25µs per
  HIP→PAL→WDDM round-trip
- **Spin-pause on idle:** Uses `_mm_pause` loops when no RPC work is
  pending to reduce PCIe coherent-memory cache-line contention

## GPU libc Patches (`patches/libc-gpu-fixes.patch`)

Patch against the LLVM libc source tree. Apply with `git apply` before
building. Still unfixed in upstream LLVM as of April 2026.

### ftell opcode bug (`libc/src/stdio/gpu/ftell.cpp`)
`ftell` opens an RPC port with `LIBC_FSEEK` instead of `LIBC_FTELL`,
causing the host to execute `fseek` with garbage offset/whence instead
of returning the file position. DOOM calls `ftell` in `M_FileLength`,
`p_saveg.c`, and `g_game.c`, so this is a required fix.

### Investigated and not needed

- **`long` -> `long long` in fseek/ftell:** On AMDGPU,
  `sizeof(long) == 8` (LP64), so `long` and `long long` are identical.
  No binary difference.
- **GFX12 coherency workaround in `rpc.h`:** The compiler's
  `__scoped_atomic_thread_fence(__ATOMIC_RELEASE, __MEMORY_SCOPE_SYSTEM)`
  already generates the correct GFX12 sequence (`global_wb scope:SCOPE_SYS`
  + `s_wait_storecnt`), confirmed by disassembly. Tested without the
  workaround with no regressions.

## Documentation

- `BUILD_REFERENCE.md` -- Environment setup, build commands, run instructions,
  architecture overview, key source files, and known issues/constraints
- `CHANGES.md` -- This file

## Third-party Dependencies

- `third_party/SDL` -- SDL2 (git submodule)
- `third_party/zlib` -- zlib (git submodule)
- `third_party/zstd` -- zstd (git submodule)
