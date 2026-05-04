// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.1.c — ClipDumpHook implementation with stratified reservoir.

#include "ClipDumpHook.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>

namespace drt::redesign::legality {

namespace {

std::string EnvOrEmpty(const char* name)
{
  const char* v = std::getenv(name);
  return v == nullptr ? std::string{} : std::string{v};
}

std::uint64_t EnvU64Or(const char* name, std::uint64_t default_v)
{
  const char* v = std::getenv(name);
  if (v == nullptr) {
    return default_v;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(v));
  } catch (...) {
    return default_v;
  }
}

}  // namespace

std::uint8_t LogBin(std::uint64_t n)
{
  if (n == 0) {
    return 0;
  }
  if (n <= 4) {
    return 1;
  }
  if (n <= 16) {
    return 2;
  }
  if (n <= 64) {
    return 3;
  }
  if (n <= 256) {
    return 4;
  }
  if (n <= 1024) {
    return 5;
  }
  return 6;
}

ClipBucketKey ComputeBucketKey(const ClipRecord& record)
{
  ClipBucketKey k;
  k.cand_bin = LogBin(record.candidates.size());
  k.ctx_bin = LogBin(record.context.size());
  std::uint8_t any_marker = 0;
  for (const auto& l : record.labels) {
    if (l.projected_marker_count != 0) {
      any_marker = 1;
      break;
    }
  }
  k.marker_present = any_marker;
  return k;
}

ClipDumpHook& ClipDumpHook::Instance()
{
  static ClipDumpHook instance;
  return instance;
}

ClipDumpHook::ClipDumpHook()
{
  out_path_ = EnvOrEmpty("DRT_DUMP_GC_CLIPS");
  if (out_path_.empty()) {
    return;
  }
  design_hint_ = EnvOrEmpty("DRT_DUMP_DESIGN");
  pdk_hint_ = EnvOrEmpty("DRT_DUMP_PDK");
  per_bucket_cap_
      = static_cast<std::size_t>(EnvU64Or("DRT_DUMP_RESERVOIR", 64));
  seed_ = EnvU64Or("DRT_DUMP_SEED", 42);
  reservoir_ = std::make_unique<
      BucketedReservoir<ClipBucketKey, ClipRecord>>(per_bucket_cap_, seed_);
  active_ = true;
}

ClipDumpHook::~ClipDumpHook()
{
  try {
    Flush();
  } catch (...) {
    // Never propagate from destructor.
  }
}

void ClipDumpHook::Dump(ClipRecord&& record)
{
  if (!active_) {
    return;
  }
  try {
    const ClipBucketKey k = ComputeBucketKey(record);
    std::lock_guard<std::mutex> lock(mu_);
    if (flushed_) {
      return;  // already shut down
    }
    reservoir_->Offer(k, std::move(record));
  } catch (const std::exception& e) {
    if (!warned_once_.exchange(true)) {
      std::fprintf(stderr,
                   "[drt::redesign] WARNING clip-dump offer failed: %s. "
                   "Subsequent failures silenced.\n",
                   e.what());
    }
  } catch (...) {
    if (!warned_once_.exchange(true)) {
      std::fprintf(stderr,
                   "[drt::redesign] WARNING clip-dump offer failed with "
                   "non-std::exception. Subsequent failures silenced.\n");
    }
  }
}

void ClipDumpHook::Flush()
{
  std::lock_guard<std::mutex> lock(mu_);
  if (!active_ || flushed_) {
    return;
  }
  flushed_ = true;
  DoFlushLocked();
}

void ClipDumpHook::DoFlushLocked()
{
  std::ofstream file(out_path_, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    std::fprintf(stderr,
                 "[drt::redesign] WARNING DRT_DUMP_GC_CLIPS=%s could not "
                 "be opened for write at flush time; corpus discarded.\n",
                 out_path_.c_str());
    return;
  }
  std::size_t bytes_total = 0;
  std::size_t bytes_max = 0;
  std::size_t records_written = 0;
  std::size_t records_marker_present = 0;
  try {
    WriteHeader(file);
    reservoir_->ForEachRetained(
        [&](const ClipBucketKey& key, const ClipRecord& r) {
          // Serialize into a memory buffer first to size each record.
          std::ostringstream tmp;
          WriteRecord(tmp, r);
          const std::string bytes = tmp.str();
          file.write(bytes.data(),
                     static_cast<std::streamsize>(bytes.size()));
          bytes_total += bytes.size();
          if (bytes.size() > bytes_max) {
            bytes_max = bytes.size();
          }
          ++records_written;
          if (key.marker_present != 0) {
            ++records_marker_present;
          }
        });
    file.flush();
  } catch (const std::exception& e) {
    std::fprintf(stderr,
                 "[drt::redesign] WARNING clip-dump flush failed midway: "
                 "%s; partial output written.\n",
                 e.what());
    return;
  }

  // Summary on stderr — the output that helps verify stratification is
  // doing what it should.
  const std::uint64_t total_seen = reservoir_->TotalSeen();
  const std::size_t avg_bytes
      = records_written == 0 ? 0u : (bytes_total / records_written);
  std::fprintf(
      stderr,
      "[drt::redesign] clip dump summary: path=%s  buckets=%zu  "
      "seen=%lu  retained=%zu  marker_present=%zu/%zu  "
      "avg_bytes/rec=%zu  max_bytes/rec=%zu  total_bytes=%zu  seed=%lu  "
      "per_bucket_cap=%zu\n",
      out_path_.c_str(),
      reservoir_->BucketCount(),
      static_cast<unsigned long>(total_seen),
      records_written,
      records_marker_present,
      records_written,
      avg_bytes,
      bytes_max,
      bytes_total,
      static_cast<unsigned long>(seed_),
      per_bucket_cap_);

  for (const auto& [key, stats] : reservoir_->Stats()) {
    std::fprintf(stderr,
                 "[drt::redesign]   bucket(cand_bin=%u, ctx_bin=%u, "
                 "marker=%u)  seen=%lu  retained=%zu\n",
                 static_cast<unsigned>(key.cand_bin),
                 static_cast<unsigned>(key.ctx_bin),
                 static_cast<unsigned>(key.marker_present),
                 static_cast<unsigned long>(stats.total_seen),
                 stats.retained);
  }
}

}  // namespace drt::redesign::legality
