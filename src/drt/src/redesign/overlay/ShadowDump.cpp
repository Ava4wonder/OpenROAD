// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// V2.1.e.5 + V2.2.a.0 — ShadowDump implementation. See header for
// activation, discipline, schema.

#include "ShadowDump.h"

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

namespace drt::redesign::overlay {

namespace {

constexpr std::size_t kEntityCount
    = static_cast<std::size_t>(ShadowDump::Entity::kCount);

// String name per entity. Index by static_cast<size_t>(Entity).
const char* EntityName(ShadowDump::Entity e) noexcept
{
  switch (e) {
    case ShadowDump::Entity::Marker:
      return "marker";
    case ShadowDump::Entity::RouteShape:
      return "route_shape";
    case ShadowDump::Entity::Guide:
      return "guide";
    case ShadowDump::Entity::Blockage:
      return "blockage";
    case ShadowDump::Entity::PinAccess:
      return "pin_access";
    case ShadowDump::Entity::Cost:
      return "cost";
    case ShadowDump::Entity::kCount:
      break;
  }
  return "?";
}

// Process-wide singletons.
std::mutex& mu()
{
  static std::mutex m;
  return m;
}

// Comparison counters (process-wide aggregates and per-entity).
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

std::array<std::atomic<std::uint64_t>, kEntityCount>& per_entity_calls()
{
  static std::array<std::atomic<std::uint64_t>, kEntityCount> a{};
  return a;
}

std::array<std::atomic<std::uint64_t>, kEntityCount>&
per_entity_mismatches()
{
  static std::array<std::atomic<std::uint64_t>, kEntityCount> a{};
  return a;
}

std::atomic<std::uint64_t>& seq_counter()
{
  static std::atomic<std::uint64_t> c{0};
  return c;
}

// Dump pipeline state.
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
    std::cerr << "[drt-redesign-overlay] V2.1.e.5 dump-disabled: "
              << what << ": " << detail << "\n";
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
  const std::uint64_t total_calls = comparison_counter().load();
  const std::uint64_t total_mm = mismatch_counter().load();
  std::ostringstream os;
  os << "[drt-redesign-overlay] V2.1.e shadow summary: total_calls="
     << total_calls << " total_mismatches=" << total_mm;
  // Per-entity breakdown only if non-zero (keeps line readable on
  // marker-only runs).
  for (std::size_t i = 0; i < kEntityCount; ++i) {
    const std::uint64_t c = per_entity_calls()[i].load();
    const std::uint64_t m = per_entity_mismatches()[i].load();
    if (c > 0 || m > 0) {
      os << " " << EntityName(static_cast<ShadowDump::Entity>(i))
         << "=" << c << "/" << m;
    }
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
          << "pid,tid,seqno,entity,query_kind,xmin,ymin,xmax,ymax,"
             "layer,legacy_hash,overlay_hash,match,legacy_count,"
             "overlay_count\n";
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

void ShadowDump::Record(const ComparisonRecord& rec)
{
  const std::size_t entity_idx = static_cast<std::size_t>(rec.entity);

  // Counters update regardless of dump-enable status — the atexit
  // summary is a separate signal from the CSV dump.
  comparison_counter().fetch_add(1, std::memory_order_relaxed);
  per_entity_calls()[entity_idx].fetch_add(1, std::memory_order_relaxed);
  if (rec.legacy_hash != rec.overlay_hash) {
    mismatch_counter().fetch_add(1, std::memory_order_relaxed);
    per_entity_mismatches()[entity_idx].fetch_add(
        1, std::memory_order_relaxed);
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

  std::ostringstream tid_os;
  tid_os << std::this_thread::get_id();
  const std::string tid_str = tid_os.str();

  std::lock_guard<std::mutex> g(mu());
  if (!dump_file().is_open()) {
    return;
  }
  dump_file() << ::getpid() << ',' << tid_str << ',' << seqno << ','
              << EntityName(rec.entity) << ',' << rec.query_kind << ','
              << rec.box.ll.x << ',' << rec.box.ll.y << ','
              << rec.box.ur.x << ',' << rec.box.ur.y << ',';
  if (rec.layer.has_value()) {
    dump_file() << rec.layer.value();
  }
  // Empty cell when no layer.
  dump_file() << ',' << rec.legacy_hash << ',' << rec.overlay_hash
              << ',' << (rec.legacy_hash == rec.overlay_hash ? 1 : 0)
              << ',' << rec.legacy_count << ',' << rec.overlay_count
              << '\n';
  dump_file().flush();
  if (!dump_file().good()) {
    WarnOnce("write failed", "subsequent rows suppressed");
    dump_disabled_after_failure().store(true,
                                        std::memory_order_release);
    dump_file().close();
  }
}

void ShadowDump::RecordMarkerComparison(std::uint64_t legacy_hash,
                                        std::uint64_t overlay_hash,
                                        std::size_t legacy_count,
                                        std::size_t overlay_count,
                                        const Rect& drc_box)
{
  ComparisonRecord rec;
  rec.entity = Entity::Marker;
  rec.query_kind = "queryMarker";
  rec.box = drc_box;
  // Marker queries are layer-less.
  rec.legacy_hash = legacy_hash;
  rec.overlay_hash = overlay_hash;
  rec.legacy_count = legacy_count;
  rec.overlay_count = overlay_count;
  Record(rec);
}

void ShadowDump::RecordRouteShapeComparison(std::uint64_t legacy_hash,
                                            std::uint64_t overlay_hash,
                                            std::size_t legacy_count,
                                            std::size_t overlay_count,
                                            const Rect& box,
                                            std::int32_t layer)
{
  ComparisonRecord rec;
  rec.entity = Entity::RouteShape;
  rec.query_kind = "query";
  rec.box = box;
  rec.layer = layer;
  rec.legacy_hash = legacy_hash;
  rec.overlay_hash = overlay_hash;
  rec.legacy_count = legacy_count;
  rec.overlay_count = overlay_count;
  Record(rec);
}

void ShadowDump::RecordGuideComparison(std::uint64_t legacy_hash,
                                       std::uint64_t overlay_hash,
                                       std::size_t legacy_count,
                                       std::size_t overlay_count,
                                       const Rect& box)
{
  ComparisonRecord rec;
  rec.entity = Entity::Guide;
  rec.query_kind = "queryGuide";
  rec.box = box;
  // Layer-less query — leave rec.layer as nullopt.
  rec.legacy_hash = legacy_hash;
  rec.overlay_hash = overlay_hash;
  rec.legacy_count = legacy_count;
  rec.overlay_count = overlay_count;
  Record(rec);
}

std::uint64_t ShadowDump::comparison_count() noexcept
{
  return comparison_counter().load(std::memory_order_relaxed);
}

std::uint64_t ShadowDump::mismatch_count() noexcept
{
  return mismatch_counter().load(std::memory_order_relaxed);
}

std::uint64_t ShadowDump::comparison_count_for(Entity e) noexcept
{
  return per_entity_calls()[static_cast<std::size_t>(e)].load(
      std::memory_order_relaxed);
}

std::uint64_t ShadowDump::mismatch_count_for(Entity e) noexcept
{
  return per_entity_mismatches()[static_cast<std::size_t>(e)].load(
      std::memory_order_relaxed);
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
  for (auto& a : per_entity_calls()) {
    a.store(0);
  }
  for (auto& a : per_entity_mismatches()) {
    a.store(0);
  }
}

}  // namespace drt::redesign::overlay
