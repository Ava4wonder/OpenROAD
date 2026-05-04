// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.2.a — RuleDeck serialization.

#include "RuleDeckDump.h"

#include <cstring>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace drt::redesign::legality {

namespace {

template <typename T>
void WriteLE(std::ostream& os, T v)
{
  static_assert(std::is_trivially_copyable_v<T>);
  char buf[sizeof(T)];
  std::memcpy(buf, &v, sizeof(T));
  os.write(buf, sizeof(T));
}

template <typename T>
bool ReadLE(std::istream& is, T* v)
{
  static_assert(std::is_trivially_copyable_v<T>);
  char buf[sizeof(T)];
  is.read(buf, sizeof(T));
  if (is.gcount() != static_cast<std::streamsize>(sizeof(T))) {
    return false;
  }
  std::memcpy(v, buf, sizeof(T));
  return true;
}

void WriteString(std::ostream& os, const std::string& s)
{
  if (s.size() > (1u << 20)) {
    throw std::runtime_error("RuleDeckDump: tag exceeds 1 MiB");
  }
  WriteLE<std::uint32_t>(os, static_cast<std::uint32_t>(s.size()));
  if (!s.empty()) {
    os.write(s.data(), static_cast<std::streamsize>(s.size()));
  }
}

bool ReadString(std::istream& is, std::string* s)
{
  std::uint32_t n = 0;
  if (!ReadLE(is, &n)) {
    return false;
  }
  if (n > (1u << 20)) {
    return false;
  }
  s->resize(n);
  if (n > 0) {
    is.read(s->data(), static_cast<std::streamsize>(n));
    if (is.gcount() != static_cast<std::streamsize>(n)) {
      return false;
    }
  }
  return true;
}

void WriteParams(std::ostream& os, const NormalizedRule& r)
{
  if (r.coverage != RuleCoverage::Supported) {
    return;
  }
  std::visit(
      [&](const auto& cfg) {
        using T = std::decay_t<decltype(cfg)>;
        if constexpr (std::is_same_v<T, MetalShortConfig>) {
          // no fields
        } else if constexpr (std::is_same_v<T, PrlSpacingConfig>) {
          WriteLE<std::int32_t>(os, cfg.min_spacing);
          WriteLE<std::int32_t>(os, cfg.prl_threshold);
        } else if constexpr (std::is_same_v<T, EolSpacingConfig>) {
          WriteLE<std::int32_t>(os, cfg.eol_width_threshold);
          WriteLE<std::int32_t>(os, cfg.eol_spacing);
          WriteLE<std::int32_t>(os, cfg.eol_within);
        } else if constexpr (std::is_same_v<T, CutSpacingConfig>) {
          WriteLE<std::int32_t>(os, cfg.min_spacing);
        }
      },
      r.params);
}

bool ReadParams(std::istream& is, NormalizedRule* r)
{
  if (r->coverage != RuleCoverage::Supported) {
    return true;  // nothing to read
  }
  switch (r->family) {
    case RuleFamily::MetalShort:
      r->params = MetalShortConfig{};
      return true;
    case RuleFamily::PrlSpacing: {
      PrlSpacingConfig cfg;
      if (!ReadLE(is, &cfg.min_spacing)
          || !ReadLE(is, &cfg.prl_threshold)) {
        return false;
      }
      r->params = cfg;
      return true;
    }
    case RuleFamily::EolSpacing: {
      EolSpacingConfig cfg;
      if (!ReadLE(is, &cfg.eol_width_threshold)
          || !ReadLE(is, &cfg.eol_spacing)
          || !ReadLE(is, &cfg.eol_within)) {
        return false;
      }
      r->params = cfg;
      return true;
    }
    case RuleFamily::CutSpacing: {
      CutSpacingConfig cfg;
      if (!ReadLE(is, &cfg.min_spacing)) {
        return false;
      }
      r->params = cfg;
      return true;
    }
  }
  return false;
}

}  // namespace

void WriteRuleDeck(std::ostream& os,
                   const RuleDeck& deck,
                   const RuleDeckProvenance& prov)
{
  WriteLE<std::uint32_t>(os, kRuleDeckDumpMagic);
  WriteLE<std::uint32_t>(os, kRuleDeckDumpVersion);
  // Provenance block.
  WriteLE<std::uint32_t>(os, prov.translator_version);
  WriteLE<std::uint32_t>(os, prov.pid);
  WriteLE<std::int64_t>(os, prov.capture_timestamp);
  WriteString(os, prov.openroad_git_sha);
  WriteString(os, prov.redesign_git_sha);
  WriteLE<std::uint32_t>(os, prov.layers_walked);
  WriteLE<std::uint32_t>(os, prov.constraints_seen);
  WriteLE<std::uint32_t>(os,
                         static_cast<std::uint32_t>(prov.per_type_counts.size()));
  for (const auto& [type_id, count] : prov.per_type_counts) {
    WriteLE<std::uint32_t>(os, type_id);
    WriteLE<std::uint32_t>(os, count);
  }

  const auto cov = deck.GetCoverage();
  WriteLE<std::uint64_t>(os, cov.total_input);
  WriteLE<std::uint64_t>(os, cov.supported);
  WriteLE<std::uint64_t>(os, cov.supported_explicit);
  WriteLE<std::uint64_t>(os, cov.supported_unknown);
  WriteLE<std::uint64_t>(os, cov.fallback);
  WriteLE<std::uint64_t>(os, cov.unsupported);
  WriteLE<std::uint32_t>(os, static_cast<std::uint32_t>(deck.Size()));
  for (std::size_t i = 0; i < deck.Size(); ++i) {
    const auto& r = deck.At(i);
    WriteLE<std::uint8_t>(os, static_cast<std::uint8_t>(r.family));
    WriteLE<std::uint8_t>(os, static_cast<std::uint8_t>(r.coverage));
    WriteLE<std::uint8_t>(os,
                          static_cast<std::uint8_t>(r.layer_knownness));
    const std::uint8_t has = r.layer_filter.has_value() ? 1u : 0u;
    WriteLE<std::uint8_t>(os, has);
    if (has != 0u) {
      WriteLE<std::int16_t>(os, r.layer_filter.value());
    }
    WriteLE<std::int32_t>(os, r.halo);
    WriteString(os, r.tag);
    WriteParams(os, r);
  }
}

