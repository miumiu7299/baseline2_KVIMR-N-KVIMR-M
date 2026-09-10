#pragma once

#include <cstdint>
#include <vector>

#include "rocksdb/status.h"

namespace rocksdb {

enum class KVIMRTrackType { TOP, BOTTOM };

struct KVIMRTrackSegment {
  uint32_t zone;
  uint32_t group;
  KVIMRTrackType type;
  uint64_t logical_sector_start;
  uint32_t sector_length;
};

class KVIMRTrackAllocator {
 public:
  static const uint32_t kGroupsPerZone = 64;
  static const uint32_t kZoneSectors = 524288;
  static const uint32_t kGroupSectors = 8192;
  static const uint32_t kTopSectors = 3648;
  static const uint32_t kBottomSectors = 4544;

  explicit KVIMRTrackAllocator(uint32_t zone_count = 1);

  Status Allocate(uint64_t sstable_id, uint64_t bytes, int level,
                  int effective_largest_level,
                  std::vector<KVIMRTrackSegment>* segments);

  Status Release(uint64_t sstable_id);

  Status Restore(uint64_t sstable_id,
                 const std::vector<KVIMRTrackSegment>& segments);

  void Reset(uint32_t zone_count);

  uint32_t zones() const { return zones_; }

 private:
  struct Reservation {
    KVIMRTrackType type;
    size_t index;
  };

  uint64_t SegmentBytes(KVIMRTrackType type) const;
  uint64_t TierFreeBytes(KVIMRTrackType type) const;

  bool ReserveNext(KVIMRTrackType type, uint64_t sstable_id,
                   KVIMRTrackSegment* segment);

  void Undo(const std::vector<Reservation>& reservations,
            uint64_t sstable_id);

  KVIMRTrackSegment SegmentAt(KVIMRTrackType type, size_t index) const;

  uint32_t zones_;
  std::vector<uint64_t> top_owner_;
  std::vector<uint64_t> bottom_owner_;
  size_t top_cursor_;
  size_t bottom_cursor_;
};

}  // namespace rocksdb