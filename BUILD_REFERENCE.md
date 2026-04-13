# DOOM GPU Build Reference

## Environment Setup

- **OS**: Windows 10/11 (win32)
- **GPU**: AMD RX 9070XT (gfx1201, RDNA4/Navi48)
- **Python venv**: `D:\jam\venv` (contains TheRock wheels + LLVM toolchain)
- **LLVM source**: `D:\jam\TheRock\compiler\amd-llvm` (for GPU libc build)
- **DOOM WAD**: Copy `DOOM.WAD` to `D:\jam\doomgeneric\build\amdgpu-loader\`

## Key Paths

```
ROCM_CORE   = D:\jam\venv\Lib\site-packages\_rocm_sdk_core
ROCM_DEVEL  = D:\jam\venv\Lib\site-packages\_rocm_sdk_devel
LLVM_BIN    = <ROCM_DEVEL>\lib\llvm\bin
CLANG       = <LLVM_BIN>\clang.exe
CLANG_CL    = <LLVM_BIN>\clang-cl.exe
CLANG++     = <LLVM_BIN>\clang++.exe
LLD_LINK    = <LLVM_BIN>\lld-link.exe
```

## Configure (from scratch)

Must run inside a VS x64 developer environment. Use `cmd /c` with vcvars:

```powershell
Remove-Item -Recurse -Force D:\jam\doomgeneric\build -ErrorAction SilentlyContinue
$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$clangCl = "D:/jam/venv/Lib/site-packages/_rocm_sdk_devel/lib/llvm/bin/clang-cl.exe"
cmd /c "`"$vcvars`" && cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release `"-DCMAKE_C_COMPILER=$clangCl`" `"-DCMAKE_CXX_COMPILER=$clangCl`" -DCMAKE_LINKER=lld-link 2>&1"
```

## Build (full)

```powershell
$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" && cmake --build build 2>&1"
```

## Force Rebuild GPU libc Only

When you change files in `D:\jam\TheRock\compiler\amd-llvm\libc\`:

```powershell
Remove-Item -Recurse -Force D:\jam\doomgeneric\build\gpu_libc_build, D:\jam\doomgeneric\build\gpu_libc_install, D:\jam\doomgeneric\build\doomgeneric\gpu_libc-prefix -ErrorAction SilentlyContinue
# Then run the full build command above
```

## Force Rebuild GPU DOOM Binary

When you change DOOM sources or libc and need to relink:

```powershell
Remove-Item -Force D:\jam\doomgeneric\build\doomgeneric\doomgeneric -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force D:\jam\doomgeneric\build\doomgeneric\gpu_obj -ErrorAction SilentlyContinue
# Then run the full build command above
```

## Force Rebuild Everything GPU (libc + DOOM objects + link)

```powershell
Remove-Item -Recurse -Force D:\jam\doomgeneric\build\gpu_libc_build, D:\jam\doomgeneric\build\gpu_libc_install, D:\jam\doomgeneric\build\doomgeneric\gpu_libc-prefix, D:\jam\doomgeneric\build\doomgeneric\doomgeneric, D:\jam\doomgeneric\build\doomgeneric\gpu_obj -ErrorAction SilentlyContinue
# Then run the full build command above
```

## Run

DLLs must be beside `hip-loader.exe`:

```powershell
# Copy DLLs (one-time after clean build)
$dest = "D:\jam\doomgeneric\build\amdgpu-loader"
$src = "D:\jam\venv\Lib\site-packages\_rocm_sdk_core\bin"
Copy-Item "$src\amdhip64_7.dll" $dest
Copy-Item "$src\amd_comgr0702.dll" $dest
Copy-Item "$src\hiprtc0702.dll" $dest
Copy-Item "$src\hiprtc-builtins0702.dll" $dest
Copy-Item "$src\rocm_kpack.dll" $dest
# SDL2.dll is built by cmake into build\third_party\SDL\SDL2.dll
Copy-Item D:\jam\doomgeneric\build\third_party\SDL\SDL2.dll $dest
# Copy WAD
Copy-Item "D:\Program Files (x86)\Steam\steamapps\common\Ultimate Doom\base\DOOM.WAD" $dest
```

Run (with HIP debug logging):

```powershell
$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && cd /d D:\jam\doomgeneric\build\amdgpu-loader && hip-loader.exe ..\doomgeneric\doomgeneric -iwad DOOM.WAD 2>&1"
```

With AMD_LOG_LEVEL (1=error, 3=info, 7=debug):

```powershell
cmd /c "`"$vcvars`" >nul 2>&1 && set AMD_LOG_LEVEL=3 && cd /d D:\jam\doomgeneric\build\amdgpu-loader && hip-loader.exe ..\doomgeneric\doomgeneric -iwad DOOM.WAD 2>&1"
```

## Architecture

- **Host loader** (`hip-loader.exe`): Windows x64, compiled with clang-cl, links LLVM libs + HIP + SDL2
- **GPU binary** (`doomgeneric`): `amdgcn-amd-amdhsa` ELF for gfx1201, compiled with clang + LTO
- **GPU libc** (`libc.a`, `crt1.o`): Built as ExternalProject from `D:\jam\TheRock\compiler\amd-llvm\libc`, uses LTO (bitcode objects)
- **Communication**: RPC ring buffer in shared memory (`hipHostMalloc` with `WriteCombined|Mapped` flags)

## Key Source Files

- `amdgpu-loader/hip-loader.cpp` - Host-side loader, RPC server, SDL2 window
- `doomgeneric/doomgeneric_gpu.cc` - GPU entry point (`_begin`, `_start`)
- `D:\jam\TheRock\compiler\amd-llvm\libc\shared\rpc.h` - RPC protocol (shared between host and GPU)
- `D:\jam\TheRock\compiler\amd-llvm\libc\src\stdio\gpu\fseek.cpp` - GPU-side fseek RPC client
- `D:\jam\TheRock\compiler\amd-llvm\libc\src\stdio\gpu\ftell.cpp` - GPU-side ftell RPC client

## Bugs Fixed

### ftell opcode mismatch
`libc/src/stdio/gpu/ftell.cpp` was calling `rpc::client.open<LIBC_FSEEK>()` instead of
`LIBC_FTELL`. This caused the host to misinterpret `ftell` calls as `fseek`, reading
uninitialized garbage for offset/whence parameters.

### clock_gettime frequency mismatch
The host was setting `__llvm_libc_clock_freq` to the shader engine clock rate
(`hipDeviceAttributeClockRate`, ~2.46 GHz) instead of the fixed-frequency steady counter
rate (100 MHz). This caused `TICKS_PER_SEC / GPU_CLOCKS_PER_SEC` to truncate to 0 in
integer division, making `clock_gettime` always return 0, which caused `TryRunTics` to
hang in an infinite wait loop. Fixed by hardcoding 100 MHz in the loader.

## Known Issues / Constraints

- **HMM/XNACK not supported**: `Direct host access: 0` on Windows RDNA4. No fine-grained coherent memory.
- **RPC buffer uses WriteCombined**: `hipHostMallocMapped | hipHostMallocWriteCombined` (flags=0x6)
- **LTO required**: Without LTO, GPU kernel crashes after 1 RPC call (missing cross-module optimization)
- **`long` type**: Replaced with `long long` throughout (Windows `long` = 4 bytes vs GPU `long` = 8 bytes)

## Current Coherency Patches in rpc.h

- `invert_outbox()`: Uses inline asm for GFX12 (`s_wait_storecnt`, `global_wb scope:SCOPE_SYS`)
