// Copyright (c) 2024, KVIMR contributors.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "db/kvimr.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "rocksdb/env.h"

namespace rocksdb {

namespace {
const uint32_t kMetadataVersion = 1;
const uint32_t kSectorSize = 512;
const char kMetadataMagic[] = "KVIMR35A";

uint64_t FileNumber(const std::string& fname) {
  size_t begin = fname.find_last_of("/\\");
  begin = begin == std::string::npos ? 0 : begin + 1;
  size_t end = fname.find('.', begin);
  if (end == std::string::npos || end == begin) {
    return 0;
  }
  uint64_t number = 0;
  for (size_t i = begin; i < end; ++i) {
    if (fname[i] < '0' || fname[i] > '9') {
      return 0;
    }
    number = number * 10 + static_cast<uint64_t>(fname[i] - '0');
  }
  return number;
}

void Put32(std::string* out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) out->push_back(static_cast<char>(value >> (i * 8)));
}
void Put64(std::string* out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) out->push_back(static_cast<char>(value >> (i * 8)));
}
bool Get32(const std::string& in, size_t* pos, uint32_t* value) {
  if (*pos + 4 > in.size()) return false;
  *value = 0;
  for (unsigned i = 0; i < 4; ++i) *value |= static_cast<uint32_t>(static_cast<unsigned char>(in[*pos + i])) << (i * 8);
  *pos += 4;
  return true;
}
bool Get64(const std::string& in, size_t* pos, uint64_t* value) {
  if (*pos + 8 > in.size()) return false;
  *value = 0;
  for (unsigned i = 0; i < 8; ++i) *value |= static_cast<uint64_t>(static_cast<unsigned char>(in[*pos + i])) << (i * 8);
  *pos += 8;
  return true;
}
Status IOError(const std::string& what) {
  return Status::IOError(what, std::strerror(errno));
}
}  // namespace

KVIMR::KVIMR() = default;

KVIMR* KVIMR::Get() {
  static KVIMR instance;
  return &instance;
}

void KVIMR::RegisterSSTable(const std::string& fname, uint64_t file_number,
                            int level, int effective_largest_level) {
  std::lock_guard<std::mutex> lock(mutex_);
  SSTableState& state = files_[fname];
  state.file_number = file_number;
  state.level = level;
  state.effective_largest_level = effective_largest_level;
  s2tmap_[file_number];
}

void KVIMR::BeginWrite(const std::string& fname) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = files_.find(fname);
  if (it != files_.end()) {
    if (!it->second.tracks.empty()) {
      allocator_.Release(it->second.file_number);
      s2tmap_.erase(it->second.file_number);
      it->second.tracks.clear();
    }
    it->second.data.clear();
    it->second.pending_updates.clear();
    it->second.merged_buffers.clear();
    it->second.synced = false;
  }
}

Status KVIMR::kvwrite(const std::string& fname, const Slice& data) {
  std::lock_guard<std::mutex> lock(mutex_);
  SSTableState& state = files_[fname];
  const uint64_t offset = static_cast<uint64_t>(state.data.size());
  state.data.append(data.data(), data.size());
  if (data.size() != 0) {
    SSTableState::PendingUpdate update;
    update.offset = offset;
    update.data.assign(data.data(), data.size());
    state.pending_updates.push_back(update);
  }
  state.synced = false;
  return Status::OK();
}

Status KVIMR::kvread(const std::string& fname, uint64_t offset, size_t n,
                     Slice* result, char* scratch) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = files_.find(fname);
  if (it == files_.end()) {
    return Status::NotFound(fname, "KVIMR SSTable not found");
  }
  const std::string& data = it->second.data;
  if (offset > data.size()) {
    *result = Slice();
    return Status::OK();
  }
  const size_t available = data.size() - static_cast<size_t>(offset);
  const size_t length = std::min(n, available);
  if (length != 0) {
    memcpy(scratch, data.data() + offset, length);
  }
  *result = Slice(scratch, length);
  return Status::OK();
}

