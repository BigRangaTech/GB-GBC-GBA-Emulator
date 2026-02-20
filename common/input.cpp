#include "input.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ostream>
#include <utility>

namespace gbemu::common {

namespace {

constexpr int kButtonA = 0;
constexpr int kButtonB = 1;
constexpr int kButtonX = 2;
constexpr int kButtonY = 3;
constexpr int kButtonBack = 4;
constexpr int kButtonGuide = 5;
constexpr int kButtonStart = 6;
constexpr int kButtonLeftStick = 7;
constexpr int kButtonRightStick = 8;
constexpr int kButtonLeftShoulder = 9;
constexpr int kButtonRightShoulder = 10;
constexpr int kButtonDpadUp = 11;
constexpr int kButtonDpadDown = 12;
constexpr int kButtonDpadLeft = 13;
constexpr int kButtonDpadRight = 14;

constexpr int kAxisLeftX = 0;
constexpr int kAxisLeftY = 1;
constexpr int kAxisRightX = 2;
constexpr int kAxisRightY = 3;
constexpr int kAxisTriggerLeft = 4;
constexpr int kAxisTriggerRight = 5;

InputAction action_from_name(const std::string& name, bool* ok) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (lower == "a") { *ok = true; return InputAction::A; }
  if (lower == "b") { *ok = true; return InputAction::B; }
  if (lower == "select") { *ok = true; return InputAction::Select; }
  if (lower == "start") { *ok = true; return InputAction::Start; }
  if (lower == "right") { *ok = true; return InputAction::Right; }
  if (lower == "left") { *ok = true; return InputAction::Left; }
  if (lower == "up") { *ok = true; return InputAction::Up; }
  if (lower == "down") { *ok = true; return InputAction::Down; }

  *ok = false;
  return InputAction::A;
}

bool button_from_name(const std::string& name, int* out) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (lower == "a") { *out = kButtonA; return true; }
  if (lower == "b") { *out = kButtonB; return true; }
  if (lower == "x") { *out = kButtonX; return true; }
  if (lower == "y") { *out = kButtonY; return true; }
  if (lower == "back" || lower == "select") { *out = kButtonBack; return true; }
  if (lower == "guide") { *out = kButtonGuide; return true; }
  if (lower == "start") { *out = kButtonStart; return true; }
  if (lower == "leftstick") { *out = kButtonLeftStick; return true; }
  if (lower == "rightstick") { *out = kButtonRightStick; return true; }
  if (lower == "leftshoulder" || lower == "l1") { *out = kButtonLeftShoulder; return true; }
  if (lower == "rightshoulder" || lower == "r1") { *out = kButtonRightShoulder; return true; }
  if (lower == "dpad_up" || lower == "dpadup") { *out = kButtonDpadUp; return true; }
  if (lower == "dpad_down" || lower == "dpaddown") { *out = kButtonDpadDown; return true; }
  if (lower == "dpad_left" || lower == "dpadleft") { *out = kButtonDpadLeft; return true; }
  if (lower == "dpad_right" || lower == "dpadright") { *out = kButtonDpadRight; return true; }
  return false;
}

bool axis_from_name(const std::string& name, int* out) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (lower == "leftx") { *out = kAxisLeftX; return true; }
  if (lower == "lefty") { *out = kAxisLeftY; return true; }
  if (lower == "rightx") { *out = kAxisRightX; return true; }
  if (lower == "righty") { *out = kAxisRightY; return true; }
  if (lower == "triggerleft" || lower == "lt") { *out = kAxisTriggerLeft; return true; }
  if (lower == "triggerright" || lower == "rt") { *out = kAxisTriggerRight; return true; }
  return false;
}

std::string button_name(int button) {
  switch (button) {
    case kButtonA: return "a";
    case kButtonB: return "b";
    case kButtonX: return "x";
    case kButtonY: return "y";
    case kButtonBack: return "back";
    case kButtonGuide: return "guide";
    case kButtonStart: return "start";
    case kButtonLeftStick: return "leftstick";
    case kButtonRightStick: return "rightstick";
    case kButtonLeftShoulder: return "leftshoulder";
    case kButtonRightShoulder: return "rightshoulder";
    case kButtonDpadUp: return "dpad_up";
    case kButtonDpadDown: return "dpad_down";
    case kButtonDpadLeft: return "dpad_left";
    case kButtonDpadRight: return "dpad_right";
    default: return "";
  }
}

std::string axis_name(int axis) {
  switch (axis) {
    case kAxisLeftX: return "leftx";
    case kAxisLeftY: return "lefty";
    case kAxisRightX: return "rightx";
    case kAxisRightY: return "righty";
    case kAxisTriggerLeft: return "triggerleft";
    case kAxisTriggerRight: return "triggerright";
    default: return "";
  }
}

} // namespace

