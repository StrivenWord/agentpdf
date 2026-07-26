#pragma once

namespace agentpdf {

// Runtime SIMD feature detection. On x86-64 this returns whether the executing
// CPU supports AVX2, SSE4.2, or SSSE3. On non-x86 platforms (ARM64, etc.) it
// always returns false. The function results are cached after the first call.
bool cpu_has_avx2();
bool cpu_has_sse42();

// Convert one row of ARGB (poppler::image byte order) into RGBA
// (Tesseract SetImage byte order). The fast x86 path is used only if the CPU
// reports AVX2 or SSE4.2 support and the corresponding file was compiled in;
// otherwise a portable scalar loop runs.  Both paths produce identical bytes.
void argb_to_rgba_row(const unsigned char* src, unsigned char* dst, int width);

// Convert one row of ARGB into a Leptonica 32-bit RGBA pixel row (packed l_uint32).
// Same portability rule as argb_to_rgba_row.
void argb_to_leptonica_row(const unsigned char* src, unsigned char* dst, int width);

}  // namespace agentpdf
