#include "agentpdf/pipeline.hpp"

#include <iostream>

namespace agentpdf {

void print_usage() {
  std::cerr
      << "agentpdf — academic PDF to Markdown converter\n"
      << "Usage:\n"
      << "  agentpdf convert <pdf|dir>... [-o DIR] [--heuristics PATH] [--metadata PATH]\n"
      << "  agentpdf --interactive | -i\n"
      << "  agentpdf --help\n";
}

CliOptions parse_args(int argc, char** argv) {
  CliOptions opt;
  if (argc <= 1) {
    opt.show_help = true;
    return opt;
  }
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      opt.show_help = true;
    } else if (a == "--interactive" || a == "-i") {
      opt.interactive = true;
    } else if (a == "convert") {
      opt.command = "convert";
    } else if (a == "-o" || a == "--output") {
      if (i + 1 < argc) opt.output_dir = argv[++i];
    } else if (a == "--heuristics") {
      if (i + 1 < argc) opt.heuristics_path = argv[++i];
    } else if (a == "--metadata") {
      if (i + 1 < argc) opt.metadata_path = argv[++i];
    } else if (a == "--no-recursive") {
      opt.recursive = false;
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "unknown option: " << a << "\n";
      opt.show_help = true;
    } else {
      if (opt.command.empty()) opt.command = "convert";
      opt.inputs.push_back(a);
    }
  }
  return opt;
}

}  // namespace agentpdf
