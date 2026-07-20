#include "agentpdf/pipeline.hpp"
#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <iostream>

namespace agentpdf {

bool convert_document(const JobEntry& job, const Heuristics& heuristics,
                      const MetadataSpec& meta_spec, std::string& err) {
  auto extracted = extract_pdf_dom(job.input_path, heuristics);
  if (!extracted.ok) {
    err = extracted.err;
    return false;
  }
  DocumentDom& dom = extracted.dom;
  build_blocks_from_lines(dom, heuristics);
  isolate_footnotes(dom, heuristics);
  extract_and_validate_metadata(dom, meta_spec);

  // Drop abstract paragraph duplicate from body if stored in YAML (keep heading).
  // Keep body abstract for ACM-style papers where handmade includes ABSTRACT section.

  std::string md = assemble_markdown(dom, heuristics);
  if (!write_text_file(job.output_path, md, err)) return false;
  write_run_stats(dom, job, "run-reports");
  std::cerr << "wrote " << job.output_path << " (" << dom.pages.size() << " pages)\n";
  return true;
}

}  // namespace agentpdf
