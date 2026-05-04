// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// P2.2.e.1.a — ClipDump serialization.
// Pure binary I/O on POD-like fields. Little-endian host assumed (x86_64).

#include "ClipDump.h"

#include <cstring>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace drt::redesign::legality {

namespace {

// Raw little-endian writers/readers. memcpy avoids alignment traps.
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
  if (s.size() > kMaxStringLen) {
    throw std::runtime_error("ClipDump: string exceeds kMaxStringLen");
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
  if (n > kMaxStringLen) {
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

void WriteShape(std::ostream& os, const Shape& s)
{
  WriteLE<std::int32_t>(os, s.x1);
  WriteLE<std::int32_t>(os, s.y1);
  WriteLE<std::int32_t>(os, s.x2);
  WriteLE<std::int32_t>(os, s.y2);
  WriteLE<std::int16_t>(os, s.layer);
  WriteLE<std::uint64_t>(os, s.net_id);
}

bool ReadShape(std::istream& is, Shape* s)
{
  if (!ReadLE(is, &s->x1)) {
    return false;
  }
  if (!ReadLE(is, &s->y1)) {
    return false;
  }
  if (!ReadLE(is, &s->x2)) {
    return false;
  }
  if (!ReadLE(is, &s->y2)) {
    return false;
  }
  if (!ReadLE(is, &s->layer)) {
    return false;
  }
  if (!ReadLE(is, &s->net_id)) {
    return false;
  }
  return true;
}

void WriteLabel(std::ostream& os, const ProjectedUpstreamLabel& l)
{
  WriteLE<std::uint32_t>(os, l.projected_marker_count);
  WriteLE<std::uint8_t>(os, l.projected_legal);
  WriteLE<std::uint8_t>(os, l.projection_ambiguous);
}

bool ReadLabel(std::istream& is, ProjectedUpstreamLabel* l)
{
  if (!ReadLE(is, &l->projected_marker_count)) {
    return false;
  }
  if (!ReadLE(is, &l->projected_legal)) {
    return false;
  }
  if (!ReadLE(is, &l->projection_ambiguous)) {
    return false;
  }
  return true;
}

}  // namespace

void WriteHeader(std::ostream& os)
{
  WriteLE<std::uint32_t>(os, kClipDumpMagic);
  WriteLE<std::uint32_t>(os, kClipDumpVersion);
}

bool ReadHeader(std::istream& is, std::uint32_t* version_out)
{
  std::uint32_t magic = 0;
  if (!ReadLE(is, &magic)) {
    return false;
  }
  if (magic != kClipDumpMagic) {
    return false;
  }
  std::uint32_t version = 0;
  if (!ReadLE(is, &version)) {
    return false;
  }
  if (version_out != nullptr) {
    *version_out = version;
  }
  return true;
}

void WriteRecord(std::ostream& os, const ClipRecord& r)
{
  if (r.candidates.size() > kMaxShapeCount
      || r.context.size() > kMaxShapeCount) {
    throw std::runtime_error("ClipDump: shape count exceeds kMaxShapeCount");
  }
  if (r.labels.size() != r.candidates.size()) {
    throw std::runtime_error(
        "ClipDump: labels.size() must equal candidates.size()");
  }

  // Build the record body in a memory buffer first so we can write a
  // length prefix without seeking back. Records are small relative to
  // typical run output (kB to MB).
  std::ostringstream body;
  WriteLE<std::uint64_t>(body, r.meta.clip_id);
  WriteString(body, r.meta.design);
  WriteString(body, r.meta.pdk);
  WriteLE<std::uint64_t>(body, r.meta.tech_hash);
  WriteLE<std::uint64_t>(body, r.meta.rule_deck_fingerprint);
  WriteLE<std::int32_t>(body, r.meta.clip_x1);
  WriteLE<std::int32_t>(body, r.meta.clip_y1);
  WriteLE<std::int32_t>(body, r.meta.clip_x2);
  WriteLE<std::int32_t>(body, r.meta.clip_y2);
  WriteLE<std::int32_t>(body, r.meta.route_x1);
  WriteLE<std::int32_t>(body, r.meta.route_y1);
  WriteLE<std::int32_t>(body, r.meta.route_x2);
  WriteLE<std::int32_t>(body, r.meta.route_y2);
  WriteLE<std::uint32_t>(body, static_cast<std::uint32_t>(r.candidates.size()));
  WriteLE<std::uint32_t>(body, static_cast<std::uint32_t>(r.context.size()));
  for (const auto& s : r.candidates) {
    WriteShape(body, s);
  }
  for (const auto& s : r.context) {
    WriteShape(body, s);
  }
  for (const auto& l : r.labels) {
    WriteLabel(body, l);
  }

  const std::string body_bytes = body.str();
  WriteLE<std::uint32_t>(os,
                         static_cast<std::uint32_t>(body_bytes.size()));
  os.write(body_bytes.data(),
           static_cast<std::streamsize>(body_bytes.size()));
}

bool ReadRecord(std::istream& is, ClipRecord* out)
{
  std::uint32_t body_len = 0;
  if (!ReadLE(is, &body_len)) {
    return false;  // clean EOF or short stream
  }
  // Best-effort sanity bound: an absurdly large body length signals a
  // corrupted stream rather than legitimate data.
  if (body_len > (1u << 30)) {
    return false;
  }

  if (!ReadLE(is, &out->meta.clip_id)) {
    return false;
  }
  if (!ReadString(is, &out->meta.design)) {
    return false;
  }
  if (!ReadString(is, &out->meta.pdk)) {
    return false;
  }
  if (!ReadLE(is, &out->meta.tech_hash)) {
    return false;
  }
  if (!ReadLE(is, &out->meta.rule_deck_fingerprint)) {
    return false;
  }
  if (!ReadLE(is, &out->meta.clip_x1)) {
    return false;
  }
  if (!ReadLE(is, &out->meta.clip_y1)) {
    return false;
  }
  if (!ReadLE(is, &out->meta.clip_x2)) {
    return false;
  }
  if (!ReadLE(is, &out->meta.clip_y2)) {
    return false;
  }
  if (!ReadLE(is, &out->meta.route_x1)) {
    return false;
  }
  if (!ReadLE(is, &out->meta.route_y1)) {
    return false;
  }
  if (!ReadLE(is, &out->meta.route_x2)) {
    return false;
  }
  if (!ReadLE(is, &out->meta.route_y2)) {
    return false;
  }

  std::uint32_t cand_count = 0;
  std::uint32_t ctx_count = 0;
  if (!ReadLE(is, &cand_count)) {
    return false;
  }
  if (!ReadLE(is, &ctx_count)) {
    return false;
  }
  if (cand_count > kMaxShapeCount || ctx_count > kMaxShapeCount) {
    return false;
  }

  out->candidates.assign(cand_count, Shape{});
  for (auto& s : out->candidates) {
    if (!ReadShape(is, &s)) {
      return false;
    }
  }
  out->context.assign(ctx_count, Shape{});
  for (auto& s : out->context) {
    if (!ReadShape(is, &s)) {
      return false;
    }
  }
  out->labels.assign(cand_count, ProjectedUpstreamLabel{});
  for (auto& l : out->labels) {
    if (!ReadLabel(is, &l)) {
      return false;
    }
  }
  return true;
}

}  // namespace drt::redesign::legality
