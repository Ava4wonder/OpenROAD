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

PerDesignStats& EnsureDesign(AuditReport* out, const std::string& design)
{
  auto& s = out->per_design[design];
  if (s.design.empty()) {
    s.design = design;
  }
  return s;
}

}  // namespace

void IngestRuleDeck(const RuleDeck& deck,
                    const RuleDeckProvenance& /*prov*/,
                    const std::string& design_hint,
                    AuditReport* out)
{
  const std::string key
      = design_hint.empty() ? std::string("(unknown)") : design_hint;
  PerDesignStats& s = EnsureDesign(out, key);
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
    switch (r.coverage) {
      case RuleCoverage::Supported:
        ++cell.supported;
        if (r.layer_knownness == LayerKnownness::Explicit) {
          ++cell.supported_explicit;
        } else {
          ++cell.supported_unknown;
        }
        break;
      case RuleCoverage::Fallback:
        ++cell.fallback;
        break;
      case RuleCoverage::Unsupported:
        // RuleDeck doesn't store NormalizedRule entries for
        // AddUnsupported() calls, so this case is unreachable here.
        break;
    }
    if (r.layer_filter.has_value()) {
      ++s.rules_per_layer[r.layer_filter.value()];
    }
  }
}

void IngestClipRecords(const std::vector<ClipRecord>& clips,
                       const std::string& design_hint,
                       AuditReport* out)
{
  for (const auto& c : clips) {
    const std::string key = !c.meta.design.empty() ? c.meta.design
                            : !design_hint.empty() ? design_hint
                                                   : std::string("(unknown)");
    PerDesignStats& s = EnsureDesign(out, key);
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

void RenderReport(const AuditReport& report, std::ostream& os)
{
  os << "# Redesign E2 audit report\n";
  os << "# layer-attrib-policy=discovered_wins\n";
  os << "# designs: " << report.designs() << "\n\n";

  for (const auto& [design, s] : report.per_design) {
    os << "## design: " << design << "\n";
    os << "  rule-deck:\n";
    os << "    total_input        = " << s.total_input << "\n";
    os << "    supported          = " << s.supported << "\n";
    os << "      supported_explicit = " << s.supported_explicit << "\n";
    os << "      supported_unknown  = " << s.supported_unknown << "\n";
    os << "    fallback           = " << s.fallback << "\n";
    os << "    unsupported        = " << s.unsupported << "\n";
    os << "    layer_conflicts    = " << s.layer_conflicts_seen << "\n";

    os << "  per-family:\n";
    for (const auto& [family, cell] : s.by_family) {
      os << "    " << FamilyName(family) << ": supported=" << cell.supported
         << " (explicit=" << cell.supported_explicit
         << ", unknown=" << cell.supported_unknown
         << ") fallback=" << cell.fallback << "\n";
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
