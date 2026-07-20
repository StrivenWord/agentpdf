#include "agentpdf/config.hpp"
#include "agentpdf/pipeline.hpp"

#include <iostream>

int main(int argc, char** argv) {
  using namespace agentpdf;
  auto opt = parse_args(argc, argv);
  if (opt.show_help || (opt.command.empty() && !opt.interactive)) {
    print_usage();
    return opt.show_help ? 0 : 1;
  }

  std::string heur_path = resolve_config_path(opt.heuristics_path, "heuristics.json");
  std::string meta_path = resolve_config_path(opt.metadata_path, "metadata.json");

  Heuristics heuristics;
  MetadataSpec meta_spec;
  std::string err;
  if (!load_heuristics(heur_path, heuristics, err)) {
    std::cerr << "warning: using built-in heuristics (" << err << ")\n";
  }
  if (!load_metadata_spec(meta_path, meta_spec, err)) {
    std::cerr << "warning: using built-in metadata spec (" << err << ")\n";
  }

  if (opt.interactive) {
    run_interactive(heuristics, meta_spec, heur_path, meta_path);
    return 0;
  }

  if (opt.command == "convert") {
    if (opt.inputs.empty()) {
      std::cerr << "convert requires one or more PDF paths or directories\n";
      return 1;
    }
    auto jobs = build_job_index(opt.inputs, opt.output_dir.empty() ? "out" : opt.output_dir,
                                opt.recursive);
    if (jobs.empty()) {
      std::cerr << "no PDF files found\n";
      return 1;
    }
    int failures = 0;
    for (const auto& job : jobs) {
      std::string job_err;
      if (!convert_document(job, heuristics, meta_spec, job_err)) {
        std::cerr << "failed: " << job.input_path << " — " << job_err << "\n";
        ++failures;
      }
    }
    return failures == 0 ? 0 : 2;
  }

  print_usage();
  return 1;
}
