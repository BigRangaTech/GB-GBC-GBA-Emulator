#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gbemu::common {

bool read_file(const std::string& path, std::vector<std::uint8_t>* out, std::string* error);

} // namespace gbemu::common
