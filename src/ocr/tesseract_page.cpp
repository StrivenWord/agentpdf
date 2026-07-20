#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>

#include <sstream>
#include <thread>
#include <vector>

namespace agentpdf {

bool tesseract_ocr_page(const unsigned char* argb, int width, int height, int bytes_per_row,
                        const Heuristics& heuristics, std::vector<TextLine>& lines_out,
                        std::string& err) {
  lines_out.clear();
  if (!argb || width <= 0 || height <= 0) {
    err = "invalid image for tesseract";
    return false;
  }

  // Build a contiguous RGBA buffer for SetImage.
  std::vector<unsigned char> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  for (int y = 0; y < height; ++y) {
    const unsigned char* src = argb + static_cast<size_t>(y) * static_cast<size_t>(bytes_per_row);
    unsigned char* dst = rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
    for (int x = 0; x < width; ++x) {
      // Approximate ARGB -> RGBA
      dst[x * 4 + 0] = src[x * 4 + 1];
      dst[x * 4 + 1] = src[x * 4 + 2];
      dst[x * 4 + 2] = src[x * 4 + 3];
      dst[x * 4 + 3] = src[x * 4 + 0];
    }
  }

  tesseract::TessBaseAPI api;
  if (api.Init(nullptr, heuristics.tesseract_lang.c_str())) {
    err = "tesseract init failed";
    return false;
  }
  api.SetImage(rgba.data(), width, height, 4, width * 4);
  api.SetSourceResolution(heuristics.ocr_dpi);
  char* text = api.GetUTF8Text();
  if (!text) {
    err = "tesseract returned no text";
    api.End();
    return false;
  }
  std::istringstream iss(text);
  std::string line;
  double y = 0;
  while (std::getline(iss, line)) {
    line = trim(line);
    if (line.empty()) continue;
    TextLine tl;
    tl.text = line;
    tl.box = BBox{0, y, static_cast<double>(width), y + 12};
    lines_out.push_back(tl);
    y += 14;
  }
  delete[] text;
  api.End();
  return true;
}

}  // namespace agentpdf
