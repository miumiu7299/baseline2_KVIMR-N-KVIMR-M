#include <cstdio>
#include <cstdlib>
#include <string>

#include "db/kvimr.h"
#include "db/kvimr_env.h"
#include "rocksdb/db.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s write|read db_path [device] [zones]\n", argv[0]);
    return 2;
  }
  const std::string mode(argv[1]);
  const std::string dbname(argv[2]);
  const std::string device = argc > 3 ? argv[3] : rocksdb::IMRSimBackend::kDefaultDevicePath;
  const uint32_t zones = argc > 4 ? static_cast<uint32_t>(std::strtoul(argv[4], 0, 10)) : 1;
  rocksdb::Status status = rocksdb::KVIMR::Get()->InitializePersistence(dbname, zones, device);
  if (!status.ok()) { std::fprintf(stderr, "KVIMR init failed: %s\n", status.ToString().c_str()); return 1; }
  rocksdb::Env* env = rocksdb::NewKVIMREnv(rocksdb::Env::Default());
  rocksdb::Options options; options.create_if_missing = mode == "write"; options.env = env;
  rocksdb::DB* db = 0;
  status = rocksdb::DB::Open(options, dbname, &db);
  if (!status.ok()) { std::fprintf(stderr, "open failed: %s\n", status.ToString().c_str()); delete env; return 1; }
  if (mode == "write") {
    status = db->Put(rocksdb::WriteOptions(), "restart-key", "restart-value");
    if (status.ok()) status = db->Flush(rocksdb::FlushOptions());
  } else {
    std::string value;
    status = db->Get(rocksdb::ReadOptions(), "restart-key", &value);
    if (status.ok() && value != "restart-value") status = rocksdb::Status::Corruption("unexpected restart value");
    if (status.ok()) std::printf("restart read: PASS\n");
  }
  delete db;
  delete env;
  if (!status.ok()) { std::fprintf(stderr, "%s failed: %s\n", mode.c_str(), status.ToString().c_str()); return 1; }
  return 0;
}
