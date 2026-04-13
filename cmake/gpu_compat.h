// Compatibility shim for __gpu_lane_scan_u32 missing in installed gpuintrin.h.
// Guarded so CMake compiler-test passes on x86 (gpuintrin.h is GPU-only).
#ifndef GPU_COMPAT_H
#define GPU_COMPAT_H

#ifdef __AMDGCN__

#include <gpuintrin.h>

#ifndef __gpu_lane_scan_u32
static __inline__ __attribute__((always_inline, convergent))
uint32_t __gpu_lane_scan_u32(uint64_t __lane_mask, uint32_t __x) {
  for (uint32_t __step = 1; __step < __gpu_num_lanes(); __step *= 2) {
    uint32_t __index = __gpu_lane_id() - __step;
    uint32_t __tmp = __gpu_shuffle_idx_u32(__lane_mask, __index, __x,
                                           __gpu_num_lanes());
    if (__gpu_lane_id() >= __step)
      __x += __tmp;
  }
  return __x;
}
#endif

#endif // __AMDGCN__

#endif // GPU_COMPAT_H
