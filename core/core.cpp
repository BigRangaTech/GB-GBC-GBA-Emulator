#include "core.h"

#include "common.h"

namespace gbemu::core {

std::string EmulatorCore::version() const {
  return gbemu::common::version();
}

} // namespace gbemu::core
