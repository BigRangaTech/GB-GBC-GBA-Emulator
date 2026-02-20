#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gbemu::common {

bool read_file(const std::string& path, std::vector<std::uint8_t>* out, std::string* error);
bool write_file(const std::string& path, const std::vector<std::uint8_t>& data, std::string* error);

} // namespace gbemu::common
