// x86-64/x86 AVX2 pixel-format helpers.  This file is only added to the
// agentpdf target on x86 builds, and is compiled with -mavx2 (GCC/Clang) or
// /arch:AVX2 (MSVC) scoped to this single translation unit.  On ARM64 or other
// non-x86 targets this file is not compiled at all.

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)

#include "agentpdf/simd.hpp"

#include <immintrin.h>

namespace agentpdf {

// ARGB (poppler memory order) -> RGBA (Tesseract SetImage order).
// Output bytes: [R, G, B, A] from input [A, R, G, B].
void avx2_argb_to_rgba_row(const unsigned char* src, unsigned char* dst, int width) {
  const __m256i shuffle = _mm256_set_epi8(
      28, 31, 30, 29, 24, 27, 26, 25, 20, 23, 22, 21, 16, 19, 18, 17,
      12, 15, 14, 13, 8, 11, 10, 9, 4, 7, 6, 5, 0, 3, 2, 1);
  int x = 0;
  for (; x + 8 <= width; x += 8) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + x * 4));
    v = _mm256_shuffle_epi8(v, shuffle);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + x * 4), v);
  }
  for (; x < width; ++x) {
    dst[x * 4 + 0] = src[x * 4 + 1];
    dst[x * 4 + 1] = src[x * 4 + 2];
    dst[x * 4 + 2] = src[x * 4 + 3];
    dst[x * 4 + 3] = src[x * 4 + 0];
  }
}

// Poppler ARGB memory order -> Leptonica 32-bit RGB packed pixels.
// Leptonica l_uint32 value is 0x00RRGGBB, which in little-endian memory is [B, G, R, 0].
// Input bytes are [A, R, G, B], so the output bytes are [B, G, R, 0].
void avx2_argb_to_leptonica_row(const unsigned char* src, unsigned char* dst, int width) {
  const __m256i shuffle = _mm256_set_epi8(
      0x80, 31, 30, 29, 0x80, 27, 26, 25, 0x80, 23, 22, 21, 0x80, 19, 18, 17,
      0x80, 15, 14, 13, 0x80, 11, 10, 9, 0x80, 7, 6, 5, 0x80, 3, 2, 1);
  int x = 0;
  for (; x + 8 <= width; x += 8) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + x * 4));
    v = _mm256_shuffle_epi8(v, shuffle);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + x * 4), v);
  }
  for (; x < width; ++x) {
    dst[x * 4 + 0] = src[x * 4 + 3];
    dst[x * 4 + 1] = src[x * 4 + 2];
    dst[x * 4 + 2] = src[x * 4 + 1];
    dst[x * 4 + 3] = 0;
  }
}

}  // namespace agentpdf

#endif  // defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
