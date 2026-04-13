cmake_minimum_required(VERSION 3.20)
project(doomgpu LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CMake's Windows-Clang-GNU platform adds -nostartfiles -nostdlib to link
# flags, breaking normal CRT startup. Remove them.
string(REPLACE "-nostartfiles" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
string(REPLACE "-nostdlib" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}" CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# Locate the ROCm SDK from the Python venv
# ---------------------------------------------------------------------------
# The user provides ROCM_VENV pointing at the venv root.
# Alternatively, auto-detect from the Python running in PATH.
if(NOT ROCM_VENV)
  set(ROCM_VENV "D:/jam/venv" CACHE PATH "Path to Python venv with ROCm SDK wheels")
endif()

set(ROCM_CORE_DIR "${ROCM_VENV}/Lib/site-packages/_rocm_sdk_core")
set(ROCM_DEVEL_DIR "${ROCM_VENV}/Lib/site-packages/_rocm_sdk_devel")
set(LLVM_DIR "${ROCM_DEVEL_DIR}/lib/llvm")
set(CLANG_EXE "${LLVM_DIR}/bin/clang.exe")
set(CLANGXX_EXE "${LLVM_DIR}/bin/clang++.exe")
set(LLD_EXE "${LLVM_DIR}/bin/ld.lld.exe")

# LLVM libc source for RPC headers and GPU libc build
if(NOT LLVM_SOURCE)
  set(LLVM_SOURCE "D:/jam/TheRock/compiler/amd-llvm" CACHE PATH
      "Path to LLVM source tree (amd-llvm) containing libc/")
endif()

# GPU target
set(AMDGPU_TARGET "gfx1201" CACHE STRING "AMDGPU target architecture")

# ---------------------------------------------------------------------------
# zlib (vendored submodule, needed by LLVM libs)
# ---------------------------------------------------------------------------
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/zlib EXCLUDE_FROM_ALL)
# zlib's CMake doesn't export a clean target name; alias it
if(NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# ---------------------------------------------------------------------------
# zstd (vendored submodule, needed by LLVM libs)
# ---------------------------------------------------------------------------
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/zstd/build/cmake EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# SDL2 (vendored submodule)
# ---------------------------------------------------------------------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

# Ensure SDL2 links the MSVC CRT (needed when using clang with lld-link)
if(MSVC OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND WIN32))
  target_link_libraries(SDL2 PRIVATE msvcrt ucrt vcruntime)
endif()

# ---------------------------------------------------------------------------
# Sub-projects
# ---------------------------------------------------------------------------
add_subdirectory(doomgeneric)
add_subdirectory(amdgpu-loader)
