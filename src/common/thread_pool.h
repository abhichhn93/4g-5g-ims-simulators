#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include "common/logger.h"

// ============================================================
// THREAD POOL — Fixed N workers, shared task queue
//
// WHY THREAD POOL instead of creating a thread per UE:
//   - Thread creation = ~50-100 μs (pthread_create → clone syscall)
//   - "BULK 1000" = 1000 thread creates = 50-100ms just on overhead
//   - Thread pool: 8 threads created ONCE at startup, reused
//   - 1000 tasks → 8 workers each handle ~125 tasks, no create overhead
//
// INTERVIEW Q: "How does a thread pool work internally?"
// ANSWER:
//   - N worker threads created at startup, sleep on condition_variable
//   - submit(): lock → push task → unlock → notify_one() (wake one worker)
//   - worker: wake → lock → pop task → unlock → execute task (NO lock held)
//   - After task: loop back to wait on cv — same thread picks next task
//   - shutdown: stop_=true → notify_all → workers see stop && empty → exit
//
// DESIGN PATTERN: Producer-Consumer
//   Producer: submit() calls (CLI thread, BULK handler)
//   Consumer: worker threads
//   Buffer:   task_queue_ (bounded by memory, not size in this impl)
//
// LINUX COMPARISON:
//   POSIX: pthread_create/join for threads, pthread_mutex + pthread_cond
//   Our impl: std::thread + std::mutex + std::condition_variable (C++11 wrappers)
//   They compile to the same system calls (clone, futex)
//
// CLOUD-NATIVE EQUIVALENT:
//   Thread pool within a pod ≡ Kubernetes Horizontal Pod Autoscaler (HPA)
//   HPA scales the number of AMF pods based on load.
//   Thread pool scales the number of workers within one process.
//   Both solve the same problem: match concurrency to load.
//
// REAL SYSTEM (Samsung MME):
//   Used a hybrid: thread pool + epoll per thread
//   Each worker had its own epoll FD monitoring multiple SCTP streams
//   This gave: parallelism (multiple threads) + efficient I/O (epoll)
//   vs our approach: thread pool + blocking recv per thread (simpler to learn)
// ============================================================
class ThreadPool {
public:
    explicit ThreadPool(int num_workers, const std::string& name = "pool") : stop_(false) {
        workers_.reserve(num_workers);
        Logger::sys("[" + name + "] creating " + std::to_string(num_workers) + " worker threads");
        for (int i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this, i, name]{ workerLoop(i, name); });
        }
        Logger::sys("[" + name + "] all workers started — waiting for tasks");
    }

    ~ThreadPool() {
        stop_.store(true);
        cv_.notify_all();  // wake ALL workers so they see stop_ and exit
        for (auto& w : workers_) { if (w.joinable()) w.join(); }
    }

    // Submit a task — called from ANY thread
    // Thread-safe: lock → push → unlock → notify_one
    void submit(std::function<void()> task) {
        { std::lock_guard<std::mutex> lk(mutex_); queue_.push(std::move(task)); }
        cv_.notify_one();  // wake ONE sleeping worker
    }

    // How many tasks are queued (not yet picked up by a worker)
    size_t queueDepth() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.size();
    }

private:
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> queue_;
    mutable std::mutex                mutex_;
    std::condition_variable           cv_;
    std::atomic<bool>                 stop_;

    // Each worker runs this loop — same pattern as 11_thread_pool.cpp
    void workerLoop(int /*id*/, const std::string& /*name*/) {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mutex_);

                // Sleep until: (a) task available, or (b) stop_ set
                // cv.wait() releases the lock while sleeping (no busy-wait)
                cv_.wait(lk, [this]{ return !queue_.empty() || stop_.load(); });

                // If stop_ set AND queue empty → this worker should exit
                if (stop_.load() && queue_.empty()) {
                    return;  // thread exits
                }

                // Pop task — lock held while accessing queue
                task = std::move(queue_.front());
                queue_.pop();
                // lock released here (lk goes out of scope at end of block)
            }
            // Execute task WITHOUT holding the lock
            // Other workers can pop their own tasks simultaneously
            task();
        }
    }
};
