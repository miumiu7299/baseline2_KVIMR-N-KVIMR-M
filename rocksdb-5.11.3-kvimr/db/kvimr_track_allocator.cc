#include "db/kvimr_track_allocator.h"

#include <algorithm>
#include <limits>

namespace rocksdb {

namespace {
size_t TrackCount(uint32_t zone_count) {
  return static_cast<size_t>(zone_count) *
         KVIMRTrackAllocator::kGroupsPerZone;
}
}  // namespace

KVIMRTrackAllocator::KVIMRTrackAllocator(uint32_t zone_count)
    : top_cursor_(0), bottom_cursor_(0) {
  Reset(zone_count);
}

void KVIMRTrackAllocator::Reset(uint32_t zone_count) {
  zones_ = zone_count;
  top_owner_.assign(TrackCount(zone_count), 0);
  bottom_owner_.assign(TrackCount(zone_count), 0);
  top_cursor_ = 0;
  bottom_cursor_ = 0;
}

uint64_t KVIMRTrackAllocator::SegmentBytes(KVIMRTrackType type) const {
  return static_cast<uint64_t>(type == KVIMRTrackType::TOP ? kTopSectors : kBottomSectors) * 512;
}

uint64_t KVIMRTrackAllocator::TierFreeBytes(KVIMRTrackType type) const {
  const std::vector<uint64_t>& owners =
      type == KVIMRTrackType::TOP ? top_owner_ : bottom_owner_;
  const uint64_t bytes = SegmentBytes(type);
  return static_cast<uint64_t>(std::count(owners.begin(), owners.end(), 0)) * bytes;
}

KVIMRTrackSegment KVIMRTrackAllocator::SegmentAt(KVIMRTrackType type,
                                                  size_t index) const {
  const uint32_t zone = static_cast<uint32_t>(index / kGroupsPerZone);
  const uint32_t group = static_cast<uint32_t>(index % kGroupsPerZone);
  const uint64_t group_start = static_cast<uint64_t>(zone) * kZoneSectors +
                               static_cast<uint64_t>(group) * kGroupSectors;
  KVIMRTrackSegment segment;
  segment.zone = zone;
  segment.group = group;
  segment.type = type;
  segment.logical_sector_start =
      group_start + (type == KVIMRTrackType::TOP ? 0 : kTopSectors);
  segment.sector_length = type == KVIMRTrackType::TOP ? kTopSectors : kBottomSectors;
  return segment;
}

bool KVIMRTrackAllocator::ReserveNext(KVIMRTrackType type, uint64_t sstable_id,
                                      KVIMRTrackSegment* segment) {
  std::vector<uint64_t>& owners =
      type == KVIMRTrackType::TOP ? top_owner_ : bottom_owner_;
  size_t& cursor = type == KVIMRTrackType::TOP ? top_cursor_ : bottom_cursor_;
  if (owners.empty()) return false;
  for (size_t step = 0; step < owners.size(); ++step) {
    const size_t index = (cursor + step) % owners.size();
    if (owners[index] == 0) {
      owners[index] = sstable_id;
      cursor = (index + 1) % owners.size();
      *segment = SegmentAt(type, index);
      return true;
    }
  }
  return false;
}

void KVIMRTrackAllocator::Undo(const std::vector<Reservation>& reservations,
                               uint64_t sstable_id) {
  for (const Reservation& reservation : reservations) {
    std::vector<uint64_t>& owners = reservation.type == KVIMRTrackType::TOP
                                        ? top_owner_
                                        : bottom_owner_;
    if (reservation.index < owners.size() && owners[reservation.index] == sstable_id)
      owners[reservation.index] = 0;
  }
}

Status KVIMRTrackAllocator::Allocate(
    uint64_t sstable_id, uint64_t bytes, int level, int effective_largest_level,
    std::vector<KVIMRTrackSegment>* segments) {
  segments->clear();
  if (sstable_id == 0 || bytes == 0)
    return Status::InvalidArgument("invalid KVIMR allocation request");
  const bool bottom_first = level == effective_largest_level;
  const uint64_t available = TierFreeBytes(KVIMRTrackType::TOP) +
      (bottom_first ? TierFreeBytes(KVIMRTrackType::BOTTOM) : 0);
  if (bytes > available)
    return Status::IOError("insufficient KVIMR track capacity");

  std::vector<Reservation> reservations;
  uint64_t remaining = bytes;
  const KVIMRTrackType first = bottom_first ? KVIMRTrackType::BOTTOM : KVIMRTrackType::TOP;
  const KVIMRTrackType second = KVIMRTrackType::TOP;
  const KVIMRTrackType tiers[2] = {first, second};
  const int tier_count = bottom_first ? 2 : 1;
  for (int tier_index = 0; tier_index < tier_count && remaining > 0; ++tier_index) {
    const KVIMRTrackType type = tiers[tier_index];
    while (remaining > 0) {
      KVIMRTrackSegment segment;
      if (!ReserveNext(type, sstable_id, &segment)) break;
      segments->push_back(segment);
      const size_t index = static_cast<size_t>(segment.zone) * kGroupsPerZone + segment.group;
      reservations.push_back({type, index});
      const uint64_t capacity = SegmentBytes(type);
      remaining = remaining > capacity ? remaining - capacity : 0;
      if (tier_index == 0 && bottom_first && remaining > 0 &&
          TierFreeBytes(type) == 0) break;
      if (!bottom_first && remaining == 0) break;
    }
  }
  if (remaining != 0) {
    Undo(reservations, sstable_id);
    segments->clear();
    return Status::IOError("insufficient KVIMR track capacity");
  }
  return Status::OK();
}

Status KVIMRTrackAllocator::Release(uint64_t sstable_id) {
  if (sstable_id == 0) return Status::InvalidArgument("invalid KVIMR SSTable ID");
  bool found = false;
  for (uint64_t& owner : top_owner_) {
    if (owner == sstable_id) { owner = 0; found = true; }
  }
  for (uint64_t& owner : bottom_owner_) {
    if (owner == sstable_id) { owner = 0; found = true; }
  }
  return found ? Status::OK() : Status::NotFound("KVIMR allocation not found");
}

Status KVIMRTrackAllocator::Restore(
    uint64_t sstable_id, const std::vector<KVIMRTrackSegment>& segments) {
  if (sstable_id == 0 || segments.empty())
    return Status::InvalidArgument("invalid KVIMR restore request");
  if (top_owner_.empty() || bottom_owner_.empty())
    return Status::InvalidArgument("KVIMR allocator has no tracks");
  std::vector<Reservation> reservations;
  size_t top_last = top_cursor_;
  size_t bottom_last = bottom_cursor_;
  for (const KVIMRTrackSegment& segment : segments) {
    if (segment.zone >= zones_ || segment.group >= kGroupsPerZone)
      return Status::InvalidArgument("invalid KVIMR track geometry");
    const bool top = segment.type == KVIMRTrackType::TOP;
    const uint64_t expected_start =
        static_cast<uint64_t>(segment.zone) * kZoneSectors +
        static_cast<uint64_t>(segment.group) * kGroupSectors +
        (top ? 0 : kTopSectors);
    const uint32_t expected_length = top ? kTopSectors : kBottomSectors;
    if (segment.logical_sector_start != expected_start ||
        segment.sector_length != expected_length)
      return Status::InvalidArgument("invalid KVIMR track segment");
    std::vector<uint64_t>& owners = top ? top_owner_ : bottom_owner_;
    const size_t index = static_cast<size_t>(segment.zone) * kGroupsPerZone +
                         segment.group;
    if (owners[index] != 0) {
      Undo(reservations, sstable_id);
      return Status::IOError("KVIMR track already owned");
    }
    owners[index] = sstable_id;
    reservations.push_back({top ? KVIMRTrackType::TOP : KVIMRTrackType::BOTTOM,
                             index});
    if (top) top_last = (index + 1) % top_owner_.size();
    else bottom_last = (index + 1) % bottom_owner_.size();
  }
  top_cursor_ = top_last;
  bottom_cursor_ = bottom_last;
  return Status::OK();
}

}  // namespace rocksdb
