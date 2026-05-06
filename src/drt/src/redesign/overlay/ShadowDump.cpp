// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.e.5 — ShadowDump implementation. See header for activation,
// discipline, and CSV column contract.

#include "ShadowDump.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace drt::redesign::overlay {

namespace {

// Process-wide singletons.
std::mutex& mu()
{
  static std::mutex m;
  return m;
}

// Comparison counters. Atomic so the atexit summary can read them
// without locking; also mutated under mu() during writes (consistent
// with the lock).
std::atomic<std::uint64_t>& comparison_counter()
{
  static std::atomic<std::uint64_t> c{0};
  return c;
}

std::atomic<std::uint64_t>& mismatch_counter()
{
  static std::atomic<std::uint64_t> c{0};
  return c;
}

std::atomic<std::uint64_t>& seq_counter()
{
  static std::atomic<std::uint64_t> c{0};
  return c;
}

// State for the dump pipeline. `state_initialised` flips on first
// RecordMarkerComparison call; `dump_enabled` reflects whether the
// env var is set AND the file opened successfully. `dump_disabled_
// after_failure` flips true if a write fails — the rest of the
// process is silent.
std::atomic<bool>& state_initialised()
{
  static std::atomic<bool> b{false};
  return b;
}

std::atomic<bool>& dump_enabled()
{
  static std::atomic<bool> b{false};
  return b;
}

std::atomic<bool>& dump_disabled_after_failure()
{
  static std::atomic<bool> b{false};
  return b;
}

std::ofstream& dump_file()
{
  static std::ofstream f;
  return f;
}

std::atomic<bool>& summary_emitted()
{
  static std::atomic<bool> b{false};
  return b;
}

// Best-effort: stderr once, then move on. Never throw.
void WarnOnce(const char* what, const std::string& detail)
{
  static std::atomic<bool> warned{false};
  bool expected = false;
  if (warned.compare_exchange_strong(expected, true)) {
    std::cerr << "[drt-redesign-overlay] V2.1.e.5 dump-disabled: "
              << what << ": " << detail << "\n";
  }
}

// Try to mkdir -p the parent directory of `path`. Best-effort; any
// failure is fine if the file open later succeeds, and the file
// open's failure path handles all bad cases.
void TryMkdirParent(const std::string& path)
{
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return;
  }
  const std::string parent = path.substr(0, slash);
  // POSIX mkdir; ignore EEXIST.
  ::mkdir(parent.c_str(), 0755);
}

void EmitSummary()
{
  if (summary_emitted().exchange(true)) {
    return;  // already emitted
  }
  const std::uint64_t calls = comparison_counter().load();
  const std::uint64_t mismatches = mismatch_counter().load();
  std::cerr << "[drt-redesign-overlay] V2.1.e shadow summary: calls="
            << calls << " mismatches=" << mismatches << "\n";
  // Best-effort flush.
  if (dump_file().is_open()) {
    dump_file().flush();
  }
}

void InitOnce()
{
  if (state_initialised().load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> g(mu());
  if (state_initialised().load(std::memory_order_relaxed)) {
    return;
  }

  const char* enable_env = std::getenv("OPENROAD_OVERLAY_DUMP_HASHES");
  const bool want_dump = (enable_env != nullptr && enable_env[0] != '\0'
                          && enable_env[0] != '0');

  if (want_dump) {
    const char* path_env = std::getenv("OPENROAD_OVERLAY_DUMP_PATH");
    std::string path;
    if (path_env != nullptr && path_env[0] != '\0') {
      path = path_env;
    } else {
      std::ostringstream os;
      os << "/tmp/openroad-overlay-hashes/" << ::getpid() << ".csv";
      path = os.str();
    }
    TryMkdirParent(path);
    dump_file().open(path, std::ios::out | std::ios::trunc);
    if (dump_file().is_open()) {
      dump_file()
          << "pid,tid,seqno,xmin,ymin,xmax,ymax,legacy_hash,"
             "overlay_hash,match,legacy_count,overlay_count\n";
      dump_file().flush();
      if (dump_file().good()) {
        dump_enabled().store(true, std::memory_order_release);
      } else {
        WarnOnce("write header failed", path);
        dump_file().close();
      }
    } else {
      WarnOnce("file open failed", path);
    }
  }

  std::atexit(&EmitSummary);
  state_initialised().store(true, std::memory_order_release);
}

}  // namespace

void ShadowDump::RecordMarkerComparison(std::uint64_t legacy_hash,
                                        std::uint64_t overlay_hash,
                                        std::size_t legacy_count,
                                        std::size_t overlay_count,
                                        const Rect& drc_box)
{
  // Counter updates run regardless of dump-enable status — the
  // atexit summary is a separate signal from the CSV dump.
  comparison_counter().fetch_add(1, std::memory_order_relaxed);
  if (legacy_hash != overlay_hash) {
    mismatch_counter().fetch_add(1, std::memory_order_relaxed);
  }

  InitOnce();

  if (!dump_enabled().load(std::memory_order_acquire)) {
    return;
  }
  if (dump_disabled_after_failure().load(std::memory_order_acquire)) {
    return;
  }

  const std::uint64_t seqno
      = seq_counter().fetch_add(1, std::memory_order_relaxed);

  // Capture thread id outside the lock to keep the critical section
  // short.
  std::ostringstream tid_os;
  tid_os << std::this_thread::get_id();
  const std::string tid_str = tid_os.str();

  std::lock_guard<std::mutex> g(mu());
  if (!dump_file().is_open()) {
    return;  // race: state may have been cleared
  }
  // One CSV row. No exceptions escape: ofstream defaults to
  // non-throwing, and we never call exceptions().
  dump_file() << ::getpid() << ',' << tid_str << ',' << seqno << ','
              << drc_box.ll.x << ',' << drc_box.ll.y << ','
              << drc_box.ur.x << ',' << drc_box.ur.y << ','
              << legacy_hash << ',' << overlay_hash << ','
              << (legacy_hash == overlay_hash ? 1 : 0) << ','
              << legacy_count << ',' << overlay_count << '\n';
  // Flush after each row: the dump is diagnostic, so we want data
  // on disk promptly so that a process crash does not hide the
  // trail. The cost (one fflush per shadow comparison) is bounded
  // by the number of FlexDRWorker initMarkers calls, not by marker
  // count, so it does not blow up under heavy routing.
  dump_file().flush();
  if (!dump_file().good()) {
    // Disable dumping for the rest of the process.
    WarnOnce("write failed", "subsequent rows suppressed");
    dump_disabled_after_failure().store(true,
                                        std::memory_order_release);
    dump_file().close();
  }
}

std::uint64_t ShadowDump::comparison_count() noexcept
{
  return comparison_counter().load(std::memory_order_relaxed);
}

std::uint64_t ShadowDump::mismatch_count() noexcept
{
  return mismatch_counter().load(std::memory_order_relaxed);
}

void ShadowDump::ResetForTest()
{
  std::lock_guard<std::mutex> g(mu());
  if (dump_file().is_open()) {
    dump_file().close();
  }
  dump_enabled().store(false, std::memory_order_release);
  dump_disabled_after_failure().store(false, std::memory_order_release);
  state_initialised().store(false, std::memory_order_release);
  summary_emitted().store(false, std::memory_order_release);
  comparison_counter().store(0);
  mismatch_counter().store(0);
  seq_counter().store(0);
}

}  // namespace drt::redesign::overlay
