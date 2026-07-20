#include "agentpdf/pipeline.hpp"
#include "agentpdf/util.hpp"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace agentpdf {

namespace {

bool is_pdf(const fs::path& p) {
  auto ext = to_lower(p.extension().string());
  return ext == ".pdf";
}

std::string make_output_path(const fs::path& input, const fs::path& input_root,
                             const std::string& output_dir) {
  fs::path rel = fs::relative(input, input_root);
  if (rel.empty() || *rel.begin() == "..") {
    rel = input.filename();
  }
  fs::path out = fs::path(output_dir) / rel;
  out.replace_extension(".md");
  return out.string();
}

}  // namespace

std::vector<JobEntry> build_job_index(const std::vector<std::string>& inputs,
                                      const std::string& output_dir,
                                      bool recursive) {
  std::vector<JobEntry> jobs;
  std::string out_root = output_dir.empty() ? "out" : output_dir;

  for (const auto& in : inputs) {
    fs::path p(in);
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && is_pdf(p)) {
      JobEntry j;
      j.input_path = fs::absolute(p).string();
      j.relative_path = p.filename().string();
      j.filename = p.filename().string();
      j.size_bytes = fs::file_size(p, ec);
      j.output_path = make_output_path(p, p.parent_path(), out_root);
      jobs.push_back(std::move(j));
      continue;
    }
    if (!fs::is_directory(p, ec)) {
      std::cerr << "skip (not found): " << in << "\n";
      continue;
    }
    fs::path root = fs::absolute(p);
    auto add_file = [&](const fs::directory_entry& e) {
      if (!e.is_regular_file() || !is_pdf(e.path())) return;
      JobEntry j;
      j.input_path = e.path().string();
      j.relative_path = fs::relative(e.path(), root).string();
      j.filename = e.path().filename().string();
      std::error_code fec;
      j.size_bytes = fs::file_size(e.path(), fec);
      j.output_path = make_output_path(e.path(), root, out_root);
      jobs.push_back(std::move(j));
    };
    if (recursive) {
      for (const auto& e : fs::recursive_directory_iterator(root, ec)) add_file(e);
    } else {
      for (const auto& e : fs::directory_iterator(root, ec)) add_file(e);
    }
  }
  return jobs;
}

}  // namespace agentpdf
