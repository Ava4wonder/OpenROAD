// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.2.c — AuditReport reducer + renderer.

#include "AuditReport.h"

#include <ostream>

namespace drt::redesign::legality {

namespace {

const char* FamilyName(RuleFamily f)
{
  switch (f) {
    case RuleFamily::MetalShort:
      return "MetalShort";
    case RuleFamily::PrlSpacing:
      return "PrlSpacing";
    case RuleFamily::EolSpacing:
      return "EolSpacing";
    case RuleFamily::CutSpacing:
      return "CutSpacing";
  }
  return "?";
}

SessionStats& EnsureSession(AuditReport* out, std::uint64_t session_id)
{
  auto& s = out->per_session[session_id];
  if (s.session_id == 0) {
    s.session_id = session_id;
  }
  return s;
}

const char* JoinStatusName(JoinStatus js)
{
  switch (js) {
    case JoinStatus::Joined:
      return "joined";
    case JoinStatus::RuleDeckOnly:
      return "ruledeck-only";
    case JoinStatus::ClipsOnly:
      return "clips-only";
  }
  return "?";
}

}  // namespace

void IngestRuleDeck(const RuleDeck& deck,
                    const RuleDeckProvenance& prov,
                    AuditReport* out)
{
  SessionStats& s = EnsureSession(out, prov.session_id);
  if (s.design.empty()) {
    s.design = prov.design;
  }
  if (s.pdk.empty()) {
    s.pdk = prov.pdk;
  }
  const auto cov = deck.GetCoverage();
  s.total_input += cov.total_input;
  s.supported += cov.supported;
  s.supported_explicit += cov.supported_explicit;
  s.supported_unknown += cov.supported_unknown;
  s.fallback += cov.fallback;
  s.unsupported += cov.unsupported;
  // Layer conflicts: provenance does not currently carry the counter;
  // when it does (small follow-up), accumulate here.

  for (std::size_t i = 0; i < deck.Size(); ++i) {
    const NormalizedRule& r = deck.At(i);
    FamilyCellStats& cell = s.by_family[r.family];
    FamilyCellStats& variant_cell = s.by_family_variant[r.family][r.tag];
    auto bump = [](FamilyCellStats& c, SupportTier cov, LayerKnownness lk) {
      switch (cov) {
        case SupportTier::Exact:
          ++c.supported;
          if (lk == LayerKnownness::Explicit) {
            ++c.supported_explicit;
          } else {
            ++c.supported_unknown;
          }
          break;
        case SupportTier::Fallback:
          ++c.fallback;
          break;
        case SupportTier::Unsupported:
          break;
      }
    };
    bump(cell, r.tier, r.layer_knownness);
    bump(variant_cell, r.tier, r.layer_knownness);
    if (r.layer_filter.has_value()) {
      ++s.rules_per_layer[r.layer_filter.value()];
    }
  }
}

void IngestClipRecords(const std::vector<ClipRecord>& clips,
                       AuditReport* out)
{
  for (const auto& c : clips) {
    SessionStats& s = EnsureSession(out, c.meta.session_id);
    if (s.design.empty()) {
      s.design = c.meta.design;
    }
    if (s.pdk.empty()) {
      s.pdk = c.meta.pdk;
    }
    ++s.clips_seen;
    bool any_marker = false;
    for (const auto& l : c.labels) {
      if (l.projected_marker_count != 0) {
        any_marker = true;
        break;
      }
    }
    if (any_marker) {
      ++s.clips_marker_present;
    }
  }
}

void FinalizeJoinStatus(AuditReport* out)
{
  for (auto& [sid, s] : out->per_session) {
    const bool has_ruledeck = (s.total_input != 0);
    const bool has_clips = (s.clips_seen != 0);
    if (has_ruledeck && has_clips) {
      s.join_status = JoinStatus::Joined;
    } else if (has_ruledeck) {
      s.join_status = JoinStatus::RuleDeckOnly;
    } else {
      s.join_status = JoinStatus::ClipsOnly;
    }
  }
}

void RenderReport(const AuditReport& report, std::ostream& os)
{
  os << "# Redesign E2 audit report\n";
  os << "# layer-attrib-policy=discovered_wins\n";
  os << "# sessions: " << report.sessions() << "\n";
  // Join-status summary up top.
  std::size_t joined = 0, ruledeck_only = 0, clips_only = 0;
  for (const auto& [sid, s] : report.per_session) {
    switch (s.join_status) {
      case JoinStatus::Joined:
        ++joined;
        break;
      case JoinStatus::RuleDeckOnly:
        ++ruledeck_only;
        break;
      case JoinStatus::ClipsOnly:
        ++clips_only;
        break;
    }
  }
  os << "# join_status: joined=" << joined
     << " ruledeck_only=" << ruledeck_only
     << " clips_only=" << clips_only << "\n\n";

  for (const auto& [sid, s] : report.per_session) {
    os << "## session " << sid << "  design=" << s.design
       << "  pdk=" << s.pdk << "  join_status=" << JoinStatusName(s.join_status)
       << "\n";
    const std::size_t supported_total
        = s.supported_exact + s.supported_conservative;
    os << "  rule-deck:\n";
    os << "    total_input              = " << s.total_input << "\n";
    os << "    supported_total          = " << supported_total << "\n";
    os << "      supported_exact        = " << s.supported_exact << "\n";
    os << "        explicit_layer       = " << s.supported_exact_explicit
       << "\n";
    os << "        unknown_layer        = " << s.supported_exact_unknown
       << "\n";
    os << "      supported_conservative = " << s.supported_conservative
       << "\n";
    os << "        explicit_layer       = "
       << s.supported_conservative_explicit << "\n";
    os << "        unknown_layer        = "
       << s.supported_conservative_unknown << "\n";
    os << "    fallback                 = " << s.fallback << "\n";
    os << "    unsupported              = " << s.unsupported << "\n";
    os << "    layer_conflicts          = " << s.layer_conflicts_seen
       << "\n";

    os << "  per-family:\n";
    for (const auto& [family, cell] : s.by_family) {
      const std::size_t cell_supported
          = cell.supported_exact + cell.supported_conservative;
      os << "    " << FamilyName(family)
         << ": supported_total=" << cell_supported
         << " (exact=" << cell.supported_exact
         << ", conservative=" << cell.supported_conservative
         << ") fallback=" << cell.fallback << "\n";
      const auto& variants = s.by_family_variant.at(family);
      for (const auto& [tag, vcell] : variants) {
        const std::size_t vsup
            = vcell.supported_exact + vcell.supported_conservative;
        os << "      " << tag << ": supported_total=" << vsup
           << " (exact=" << vcell.supported_exact
           << ", conservative=" << vcell.supported_conservative
           << ") fallback=" << vcell.fallback << "\n";
      }
    }

    os << "  per-layer (Explicit only):\n";
    if (s.rules_per_layer.empty()) {
      os << "    (none)\n";
    } else {
      for (const auto& [layer, count] : s.rules_per_layer) {
        os << "    layer " << layer << ": " << count << " rule(s)\n";
      }
    }

    os << "  clips:\n";
    os << "    clips_seen          = " << s.clips_seen << "\n";
    os << "    clips_marker_present = " << s.clips_marker_present << "\n";
    if (s.clips_seen > 0) {
      const double frac = 100.0 * static_cast<double>(s.clips_marker_present)
                          / static_cast<double>(s.clips_seen);
      os << "    marker_present_pct  = " << frac << "%\n";
    }
    os << "\n";
  }
}

}  // namespace drt::redesign::legality
