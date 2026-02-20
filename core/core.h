#pragma once

#include <string>

namespace gbemu::core {

class EmulatorCore {
 public:
  std::string version() const;
};

} // namespace gbemu::core
