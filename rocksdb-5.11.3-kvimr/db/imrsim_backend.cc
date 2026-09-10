#include "db/imrsim_backend.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <limits>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rocksdb {

const char* const IMRSimBackend::kDefaultDevicePath = "/dev/mapper/imrsim";

namespace {
const size_t kBounceBufferAlignment = 4096;

Status ErrorStatus(const char* operation, const std::string& path,
                   int error_number) {
  return Status::IOError(
      operation, path + ": " + std::strerror(error_number));
}

Status ErrnoStatus(const char* operation, const std::string& path) {
  return ErrorStatus(operation, path, errno);
}

class AlignedBuffer {
 public:
  AlignedBuffer() : data_(nullptr) {}
  ~AlignedBuffer() { std::free(data_); }

  int Allocate(size_t alignment, size_t size) {
    return posix_memalign(&data_, alignment, size);
  }

  void* data() { return data_; }

 private:
  void* data_;
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
};

bool IsPowerOfTwo(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}
}

IMRSimBackend::IMRSimBackend()
    : fd_(-1), direct_io_alignment_(kSectorSize) {}
IMRSimBackend::~IMRSimBackend() { Close(); }

Status IMRSimBackend::Open(const std::string& device_path) {
  if (fd_ >= 0) return Status::IOError("IMRSim backend is already open");
  int fd = ::open(device_path.c_str(), O_RDWR | O_DIRECT);
  if (fd < 0) return ErrnoStatus("open IMRSim device", device_path);

  size_t direct_io_alignment = kSectorSize;
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    const int error_number = errno;
    ::close(fd);
    return ErrorStatus("stat IMRSim device", device_path, error_number);
  }
  if (S_ISBLK(st.st_mode)) {
    // Do not round direct I/O up to this value: doing so could overwrite an
    // adjacent IMR track. Validate the exact range requested by KVIMR instead.
    int logical_block_size = 0;
    if (::ioctl(fd, BLKSSZGET, &logical_block_size) != 0) {
      const int error_number = errno;
      ::close(fd);
      return ErrorStatus("query IMRSim device logical block size", device_path,
                         error_number);
    }
    if (logical_block_size < static_cast<int>(kSectorSize) ||
        !IsPowerOfTwo(static_cast<uint64_t>(logical_block_size))) {
      ::close(fd);
      return Status::InvalidArgument(
          "invalid IMRSim device logical block size", device_path);
    }
    direct_io_alignment = static_cast<size_t>(logical_block_size);
  }

  fd_ = fd;
  direct_io_alignment_ = direct_io_alignment;
  device_path_ = device_path;
  return Status::OK();
}

Status IMRSimBackend::Close() {
  if (fd_ < 0) return Status::OK();
  const int fd = fd_;
  fd_ = -1;
  direct_io_alignment_ = kSectorSize;
  if (::close(fd) != 0) return ErrnoStatus("close IMRSim device", device_path_);
  return Status::OK();
}

Status IMRSimBackend::ValidateRange(uint64_t offset, size_t length) const {
  if (fd_ < 0) return Status::IOError("IMRSim backend is not open");
  if ((offset % kSectorSize) != 0 ||
      (static_cast<uint64_t>(length) % kSectorSize) != 0) {
    return Status::InvalidArgument("IMRSim I/O must be 512-byte aligned");
  }
  if ((offset % direct_io_alignment_) != 0 ||
      (static_cast<uint64_t>(length) % direct_io_alignment_) != 0) {
    return Status::InvalidArgument(
        "IMRSim O_DIRECT I/O does not meet device alignment",
        device_path_ + ": required alignment is " +
            std::to_string(direct_io_alignment_) + " bytes");
  }
  if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
      static_cast<uint64_t>(length) >
          static_cast<uint64_t>(std::numeric_limits<ssize_t>::max()) ||
      offset + static_cast<uint64_t>(length) < offset) {
    return Status::InvalidArgument("IMRSim I/O range is too large");
  }
  return Status::OK();
}

