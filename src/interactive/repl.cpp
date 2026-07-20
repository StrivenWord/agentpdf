#include "agentpdf/config.hpp"
#include "agentpdf/pipeline.hpp"
#include "agentpdf/util.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace agentpdf {

namespace {

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> toks;
  std::istringstream iss(line);
  std::string t;
  while (iss >> t) toks.push_back(t);
  return toks;
}

void print_heuristics(const Heuristics& h) {
  std::cout << "header_band_frac=" << h.header_band_frac << "\n"
            << "footer_band_frac=" << h.footer_band_frac << "\n"
            << "column_gap_min_pts=" << h.column_gap_min_pts << "\n"
            << "paragraph_gap_pts=" << h.paragraph_gap_pts << "\n"
            << "rejoin_hyphenation=" << (h.rejoin_hyphenation ? "true" : "false") << "\n"
            << "ocr_workers=" << h.ocr_workers << "\n"
            << "ocr_dpi=" << h.ocr_dpi << "\n";
}

bool set_heuristic(Heuristics& h, const std::string& key, const std::string& val) {
  try {
    if (key == "header_band_frac") h.header_band_frac = std::stod(val);
    else if (key == "footer_band_frac") h.footer_band_frac = std::stod(val);
    else if (key == "column_gap_min_pts") h.column_gap_min_pts = std::stod(val);
    else if (key == "paragraph_gap_pts") h.paragraph_gap_pts = std::stod(val);
    else if (key == "ocr_workers") h.ocr_workers = std::stoi(val);
    else if (key == "ocr_dpi") h.ocr_dpi = std::stoi(val);
    else if (key == "rejoin_hyphenation") h.rejoin_hyphenation = (val == "true" || val == "1");
    else if (key == "strip_running_headers")
      h.strip_running_headers = (val == "true" || val == "1");
    else return false;
  } catch (...) {
    return false;
  }
  return true;
}

}  // namespace

void run_interactive(Heuristics heuristics, MetadataSpec meta_spec,
                     const std::string& heuristics_path,
                     const std::string& metadata_path) {
  std::cout << "agentpdf interactive mode. Type help for commands.\n";
  std::vector<std::future<void>> background;
  std::string line;
  while (true) {
    std::cout << "agentpdf> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    line = trim(line);
    if (line.empty()) continue;
    auto toks = tokenize(line);
    if (toks.empty()) continue;
    const auto& cmd = toks[0];
    if (cmd == "quit" || cmd == "exit") break;
    if (cmd == "help") {
      std::cout << "commands:\n"
                << "  convert <pdf|dir>... [-o DIR]\n"
                << "  show heuristics\n"
                << "  set heuristic <key> <value>\n"
                << "  save heuristics [path]\n"
                << "  jobs\n"
                << "  help | quit\n";
      continue;
    }
    if (cmd == "show" && toks.size() >= 2 && toks[1] == "heuristics") {
      print_heuristics(heuristics);
      continue;
    }
    if (cmd == "set" && toks.size() >= 4 && toks[1] == "heuristic") {
      if (!set_heuristic(heuristics, toks[2], toks[3])) {
        std::cout << "unknown or invalid heuristic\n";
      } else {
        std::cout << "ok\n";
      }
      continue;
    }
    if (cmd == "save" && toks.size() >= 2 && toks[1] == "heuristics") {
      std::string path = toks.size() >= 3 ? toks[2] : heuristics_path;
      std::string err;
      if (!save_heuristics(path, heuristics, err)) std::cout << err << "\n";
      else std::cout << "saved " << path << "\n";
      continue;
    }
    if (cmd == "jobs") {
      size_t running = 0;
      for (auto& f : background) {
        if (f.valid() && f.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
          ++running;
      }
      std::cout << "background jobs running: " << running << "\n";
      continue;
    }
    if (cmd == "convert") {
      std::vector<std::string> inputs;
      std::string out_dir = "out";
      for (size_t i = 1; i < toks.size(); ++i) {
        if ((toks[i] == "-o" || toks[i] == "--output") && i + 1 < toks.size()) {
          out_dir = toks[++i];
        } else {
          inputs.push_back(toks[i]);
        }
      }
      if (inputs.empty()) {
        std::cout << "convert requires paths\n";
        continue;
      }
      auto jobs = build_job_index(inputs, out_dir, true);
      Heuristics h_copy = heuristics;
      MetadataSpec m_copy = meta_spec;
      background.push_back(std::async(std::launch::async, [jobs, h_copy, m_copy]() {
        for (const auto& job : jobs) {
          std::string err;
          if (!convert_document(job, h_copy, m_copy, err)) {
            std::cerr << "error: " << err << "\n";
          }
        }
      }));
      std::cout << "started " << jobs.size() << " job(s) in background\n";
      continue;
    }
    std::cout << "unknown command; try help\n";
  }

  for (auto& f : background) {
    if (f.valid()) f.wait();
  }
  (void)metadata_path;
}

}  // namespace agentpdf
