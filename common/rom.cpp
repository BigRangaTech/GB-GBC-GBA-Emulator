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

bool write_file(const std::string& path, const std::vector<std::uint8_t>& data, std::string* error) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    if (error) {
      *error = "Failed to open file for writing";
    }
    return false;
  }
  if (!data.empty()) {
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    if (!file) {
      if (error) {
        *error = "Failed to write file";
      }
      return false;
    }
  }
  return true;
}

} // namespace gbemu::common
