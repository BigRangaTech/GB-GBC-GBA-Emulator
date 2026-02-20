#pragma once

#include <string>
#include <unordered_map>

namespace gbemu::common {

enum class InputAction {
  A,
  B,
  Select,
  Start,
  Right,
  Left,
  Up,
  Down,
};

struct InputBinding {
  int keycode = 0;
};

class InputConfig {
 public:
  void set_default();
  bool load_from_config(const std::unordered_map<std::string, std::string>& values);
  bool resolve(InputAction action, int keycode) const;
  int key_for(InputAction action) const;

 private:
  std::unordered_map<InputAction, int> bindings_;
};

std::string action_name(InputAction action);

} // namespace gbemu::common
