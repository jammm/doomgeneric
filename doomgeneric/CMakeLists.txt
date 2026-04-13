# ---------------------------------------------------------------------------
# Cross-compile DOOM for AMDGPU (amdgcn-amd-amdhsa)
#
# This cannot be a normal CMake target because we are cross-compiling for a
# GPU target.  Instead we use add_custom_command to invoke clang directly.
# ---------------------------------------------------------------------------

set(GPU_TRIPLE "amdgcn-amd-amdhsa")
set(GPU_CC "${CLANG_EXE}")
set(GPU_CXX "${CLANGXX_EXE}")
set(GPU_LINKER "${CLANG_EXE}")

set(GPU_OBJDIR "${CMAKE_CURRENT_BINARY_DIR}/gpu_obj")
file(MAKE_DIRECTORY ${GPU_OBJDIR})

set(GPU_CFLAGS
  -O3
  --target=${GPU_TRIPLE}
  -mcpu=${AMDGPU_TARGET}
  -flto
  -nogpulib
  -fno-builtin-printf
  -mllvm -amdgpu-function-calls=false
  -Wall
  -DNORMALUNIX -DLINUX -DSNDSERV -D_DEFAULT_SOURCE -DFEATURE_SOUND
  -I${LLVM_SOURCE}/libc
  -I${LLVM_DIR}/include
  -isystem${CMAKE_BINARY_DIR}/gpu_libc_install/include
)

# All C sources that make up the DOOM game (from Makefile.amdgpu)
set(GPU_C_SOURCES
  dummy.c
  am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c d_iwad.c
  d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c
  hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c
  i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c
  m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c
  p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c
  p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c
  p_spec.c p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c r_data.c
  r_draw.c r_main.c r_plane.c r_segs.c r_sky.c r_things.c sha1.c
  sounds.c statdump.c st_lib.c st_stuff.c s_sound.c tables.c v_video.c
  wi_stuff.c w_checksum.c w_file.c w_main.c w_wad.c z_zone.c
  w_file_stdc.c i_input.c i_video.c doomgeneric.c
  i_rpcsound.c i_rpcmusic.c
)

set(GPU_CC_SOURCES
  doomgeneric_gpu.cc
)

# Build a list of all .o outputs and custom commands for each source
set(GPU_OBJECTS)

foreach(src ${GPU_C_SOURCES})
  get_filename_component(basename ${src} NAME_WE)
  set(obj "${GPU_OBJDIR}/${basename}.o")
  list(APPEND GPU_OBJECTS ${obj})
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${GPU_CC} ${GPU_CFLAGS} -c
            "${CMAKE_CURRENT_SOURCE_DIR}/${src}" -o ${obj}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
    COMMENT "GPU: ${src}"
    VERBATIM
  )
endforeach()

foreach(src ${GPU_CC_SOURCES})
  get_filename_component(basename ${src} NAME_WE)
  set(obj "${GPU_OBJDIR}/${basename}.o")
  list(APPEND GPU_OBJECTS ${obj})
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${GPU_CXX} ${GPU_CFLAGS} -c
            "${CMAKE_CURRENT_SOURCE_DIR}/${src}" -o ${obj}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
    COMMENT "GPU: ${src}"
    VERBATIM
  )
endforeach()

# ---------------------------------------------------------------------------
# GPU libc: build crt1.o and libc.a from LLVM source
# ---------------------------------------------------------------------------
# We build the GPU libc as an ExternalProject so it gets its own CMake
# configure / build cycle targeting amdgcn-amd-amdhsa.
include(ExternalProject)

set(GPU_LIBC_BUILD_DIR "${CMAKE_BINARY_DIR}/gpu_libc_build")
set(GPU_LIBC_INSTALL_DIR "${CMAKE_BINARY_DIR}/gpu_libc_install")

set(AMDGPU_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/amdgpu-toolchain.cmake")

ExternalProject_Add(gpu_libc
  SOURCE_DIR "${LLVM_SOURCE}/runtimes"
  BINARY_DIR "${GPU_LIBC_BUILD_DIR}"
  CMAKE_ARGS
    -DCMAKE_TOOLCHAIN_FILE=${AMDGPU_TOOLCHAIN_FILE}
    -DCMAKE_C_COMPILER=${CLANG_EXE}
    -DCMAKE_CXX_COMPILER=${CLANGXX_EXE}
    -DLLVM_ENABLE_RUNTIMES=libc
    -DLIBC_TARGET_TRIPLE=${GPU_TRIPLE}
    -DLIBC_GPU_BUILD=ON
    -DLIBC_GPU_TARGET_ARCHITECTURE=${AMDGPU_TARGET}
    -DLIBC_GPU_TEST_ARCHITECTURE=${AMDGPU_TARGET}
    -DCMAKE_INSTALL_PREFIX=${GPU_LIBC_INSTALL_DIR}
    -DLLVM_CMAKE_DIR=${LLVM_DIR}/lib/cmake/llvm
    -DCMAKE_BUILD_TYPE=Release
    -DLLVM_VERSION_MAJOR=23
    -DLLVM_VERSION_MINOR=0
    -DLLVM_VERSION_PATCH=0
    -DLLVM_LIBC_FULL_BUILD=ON
    -DLIBC_HDRGEN_EXE=IGNORE
    -DCOMPILER_RESOURCE_DIR=${LLVM_DIR}/lib/clang/23
    "-DCMAKE_CXX_FLAGS=-include ${CMAKE_SOURCE_DIR}/cmake/gpu_compat.h"
    "-DCMAKE_C_FLAGS=-include ${CMAKE_SOURCE_DIR}/cmake/gpu_compat.h"
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
  BUILD_BYPRODUCTS
    "${GPU_LIBC_INSTALL_DIR}/lib/${GPU_TRIPLE}/crt1.o"
    "${GPU_LIBC_INSTALL_DIR}/lib/${GPU_TRIPLE}/libc.a"
    "${GPU_LIBC_INSTALL_DIR}/lib/${GPU_TRIPLE}/libm.a"
)

set(GPU_CRT1 "${GPU_LIBC_INSTALL_DIR}/lib/${GPU_TRIPLE}/crt1.o")
set(GPU_LIBC_LIB_DIR "${GPU_LIBC_INSTALL_DIR}/lib/${GPU_TRIPLE}")

# ---------------------------------------------------------------------------
# Link the GPU binary
# ---------------------------------------------------------------------------
set(GPU_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/doomgeneric")

add_custom_command(
  OUTPUT ${GPU_OUTPUT}
  COMMAND ${GPU_LINKER}
    --target=${GPU_TRIPLE}
    -mcpu=${AMDGPU_TARGET}
    -flto
    -nogpulib
    -L${GPU_LIBC_LIB_DIR}
    ${GPU_OBJECTS}
    ${GPU_CRT1}
    -lm -lc
    -o ${GPU_OUTPUT}
  DEPENDS ${GPU_OBJECTS}
  COMMENT "Linking GPU DOOM binary"
  VERBATIM
)

add_custom_target(doomgeneric_gpu ALL
  DEPENDS ${GPU_OUTPUT} gpu_libc
)