void InputConfig::set_default() {
  bindings_.clear();
  bindings_[InputAction::A] = 'z';
  bindings_[InputAction::B] = 'x';
  bindings_[InputAction::Select] = 8; // backspace
  bindings_[InputAction::Start] = 13; // enter
  bindings_[InputAction::Right] = 1073741903; // SDL keycode
  bindings_[InputAction::Left] = 1073741904;
  bindings_[InputAction::Up] = 1073741906;
  bindings_[InputAction::Down] = 1073741905;

  controller_buttons_.clear();
  controller_axis_pos_.clear();
  controller_axis_neg_.clear();
  axis_deadzone_ = 16000;

  controller_buttons_[kButtonA] = InputAction::A;
  controller_buttons_[kButtonB] = InputAction::B;
  controller_buttons_[kButtonBack] = InputAction::Select;
  controller_buttons_[kButtonStart] = InputAction::Start;
  controller_buttons_[kButtonDpadRight] = InputAction::Right;
  controller_buttons_[kButtonDpadLeft] = InputAction::Left;
  controller_buttons_[kButtonDpadUp] = InputAction::Up;
  controller_buttons_[kButtonDpadDown] = InputAction::Down;

  controller_axis_pos_[kAxisLeftX] = InputAction::Right;
  controller_axis_neg_[kAxisLeftX] = InputAction::Left;
  controller_axis_pos_[kAxisLeftY] = InputAction::Down;
  controller_axis_neg_[kAxisLeftY] = InputAction::Up;
}

