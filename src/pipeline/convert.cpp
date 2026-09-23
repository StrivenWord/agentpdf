#include "agentpdf/pipeline.hpp"
#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <iostream>
#include <string>

namespace agentpdf {

bool convert_document(const JobEntry& job, const Heuristics& heuristics,
                      const MetadataSpec& meta_spec, std::string& err) {
  auto extracted = extract_pdf_dom(job.input_path, heuristics);
  if (!extracted.ok) {
    err = extracted.err;
    return false;
  }
  DocumentDom& dom = extracted.dom;
  // Title/authors/DOI come from front-matter evidence first: body assembly
  // needs the title to recognise and drop the printed title block.
  extract_front_matter_metadata(dom);
  build_blocks_from_lines(dom, heuristics);
  isolate_footnotes(dom, heuristics);
  extract_and_validate_metadata(dom, meta_spec);

  // A conversion without body text is a failure, whatever metadata was
  // found: no Markdown is written, so an empty note never enters the vault.
  // (Missing metadata alone does not fail a conversion.)
  size_t body_words = 0;
  for (const auto& page : dom.pages) {
    for (const auto& block : page.blocks) body_words += split_words(block.text).size();
  }
  for (const auto& note : dom.endnotes) body_words += split_words(note).size();
  if (body_words == 0) {
    err = "empty body: no text could be extracted (" + std::to_string(dom.pages.size()) +
          " pages)";
    return false;
  }

  // Drop abstract paragraph duplicate from body if stored in YAML (keep heading).
  // Keep body abstract for ACM-style papers where handmade includes ABSTRACT section.

  std::string md = assemble_markdown(dom, heuristics);
  if (!write_text_file(job.output_path, md, err)) return false;
  write_run_stats(dom, job, "run-reports");
  std::cerr << "wrote " << job.output_path << " (" << dom.pages.size() << " pages)\n";
  return true;
}

}  // namespace agentpdf
