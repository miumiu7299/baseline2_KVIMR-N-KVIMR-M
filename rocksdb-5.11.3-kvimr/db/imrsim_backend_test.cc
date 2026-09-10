#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "db/imrsim_backend.h"

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : rocksdb::IMRSimBackend::kDefaultDevicePath;
  const size_t top_length = 3648U * rocksdb::IMRSimBackend::kSectorSize;
  const size_t bottom_length = rocksdb::IMRSimBackend::kSectorSize;
  std::vector<char> written_storage(top_length + 1);
  std::vector<char> read_storage(top_length + 1);
  std::vector<char> bottom_written_storage(bottom_length + 1, 'b');
  std::vector<char> bottom_read_storage(bottom_length + 1);
  char* written = written_storage.data();
  char* read_back = read_storage.data();
  char* bottom_written = bottom_written_storage.data();
  char* bottom_read_back = bottom_read_storage.data();
  if ((reinterpret_cast<uintptr_t>(written) %
       rocksdb::IMRSimBackend::kSectorSize) == 0) {
    ++written;
  }
  if ((reinterpret_cast<uintptr_t>(read_back) %
       rocksdb::IMRSimBackend::kSectorSize) == 0) {
    ++read_back;
  }
  if ((reinterpret_cast<uintptr_t>(bottom_written) %
       rocksdb::IMRSimBackend::kSectorSize) == 0) {
    ++bottom_written;
  }
  if ((reinterpret_cast<uintptr_t>(bottom_read_back) %
       rocksdb::IMRSimBackend::kSectorSize) == 0) {
    ++bottom_read_back;
  }
  for (size_t i = 0; i < top_length; ++i)
    written[i] = static_cast<char>((i * 131 + 17) & 0xff);
  rocksdb::IMRSimBackend backend;
  rocksdb::Status s = backend.Open(path);
  if (s.ok()) s = backend.Write(0, top_length, written);
  if (s.ok()) s = backend.Sync();
  if (s.ok()) s = backend.Read(0, top_length, read_back);
  if (s.ok() && std::memcmp(written, read_back, top_length) != 0) {
    s = rocksdb::Status::Corruption("IMRSim TOP-track readback mismatch");
  }
  if (s.ok()) s = backend.Write(top_length, bottom_length, bottom_written);
  if (s.ok()) s = backend.Sync();
  if (s.ok()) {
    s = backend.Read(top_length, bottom_length, bottom_read_back);
  }
  const bool pass =
      s.ok() &&
      std::memcmp(bottom_written, bottom_read_back, bottom_length) == 0;
  std::printf("imrsim_backend_round_trip=%s\n", pass ? "PASS" : "FAIL");
  if (!s.ok()) std::printf("status=%s\n", s.ToString().c_str());
  return pass ? 0 : 1;
}
