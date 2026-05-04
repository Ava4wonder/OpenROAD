// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.2.c — Offline E2 audit harness.
//
// Usage:
//   redesign_e2_audit <base-glob>
// where <base-glob> is a directory + prefix; the tool reads every
// ".ruledeck" file matching <base-glob>*.ruledeck and every clip-dump
// file matching <base-glob>* (excluding .ruledeck), aggregates per
// design, and prints a structured report to stdout.
//
// Per the closeout review of R15: report breaks down per-design AND
// per-family AND per-layer (not aggregate-only).

#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "redesign/legality/AuditReport.h"
#include "redesign/legality/ClipDump.h"
#include "redesign/legality/RuleDeckDump.h"

namespace lg = drt::redesign::legality;
namespace fs = std::filesystem;

namespace {

bool EndsWith(const std::string& s, const std::string& suffix)
{
  return s.size() >= suffix.size()
         && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool ProcessRuleDeckFile(const fs::path& path, lg::AuditReport* out)
{
  std::ifstream is(path, std::ios::binary);
  if (!is.is_open()) {
    std::fprintf(stderr,
                 "[audit] WARNING could not open %s: %s\n",
                 path.c_str(),
                 std::strerror(errno));
    return false;
  }
  lg::RuleDeck deck;
  lg::RuleDeckProvenance prov;
  if (!lg::ReadRuleDeck(is, &deck, &prov)) {
    std::fprintf(stderr, "[audit] WARNING failed to parse %s\n", path.c_str());
    return false;
  }
  lg::IngestRuleDeck(deck, prov, out);
  return true;
}

bool ProcessClipDumpFile(const fs::path& path, lg::AuditReport* out)
{
  std::ifstream is(path, std::ios::binary);
  if (!is.is_open()) {
    std::fprintf(stderr,
                 "[audit] WARNING could not open %s: %s\n",
                 path.c_str(),
                 std::strerror(errno));
    return false;
  }
  std::uint32_t version = 0;
  if (!lg::ReadHeader(is, &version)) {
    std::fprintf(stderr,
                 "[audit] WARNING %s: bad clip-dump header\n",
                 path.c_str());
    return false;
  }
  if (version != lg::kClipDumpVersion) {
    std::fprintf(
        stderr,
        "[audit] WARNING %s: clip-dump version %08x != expected %08x\n",
        path.c_str(),
        version,
        lg::kClipDumpVersion);
    return false;
  }
  std::vector<lg::ClipRecord> clips;
  while (true) {
    lg::ClipRecord r;
    if (!lg::ReadRecord(is, &r)) {
      break;
    }
    clips.push_back(std::move(r));
  }
  lg::IngestClipRecords(clips, out);
  return true;
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc != 2) {
    std::fprintf(stderr,
                 "Usage: %s <base-glob>\n"
                 "  Reads <base-glob>*.ruledeck and <base-glob>* clip files\n"
                 "  from the directory containing <base-glob>, and emits a\n"
                 "  per-design audit report to stdout.\n",
                 argv[0]);
    return 2;
  }
  const std::string base = argv[1];
  fs::path base_path = base;
  fs::path dir = base_path.parent_path();
  std::string prefix = base_path.filename().string();
  if (dir.empty()) {
    dir = ".";
  }

  if (!fs::exists(dir)) {
    std::fprintf(stderr, "[audit] directory %s does not exist\n", dir.c_str());
    return 1;
  }

  std::vector<fs::path> ruledeck_files;
  std::vector<fs::path> clip_files;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    if (EndsWith(name, ".ruledeck")) {
      ruledeck_files.push_back(entry.path());
    } else {
      clip_files.push_back(entry.path());
    }
  }

  std::fprintf(stderr,
               "[audit] dir=%s prefix=%s  found ruledecks=%zu clips=%zu\n",
               dir.c_str(),
               prefix.c_str(),
               ruledeck_files.size(),
               clip_files.size());

  if (ruledeck_files.empty() && clip_files.empty()) {
    std::fprintf(stderr,
                 "[audit] no matching files found; nothing to report\n");
    return 1;
  }

  lg::AuditReport report;
  for (const auto& p : ruledeck_files) {
    ProcessRuleDeckFile(p, &report);
  }
  for (const auto& p : clip_files) {
    ProcessClipDumpFile(p, &report);
  }
  lg::FinalizeJoinStatus(&report);
  lg::RenderReport(report, std::cout);
  return 0;
}
