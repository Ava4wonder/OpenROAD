// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.3.b.dump — best-effort CSV dump of one row per (worker, batch).
//
// Purpose: enough observability to answer
//   * Did we actually generate multiple candidates?
//   * Why did each candidate lose?
//   * How often did the batch yield a commit-eligible winner?
//   * What is the rejection-reason distribution?
//
// Sibling of overlay/ShadowDump but with its own schema and its own
// env-var pair (kept distinct so a dual-channel A/B run can produce
// per-entity hash dumps and per-batch summaries side-by-side).
//
// Activation:
//   OPENROAD_REDESIGN_BATCH_DUMP=1     enable
//   OPENROAD_REDESIGN_BATCH_DUMP_PATH  override path
//   default path: /tmp/openroad-redesign-batch/<pid>.csv
//
// Discipline (mirrored from ShadowDump):
//   - Diagnostic-only. Never throws, never blocks routing, never
//     alters the legacy authoritative path. File I/O failures are
//     logged ONCE to stderr and dumping is then silently disabled
//     for the rest of the process.
//   - Multi-threaded callers serialise via internal mutex.
//   - On process exit (atexit), a one-line summary lists batch
//     totals and rejection-reason breakdown.
//
// CSV columns (one row per batch):
//   pid,tid,seqno,net_id,batch_size,winner_delta_id,has_winner,
//   winner_score,num_commit_eligible,num_illegal,num_unresolved,
//   num_unsupported,num_lower_score,num_conflict
//
// `winner_delta_id` is encoded as "region:proposer:attempt" when
// has_winner=1, empty when has_winner=0. `winner_score` is the
// aggregate (Score::aggregate); empty when has_winner=0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "BatchEval.h"
#include "Footprint.h"
#include "Selection.h"

namespace drt::redesign {

struct BatchSummaryRow
{
  std::uint64_t seqno = 0;
  std::uint64_t net_id = 0;
  std::size_t batch_size = 0;
  bool has_winner = false;
  DeltaId winner_delta_id{};
  double winner_score = 0.0;  // Score::aggregate; meaningful iff has_winner.
  std::size_t num_commit_eligible = 0;
  std::size_t num_illegal = 0;
  std::size_t num_unresolved = 0;
  std::size_t num_unsupported = 0;
  std::size_t num_lower_score = 0;
  std::size_t num_conflict = 0;

  // Encode winner_delta_id as "region:proposer:attempt", or "" when
  // has_winner is false. Plain function, not a member, to keep the
  // POD nature of the row.
  std::string EncodeWinnerDeltaId() const;
};

// Build a BatchSummaryRow from a (BatchEvalResult, SelectionResult)
// pair. The seqno/net_id come from the caller (the V2.3.c shadow
// site knows them).
//
// Counts num_commit_eligible from BatchEvalResult::summary
// (already tallied during batch_eval) and per-reason counts from
// SelectionResult::rejected. Conflict is reserved for V2.4 — the
// counter is wired now so the schema is stable.
BatchSummaryRow MakeBatchSummaryRow(const BatchEvalResult& batch,
                                    const SelectionResult& sel,
                                    std::uint64_t net_id,
                                    std::uint64_t seqno);

class BatchSummaryDump
{
 public:
  // Append one row to the CSV file (no-op when the dump is disabled
  // by env var or has been disabled after a write failure). Always
  // updates process-wide counters.
  static void Record(const BatchSummaryRow& row);

  // Process-wide aggregates (still updated when CSV dump is off).
  static std::uint64_t batch_count() noexcept;
  static std::uint64_t batches_with_winner() noexcept;
  static std::uint64_t total_proposals() noexcept;
  static std::uint64_t total_rejection_count_for(
      RejectionReason reason) noexcept;

  // Test-only — clears file state and counters.
  static void ResetForTest();
};

}  // namespace drt::redesign
