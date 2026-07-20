#pragma once

#include "agentpdf/types.hpp"

#include <string>
#include <vector>

namespace agentpdf {

CliOptions parse_args(int argc, char** argv);
void print_usage();

std::vector<JobEntry> build_job_index(const std::vector<std::string>& inputs,
                                      const std::string& output_dir,
                                      bool recursive);

bool convert_document(const JobEntry& job, const Heuristics& heuristics,
                      const MetadataSpec& meta_spec, std::string& err);

void run_interactive(Heuristics heuristics, MetadataSpec meta_spec,
                     const std::string& heuristics_path,
                     const std::string& metadata_path);

}  // namespace agentpdf
