#pragma once

#include <optional>
#include <ostream>
#include <string>
#include <utility>
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
  std::optional<InputAction> action_for_controller_button(int button) const;
  std::optional<InputAction> action_for_controller_axis_pos(int axis) const;
  std::optional<InputAction> action_for_controller_axis_neg(int axis) const;
  std::optional<int> controller_button_for_action(InputAction action) const;
  std::optional<std::pair<int, bool>> controller_axis_for_action(InputAction action) const;
  int axis_deadzone() const;
  void set_axis_deadzone(int value);
  void set_key_binding(InputAction action, int keycode);
  void set_controller_button_binding(InputAction action, int button);
  void set_controller_axis_binding(InputAction action, int axis, bool positive);
  void clear_controller_binding(InputAction action);
  void write_config(std::ostream& out) const;

 private:
  std::unordered_map<InputAction, int> bindings_;
  std::unordered_map<int, InputAction> controller_buttons_;
  std::unordered_map<int, InputAction> controller_axis_pos_;
  std::unordered_map<int, InputAction> controller_axis_neg_;
  int axis_deadzone_ = 16000;
};

std::string action_name(InputAction action);

} // namespace gbemu::common
