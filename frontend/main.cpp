#include <iostream>

#include "core.h"

int main() {
  gbemu::core::EmulatorCore core;
  std::cout << "GBEmu skeleton v" << core.version() << "\n";
  return 0;
}
