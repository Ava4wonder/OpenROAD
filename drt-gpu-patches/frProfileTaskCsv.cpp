// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, topic contributor.
//
// CSV-backed implementation of drt::ProfileTask. Enabled only when the build
// define PROFILE_CSV is set (see src/drt/CMakeLists.txt).
//
// Design:
//   * Each thread has a thread-local shared_ptr<Buffer> appended to a global
//     registry on first use (one mutex acquire per thread, per process).
//   * Subsequent ProfileTask enter/exit calls are pure thread-local vector
//     appends, lock-free.
//   * The shared_ptr keeps per-thread buffers alive past thread exit so the
//     atexit handler can read them.
//   * At process exit, dump all records to the path in env var
//     DRT_PROFILE_CSV (default: "drt_profile.csv").

#ifdef PROFILE_CSV

#include "frProfileTask.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace drt {

namespace {

struct Record
{
  std::size_t tid_hash;
  std::string name;  // owned; callers may pass .c_str() of transient strings
  long long start_ns;
  long long duration_ns;
};

struct Buffer
{
  std::size_t tid_hash{};
  std::vector<Record> records;
};

struct Sink
{
  std::mutex mtx;
  std::vector<std::shared_ptr<Buffer>> buffers;
};

Sink& global_sink()
{
  // Intentionally leaked: avoids the static-destruction-order fiasco where
  // the Sink's destructor would run before dump_csv() at process exit.
  // dump_csv is registered via std::atexit; __cxa_atexit destructors and
  // atexit handlers share one LIFO queue, so a function-local static would
  // be destroyed first if constructed lazily after AtExitInstaller.
  static Sink* s = new Sink();
  return *s;
}

std::shared_ptr<Buffer>& tls_buffer()
{
  thread_local std::shared_ptr<Buffer> b;
  if (!b) {
    b = std::make_shared<Buffer>();
    b->tid_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto& s = global_sink();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.buffers.push_back(b);
  }
  return b;
}

long long now_ns()
{
  auto t = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

void dump_csv()
{
  const char* env = std::getenv("DRT_PROFILE_CSV");
  if (!env || !*env) {
    env = "drt_profile.csv";
  }
  auto& s = global_sink();
  std::lock_guard<std::mutex> lk(s.mtx);
  // Skip write if this process never pushed any record (avoids clobbering
  // a sibling openroad process's output when multiple stages of the ORFS
  // flow invoke openroad in sequence).
  std::size_t n_records = 0;
  for (const auto& buf : s.buffers) {
    n_records += buf->records.size();
  }
  if (n_records == 0) {
    return;
  }
  char path[4096];
  std::snprintf(path, sizeof(path), "%s.%d", env, static_cast<int>(getpid()));
  std::FILE* f = std::fopen(path, "w");
  if (!f) {
    return;
  }
  std::fprintf(f, "thread_id,task_name,start_ns,duration_ns\n");
  for (const auto& buf : s.buffers) {
    for (const auto& r : buf->records) {
      std::fprintf(f,
                   "%zu,%s,%lld,%lld\n",
                   r.tid_hash,
                   r.name.c_str(),
                   r.start_ns,
                   r.duration_ns);
    }
  }
  std::fclose(f);
}

struct AtExitInstaller
{
  AtExitInstaller()
  {
    // Force eager construction of the Sink's function-local static BEFORE
    // registering the atexit handler. Without this, the static destructor
    // of `Sink s` could run BEFORE dump_csv at process exit (destroyed-
    // before-use UB in the handler).
    (void) global_sink();
    std::atexit(&dump_csv);
  }
};

AtExitInstaller g_atexit_installer;

}  // namespace

ProfileTask::ProfileTask(const char* name)
    : name_(name ? name : "(null)"), start_ns_(now_ns()), done_(false)
{
  (void) tls_buffer();  // ensure this thread is registered
}

ProfileTask::~ProfileTask()
{
  if (!done_) {
    done();
  }
}

void ProfileTask::done()
{
  if (done_) {
    return;
  }
  done_ = true;
  const long long end_ns = now_ns();
  auto& b = tls_buffer();
  b->records.push_back(
      Record{b->tid_hash, std::move(name_), start_ns_, end_ns - start_ns_});
}

}  // namespace drt

#endif  // PROFILE_CSV
