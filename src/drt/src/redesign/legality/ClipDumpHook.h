// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.1.b/c — runtime sink for FlexGCWorker clip dumps.
//
// As of P2.2.e.1.c the dump hook performs **stratified reservoir
// sampling** rather than write-everything. Records are held in memory
// per (candidate_count_bin, context_count_bin, marker_present) bucket
// and serialized to disk at process exit (destructor flush). This:
//   * caps corpus size aggressively (yellow flag 1 from review),
//   * preserves difficult/rare cases (large clips, marker-present),
//   * stays observe-only (guardrail 1 unchanged),
//   * is deterministic under a fixed seed (env DRT_DUMP_SEED).
//
// Env vars:
//   DRT_DUMP_GC_CLIPS=<path>     activate dumping; output file BASE path.
//                                Actual path is <path>.<pid> so multi-
//                                process flows (ORFS spawns one openroad
//                                per stage) don't truncate each other.
//                                Glob with <path>.* to read all.
//   DRT_DUMP_DESIGN=<name>       optional, written to ClipMeta.design
//   DRT_DUMP_PDK=<name>          optional, written to ClipMeta.pdk
//   DRT_DUMP_RESERVOIR=<n>       per-bucket retention cap (default 64)
//   DRT_DUMP_SEED=<n>            RNG seed (default 42)
//
// **Guardrails honored:**
//   1. Observe-only.
//   5. Dump failure never changes routing: every public Dump() call is
//      wrapped in try/catch; destructor flush is wrapped too.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

#include "BucketedReservoir.h"
#include "ClipDump.h"

namespace drt {
class frTechObject;
}  // namespace drt

namespace drt::redesign::legality {

struct ClipBucketKey
{
  std::uint8_t cand_bin = 0;
  std::uint8_t ctx_bin = 0;
  std::uint8_t marker_present = 0;

  bool operator<(const ClipBucketKey& o) const noexcept
  {
    return std::tie(cand_bin, ctx_bin, marker_present)
           < std::tie(o.cand_bin, o.ctx_bin, o.marker_present);
  }
  bool operator==(const ClipBucketKey& o) const noexcept
  {
    return cand_bin == o.cand_bin && ctx_bin == o.ctx_bin
           && marker_present == o.marker_present;
  }
};

// Compute bin index 0..6 from a count (log-bucketed).
//   0      -> 0
//   1..4   -> 1
//   5..16  -> 2
//   17..64 -> 3
//   65..256 -> 4
//   257..1024 -> 5
//   1025+  -> 6
std::uint8_t LogBin(std::uint64_t n);

ClipBucketKey ComputeBucketKey(const ClipRecord& record);

// Resolve a base output path to a per-process file path by appending
// "."<pid>. Empty input returns empty (caller treats as "no dump").
// Pure function; used by both the live hook and unit tests.
std::string ResolveDumpPath(const std::string& base, int pid);

class ClipDumpHook
{
 public:
  static ClipDumpHook& Instance();

  bool IsActive() const { return active_; }
  const std::string& DesignHint() const { return design_hint_; }
  const std::string& PdkHint() const { return pdk_hint_; }
  std::uint64_t NextClipId() { return next_clip_id_.fetch_add(1u); }

  // Process-local session identifier (currently the PID). Stored in
  // both ClipMeta.session_id and RuleDeckProvenance.session_id so the
  // offline audit can join the two artifact families.
  std::uint64_t SessionId() const noexcept { return session_id_; }

  // Offer a record. Internally:
  //   1. Compute bucket key.
  //   2. Pass to BucketedReservoir::Offer (locked under mu_).
  // No file I/O until destructor flush. Exception-wrapped per
  // guardrail 5.
  void Dump(ClipRecord&& record);

  // Force the destructor flush early (for tests / explicit shutdown).
  // After Flush() the hook stops accepting new records. Idempotent.
  void Flush();

  // Capture-once-per-process: walk `tech` for constraints, translate
  // via FlexConstraintTranslator, serialize to <out_path>.ruledeck. The
  // first thread to call this races-and-wins via std::call_once;
  // subsequent calls (any thread, any FlexGCWorker) are no-ops. If the
  // hook is inactive or `tech` is null, no-op. Exception-isolated per
  // guardrail 5 — failures log once and never propagate.
  void EnsureRuleDeckDumped(const ::drt::frTechObject* tech);

 private:
  ClipDumpHook();
  ~ClipDumpHook();
  ClipDumpHook(const ClipDumpHook&) = delete;
  ClipDumpHook& operator=(const ClipDumpHook&) = delete;

  void DoFlushLocked();  // requires mu_ held

  bool active_ = false;
  bool flushed_ = false;
  std::string out_path_;
  std::string design_hint_;
  std::string pdk_hint_;
  std::size_t per_bucket_cap_ = 64;
  std::uint64_t seed_ = 42;
  std::uint64_t session_id_ = 0;
  std::atomic<std::uint64_t> next_clip_id_{0};
  std::mutex mu_;
  std::unique_ptr<BucketedReservoir<ClipBucketKey, ClipRecord>> reservoir_;
  std::atomic<bool> warned_once_{false};
  std::once_flag ruledeck_once_;
};

}  // namespace drt::redesign::legality
