#include "agentpdf/pdf.hpp"
#include "agentpdf/simd.hpp"

#include <leptonica/allheaders.h>

namespace agentpdf {

bool leptonica_analyze_raw(const unsigned char* argb, int width, int height, int bytes_per_row,
                           double& skew_deg, std::vector<BBox>& content_regions, std::string& err) {
  skew_deg = 0;
  content_regions.clear();
  if (!argb || width <= 0 || height <= 0) {
    err = "invalid image for leptonica";
    return false;
  }

  PIX* pix = pixCreate(width, height, 32);
  if (!pix) {
    err = "pixCreate failed";
    return false;
  }
  l_uint32* data = pixGetData(pix);
  int wpl = pixGetWpl(pix);
  for (int y = 0; y < height; ++y) {
    const unsigned char* src = argb + static_cast<size_t>(y) * static_cast<size_t>(bytes_per_row);
    unsigned char* dst = reinterpret_cast<unsigned char*>(data + static_cast<size_t>(y) * wpl);
    argb_to_leptonica_row(src, dst, width);
  }

  PIX* gray = pixConvertRGBToGray(pix, 0.0f, 0.0f, 0.0f);
  pixDestroy(&pix);
  if (!gray) {
    err = "grayscale convert failed";
    return false;
  }

  l_float32 angle = 0, conf = 0;
  PIX* deskewed = pixFindSkewAndDeskew(gray, 2, &angle, &conf);
  if (deskewed) {
    skew_deg = static_cast<double>(angle);
    pixDestroy(&deskewed);
  }

  PIX* bin = pixThresholdToBinary(gray, 180);
  pixDestroy(&gray);
  if (!bin) {
    err = "binarize failed";
    return false;
  }

  BOX* box = nullptr;
  if (pixClipToForeground(bin, nullptr, &box) == 0 && box) {
    l_int32 bx = 0, by = 0, bw = 0, bh = 0;
    boxGetGeometry(box, &bx, &by, &bw, &bh);
    BBox b;
    b.x0 = bx;
    b.y0 = by;
    b.x1 = bx + bw;
    b.y1 = by + bh;
    content_regions.push_back(b);
    boxDestroy(&box);
  }
  pixDestroy(&bin);
  return true;
}

}  // namespace agentpdf
