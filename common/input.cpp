#include "input.h"

#include <algorithm>
#include <cctype>

namespace gbemu::common {

namespace {

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
}

bool InputConfig::load_from_config(const std::unordered_map<std::string, std::string>& values) {
  bool ok = true;
  for (const auto& kv : values) {
    const std::string& key = kv.first;
    if (key.rfind("input.", 0) != 0) {
      continue;
    }
    std::string name = key.substr(6);
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
    int code = 0;
    if (kv.second.rfind("sdl:", 0) == 0) {
      try {
        code = std::stoi(kv.second.substr(4));
      } catch (...) {
        ok = false;
        continue;
      }
    } else if (kv.second == "\\b") {
      code = 8;
    } else if (kv.second == "\\r") {
      code = 13;
    } else {
      code = static_cast<int>(kv.second[0]);
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
