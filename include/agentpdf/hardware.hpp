#pragma once

#include <cstddef>
#include <thread>

namespace agentpdf {

// Lightweight hardware capability detection. All results are cached after first
// call. These values are used to tune thread pools, batch sizes, and memory
// usage without requiring external dependencies.

// Returns std::thread::hardware_concurrency(), capped at 1.
inline size_t hardware_thread_count() {
  static const size_t count = [] {
    size_t n = std::thread::hardware_concurrency();
    return n > 0 ? n : 1;
  }();
  return count;
}

// Returns true if the system has 2 or fewer hardware threads (e.g. Celeron
// dual-core, older Intel Mac Minis). On such systems, thread pool contention
// overhead is proportionally higher and batch sizes should be conservative.
inline bool is_low_core_system() {
  return hardware_thread_count() <= 2;
}

// Returns total system RAM in bytes. On Linux reads /proc/meminfo; on other
// platforms returns a conservative default (4 GB). Result is cached.
inline size_t system_memory_bytes() {
  static const size_t bytes = [] {
#if defined(__linux__)
    // Read MemTotal from /proc/meminfo (value is in kB).
    FILE* f = fopen("/proc/meminfo", "r");
    if (f) {
      char line[256];
      while (fgets(line, sizeof(line), f)) {
        unsigned long kb = 0;
        if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) {
          fclose(f);
          return static_cast<size_t>(kb) * 1024;
        }
      }
      fclose(f);
    }
#endif
    // Conservative default: 4 GB
    return static_cast<size_t>(4) * 1024 * 1024 * 1024;
  }();
  return bytes;
}

// Returns true if the system has less than 4 GB of RAM.
inline bool is_memory_constrained() {
  return system_memory_bytes() < static_cast<size_t>(4) * 1024 * 1024 * 1024;
}

// Recommended OCR worker count based on hardware. Caps at hardware threads
// and further reduces on memory-constrained systems to avoid OOM.
inline size_t recommended_ocr_workers() {
  size_t n = hardware_thread_count();
  if (is_memory_constrained() && n > 1) {
    // On <4GB systems, leave one core free for the main thread and I/O.
    n = n - 1;
  }
  return n > 0 ? n : 1;
}

}  // namespace agentpdf
