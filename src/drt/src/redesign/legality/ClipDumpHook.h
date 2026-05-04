// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.1.b — runtime sink for FlexGCWorker clip dumps.
//
// Singleton because FlexGCWorker is invoked from many threads under OMP
// parallel-for; one shared file handle + mutex is simpler than per-
// worker files (which would need a separate join step) and acceptable
// throughput-wise for a debug instrument.
//
// **Guardrails honored here:**
//   1. Observe-only: this class never modifies routing state.
//   5. Dump failure never changes routing: every public Dump() call is
//      wrapped in try/catch so a disk-full / permission / serialization
//      error surfaces as a logged warning, not a propagated exception.
//
// Activated by setting env var DRT_DUMP_GC_CLIPS=<path>. Unset → no-op.
// Optional env vars DRT_DUMP_DESIGN and DRT_DUMP_PDK pre-seed the
// design and pdk fields of ClipMeta when the caller doesn't supply them.

#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include "ClipDump.h"

namespace drt::redesign::legality {

class ClipDumpHook
{
 public:
  // Process-wide singleton. Lazy-initialized on first call. Thread-safe.
  static ClipDumpHook& Instance();

  // True iff DRT_DUMP_GC_CLIPS is set and the file opened successfully.
  // Callers can short-circuit clip-record construction when this is
  // false to avoid extraction overhead on default runs.
  bool IsActive() const { return active_; }

  // Returns the env-var-supplied design hint (or empty string).
  const std::string& DesignHint() const { return design_hint_; }
  // Returns the env-var-supplied pdk hint (or empty string).
  const std::string& PdkHint() const { return pdk_hint_; }

  // Atomically allocates a unique clip_id for this dump session.
  std::uint64_t NextClipId() { return next_clip_id_.fetch_add(1u); }

  // Append one record. No-op if !IsActive(). Any exception inside the
  // serialization or stream write is caught here and turned into a
  // logged warning on first failure (subsequent failures are silenced
  // to avoid log spam). Routing is never affected.
  void Dump(const ClipRecord& record);

 private:
  ClipDumpHook();
  ~ClipDumpHook();
  ClipDumpHook(const ClipDumpHook&) = delete;
  ClipDumpHook& operator=(const ClipDumpHook&) = delete;

  bool active_ = false;
  std::string design_hint_;
  std::string pdk_hint_;
  std::atomic<std::uint64_t> next_clip_id_{0};
  std::mutex mu_;
  std::ofstream file_;
  std::atomic<bool> warned_once_{false};
};

}  // namespace drt::redesign::legality
