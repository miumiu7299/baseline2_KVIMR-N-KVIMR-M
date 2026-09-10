#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "rocksdb/status.h"

namespace rocksdb {

class IMRSimBackend {
 public:
  static const uint64_t kSectorSize = 512;
  static const uint64_t kTrackSize = 2ULL * 1024 * 1024;
  static const char* const kDefaultDevicePath;

  IMRSimBackend();
  ~IMRSimBackend();
  Status Open(const std::string& device_path = kDefaultDevicePath);
  Status Close();
  bool IsOpen() const { return fd_ >= 0; }
  Status Read(uint64_t offset, size_t length, void* buffer) const;
  Status Write(uint64_t offset, size_t length, const void* buffer);
  Status Sync();
  Status ReadTrack(uint64_t track_index, void* buffer) const;
  Status WriteTrack(uint64_t track_index, const void* buffer);
  static Status TrackOffset(uint64_t track_index, uint64_t* offset);

 private:
  Status ValidateRange(uint64_t offset, size_t length) const;
  int fd_;
  size_t direct_io_alignment_;
  std::string device_path_;
};

}  // namespace rocksdb
