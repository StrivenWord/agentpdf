#include "agentpdf/thread_pool.hpp"
#include "agentpdf/hardware.hpp"

#include <chrono>

namespace agentpdf {

ThreadPool::ThreadPool(size_t worker_count)
    : low_core_(is_low_core_system()) {
  for (size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this]() {
      while (true) {
        std::function<void()> job;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          if (low_core_) {
            // On low-core systems, avoid long condition_variable waits that
            // cause unnecessary context switches. Use a short timed_wait and
            // fall back to a brief yield-based spin when no job is available.
            while (!stop_ && jobs_.empty()) {
              cv_.wait_for(lock, std::chrono::milliseconds(1));
              if (stop_ && jobs_.empty()) return;
            }
            if (stop_ && jobs_.empty()) return;
          } else {
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_ && jobs_.empty()) return;
          }
          job = std::move(jobs_.front());
          jobs_.pop();
          ++active_;
        }
        if (job) job();
        {
          std::unique_lock<std::mutex> lock(mutex_);
          if (--active_ == 0 && jobs_.empty()) done_cv_.notify_all();
        }
      }
    });
  }
}

ThreadPool::~ThreadPool() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
}

void ThreadPool::submit(std::function<void()> job) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    jobs_.push(std::move(job));
  }
  cv_.notify_one();
}

void ThreadPool::wait_for_all() {
  std::unique_lock<std::mutex> lock(mutex_);
  done_cv_.wait(lock, [this] { return jobs_.empty() && active_ == 0; });
}

}  // namespace agentpdf