Status KVIMR::kvsync(const std::string& fname) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = files_.find(fname);
  if (it == files_.end()) {
    return Status::NotFound(fname);
  }
  if (it->second.data.empty() && it->second.pending_updates.empty()) {
    it->second.synced = true;
    if (!persistence_enabled_) return Status::OK();
    Status status = backend_.Sync();
    if (!status.ok()) return status;
    status = PersistSnapshotLocked();
    if (!status.ok()) it->second.synced = false;
    return status;
  }
  if (it->second.tracks.empty()) {
    Status allocation = allocator_.Allocate(
        it->second.file_number, it->second.data.size(), it->second.level,
        it->second.effective_largest_level, &it->second.tracks);
    if (!allocation.ok()) return allocation;
    s2tmap_[it->second.file_number] = it->second.tracks;
  }
  if (persistence_enabled_) {
    Status status = PersistPayloadLocked(&it->second);
    if (!status.ok()) return status;
    status = backend_.Sync();
    if (!status.ok()) return status;
    it->second.synced = true;
    status = PersistSnapshotLocked();
    if (!status.ok()) {
      it->second.synced = false;
      return status;
    }
    it->second.pending_updates.clear();
    it->second.merged_buffers.clear();
    return status;
  }
  it->second.synced = true;
  return Status::OK();
}

Status KVIMR::kvunlink(const std::string& fname) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = files_.find(fname);
  if (it != files_.end()) {
    if (persistence_enabled_ && !it->second.pending_updates.empty()) {
      Status status = PersistPayloadLocked(&it->second);
      if (!status.ok()) return status;
      status = backend_.Sync();
      if (!status.ok()) return status;
    }
    allocator_.Release(it->second.file_number);
    s2tmap_.erase(it->second.file_number);
    files_.erase(it);
  } else {
    s2tmap_.erase(FileNumber(fname));
  }
  return persistence_enabled_ ? PersistSnapshotLocked() : Status::OK();
}

bool KVIMR::HasSSTable(const std::string& fname) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return files_.find(fname) != files_.end();
}

Status KVIMR::GetSSTableSize(const std::string& fname, uint64_t* size) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = files_.find(fname);
  if (it == files_.end()) {
    return Status::NotFound(fname);
  }
  *size = it->second.data.size();
  return Status::OK();
}

void KVIMR::ListSSTables(const std::string& dir,
                         std::vector<std::string>* names) const {
  std::lock_guard<std::mutex> lock(mutex_);
  names->clear();
  for (const auto& entry : files_) {
    size_t slash = entry.first.find_last_of("/\\");
    std::string parent = slash == std::string::npos ? "" : entry.first.substr(0, slash);
    if (parent == dir) {
      names->push_back(entry.first.substr(slash == std::string::npos ? 0 : slash + 1));
    }
  }
}

size_t KVIMR::SSTableCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return files_.size();
}

size_t KVIMR::S2TMapSize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return s2tmap_.size();
}

void KVIMR::GetTrackSegmentCounts(size_t* top, size_t* bottom) const {
  std::lock_guard<std::mutex> lock(mutex_);
  *top = 0;
  *bottom = 0;
  for (const auto& entry : files_) {
    for (const KVIMRTrackSegment& segment : entry.second.tracks) {
      if (segment.type == KVIMRTrackType::TOP) {
        ++*top;
      } else {
        ++*bottom;
      }
    }
  }
}

void KVIMR::ConfigureTrackAllocator(uint32_t zones) {
  std::lock_guard<std::mutex> lock(mutex_);
  allocator_.Reset(zones);
  for (auto& entry : files_) entry.second.tracks.clear();
  s2tmap_.clear();
}

Status KVIMR::InitializePersistence(const std::string& dbname, uint32_t zones,
                                    const std::string& device_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (zones == 0) return Status::InvalidArgument("KVIMR zone count must be nonzero");
  if (persistence_enabled_) backend_.Close();
  dbname_ = dbname;
  metadata_path_ = dbname_ + "/KVIMR-META";
  configured_zones_ = zones;
  files_.clear();
  s2tmap_.clear();
  allocator_.Reset(zones);
  Status status = backend_.Open(device_path);
  if (!status.ok()) return status;
  persistence_enabled_ = true;
  return LoadSnapshotLocked();
}

Status KVIMR::PersistPayloadLocked(SSTableState* state) {
  if (state->tracks.empty()) {
    Status allocation = allocator_.Allocate(
        state->file_number, state->data.size(), state->level,
        state->effective_largest_level, &state->tracks);
    if (!allocation.ok()) return allocation;
    s2tmap_[state->file_number] = state->tracks;
  }
  return ApplyPendingUpdatesLocked(state);
}

