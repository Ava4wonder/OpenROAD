// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.3.b.dump — implementation. See header.

#include "BatchSummaryDump.h"

#include <array>
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

#include "Selection.h"  // V2.4.f — MisSelectionResult definition

namespace drt::redesign {

namespace {

constexpr std::size_t kReasonCount = 7;  // RejectionReason values.
static_assert(static_cast<std::size_t>(RejectionReason::Unknown) + 1
                  == kReasonCount,
              "kReasonCount must match RejectionReason enum size");

const char* ReasonName(RejectionReason r) noexcept
{
  switch (r) {
    case RejectionReason::None:
      return "none";
    case RejectionReason::Illegal:
      return "illegal";
    case RejectionReason::UnresolvedFootprint:
      return "unresolved";
    case RejectionReason::UnsupportedDelta:
      return "unsupported";
    case RejectionReason::LowerScore:
      return "lower_score";
    case RejectionReason::Conflict:
      return "conflict";
    case RejectionReason::Unknown:
      return "unknown";
  }
  return "?";
}

std::mutex& mu()
{
  static std::mutex m;
  return m;
}

std::atomic<std::uint64_t>& batch_counter()
{
  static std::atomic<std::uint64_t> c{0};
  return c;
}
std::atomic<std::uint64_t>& batches_with_winner_counter()
{
  static std::atomic<std::uint64_t> c{0};
  return c;
}
std::atomic<std::uint64_t>& total_proposals_counter()
{
  static std::atomic<std::uint64_t> c{0};
  return c;
}
std::array<std::atomic<std::uint64_t>, kReasonCount>& reason_counters()
{
  static std::array<std::atomic<std::uint64_t>, kReasonCount> a{};
  return a;
}

std::atomic<std::uint64_t>& seq_counter()
{
  static std::atomic<std::uint64_t> c{0};
  return c;
}

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

void WarnOnce(const char* what, const std::string& detail)
{
  static std::atomic<bool> warned{false};
  bool expected = false;
  if (warned.compare_exchange_strong(expected, true)) {
    std::cerr << "[drt-redesign-batch] dump-disabled: " << what << ": "
              << detail << "\n";
  }
}

void TryMkdirParent(const std::string& path)
{
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return;
  }
  const std::string parent = path.substr(0, slash);
  ::mkdir(parent.c_str(), 0755);
}

void EmitSummary()
{
  if (summary_emitted().exchange(true)) {
    return;
  }
  const std::uint64_t total = batch_counter().load();
  const std::uint64_t with_winner = batches_with_winner_counter().load();
  const std::uint64_t total_props = total_proposals_counter().load();
  std::ostringstream os;
  os << "[drt-redesign-batch] V2.3.b summary: batches=" << total
     << " with_winner=" << with_winner
     << " total_proposals=" << total_props;
  // Per-reason breakdown (skip None/Conflict if zero — None is the
  // sentinel and Conflict is V2.4-reserved).
  for (std::size_t i = 0; i < kReasonCount; ++i) {
    const auto r = static_cast<RejectionReason>(i);
    const std::uint64_t c = reason_counters()[i].load();
    if (c == 0
        && (r == RejectionReason::None || r == RejectionReason::Conflict)) {
      continue;
    }
    os << " " << ReasonName(r) << "=" << c;
  }
  os << "\n";
  std::cerr << os.str();
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

  const char* enable_env = std::getenv("OPENROAD_REDESIGN_BATCH_DUMP");
  const bool want_dump = (enable_env != nullptr && enable_env[0] != '\0'
                          && enable_env[0] != '0');

  if (want_dump) {
    const char* path_env
        = std::getenv("OPENROAD_REDESIGN_BATCH_DUMP_PATH");
    std::string path;
    if (path_env != nullptr && path_env[0] != '\0') {
      path = path_env;
    } else {
      std::ostringstream os;
      os << "/tmp/openroad-redesign-batch/" << ::getpid() << ".csv";
      path = os.str();
    }
    TryMkdirParent(path);
    dump_file().open(path, std::ios::out | std::ios::trunc);
    if (dump_file().is_open()) {
      dump_file()
          << "pid,tid,seqno,net_id,batch_size,winner_delta_id,"
             "has_winner,winner_score,num_commit_eligible,num_illegal,"
             "num_unresolved,num_unsupported,num_lower_score,"
             "num_conflict\n";
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

std::string BatchSummaryRow::EncodeWinnerDeltaId() const
{
  if (!has_winner) {
    return std::string();
  }
  std::ostringstream os;
  os << winner_delta_id.region_id << ':' << winner_delta_id.proposer_id
     << ':' << winner_delta_id.attempt_index;
  return os.str();
}

BatchSummaryRow MakeBatchSummaryRow(const BatchEvalResult& batch,
                                    const SelectionResult& sel,
                                    std::uint64_t net_id,
                                    std::uint64_t seqno)
{
  BatchSummaryRow row;
  row.seqno = seqno;
  row.net_id = net_id;
  row.batch_size = batch.summary.batch_size;
  row.has_winner = sel.has_winner;
  if (sel.has_winner) {
    row.winner_delta_id = sel.winner_id;
    if (sel.winner_score.has_value()) {
      row.winner_score = sel.winner_score->aggregate;
    }
  }
  row.num_commit_eligible = batch.summary.commit_eligible_count;

  for (const auto& rj : sel.rejected) {
    switch (rj.reason) {
      case RejectionReason::Illegal:
        row.num_illegal += 1;
        break;
      case RejectionReason::UnresolvedFootprint:
        row.num_unresolved += 1;
        break;
      case RejectionReason::UnsupportedDelta:
        row.num_unsupported += 1;
        break;
      case RejectionReason::LowerScore:
        row.num_lower_score += 1;
        break;
      case RejectionReason::Conflict:
        row.num_conflict += 1;
        break;
      case RejectionReason::Unknown:
      case RejectionReason::None:
        // Unknown is the catch-all for non-committable stub-source
        // verdicts. None must not appear in `rejected`. Neither has
        // a column today; intentionally not surfaced — the
        // batch_size minus the named buckets minus winner gives the
        // residue.
        break;
    }
  }
  return row;
}

BatchSummaryRow MakeCrossWorkerSummaryRow(const BatchEvalResult& batch,
                                          const MisSelectionResult& sel,
                                          std::uint64_t seqno)
{
  BatchSummaryRow row;
  row.seqno = seqno;
  row.net_id = 0;  // sentinel for cross-worker rows (see header).
  row.batch_size = batch.summary.batch_size;
  // has_winner stays false — MIS yields multiple winners and the
  // schema does not encode that. Consumers compute the committed
  // count as batch_size - num_conflict.
  row.has_winner = false;
  row.num_commit_eligible = batch.summary.commit_eligible_count;

  for (const auto& rj : sel.rejected) {
    switch (rj.reason) {
      case RejectionReason::Illegal:
        row.num_illegal += 1;
        break;
      case RejectionReason::UnresolvedFootprint:
        row.num_unresolved += 1;
        break;
      case RejectionReason::UnsupportedDelta:
        row.num_unsupported += 1;
        break;
      case RejectionReason::LowerScore:
        // V2.4.f cross-worker resolve does not produce LowerScore
        // (per-worker SelectBest already filtered). If it ever
        // does, count it.
        row.num_lower_score += 1;
        break;
      case RejectionReason::Conflict:
        row.num_conflict += 1;
        break;
      case RejectionReason::Unknown:
      case RejectionReason::None:
        break;
    }
  }
  return row;
}

void BatchSummaryDump::Record(const BatchSummaryRow& row)
{
  // Counters update regardless of dump-enable status — the atexit
  // summary is a separate signal from the CSV dump.
  batch_counter().fetch_add(1, std::memory_order_relaxed);
  if (row.has_winner) {
    batches_with_winner_counter().fetch_add(1, std::memory_order_relaxed);
  }
  total_proposals_counter().fetch_add(row.batch_size,
                                      std::memory_order_relaxed);
  // Per-reason aggregate. Driven by the row's named columns so the
  // counter row aligns with the CSV row and the same source of truth
  // (the SelectionResult walk) feeds both.
  reason_counters()[static_cast<std::size_t>(RejectionReason::Illegal)]
      .fetch_add(row.num_illegal, std::memory_order_relaxed);
  reason_counters()[static_cast<std::size_t>(
                        RejectionReason::UnresolvedFootprint)]
      .fetch_add(row.num_unresolved, std::memory_order_relaxed);
  reason_counters()[static_cast<std::size_t>(
                        RejectionReason::UnsupportedDelta)]
      .fetch_add(row.num_unsupported, std::memory_order_relaxed);
  reason_counters()[static_cast<std::size_t>(RejectionReason::LowerScore)]
      .fetch_add(row.num_lower_score, std::memory_order_relaxed);
  reason_counters()[static_cast<std::size_t>(RejectionReason::Conflict)]
      .fetch_add(row.num_conflict, std::memory_order_relaxed);

  InitOnce();

  if (!dump_enabled().load(std::memory_order_acquire)) {
    return;
  }
  if (dump_disabled_after_failure().load(std::memory_order_acquire)) {
    return;
  }

  const std::uint64_t seq
      = (row.seqno != 0)
            ? row.seqno
            : seq_counter().fetch_add(1, std::memory_order_relaxed) + 1;

  std::ostringstream tid_os;
  tid_os << std::this_thread::get_id();
  const std::string tid_str = tid_os.str();

  std::lock_guard<std::mutex> g(mu());
  if (!dump_file().is_open()) {
    return;
  }
  dump_file() << ::getpid() << ',' << tid_str << ',' << seq << ','
              << row.net_id << ',' << row.batch_size << ','
              << row.EncodeWinnerDeltaId() << ','
              << (row.has_winner ? 1 : 0) << ',';
  if (row.has_winner) {
    dump_file() << row.winner_score;
  }
  // Empty cell when no winner.
  dump_file() << ',' << row.num_commit_eligible << ','
              << row.num_illegal << ',' << row.num_unresolved << ','
              << row.num_unsupported << ',' << row.num_lower_score << ','
              << row.num_conflict << '\n';
  dump_file().flush();
  if (!dump_file().good()) {
    WarnOnce("write failed", "subsequent rows suppressed");
    dump_disabled_after_failure().store(true, std::memory_order_release);
    dump_file().close();
  }
}

std::uint64_t BatchSummaryDump::batch_count() noexcept
{
  return batch_counter().load(std::memory_order_relaxed);
}
std::uint64_t BatchSummaryDump::batches_with_winner() noexcept
{
  return batches_with_winner_counter().load(std::memory_order_relaxed);
}
std::uint64_t BatchSummaryDump::total_proposals() noexcept
{
  return total_proposals_counter().load(std::memory_order_relaxed);
}
std::uint64_t BatchSummaryDump::total_rejection_count_for(
    RejectionReason reason) noexcept
{
  return reason_counters()[static_cast<std::size_t>(reason)].load(
      std::memory_order_relaxed);
}

void BatchSummaryDump::ResetForTest()
{
  std::lock_guard<std::mutex> g(mu());
  if (dump_file().is_open()) {
    dump_file().close();
  }
  dump_enabled().store(false, std::memory_order_release);
  dump_disabled_after_failure().store(false, std::memory_order_release);
  state_initialised().store(false, std::memory_order_release);
  summary_emitted().store(false, std::memory_order_release);
  batch_counter().store(0);
  batches_with_winner_counter().store(0);
  total_proposals_counter().store(0);
  seq_counter().store(0);
  for (auto& a : reason_counters()) {
    a.store(0);
  }
}

}  // namespace drt::redesign
