// Copyright (c) 2024, KVIMR contributors.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "db/kvimr_env.h"

#include <cstring>
#include <memory>

#include "db/kvimr.h"

namespace rocksdb {

namespace {
bool IsSSTable(const std::string& fname) {
  return fname.size() >= 4 && fname.compare(fname.size() - 4, 4, ".sst") == 0;
}

class KVIMRWritableFile : public WritableFile {
 public:
  explicit KVIMRWritableFile(const std::string& fname) : fname_(fname) {}
  Status Append(const Slice& data) override {
    return KVIMR::Get()->kvwrite(fname_, data);
  }
  Status Close() override { return KVIMR::Get()->kvsync(fname_); }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return KVIMR::Get()->kvsync(fname_); }
  uint64_t GetFileSize() override {
    uint64_t size = 0;
    KVIMR::Get()->GetSSTableSize(fname_, &size);
    return size;
  }

 private:
  std::string fname_;
};

class KVIMRRandomAccessFile : public RandomAccessFile {
 public:
  explicit KVIMRRandomAccessFile(const std::string& fname) : fname_(fname) {}
  Status Read(uint64_t offset, size_t n, Slice* result,
              char* scratch) const override {
    return KVIMR::Get()->kvread(fname_, offset, n, result, scratch);
  }

 private:
  std::string fname_;
};

class KVIMREnv : public EnvWrapper {
 public:
  explicit KVIMREnv(Env* base_env) : EnvWrapper(base_env) {}

  Status NewRandomAccessFile(const std::string& fname,
                             unique_ptr<RandomAccessFile>* result,
                             const EnvOptions& options) override {
    if (!IsSSTable(fname)) {
      return EnvWrapper::NewRandomAccessFile(fname, result, options);
    }
    if (!KVIMR::Get()->HasSSTable(fname)) {
      return Status::NotFound(fname);
    }
    result->reset(new KVIMRRandomAccessFile(fname));
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         unique_ptr<WritableFile>* result,
                         const EnvOptions& options) override {
    if (!IsSSTable(fname)) {
      return EnvWrapper::NewWritableFile(fname, result, options);
    }
    KVIMR::Get()->BeginWrite(fname);
    result->reset(new KVIMRWritableFile(fname));
    return Status::OK();
  }

  Status DeleteFile(const std::string& fname) override {
    if (!IsSSTable(fname)) {
      return EnvWrapper::DeleteFile(fname);
    }
    return KVIMR::Get()->kvunlink(fname);
  }

  Status FileExists(const std::string& fname) override {
    if (!IsSSTable(fname)) {
      return EnvWrapper::FileExists(fname);
    }
    return KVIMR::Get()->HasSSTable(fname) ? Status::OK()
                                           : Status::NotFound(fname);
  }

  Status GetChildren(const std::string& dir,
                     std::vector<std::string>* result) override {
    Status s = EnvWrapper::GetChildren(dir, result);
    if (!s.ok()) {
      return s;
    }
    std::vector<std::string> virtual_files;
    KVIMR::Get()->ListSSTables(dir, &virtual_files);
    result->insert(result->end(), virtual_files.begin(), virtual_files.end());
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    if (!IsSSTable(fname)) {
      return EnvWrapper::GetFileSize(fname, size);
    }
    return KVIMR::Get()->GetSSTableSize(fname, size);
  }
};
}  // namespace

Env* NewKVIMREnv(Env* base_env) { return new KVIMREnv(base_env); }

}  // namespace rocksdb
