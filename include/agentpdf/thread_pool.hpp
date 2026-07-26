#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace agentpdf {

// A simple, dependency-free thread pool for parallel work across all supported
// platforms (Intel/ARM Mac, Linux, Windows). Jobs are submitted as
// std::function<void()>. The destructor waits for all queued and running jobs.
//
// On low-core systems (<=2 hardware threads) the pool uses a shorter sleep
// interval between job checks to reduce context-switch overhead and improve
// throughput when the worker count matches the physical core count.
class ThreadPool {
 public:
  explicit ThreadPool(size_t worker_count);
  ~ThreadPool();

  void submit(std::function<void()> job);
  void wait_for_all();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

 private:
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> jobs_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable done_cv_;
  bool stop_ = false;
  size_t active_ = 0;
  bool low_core_;  // true when hardware_concurrency() <= 2
};

}  // namespace agentpdf
