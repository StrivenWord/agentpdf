#include "agentpdf/thread_pool.hpp"

namespace agentpdf {

ThreadPool::ThreadPool(size_t worker_count) {
  for (size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this]() {
      while (true) {
        std::function<void()> job;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
          if (stop_ && jobs_.empty()) return;
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
