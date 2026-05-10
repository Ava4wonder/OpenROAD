// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.4.e — cross-worker barrier primitive implementation.

#include "ProposalStaging.h"

#include <algorithm>
#include <mutex>

#include "ConflictPolicy.h"

namespace drt::redesign {

namespace {

class StagingState
{
 public:
  std::mutex mu;
  std::vector<StagedProposal> queue;
};

StagingState& State()
{
  static StagingState s;
  return s;
}

}  // namespace

ProposalStaging& ProposalStaging::Instance()
{
  static ProposalStaging instance;
  return instance;
}

void ProposalStaging::Stage(StagedProposal sp)
{
  auto& s = State();
  std::lock_guard<std::mutex> g(s.mu);
  s.queue.push_back(std::move(sp));
}

std::vector<StagedProposal> ProposalStaging::Drain()
{
  std::vector<StagedProposal> out;
  {
    auto& s = State();
    std::lock_guard<std::mutex> g(s.mu);
    out.swap(s.queue);
  }
  // Canonical (DeltaId-sorted) ordering — matches batch_eval's
  // contract so downstream consumers see consistent indexing.
  std::sort(out.begin(), out.end(),
            [](const StagedProposal& a, const StagedProposal& b) {
              return a.proposal.id < b.proposal.id;
            });
  return out;
}

std::size_t ProposalStaging::size() const noexcept
{
  auto& s = State();
  std::lock_guard<std::mutex> g(s.mu);
  return s.queue.size();
}

void ProposalStaging::ResetForTest()
{
  auto& s = State();
  std::lock_guard<std::mutex> g(s.mu);
  s.queue.clear();
}

CrossWorkerResolveResult ResolveStagedProposals(ConflictPolicy& policy)
{
  CrossWorkerResolveResult out;

  auto staged = ProposalStaging::Instance().Drain();
  const std::size_t n = staged.size();

  // Build the synthetic BatchEvalResult from the drained outcomes.
  // Since Drain() already DeltaId-sorted the staged units, the
  // BatchEvalResult ordering is canonical and matches the
  // ConflictGraph node indices below.
  out.batch.outcomes.reserve(n);
  out.batch.outcome_proposal_ids.reserve(n);
  out.batch.adapter_unsupported.reserve(n);
  out.batch.summary.batch_size = n;

  // Build a ProposalSet alongside, so SelectMis can consume it
  // directly — the cross-worker resolve is conceptually one big
  // batch from a single virtual worker.
  ProposalSet set;
  set.proposals.reserve(n);

  for (const auto& sp : staged) {
    out.batch.outcomes.push_back(sp.outcome);
    out.batch.outcome_proposal_ids.push_back(sp.proposal.id);
    out.batch.adapter_unsupported.push_back(sp.adapter_unsupported);

    if (sp.outcome.legality.legal) {
      out.batch.summary.legal_count += 1;
    }
    if (sp.outcome.legality.commit_eligible) {
      out.batch.summary.commit_eligible_count += 1;
    }
    if (sp.outcome.legality.source == LegalitySource::UnresolvedFootprint) {
      out.batch.summary.unresolved_count += 1;
    }
    if (sp.adapter_unsupported) {
      out.batch.summary.unsupported_count += 1;
    }

    set.proposals.push_back(sp.proposal);
  }

  // Cross-worker conflict graph + MIS selection. ConflictGraph
  // builder expects ProposalSet — we have one. SelectMis closes
  // the loop and tags eligible-but-MIS-loser proposals as
  // RejectionReason::Conflict (the V2.3.b reserved slot).
  out.graph = BuildConflictGraph(set);
  out.selection = SelectMis(out.batch, set, out.graph, policy);

  return out;
}

}  // namespace drt::redesign
