# CMake toolchain file for cross-compiling to amdgcn-amd-amdhsa (AMDGPU)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR amdgcn)
set(CMAKE_CROSSCOMPILING ON)

set(CMAKE_C_COMPILER_TARGET amdgcn-amd-amdhsa)
set(CMAKE_CXX_COMPILER_TARGET amdgcn-amd-amdhsa)
set(CMAKE_ASM_COMPILER_TARGET amdgcn-amd-amdhsa)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
