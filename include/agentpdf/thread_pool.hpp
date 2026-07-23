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
};

}  // namespace agentpdf
