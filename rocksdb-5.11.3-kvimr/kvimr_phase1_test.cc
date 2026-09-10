#include <iostream>
#include <string>
#include <vector>

#include "db/kvimr.h"
#include "db/kvimr_env.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"

int main() {
  const std::string dbname = "kvimr_phase1_db";
  rocksdb::Env* env = rocksdb::NewKVIMREnv(rocksdb::Env::Default());
  rocksdb::Options options;
  options.create_if_missing = true;
  options.env = env;
  options.write_buffer_size = 4 * 1024;
  options.target_file_size_base = 16 * 1024;

  rocksdb::DB* db = nullptr;
  rocksdb::Status s = rocksdb::DB::Open(options, dbname, &db);
  std::cerr << "opened=" << (s.ok() ? "true" : "false") << "\n";
  if (!s.ok()) {
    std::cerr << "open failed: " << s.ToString() << "\n";
    delete env;
    return 1;
  }
  for (int i = 0; i < 200 && s.ok(); ++i) {
    s = db->Put(rocksdb::WriteOptions(), "key" + std::to_string(i),
                std::string(256, 'v'));
  }
  if (s.ok()) s = db->Flush(rocksdb::FlushOptions());
  std::cerr << "flushed=" << (s.ok() ? "true" : "false") << "\n";
  if (!s.ok()) std::cerr << "write/flush status=" << s.ToString() << "\n";

  std::string value;
  rocksdb::Status read_status = db->Get(rocksdb::ReadOptions(), "key199", &value);
  std::cout << "read_success=" << (read_status.ok() ? "true" : "false") << "\n";
  std::cout << "kvimr_sstables=" << rocksdb::KVIMR::Get()->SSTableCount()
            << " s2t_entries=" << rocksdb::KVIMR::Get()->S2TMapSize() << "\n";

  std::vector<std::string> physical;
  rocksdb::Env::Default()->GetChildren(dbname, &physical);
  std::cout << "physical_files=";
  for (const auto& name : physical) std::cout << name << ",";
  std::cout << "\n";
  std::vector<std::string> visible;
  env->GetChildren(dbname, &visible);
  std::cout << "kvimr_visible_files=";
  for (const auto& name : visible) std::cout << name << ",";
  std::cout << "\n";

  db->CompactRange(nullptr, nullptr);
  std::cerr << "compacted\n";
  std::cout << "after_compaction_kvimr_sstables="
            << rocksdb::KVIMR::Get()->SSTableCount() << "\n";
  visible.clear();
  env->GetChildren(dbname, &visible);
  for (const auto& name : visible) {
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".sst") == 0) {
      env->DeleteFile(dbname + "/" + name);
      break;
    }
  }
  std::cout << "after_delete_kvimr_sstables="
            << rocksdb::KVIMR::Get()->SSTableCount() << "\n";
  delete db;
  delete env;
  return (s.ok() && read_status.ok()) ? 0 : 1;
}
