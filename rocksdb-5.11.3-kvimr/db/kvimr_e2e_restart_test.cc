#include <cstdio>
#include <cstdlib>
#include <string>

#include "db/kvimr.h"
#include "db/kvimr_env.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"

namespace {

bool ParseUnsigned(const char* text, uint64_t* value) {
  char* end = 0;
  unsigned long long parsed = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0') return false;
  *value = static_cast<uint64_t>(parsed);
  return true;
}

std::string Key(uint64_t number) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "key%08llu",
                static_cast<unsigned long long>(number));
  return std::string(buffer);
}

std::string Value(uint64_t number) {
  std::string value(4096, 'v');
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "value%011llu",
                static_cast<unsigned long long>(number));
  value.replace(0, 16, buffer);
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr,
                 "usage: %s <db_path> <imrsim_device> <zone_count> <num_keys>\n",
                 argv[0]);
    return 2;
  }

  uint64_t zones = 0;
  uint64_t num_keys = 0;
  if (!ParseUnsigned(argv[3], &zones) || zones == 0 || zones > 0xffffffffULL ||
      !ParseUnsigned(argv[4], &num_keys) || num_keys == 0) {
    std::fprintf(stderr, "zone_count and num_keys must be positive integers\n");
    return 2;
  }

  rocksdb::KVIMR* kvimr = rocksdb::KVIMR::Get();
  rocksdb::Status status = kvimr->InitializePersistence(
      argv[1], static_cast<uint32_t>(zones), argv[2]);
  if (!status.ok()) {
    std::fprintf(stderr, "KVIMR persistence initialization failed: %s\n",
                 status.ToString().c_str());
    return 1;
  }

  rocksdb::Env* env = rocksdb::NewKVIMREnv(rocksdb::Env::Default());
  rocksdb::Options options;
  options.create_if_missing = false;
  options.env = env;
  rocksdb::DB* db = 0;
  status = rocksdb::DB::Open(options, argv[1], &db);
  if (!status.ok()) {
    std::fprintf(stderr, "DB::Open failed: %s\n", status.ToString().c_str());
    delete env;
    return 1;
  }

  uint64_t verified = 0;
  uint64_t failed = 0;
  for (uint64_t i = 0; i < num_keys; ++i) {
    std::string value;
    status = db->Get(rocksdb::ReadOptions(), Key(i), &value);
    if (!status.ok() || value != Value(i)) {
      ++failed;
    } else {
      ++verified;
    }
  }

  size_t top_segments = 0;
  size_t bottom_segments = 0;
  kvimr->GetTrackSegmentCounts(&top_segments, &bottom_segments);

  std::printf("restart_validation_pass=%s\n",
              failed == 0 && verified == num_keys ? "true" : "false");
  std::printf("verified_keys=%llu\n",
              static_cast<unsigned long long>(verified));
  std::printf("failed_reads=%llu\n",
              static_cast<unsigned long long>(failed));
  std::printf("kvimr_sstables=%lu\n",
              static_cast<unsigned long>(kvimr->SSTableCount()));
  std::printf("s2t_entries=%lu\n",
              static_cast<unsigned long>(kvimr->S2TMapSize()));
  std::printf("top_segments=%lu\n", static_cast<unsigned long>(top_segments));
  std::printf("bottom_segments=%lu\n",
              static_cast<unsigned long>(bottom_segments));

  delete db;
  delete env;
  return failed == 0 && verified == num_keys ? 0 : 1;
}