bool InputConfig::load_from_config(const std::unordered_map<std::string, std::string>& values) {
  bool ok = true;
  for (const auto& kv : values) {
    const std::string& key = kv.first;
    if (key == "pad_axis_deadzone") {
      try {
        axis_deadzone_ = std::stoi(kv.second);
      } catch (...) {
        ok = false;
      }
      continue;
    }
    if (key.rfind("input.", 0) != 0 && key.rfind("pad.", 0) != 0) {
      continue;
    }
    std::string name = key.rfind("input.", 0) == 0 ? key.substr(6) : key.substr(4);
    bool action_ok = false;
    InputAction action = action_from_name(name, &action_ok);
    if (!action_ok) {
      ok = false;
      continue;
    }
    if (kv.second.empty()) {
      ok = false;
      continue;
    }

    if (key.rfind("pad.", 0) == 0) {
      for (auto it = controller_buttons_.begin(); it != controller_buttons_.end();) {
        if (it->second == action) {
          it = controller_buttons_.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = controller_axis_pos_.begin(); it != controller_axis_pos_.end();) {
        if (it->second == action) {
          it = controller_axis_pos_.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = controller_axis_neg_.begin(); it != controller_axis_neg_.end();) {
        if (it->second == action) {
          it = controller_axis_neg_.erase(it);
        } else {
          ++it;
        }
      }

      std::string value = kv.second;
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (value == "none") {
        continue;
      }
      if (value.rfind("button:", 0) == 0) {
        int button = 0;
        if (!button_from_name(value.substr(7), &button)) {
          ok = false;
          continue;
        }
        controller_buttons_[button] = action;
        continue;
      }
      if (value.rfind("axis:", 0) == 0) {
        std::string axis_token = value.substr(5);
        if (axis_token.empty()) {
          ok = false;
          continue;
        }
        char dir = axis_token.back();
        if (dir != '+' && dir != '-') {
          ok = false;
          continue;
        }
        axis_token.pop_back();
        int axis = 0;
        if (!axis_from_name(axis_token, &axis)) {
          ok = false;
          continue;
        }
        if (dir == '+') {
          controller_axis_pos_[axis] = action;
        } else {
          controller_axis_neg_[axis] = action;
        }
        continue;
      }
      int button = 0;
      if (button_from_name(value, &button)) {
        controller_buttons_[button] = action;
        continue;
      }
      ok = false;
      continue;
    }

    int code = 0;
    std::string value = kv.second;
    if (value.rfind("sdl:", 0) == 0) {
      auto number = value.substr(4);
      auto* begin = number.data();
      auto* end = begin + number.size();
      auto result = std::from_chars(begin, end, code);
      if (result.ec != std::errc() || result.ptr != end) {
        ok = false;
        continue;
      }
    } else {
      auto* begin = value.data();
      auto* end = begin + value.size();
      auto result = std::from_chars(begin, end, code);
      if (result.ec == std::errc() && result.ptr == end) {
        bindings_[action] = code;
        continue;
      }
      if (value == "\\b") {
        code = 8;
      } else if (value == "\\r") {
        code = 13;
      } else {
        code = static_cast<int>(value[0]);
      }
    }
    bindings_[action] = code;
  }
  return ok;
}

bool InputConfig::resolve(InputAction action, int keycode) const {
  auto it = bindings_.find(action);
  if (it == bindings_.end()) {
    return false;
  }
  return it->second == keycode;
}

int InputConfig::key_for(InputAction action) const {
  auto it = bindings_.find(action);
  if (it == bindings_.end()) {
    return 0;
  }
  return it->second;
}

std::optional<InputAction> InputConfig::action_for_controller_button(int button) const {
  auto it = controller_buttons_.find(button);
  if (it == controller_buttons_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<InputAction> InputConfig::action_for_controller_axis_pos(int axis) const {
  auto it = controller_axis_pos_.find(axis);
  if (it == controller_axis_pos_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<InputAction> InputConfig::action_for_controller_axis_neg(int axis) const {
  auto it = controller_axis_neg_.find(axis);
  if (it == controller_axis_neg_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<int> InputConfig::controller_button_for_action(InputAction action) const {
  for (const auto& entry : controller_buttons_) {
    if (entry.second == action) {
      return entry.first;
    }
  }
  return std::nullopt;
}

std::optional<std::pair<int, bool>> InputConfig::controller_axis_for_action(InputAction action) const {
  for (const auto& entry : controller_axis_pos_) {
    if (entry.second == action) {
      return std::make_pair(entry.first, true);
    }
  }
  for (const auto& entry : controller_axis_neg_) {
    if (entry.second == action) {
      return std::make_pair(entry.first, false);
    }
  }
  return std::nullopt;
}

int InputConfig::axis_deadzone() const {
  return axis_deadzone_;
}

void InputConfig::set_axis_deadzone(int value) {
  axis_deadzone_ = value;
}

void InputConfig::set_key_binding(InputAction action, int keycode) {
  bindings_[action] = keycode;
}

void InputConfig::set_controller_button_binding(InputAction action, int button) {
  for (auto it = controller_buttons_.begin(); it != controller_buttons_.end();) {
    if (it->second == action || it->first == button) {
      it = controller_buttons_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = controller_axis_pos_.begin(); it != controller_axis_pos_.end();) {
    if (it->second == action) {
      it = controller_axis_pos_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = controller_axis_neg_.begin(); it != controller_axis_neg_.end();) {
    if (it->second == action) {
      it = controller_axis_neg_.erase(it);
    } else {
      ++it;
    }
  }
  controller_buttons_[button] = action;
}

void InputConfig::set_controller_axis_binding(InputAction action, int axis, bool positive) {
  for (auto it = controller_buttons_.begin(); it != controller_buttons_.end();) {
    if (it->second == action) {
      it = controller_buttons_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = controller_axis_pos_.begin(); it != controller_axis_pos_.end();) {
    if (it->second == action || it->first == axis) {
      it = controller_axis_pos_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = controller_axis_neg_.begin(); it != controller_axis_neg_.end();) {
    if (it->second == action || it->first == axis) {
      it = controller_axis_neg_.erase(it);
    } else {
      ++it;
    }
  }
  if (positive) {
    controller_axis_pos_[axis] = action;
  } else {
    controller_axis_neg_[axis] = action;
  }
}

void InputConfig::clear_controller_binding(InputAction action) {
  for (auto it = controller_buttons_.begin(); it != controller_buttons_.end();) {
    if (it->second == action) {
      it = controller_buttons_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = controller_axis_pos_.begin(); it != controller_axis_pos_.end();) {
    if (it->second == action) {
      it = controller_axis_pos_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = controller_axis_neg_.begin(); it != controller_axis_neg_.end();) {
    if (it->second == action) {
      it = controller_axis_neg_.erase(it);
    } else {
      ++it;
    }
  }
}

void InputConfig::write_config(std::ostream& out) const {
  auto action_key = [](InputAction action) {
    switch (action) {
      case InputAction::A: return "a";
      case InputAction::B: return "b";
      case InputAction::Select: return "select";
      case InputAction::Start: return "start";
      case InputAction::Right: return "right";
      case InputAction::Left: return "left";
      case InputAction::Up: return "up";
      case InputAction::Down: return "down";
      default: return "a";
    }
  };
  const InputAction actions[] = {
      InputAction::A,
      InputAction::B,
      InputAction::Select,
      InputAction::Start,
      InputAction::Right,
      InputAction::Left,
      InputAction::Up,
      InputAction::Down,
  };
  out << "pad_axis_deadzone=" << axis_deadzone_ << "\n";
  for (InputAction action : actions) {
    out << "input." << action_key(action) << "=sdl:" << key_for(action) << "\n";
    std::string pad_value = "none";
    if (auto button = controller_button_for_action(action)) {
      std::string name = button_name(*button);
      if (!name.empty()) {
        pad_value = "button:" + name;
      }
    } else if (auto axis = controller_axis_for_action(action)) {
      std::string name = axis_name(axis->first);
      if (!name.empty()) {
        pad_value = "axis:" + name + (axis->second ? "+" : "-");
      }
    }
    out << "pad." << action_key(action) << "=" << pad_value << "\n";
  }
}

std::string action_name(InputAction action) {
  switch (action) {
    case InputAction::A: return "A";
    case InputAction::B: return "B";
    case InputAction::Select: return "Select";
    case InputAction::Start: return "Start";
    case InputAction::Right: return "Right";
    case InputAction::Left: return "Left";
    case InputAction::Up: return "Up";
    case InputAction::Down: return "Down";
    default: return "";
  }
}

} // namespace gbemu::common
