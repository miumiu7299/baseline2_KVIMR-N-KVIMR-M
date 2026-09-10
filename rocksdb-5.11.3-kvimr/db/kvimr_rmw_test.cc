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

void PutAndSync(KVIMR* kvimr, const std::string& name, uint64_t number,
                int level, int largest, const std::string& value) {
  kvimr->RegisterSSTable(name, number, level, largest);
  kvimr->BeginWrite(name);
  assert(kvimr->kvwrite(name, Slice(value)).ok());
  assert(kvimr->kvsync(name).ok());
}
}

int main() {
  const std::string db = "/tmp/kvimr-rmw-test";
  const std::string device = db + ".device";
  mkdir(db.c_str(), 0755);
  int fd = open(device.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  assert(fd >= 0);
  assert(ftruncate(fd, 256 * 1024 * 1024) == 0);
  close(fd);

  KVIMR* kvimr = KVIMR::Get();
  assert(kvimr->InitializePersistence(db, 1, device).ok());

  const std::string bottom_name = db + "/000001.sst";
  kvimr->ResetRMWCounters();
  PutAndSync(kvimr, bottom_name, 1, 3, 3, std::string(1000, 'b'));
  KVIMR::RMWCounters counters = kvimr->GetRMWCounters();
  assert(counters.naive_rmw_count == 0);
  assert(counters.naive_rmw_read_bytes == 0);
  assert(counters.naive_rmw_write_bytes == 0);
  assert(counters.direct_write_bytes == 1024);
  assert(kvimr->kvunlink(bottom_name).ok());

  const std::string top_name = db + "/000002.sst";
  kvimr->ResetRMWCounters();
  PutAndSync(kvimr, top_name, 2, 0, 3, std::string(1000, 't'));
  counters = kvimr->GetRMWCounters();
  assert(counters.naive_rmw_count == 1);
  assert(counters.naive_rmw_read_bytes == kTopBytes);
  assert(counters.naive_rmw_write_bytes == kTopBytes);
  assert(counters.direct_write_bytes == 0);
  char scratch[1000]; Slice result;
  assert(kvimr->kvread(top_name, 0, sizeof(scratch), &result, scratch).ok());
  assert(result.size() == sizeof(scratch));
  assert(result[0] == 't' && result[999] == 't');
  assert(kvimr->kvunlink(top_name).ok());

  const std::string crossing_name = db + "/000003.sst";
  kvimr->ResetRMWCounters();
  PutAndSync(kvimr, crossing_name, 3, 0, 3,
             std::string(kTopBytes + 1, 'x'));
  counters = kvimr->GetRMWCounters();
  assert(counters.naive_rmw_count == 2);
  assert(counters.naive_rmw_read_bytes == 2 * kTopBytes);
  assert(counters.naive_rmw_write_bytes == 2 * kTopBytes);
  assert(counters.direct_write_bytes == 0);
  std::string restored(kTopBytes + 1, '\0');
  assert(kvimr->kvread(crossing_name, 0, restored.size(), &result,
                       &restored[0]).ok());
  assert(result.size() == restored.size());
  assert(restored[0] == 'x' && restored[kTopBytes] == 'x');
  assert(kvimr->kvunlink(crossing_name).ok());

  unlink((db + "/KVIMR-META").c_str());
  unlink(device.c_str());
  rmdir(db.c_str());
  return 0;
}
