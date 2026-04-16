# ---------------------------------------------------------------------------
# Common settings for all loaders
# ---------------------------------------------------------------------------

string(REPLACE "-nostartfiles" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
string(REPLACE "-nostdlib" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

set(LLVM_LIB_DIR "${LLVM_DIR}/lib")
set(LLVM_LIBS
  LLVMBinaryFormat
  LLVMFrontendOffloading
  LLVMAMDGPUDesc
  LLVMAMDGPUInfo
  LLVMAMDGPUUtils
  LLVMObject
  LLVMSupport
  LLVMDemangle
  LLVMTargetParser
  LLVMMCParser
  LLVMMC
  LLVMCore
  LLVMBitReader
  LLVMRemarks
  LLVMBitstreamReader
  LLVMDebugInfoDWARF
  LLVMDebugInfoDWARFLowLevel
  LLVMTransformUtils
  LLVMIRReader
  LLVMAsmParser
  LLVMTextAPI
  LLVMAnalysis
  LLVMProfileData
)

# ---------------------------------------------------------------------------
# HIP-based loader
# ---------------------------------------------------------------------------

add_executable(hip-loader hip-loader.cpp)

target_include_directories(hip-loader PRIVATE
  ${ROCM_CORE_DIR}/include
  ${ROCM_DEVEL_DIR}/include
  ${LLVM_DIR}/include
  ${LLVM_SOURCE}/libc
  ${CMAKE_SOURCE_DIR}/doomgeneric
)

target_link_libraries(hip-loader PRIVATE SDL2::SDL2 SDL2_mixer::SDL2_mixer)
target_link_libraries(hip-loader PRIVATE "${ROCM_CORE_DIR}/lib/amdhip64.lib")

foreach(lib ${LLVM_LIBS})
  target_link_libraries(hip-loader PRIVATE "${LLVM_LIB_DIR}/${lib}.lib")
endforeach()

target_link_libraries(hip-loader PRIVATE zlibstatic libzstd_static)
target_link_libraries(hip-loader PRIVATE advapi32 shell32 ole32 ntdll)

target_compile_definitions(hip-loader PRIVATE
  _CRT_SECURE_NO_WARNINGS
  NOMINMAX
  __HIP_PLATFORM_AMD__
  LIBC_NAMESPACE=__llvm_libc
)

target_compile_options(hip-loader PRIVATE -fno-stack-protector)
set_target_properties(hip-loader PROPERTIES LINK_FLAGS "-Wl,/subsystem:console")
add_dependencies(hip-loader doomgeneric_gpu)

# Copy vendored shared library DLLs beside hip-loader.exe after build
if(WIN32)
  add_custom_command(TARGET hip-loader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:SDL2>" "$<TARGET_FILE_DIR:hip-loader>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:SDL2_mixer>" "$<TARGET_FILE_DIR:hip-loader>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/soundfonts/Roland.SC-55.sf2" "$<TARGET_FILE_DIR:hip-loader>"
    COMMENT "Copying SDL2.dll, SDL2_mixer.dll, and soundfont beside hip-loader.exe"
  )
endif()

