// Copyright (c) 2024, KVIMR contributors.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "db/kvimr_track_allocator.h"
#include "db/imrsim_backend.h"

namespace rocksdb {

// KVIMR userspace middleware state. RMW policy is intentionally kept here,
// outside RocksDB core and the IMRSim kernel target.
class KVIMR {
 public:
  enum class RMWMode { NAIVE, MERGED };

  struct RMWCounters {
    uint64_t naive_rmw_count = 0;
    uint64_t naive_rmw_read_bytes = 0;
    uint64_t naive_rmw_write_bytes = 0;
    uint64_t direct_write_bytes = 0;
    uint64_t merged_rmw_count = 0;
    uint64_t merged_rmw_read_bytes = 0;
    uint64_t merged_rmw_write_bytes = 0;
    // Counted per TOP-track touch so it matches physical RMW plus saved hits.
    uint64_t merged_logical_update_count = 0;
    uint64_t merged_updates_saved = 0;
    uint64_t merged_buffer_create_count = 0;
    uint64_t merged_buffer_hit_count = 0;
    uint64_t merged_buffer_flush_count = 0;
  };

  static KVIMR* Get();

  void RegisterSSTable(const std::string& fname, uint64_t file_number,
                       int level, int effective_largest_level);
  void BeginWrite(const std::string& fname);
  Status kvwrite(const std::string& fname, const Slice& data);
  Status kvread(const std::string& fname, uint64_t offset, size_t n,
                Slice* result, char* scratch) const;
  Status kvsync(const std::string& fname);
  Status kvunlink(const std::string& fname);
  bool HasSSTable(const std::string& fname) const;
  Status GetSSTableSize(const std::string& fname, uint64_t* size) const;
  void ListSSTables(const std::string& dir, std::vector<std::string>* names) const;
  size_t SSTableCount() const;
  size_t S2TMapSize() const;
  void GetTrackSegmentCounts(size_t* top, size_t* bottom) const;
  void ConfigureTrackAllocator(uint32_t zones);
  Status InitializePersistence(const std::string& dbname, uint32_t zones,
                               const std::string& device_path =
                                   IMRSimBackend::kDefaultDevicePath);
  RMWCounters GetRMWCounters() const;
  void ResetRMWCounters();
  void SetRMWMode(RMWMode mode);
  RMWMode GetRMWMode() const;

 private:
  KVIMR();
  KVIMR(const KVIMR&) = delete;
  KVIMR& operator=(const KVIMR&) = delete;

  struct SSTableState {
    uint64_t file_number = 0;
    int level = -1;
    int effective_largest_level = -1;
    bool synced = false;
    std::string data;
    std::vector<KVIMRTrackSegment> tracks;
    struct PendingUpdate {
      uint64_t offset;
      std::string data;
    };
    struct MergedTrackBuffer {
      KVIMRTrackSegment segment;
      std::vector<char> data;
      uint64_t update_count = 0;
      bool loaded = false;
      bool dirty = false;
    };
    std::vector<PendingUpdate> pending_updates;
    std::map<size_t, MergedTrackBuffer> merged_buffers;
  };

  mutable std::mutex mutex_;
  std::map<std::string, SSTableState> files_;
  std::map<uint64_t, std::vector<KVIMRTrackSegment>> s2tmap_;
  KVIMRTrackAllocator allocator_;
  IMRSimBackend backend_;
  bool persistence_enabled_ = false;
  uint32_t configured_zones_ = 0;
  std::string dbname_;
  std::string metadata_path_;
  RMWCounters rmw_counters_;
  RMWMode rmw_mode_ = RMWMode::NAIVE;

  Status PersistSnapshotLocked();
  Status PersistPayloadLocked(SSTableState* state);
  Status ApplyPendingUpdatesLocked(SSTableState* state);
  Status FlushMergedBuffersLocked(SSTableState* state);
  Status RestorePayloadLocked(SSTableState* state);
  Status LoadSnapshotLocked();
};

}  // namespace rocksdb
