#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace agentpdf {

void write_run_stats(const DocumentDom& dom, const JobEntry& job, const std::string& report_dir) {
  fs::create_directories(report_dir);
  std::ostringstream oss;
  oss << "{\n"
      << "  \"input\": \"" << job.input_path << "\",\n"
      << "  \"output\": \"" << job.output_path << "\",\n"
      << "  \"pages\": " << dom.pages.size() << ",\n"
      << "  \"headings\": " << dom.heading_count << ",\n"
      << "  \"tables\": " << dom.table_count << ",\n"
      << "  \"figures\": " << dom.figure_count << ",\n"
      << "  \"footnotes\": " << dom.footnote_count << ",\n"
      << "  \"title\": \"" << dom.meta.title << "\",\n"
      << "  \"doi\": \"" << dom.meta.doi << "\"\n"
      << "}\n";
  std::string err;
  auto name = stem_filename(job.filename) + ".stats.json";
  write_text_file((fs::path(report_dir) / name).string(), oss.str(), err);
}

}  // namespace agentpdf
