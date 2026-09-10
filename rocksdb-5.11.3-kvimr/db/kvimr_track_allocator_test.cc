#include <cassert>
#include <set>
#include <vector>

#include "db/kvimr_track_allocator.h"

using rocksdb::KVIMRTrackAllocator;
using rocksdb::KVIMRTrackSegment;
using rocksdb::KVIMRTrackType;

int main() {
  KVIMRTrackAllocator allocator(2);
  std::vector<KVIMRTrackSegment> top;
  assert(allocator.Allocate(1, 1, 0, 3, &top).ok());
  assert(top.size() == 1 && top[0].type == KVIMRTrackType::TOP);

  std::vector<KVIMRTrackSegment> largest;
  const uint64_t bottom_bytes = 4544ULL * 512 * 2 * 64;
  assert(allocator.Allocate(2, bottom_bytes + 1, 3, 3, &largest).ok());
  assert(largest.size() == 2 * 64 + 1);
  assert(largest[0].type == KVIMRTrackType::BOTTOM);
  assert(largest.back().type == KVIMRTrackType::TOP);

  std::set<uint64_t> starts;
  for (const auto& segment : top) starts.insert(segment.logical_sector_start);
  for (const auto& segment : largest) {
    assert(starts.insert(segment.logical_sector_start).second);
  }

  assert(allocator.Release(1).ok());
  std::vector<KVIMRTrackSegment> reused;
  assert(allocator.Allocate(3, 1, 1, 3, &reused).ok());
  assert(reused[0].logical_sector_start != 0);

  KVIMRTrackAllocator one_zone(2);
  std::vector<KVIMRTrackSegment> boundary;
  assert(one_zone.Allocate(4, 130ULL * 1024 * 1024, 0, 0, &boundary).ok());
  for (size_t i = 1; i < boundary.size(); ++i) {
    assert(boundary[i].zone > boundary[i - 1].zone ||
           boundary[i].group > boundary[i - 1].group ||
           boundary[i].type != boundary[i - 1].type);
  }

  KVIMRTrackAllocator rollback(1);
  std::vector<KVIMRTrackSegment> impossible;
  assert(!rollback.Allocate(5, 1024ULL * 1024 * 1024, 0, 0, &impossible).ok());
  assert(impossible.empty());
  return 0;
}
