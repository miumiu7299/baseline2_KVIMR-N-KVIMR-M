#include <chrono>
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

bool CheckStatus(const rocksdb::Status& status, const char* operation) {
  if (status.ok()) return true;
  std::fprintf(stderr, "%s failed: %s\n", operation,
               status.ToString().c_str());
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5 && argc != 6) {
    std::fprintf(stderr,
                 "usage: %s <db_path> <imrsim_device> <zone_count> <num_keys> [--rmw_mode=naive|merged]\n",
                 argv[0]);
    return 2;
  }

  rocksdb::KVIMR::RMWMode mode = rocksdb::KVIMR::RMWMode::NAIVE;
  const char* mode_name = "naive";
  if (argc == 6) {
    if (std::string(argv[5]) == "--rmw_mode=merged") {
      mode = rocksdb::KVIMR::RMWMode::MERGED;
      mode_name = "merged";
    } else if (std::string(argv[5]) != "--rmw_mode=naive") {
      std::fprintf(stderr, "mode must be --rmw_mode=naive or --rmw_mode=merged\n");
      return 2;
    }
  }

  uint64_t zones = 0;
  uint64_t num_keys = 0;
  if (!ParseUnsigned(argv[3], &zones) || zones == 0 || zones > 0xffffffffULL ||
      !ParseUnsigned(argv[4], &num_keys) || num_keys == 0) {
    std::fprintf(stderr, "zone_count and num_keys must be positive integers\n");
    return 2;
  }

  rocksdb::KVIMR* kvimr = rocksdb::KVIMR::Get();
  kvimr->SetRMWMode(mode);
  rocksdb::Status status = kvimr->InitializePersistence(
      argv[1], static_cast<uint32_t>(zones), argv[2]);
  if (!CheckStatus(status, "KVIMR persistence initialization")) return 1;
  kvimr->ResetRMWCounters();

  rocksdb::Env* env = rocksdb::NewKVIMREnv(rocksdb::Env::Default());
  rocksdb::Options options;
  options.create_if_missing = true;
  options.env = env;
  options.write_buffer_size = 1 << 20;
  options.max_write_buffer_number = 3;
  options.target_file_size_base = 1 << 20;
  options.compression = rocksdb::kNoCompression;

  rocksdb::DB* db = 0;
  status = rocksdb::DB::Open(options, argv[1], &db);
  if (!CheckStatus(status, "DB::Open")) {
    delete env;
    return 1;
  }

  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < num_keys; ++i) {
    status = db->Put(rocksdb::WriteOptions(), Key(i), Value(i));
    if (!CheckStatus(status, "Put")) {
      delete db;
      delete env;
      return 1;
    }
  }

  status = db->Flush(rocksdb::FlushOptions());
  if (!CheckStatus(status, "Flush")) {
    delete db;
    delete env;
    return 1;
  }

  status = db->CompactRange(nullptr, nullptr);
  if (!CheckStatus(status, "CompactRange")) {
    delete db;
    delete env;
    return 1;
  }

  uint64_t verified = 0;
  uint64_t sample_step = num_keys / 100;
  if (sample_step == 0) sample_step = 1;
  for (uint64_t i = 0; i < num_keys; i += sample_step) {
    std::string value;
    status = db->Get(rocksdb::ReadOptions(), Key(i), &value);
    if (!CheckStatus(status, "Get")) {
      delete db;
      delete env;
      return 1;
    }
    if (value != Value(i)) {
      std::fprintf(stderr, "readback mismatch for key %llu\n",
                   static_cast<unsigned long long>(i));
      delete db;
      delete env;
      return 1;
    }
    ++verified;
  }
  if ((num_keys - 1) % sample_step != 0) {
    std::string value;
    status = db->Get(rocksdb::ReadOptions(), Key(num_keys - 1), &value);
    if (!CheckStatus(status, "Get last key")) {
      delete db;
      delete env;
      return 1;
    }
    if (value != Value(num_keys - 1)) {
      std::fprintf(stderr, "readback mismatch for last key\n");
      delete db;
      delete env;
      return 1;
    }
    ++verified;
  }

  const std::chrono::steady_clock::time_point finish =
      std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration_cast<std::chrono::duration<double> >(
      finish - start).count();
  const double throughput = elapsed > 0.0 ? num_keys / elapsed : 0.0;
  const rocksdb::KVIMR::RMWCounters counters = kvimr->GetRMWCounters();
  const uint64_t merged_top_track_update_count =
      counters.merged_rmw_count + counters.merged_updates_saved;
  const double merged_rmw_reduction_ratio =
      merged_top_track_update_count == 0
          ? 0.0
          : static_cast<double>(counters.merged_updates_saved) /
                static_cast<double>(merged_top_track_update_count);
  size_t top_segments = 0;
  size_t bottom_segments = 0;
  kvimr->GetTrackSegmentCounts(&top_segments, &bottom_segments);

  std::printf("inserted_keys=%llu\n", static_cast<unsigned long long>(num_keys));
  std::printf("rmw_mode=%s\n", mode_name);
  std::printf("verified_keys=%llu\n", static_cast<unsigned long long>(verified));
  std::printf("kvimr_sstables=%lu\n",
              static_cast<unsigned long>(kvimr->SSTableCount()));
  std::printf("s2t_entries=%lu\n",
              static_cast<unsigned long>(kvimr->S2TMapSize()));
  std::printf("top_segments=%lu\n", static_cast<unsigned long>(top_segments));
  std::printf("bottom_segments=%lu\n",
              static_cast<unsigned long>(bottom_segments));
  std::printf("naive_rmw_count=%llu\n",
              static_cast<unsigned long long>(counters.naive_rmw_count));
  std::printf("naive_rmw_read_bytes=%llu\n",
              static_cast<unsigned long long>(counters.naive_rmw_read_bytes));
  std::printf("naive_rmw_write_bytes=%llu\n",
              static_cast<unsigned long long>(counters.naive_rmw_write_bytes));
  std::printf("direct_write_bytes=%llu\n",
              static_cast<unsigned long long>(counters.direct_write_bytes));
  std::printf("merged_rmw_count=%llu\n",
              static_cast<unsigned long long>(counters.merged_rmw_count));
  std::printf("merged_logical_update_count=%llu\n",
              static_cast<unsigned long long>(counters.merged_logical_update_count));
  std::printf("merged_updates_saved=%llu\n",
              static_cast<unsigned long long>(counters.merged_updates_saved));
  std::printf("merged_rmw_read_bytes=%llu\n",
              static_cast<unsigned long long>(counters.merged_rmw_read_bytes));
  std::printf("merged_rmw_write_bytes=%llu\n",
              static_cast<unsigned long long>(counters.merged_rmw_write_bytes));
  std::printf("merged_top_track_update_count=%llu\n",
              static_cast<unsigned long long>(merged_top_track_update_count));
  std::printf("merged_rmw_reduction_count=%llu\n",
              static_cast<unsigned long long>(counters.merged_updates_saved));
  std::printf("merged_rmw_reduction_ratio=%.6f\n",
              merged_rmw_reduction_ratio);
  for (int level = 0; level < options.num_levels; ++level) {
    std::string property;
    char property_name[64];
    std::snprintf(property_name, sizeof(property_name),
                  "rocksdb.num-files-at-level%d", level);
    if (db->GetProperty(property_name, &property)) {
      std::printf("level_files_L%d=%s\n", level, property.c_str());
    }
  }
  std::printf("elapsed_seconds=%.6f\n", elapsed);
  std::printf("throughput_ops_per_sec=%.3f\n", throughput);

  delete db;
  delete env;
  const uint64_t expected_rmw_count =
      mode == rocksdb::KVIMR::RMWMode::MERGED
          ? counters.merged_rmw_count
          : counters.naive_rmw_count;
  if (verified == 0 || expected_rmw_count == 0 ||
      counters.direct_write_bytes == 0) {
    std::fprintf(stderr,
                 "workload did not exercise the selected RMW mode and direct writes\n");
    return 1;
  }
  return 0;
}
