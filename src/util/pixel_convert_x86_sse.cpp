// x86-64/x86 SSE4.2/SSSE3 pixel-format helpers.  This file is only added to the
// agentpdf target on x86 builds, and is compiled with -mssse3 -msse4.2 (GCC/Clang)
// scoped to this single translation unit.  On ARM64 or other non-x86 targets this
// file is not compiled at all.  This path is useful for CPUs that lack AVX2 but
// still have SSSE3/SSE4.2 (e.g. Intel Celeron N3350, pre-Haswell Intel Macs).

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)

#include "agentpdf/simd.hpp"

#include <tmmintrin.h>  // SSSE3: _mm_shuffle_epi8
#include <emmintrin.h>  // SSE2: basic load/store

namespace agentpdf {

// ARGB (poppler memory order) -> RGBA (Tesseract SetImage order).
// Output bytes: [R, G, B, A] from input [A, R, G, B].
// Processes 4 pixels (16 bytes) per iteration via SSSE3 pshufb.
void sse42_argb_to_rgba_row(const unsigned char* src, unsigned char* dst, int width) {
  // Shuffle mask: for each 4-byte group [A,R,G,B] -> [R,G,B,A].
  const __m128i shuffle = _mm_set_epi8(
      15, 12, 13, 14,  // pixel 3: [A3,R3,G3,B3] -> [R3,G3,B3,A3]
      11,  8,  9, 10,  // pixel 2
       7,  4,  5,  6,  // pixel 1
       3,  0,  1,  2   // pixel 0
  );
  int x = 0;
  for (; x + 4 <= width; x += 4) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
    v = _mm_shuffle_epi8(v, shuffle);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x * 4), v);
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
void sse42_argb_to_leptonica_row(const unsigned char* src, unsigned char* dst, int width) {
  // Shuffle mask: [A,R,G,B] -> [B,G,R,0x80] per pixel.
  // 0x80 in pshufb produces a zero byte.
  const __m128i shuffle = _mm_set_epi8(
      0x80, 15, 14, 13,  // pixel 3: [A3,R3,G3,B3] -> [0,B3,G3,R3]
      0x80, 11, 10,  9,  // pixel 2
      0x80,  7,  6,  5,  // pixel 1
      0x80,  3,  2,  1   // pixel 0
  );
  int x = 0;
  for (; x + 4 <= width; x += 4) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
    v = _mm_shuffle_epi8(v, shuffle);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x * 4), v);
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