Status IMRSimBackend::Read(uint64_t offset, size_t length, void* buffer) const {
  Status s = ValidateRange(offset, length);
  if (!s.ok()) return s;
  if (length == 0) return Status::OK();
  if (buffer == nullptr) {
    return Status::InvalidArgument("read IMRSim device with null buffer",
                                   device_path_);
  }

  AlignedBuffer aligned_buffer;
  // Callers use std::vector<char> and std::string storage, neither of which
  // guarantees the memory alignment required by O_DIRECT.
  const size_t buffer_alignment =
      std::max(kBounceBufferAlignment, direct_io_alignment_);
  const int allocation_error =
      aligned_buffer.Allocate(buffer_alignment, length);
  if (allocation_error != 0) {
    return ErrorStatus("allocate aligned IMRSim read buffer", device_path_,
                       allocation_error);
  }

  char* out = static_cast<char*>(buffer);
  size_t done = 0;
  while (done < length) {
    ssize_t n = ::pread(fd_, aligned_buffer.data(), length - done,
                        static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus("read IMRSim device", device_path_);
    }
    if (n == 0) {
      return Status::IOError("short read from IMRSim device", device_path_);
    }
    std::memcpy(out + done, aligned_buffer.data(), static_cast<size_t>(n));
    done += static_cast<size_t>(n);
  }
  return Status::OK();
}

Status IMRSimBackend::Write(uint64_t offset, size_t length, const void* buffer) {
  Status s = ValidateRange(offset, length);
  if (!s.ok()) return s;
  if (length == 0) return Status::OK();
  if (buffer == nullptr) {
    return Status::InvalidArgument("write IMRSim device with null buffer",
                                   device_path_);
  }

  AlignedBuffer aligned_buffer;
  // Always bounce, including already-aligned callers, so all backend paths
  // have the same direct-I/O behavior.
  const size_t buffer_alignment =
      std::max(kBounceBufferAlignment, direct_io_alignment_);
  const int allocation_error =
      aligned_buffer.Allocate(buffer_alignment, length);
  if (allocation_error != 0) {
    return ErrorStatus("allocate aligned IMRSim write buffer", device_path_,
                       allocation_error);
  }

  const char* input = static_cast<const char*>(buffer);
  size_t done = 0;
  while (done < length) {
    std::memcpy(aligned_buffer.data(), input + done, length - done);
    ssize_t n = ::pwrite(fd_, aligned_buffer.data(), length - done,
                         static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus("write IMRSim device", device_path_);
    }
    if (n == 0) {
      return Status::IOError("short write to IMRSim device", device_path_);
    }
    done += static_cast<size_t>(n);
  }
  return Status::OK();
}

Status IMRSimBackend::Sync() {
  if (fd_ < 0) return Status::IOError("IMRSim backend is not open");
  if (::fdatasync(fd_) != 0) return ErrnoStatus("sync IMRSim device", device_path_);
  return Status::OK();
}

Status IMRSimBackend::TrackOffset(uint64_t track_index, uint64_t* offset) {
  if (track_index > std::numeric_limits<uint64_t>::max() / kTrackSize)
    return Status::InvalidArgument("IMRSim track index overflows byte offset");
  *offset = track_index * kTrackSize;
  return Status::OK();
}

Status IMRSimBackend::ReadTrack(uint64_t track_index, void* buffer) const {
  uint64_t offset = 0;
  Status s = TrackOffset(track_index, &offset);
  return s.ok() ? Read(offset, kTrackSize, buffer) : s;
}

Status IMRSimBackend::WriteTrack(uint64_t track_index, const void* buffer) {
  uint64_t offset = 0;
  Status s = TrackOffset(track_index, &offset);
  return s.ok() ? Write(offset, kTrackSize, buffer) : s;
}

}  // namespace rocksdb
