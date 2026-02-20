#include "rom.h"

#include <fstream>

namespace gbemu::common {

bool read_file(const std::string& path, std::vector<std::uint8_t>* out, std::string* error) {
  if (!out) {
    if (error) {
      *error = "Output buffer is null";
    }
    return false;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error) {
      *error = "Failed to open file";
    }
    return false;
  }

  file.seekg(0, std::ios::end);
  std::streampos size = file.tellg();
  if (size <= 0) {
    if (error) {
      *error = "File is empty";
    }
    return false;
  }

  out->resize(static_cast<std::size_t>(size));
  file.seekg(0, std::ios::beg);
  file.read(reinterpret_cast<char*>(out->data()), size);

  if (!file) {
    if (error) {
      *error = "Failed to read file";
    }
    return false;
  }

  return true;
}

} // namespace gbemu::common
