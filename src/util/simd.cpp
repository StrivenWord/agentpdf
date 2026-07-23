#include "agentpdf/simd.hpp"

#include <cstdint>
#include <cstring>

namespace agentpdf {

bool cpu_has_avx2() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#if defined(__GNUC__) || defined(__clang__)
  static const bool has = __builtin_cpu_supports("avx2");
  return has;
#elif defined(_MSC_VER)
#include <intrin.h>
  int info[4] = {0};
  __cpuid(info, 7);
  static const bool has = (info[1] & (1 << 5)) != 0;
  return has;
#else
  return false;
#endif
#else
  return false;
#endif
}

// Forward declarations for the x86 AVX2 helpers.  These are only defined when
// AGENTPDF_HAS_AVX2_PATH is set by CMake (i.e., x86-64/x86 build with the AVX2
// file enabled), so the calls below are guarded by both the macro and the
// architecture.
#if defined(AGENTPDF_HAS_AVX2_PATH) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__))
extern void avx2_argb_to_rgba_row(const unsigned char* src, unsigned char* dst, int width);
extern void avx2_argb_to_leptonica_row(const unsigned char* src, unsigned char* dst, int width);
#endif

void argb_to_rgba_row(const unsigned char* src, unsigned char* dst, int width) {
#if defined(AGENTPDF_HAS_AVX2_PATH) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__))
  if (cpu_has_avx2()) {
    avx2_argb_to_rgba_row(src, dst, width);
    return;
  }
#endif
  for (int x = 0; x < width; ++x) {
    dst[x * 4 + 0] = src[x * 4 + 1];
    dst[x * 4 + 1] = src[x * 4 + 2];
    dst[x * 4 + 2] = src[x * 4 + 3];
    dst[x * 4 + 3] = src[x * 4 + 0];
  }
}

void argb_to_leptonica_row(const unsigned char* src, unsigned char* dst, int width) {
#if defined(AGENTPDF_HAS_AVX2_PATH) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__))
  if (cpu_has_avx2()) {
    avx2_argb_to_leptonica_row(src, dst, width);
    return;
  }
#endif
  for (int x = 0; x < width; ++x) {
    // Poppler ARGB memory order: [A, R, G, B].
    // Leptonica 32-bit RGB pixel value is 0x00RRGGBB, which in memory is [B, G, R, 0].
    dst[x * 4 + 0] = src[x * 4 + 3];  // B
    dst[x * 4 + 1] = src[x * 4 + 2];  // G
    dst[x * 4 + 2] = src[x * 4 + 1];  // R
    dst[x * 4 + 3] = 0;               // A
  }
}

}  // namespace agentpdf
