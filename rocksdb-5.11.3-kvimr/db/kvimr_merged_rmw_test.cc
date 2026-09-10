#include <cassert>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "db/kvimr.h"

using rocksdb::KVIMR;
using rocksdb::Slice;

namespace {
const size_t kTopBytes = 3648U * 512U;

void PrepareDevice(const std::string& db, const std::string& device) {
  mkdir(db.c_str(), 0755);
  int fd = open(device.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  assert(fd >= 0);
  assert(ftruncate(fd, 256 * 1024 * 1024) == 0);
  close(fd);
}

void StartFile(KVIMR* kvimr, const std::string& name, uint64_t number,
               int level, int largest) {
  kvimr->RegisterSSTable(name, number, level, largest);
  kvimr->BeginWrite(name);
}

void Write(KVIMR* kvimr, const std::string& name, const std::string& value) {
  assert(kvimr->kvwrite(name, Slice(value)).ok());
}
}

int main() {
  const std::string db = "/tmp/kvimr-merged-rmw-test";
  const std::string device = db + ".device";
  PrepareDevice(db, device);
  KVIMR* kvimr = KVIMR::Get();
  assert(kvimr->InitializePersistence(db, 1, device).ok());

  const std::string naive_name = db + "/000001.sst";
  kvimr->SetRMWMode(KVIMR::RMWMode::NAIVE);
  kvimr->ResetRMWCounters();
  StartFile(kvimr, naive_name, 1, 0, 3);
  Write(kvimr, naive_name, std::string(1000, 'a'));
  Write(kvimr, naive_name, std::string(1000, 'b'));
  KVIMR::RMWCounters counters = kvimr->GetRMWCounters();
  char scratch[2000]; Slice result;
  assert(kvimr->kvread(naive_name, 0, sizeof(scratch), &result, scratch).ok());
  assert(result.size() == sizeof(scratch));
  assert(kvimr->kvsync(naive_name).ok());
  counters = kvimr->GetRMWCounters();
  assert(counters.naive_rmw_count == 2);
  assert(kvimr->kvunlink(naive_name).ok());

  const std::string bottom_name = db + "/000006.sst";
  kvimr->ResetRMWCounters();
  StartFile(kvimr, bottom_name, 6, 3, 3);
  Write(kvimr, bottom_name, std::string(1000, 'd'));
  assert(kvimr->kvsync(bottom_name).ok());
  counters = kvimr->GetRMWCounters();
  assert(counters.naive_rmw_count == 0);
  assert(counters.merged_rmw_count == 0);
  assert(counters.direct_write_bytes != 0);
  assert(kvimr->kvunlink(bottom_name).ok());

  const std::string merged_name = db + "/000002.sst";
  kvimr->SetRMWMode(KVIMR::RMWMode::MERGED);
  kvimr->ResetRMWCounters();
  StartFile(kvimr, merged_name, 2, 0, 3);
  Write(kvimr, merged_name, std::string(1000, 'a'));
  Write(kvimr, merged_name, std::string(1000, 'b'));
  assert(kvimr->kvread(merged_name, 0, sizeof(scratch), &result, scratch).ok());
  assert(result.size() == sizeof(scratch));
  assert(result[0] == 'a' && result[1999] == 'b');
  assert(kvimr->kvsync(merged_name).ok());
  counters = kvimr->GetRMWCounters();
  assert(counters.merged_rmw_count == 1);
  assert(counters.merged_logical_update_count == 2);
  assert(counters.merged_updates_saved == 1);
  assert(kvimr->kvunlink(merged_name).ok());

  const std::string three_name = db + "/000003.sst";
  kvimr->ResetRMWCounters();
  StartFile(kvimr, three_name, 3, 0, 3);
  Write(kvimr, three_name, std::string(500, 'a'));
  Write(kvimr, three_name, std::string(500, 'b'));
  Write(kvimr, three_name, std::string(500, 'c'));
  assert(kvimr->kvsync(three_name).ok());
  counters = kvimr->GetRMWCounters();
  assert(counters.merged_rmw_count == 1);
  assert(counters.merged_logical_update_count == 3);
  assert(counters.merged_updates_saved == 2);
  assert(kvimr->kvunlink(three_name).ok());

  const std::string split_name = db + "/000004.sst";
  kvimr->ResetRMWCounters();
  StartFile(kvimr, split_name, 4, 0, 3);
  Write(kvimr, split_name, std::string(kTopBytes, 'x'));
  Write(kvimr, split_name, std::string(1, 'y'));
  assert(kvimr->kvsync(split_name).ok());
  counters = kvimr->GetRMWCounters();
  assert(counters.merged_rmw_count == 2);
  assert(counters.merged_logical_update_count == 2);
  assert(counters.merged_updates_saved == 0);

  std::string expected(kTopBytes + 1, 'x');
  expected[kTopBytes] = 'y';
  std::string restored(expected.size(), '\0');
  assert(kvimr->kvread(split_name, 0, restored.size(), &result,
                       &restored[0]).ok());
  assert(result.size() == restored.size());
  assert(restored == expected);

  // A dirty merged buffer must be flushed by kvunlink rather than discarded.
  Write(kvimr, split_name, std::string(1, 'z'));
  assert(kvimr->kvunlink(split_name).ok());

  // The already-synced merged payload survives a clean metadata reload.
  const std::string restart_name = db + "/000005.sst";
  kvimr->ResetRMWCounters();
  StartFile(kvimr, restart_name, 5, 0, 3);
  Write(kvimr, restart_name, std::string(1000, 'r'));
  assert(kvimr->kvsync(restart_name).ok());
  assert(kvimr->InitializePersistence(db, 1, device).ok());
  restored.assign(1000, '\0');
  assert(kvimr->kvread(restart_name, 0, restored.size(), &result,
                       &restored[0]).ok());
  assert(result.size() == restored.size());
  assert(restored == std::string(1000, 'r'));

  unlink((db + "/KVIMR-META").c_str());
  unlink(device.c_str());
  rmdir(db.c_str());
  return 0;
}