bool ReadRuleDeck(std::istream& is,
                  RuleDeck* out,
                  RuleDeckProvenance* prov_out)
{
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  if (!ReadLE(is, &magic) || magic != kRuleDeckDumpMagic) {
    return false;
  }
  if (!ReadLE(is, &version) || version != kRuleDeckDumpVersion) {
    return false;
  }
  // Provenance block.
  RuleDeckProvenance prov;
  if (!ReadLE(is, &prov.translator_version) || !ReadLE(is, &prov.pid)
      || !ReadLE(is, &prov.capture_timestamp)
      || !ReadString(is, &prov.openroad_git_sha)
      || !ReadString(is, &prov.redesign_git_sha)
      || !ReadLE(is, &prov.layers_walked)
      || !ReadLE(is, &prov.constraints_seen)) {
    return false;
  }
  std::uint32_t per_type_n = 0;
  if (!ReadLE(is, &per_type_n)) {
    return false;
  }
  for (std::uint32_t i = 0; i < per_type_n; ++i) {
    std::uint32_t type_id = 0;
    std::uint32_t count = 0;
    if (!ReadLE(is, &type_id) || !ReadLE(is, &count)) {
      return false;
    }
    prov.per_type_counts[type_id] = count;
  }
  if (prov_out != nullptr) {
    *prov_out = std::move(prov);
  }

  RuleDeck::Coverage cov;
  if (!ReadLE(is, &cov.total_input) || !ReadLE(is, &cov.supported)
      || !ReadLE(is, &cov.supported_explicit)
      || !ReadLE(is, &cov.supported_unknown) || !ReadLE(is, &cov.fallback)
      || !ReadLE(is, &cov.unsupported)) {
    return false;
  }
  std::uint32_t n = 0;
  if (!ReadLE(is, &n)) {
    return false;
  }

  // Reconstruct rules via Add() to keep the deck's accounting
  // consistent. After we finish, sanity-check the recomputed Coverage
  // matches what we read from the header.
  *out = RuleDeck{};
  for (std::uint32_t i = 0; i < n; ++i) {
    NormalizedRule r;
    std::uint8_t fam = 0, cvg = 0, lk = 0, has = 0;
    if (!ReadLE(is, &fam) || !ReadLE(is, &cvg) || !ReadLE(is, &lk)
        || !ReadLE(is, &has)) {
      return false;
    }
    if (fam > static_cast<std::uint8_t>(RuleFamily::CutSpacing)) {
      return false;
    }
    if (cvg > static_cast<std::uint8_t>(RuleCoverage::Unsupported)) {
      return false;
    }
    if (lk > static_cast<std::uint8_t>(LayerKnownness::Unknown)) {
      return false;
    }
    r.family = static_cast<RuleFamily>(fam);
    r.coverage = static_cast<RuleCoverage>(cvg);
    r.layer_knownness = static_cast<LayerKnownness>(lk);
    if (has != 0u) {
      std::int16_t lyr = 0;
      if (!ReadLE(is, &lyr)) {
        return false;
      }
      r.layer_filter = lyr;
    }
    if (!ReadLE(is, &r.halo)) {
      return false;
    }
    if (!ReadString(is, &r.tag)) {
      return false;
    }
    if (!ReadParams(is, &r)) {
      return false;
    }
    out->Add(r);
  }

  if (out->GetCoverage().total_input != cov.total_input
      || out->GetCoverage().supported != cov.supported
      || out->GetCoverage().fallback != cov.fallback
      || out->GetCoverage().unsupported != cov.unsupported
      || out->GetCoverage().supported_explicit != cov.supported_explicit
      || out->GetCoverage().supported_unknown != cov.supported_unknown) {
    // The header-recorded coverage was likely written before
    // AddUnsupported() calls (which don't push a NormalizedRule). Add
    // the missing accounting to reconcile. This is honest: the file's
    // header is authoritative for the original total.
    const auto cur = out->GetCoverage();
    const std::size_t missing_unsupported
        = cov.unsupported - cur.unsupported;
    for (std::size_t i = 0; i < missing_unsupported; ++i) {
      out->AddUnsupported();
    }
  }
  return true;
}

}  // namespace drt::redesign::legality