Status KVIMR::ApplyPendingUpdatesLocked(SSTableState* state) {
  uint64_t total_capacity = 0;
  for (const KVIMRTrackSegment& segment : state->tracks)
    total_capacity += static_cast<uint64_t>(segment.sector_length) * kSectorSize;
  if (state->data.size() > total_capacity)
    return Status::IOError("KVIMR payload exceeds allocated tracks");
  for (const SSTableState::PendingUpdate& update : state->pending_updates) {
    const uint64_t update_end = update.offset + update.data.size();
    uint64_t segment_start = 0;
    for (size_t index = 0; index < state->tracks.size(); ++index) {
      const KVIMRTrackSegment& segment = state->tracks[index];
      const uint64_t capacity = static_cast<uint64_t>(segment.sector_length) * kSectorSize;
      const uint64_t segment_end = segment_start + capacity;
      const uint64_t overlap_start = std::max(update.offset, segment_start);
      const uint64_t overlap_end = std::min(update_end, segment_end);
      if (overlap_start < overlap_end) {
        const size_t local = static_cast<size_t>(overlap_start - segment_start);
        const size_t source = static_cast<size_t>(overlap_start - update.offset);
        const size_t length = static_cast<size_t>(overlap_end - overlap_start);
        const uint64_t byte_offset = segment.logical_sector_start * kSectorSize;
        if (segment.type == KVIMRTrackType::TOP) {
          if (rmw_mode_ == RMWMode::NAIVE) {
            std::vector<char> track(static_cast<size_t>(capacity), 0);
            Status status = backend_.Read(byte_offset, static_cast<size_t>(capacity), track.data());
            if (!status.ok()) return status;
            memcpy(track.data() + local, update.data.data() + source, length);
            status = backend_.Write(byte_offset, static_cast<size_t>(capacity), track.data());
            if (!status.ok()) return status;
            ++rmw_counters_.naive_rmw_count;
            rmw_counters_.naive_rmw_read_bytes += capacity;
            rmw_counters_.naive_rmw_write_bytes += capacity;
          } else {
            ++rmw_counters_.merged_logical_update_count;
            SSTableState::MergedTrackBuffer& buffer = state->merged_buffers[index];
            if (!buffer.loaded) {
              buffer.segment = segment;
              buffer.data.resize(static_cast<size_t>(capacity), 0);
              Status status = backend_.Read(byte_offset, static_cast<size_t>(capacity), buffer.data.data());
              if (!status.ok()) return status;
              rmw_counters_.merged_rmw_read_bytes += capacity;
              ++rmw_counters_.merged_buffer_create_count;
              buffer.loaded = true;
            } else {
              ++rmw_counters_.merged_updates_saved;
              ++rmw_counters_.merged_buffer_hit_count;
            }
            memcpy(buffer.data.data() + local, update.data.data() + source, length);
            ++buffer.update_count;
            buffer.dirty = true;
          }
        } else {
          const uint64_t aligned_start = overlap_start / kSectorSize * kSectorSize;
          const uint64_t aligned_end = (overlap_end + kSectorSize - 1) / kSectorSize * kSectorSize;
          const size_t write_size = static_cast<size_t>(aligned_end - aligned_start);
          std::vector<char> direct(write_size, 0);
          const uint64_t file_offset = aligned_start;
          if (file_offset < state->data.size()) {
            const size_t copy_size = std::min(write_size, state->data.size() - static_cast<size_t>(file_offset));
            memcpy(direct.data(), state->data.data() + file_offset, copy_size);
          }
          Status status = backend_.Write(byte_offset + aligned_start - segment_start,
                                         write_size, direct.data());
          if (!status.ok()) return status;
          rmw_counters_.direct_write_bytes += write_size;
        }
      }
      segment_start = segment_end;
      if (segment_start >= update_end) break;
    }
  }
  if (rmw_mode_ == RMWMode::MERGED) return FlushMergedBuffersLocked(state);
  return Status::OK();
}

Status KVIMR::FlushMergedBuffersLocked(SSTableState* state) {
  for (const auto& entry : state->merged_buffers) {
    const SSTableState::MergedTrackBuffer& buffer = entry.second;
    if (!buffer.dirty) continue;
    const uint64_t offset = buffer.segment.logical_sector_start * kSectorSize;
    const size_t bytes = buffer.data.size();
    Status status = backend_.Write(offset, bytes, buffer.data.data());
    if (!status.ok()) return status;
    ++rmw_counters_.merged_rmw_count;
    rmw_counters_.merged_rmw_write_bytes += bytes;
    ++rmw_counters_.merged_buffer_flush_count;
  }
  return Status::OK();
}

