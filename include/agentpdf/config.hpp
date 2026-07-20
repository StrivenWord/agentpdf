#pragma once

#include "agentpdf/types.hpp"

#include <string>

namespace agentpdf {

bool load_heuristics(const std::string& path, Heuristics& out, std::string& err);
bool load_metadata_spec(const std::string& path, MetadataSpec& out, std::string& err);
bool save_heuristics(const std::string& path, const Heuristics& h, std::string& err);

std::string default_config_dir();
std::string resolve_config_path(const std::string& explicit_path,
                                const std::string& filename);

}  // namespace agentpdf