Status KVIMR::RestorePayloadLocked(SSTableState* state) {
  std::string restored;
  restored.resize(state->data.size());
  size_t offset = 0;
  for (const KVIMRTrackSegment& segment : state->tracks) {
    if (offset >= restored.size()) break;
    const size_t capacity = static_cast<size_t>(segment.sector_length) * kSectorSize;
    const size_t chunk = std::min(capacity, restored.size() - offset);
    const size_t read_size = (chunk + kSectorSize - 1) / kSectorSize * kSectorSize;
    std::vector<char> buffer(read_size);
    Status status = backend_.Read(segment.logical_sector_start * kSectorSize,
                                  read_size, buffer.data());
    if (!status.ok()) return status;
    memcpy(&restored[offset], buffer.data(), chunk);
    offset += chunk;
  }
  if (offset != restored.size()) return Status::IOError("KVIMR payload shorter than metadata");
  state->data.swap(restored);
  return Status::OK();
}

Status KVIMR::PersistSnapshotLocked() {
  std::string bytes(kMetadataMagic, sizeof(kMetadataMagic) - 1);
  Put32(&bytes, kMetadataVersion); Put32(&bytes, kSectorSize);
  Put32(&bytes, KVIMRTrackAllocator::kZoneSectors);
  Put32(&bytes, KVIMRTrackAllocator::kGroupsPerZone);
  Put32(&bytes, KVIMRTrackAllocator::kTopSectors);
  Put32(&bytes, KVIMRTrackAllocator::kBottomSectors);
  Put32(&bytes, configured_zones_); Put32(&bytes, static_cast<uint32_t>(files_.size()));
  for (const auto& entry : files_) {
    const SSTableState& state = entry.second;
    Put64(&bytes, state.file_number); Put32(&bytes, static_cast<uint32_t>(state.level));
    Put32(&bytes, static_cast<uint32_t>(state.effective_largest_level));
    Put64(&bytes, static_cast<uint64_t>(state.data.size()));
    bytes.push_back(static_cast<char>(state.synced));
    std::string relative = entry.first;
    if (relative.compare(0, dbname_.size(), dbname_) == 0) {
      relative = relative.substr(dbname_.size());
      while (!relative.empty() && (relative[0] == '/' || relative[0] == '\\')) relative.erase(0, 1);
    }
    Put32(&bytes, static_cast<uint32_t>(relative.size())); bytes.append(relative);
    Put32(&bytes, static_cast<uint32_t>(state.tracks.size()));
    for (const KVIMRTrackSegment& segment : state.tracks) {
      Put32(&bytes, segment.zone); Put32(&bytes, segment.group);
      bytes.push_back(static_cast<char>(segment.type == KVIMRTrackType::BOTTOM));
      Put64(&bytes, segment.logical_sector_start); Put32(&bytes, segment.sector_length);
    }
  }
  const std::string temp = metadata_path_ + ".tmp";
  int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return IOError("open KVIMR metadata temporary file");
  size_t written = 0;
  while (written < bytes.size()) {
    ssize_t n = write(fd, bytes.data() + written, bytes.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { close(fd); return IOError("write KVIMR metadata"); }
    written += static_cast<size_t>(n);
  }
  if (::fdatasync(fd) != 0) { close(fd); return IOError("fdatasync KVIMR metadata"); }
  if (close(fd) != 0) return IOError("close KVIMR metadata");
  if (rename(temp.c_str(), metadata_path_.c_str()) != 0) return IOError("rename KVIMR metadata");
  int dirfd = open(dbname_.c_str(), O_RDONLY);
  if (dirfd < 0) {
    if (errno != EINVAL && errno != ENOTSUP) return IOError("open KVIMR metadata directory");
  } else {
    if (fsync(dirfd) != 0 && errno != EINVAL && errno != ENOTSUP) { close(dirfd); return IOError("fsync KVIMR metadata directory"); }
    close(dirfd);
  }
  return Status::OK();
}

Status KVIMR::LoadSnapshotLocked() {
  int fd = open(metadata_path_.c_str(), O_RDONLY);
  if (fd < 0) return errno == ENOENT ? Status::OK() : IOError("open KVIMR metadata");
  std::string bytes; char buffer[4096];
  for (;;) { ssize_t n = read(fd, buffer, sizeof(buffer)); if (n == 0) break; if (n < 0 && errno == EINTR) continue; if (n < 0) { close(fd); return IOError("read KVIMR metadata"); } bytes.append(buffer, static_cast<size_t>(n)); }
  close(fd);
  size_t pos = 0;
  if (bytes.size() < sizeof(kMetadataMagic)-1 || bytes.compare(0, sizeof(kMetadataMagic)-1, kMetadataMagic) != 0) return Status::Corruption("invalid KVIMR metadata magic");
  pos += sizeof(kMetadataMagic)-1;
  uint32_t version, sector, zone_sectors, groups, top, bottom, zones, count;
  if (!Get32(bytes,&pos,&version)||!Get32(bytes,&pos,&sector)||!Get32(bytes,&pos,&zone_sectors)||!Get32(bytes,&pos,&groups)||!Get32(bytes,&pos,&top)||!Get32(bytes,&pos,&bottom)||!Get32(bytes,&pos,&zones)||!Get32(bytes,&pos,&count)) return Status::Corruption("truncated KVIMR metadata");
  if (version != kMetadataVersion || sector != kSectorSize || zone_sectors != KVIMRTrackAllocator::kZoneSectors || groups != KVIMRTrackAllocator::kGroupsPerZone || top != KVIMRTrackAllocator::kTopSectors || bottom != KVIMRTrackAllocator::kBottomSectors || zones != configured_zones_) return Status::InvalidArgument("KVIMR metadata geometry mismatch");
  for (uint32_t i=0; i<count; ++i) {
    SSTableState state; uint32_t level, largest, path_len, seg_count; uint64_t size;
    if (!Get64(bytes,&pos,&state.file_number)||!Get32(bytes,&pos,&level)||!Get32(bytes,&pos,&largest)||!Get64(bytes,&pos,&size)||pos>=bytes.size()) return Status::Corruption("truncated KVIMR metadata entry");
    state.synced = bytes[pos++] != 0;
    if (!Get32(bytes,&pos,&path_len)||pos+path_len>bytes.size()) return Status::Corruption("truncated KVIMR metadata entry");
    if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) return Status::InvalidArgument("KVIMR metadata file is too large");
    state.level = static_cast<int>(level); state.effective_largest_level = static_cast<int>(largest);
    std::string relative = bytes.substr(pos,path_len); pos += path_len;
    state.data.resize(static_cast<size_t>(size));
    if (!Get32(bytes,&pos,&seg_count)) return Status::Corruption("truncated KVIMR segment list");
    for (uint32_t s=0; s<seg_count; ++s) { uint32_t zone, group, length; uint64_t start; if (!Get32(bytes,&pos,&zone)||!Get32(bytes,&pos,&group)||pos>=bytes.size()) return Status::Corruption("truncated KVIMR segment"); const bool is_bottom = bytes[pos++] != 0; if (!Get64(bytes,&pos,&start)||!Get32(bytes,&pos,&length)) return Status::Corruption("truncated KVIMR segment"); KVIMRTrackSegment seg = {zone, group, is_bottom ? KVIMRTrackType::BOTTOM : KVIMRTrackType::TOP, start, length}; state.tracks.push_back(seg); }
    const std::string fname = dbname_ + "/" + relative;
    if (!state.tracks.empty()) {
      Status restore = allocator_.Restore(state.file_number, state.tracks);
      if (!restore.ok()) return restore;
    }
    s2tmap_[state.file_number] = state.tracks; files_[fname] = state;
    Status payload = RestorePayloadLocked(&files_[fname]); if (!payload.ok()) return payload;
  }
  return Status::OK();
}

KVIMR::RMWCounters KVIMR::GetRMWCounters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rmw_counters_;
}

void KVIMR::ResetRMWCounters() {
  std::lock_guard<std::mutex> lock(mutex_);
  rmw_counters_ = RMWCounters();
}

void KVIMR::SetRMWMode(RMWMode mode) {
  std::lock_guard<std::mutex> lock(mutex_);
  rmw_mode_ = mode;
}

KVIMR::RMWMode KVIMR::GetRMWMode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rmw_mode_;
}

}  // namespace rocksdb
