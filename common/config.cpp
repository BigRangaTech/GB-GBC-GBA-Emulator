#include "config.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <string_view>

namespace gbemu::common {

namespace {

std::string trim(std::string_view input) {
  std::size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }
  std::size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }
  return std::string(input.substr(start, end - start));
}

} // namespace

bool Config::load_file(const std::string& path, std::string* error) {
  values_.clear();

  std::ifstream file(path);
  if (!file) {
    if (error) {
      *error = "Failed to open config file";
    }
    return false;
  }

  std::string line;
  std::size_t line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;

    std::size_t comment_pos = line.find_first_of("#;");
    if (comment_pos != std::string::npos) {
      line.erase(comment_pos);
    }

    std::string trimmed = trim(line);
    if (trimmed.empty()) {
      continue;
    }

    std::size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      continue;
    }

    std::string key = trim(std::string_view(trimmed).substr(0, eq));
    std::string value = trim(std::string_view(trimmed).substr(eq + 1));
    if (key.empty()) {
      continue;
    }

    values_[normalize_key(key)] = value;
  }

  return true;
}

bool Config::has(const std::string& key) const {
  return values_.find(normalize_key(key)) != values_.end();
}

std::string Config::get_string(const std::string& key, const std::string& default_value) const {
  auto it = values_.find(normalize_key(key));
  if (it == values_.end()) {
    return default_value;
  }
  return it->second;
}

std::optional<int> Config::get_int(const std::string& key) const {
  auto it = values_.find(normalize_key(key));
  if (it == values_.end()) {
    return std::nullopt;
  }

  int value = 0;
  auto* begin = it->second.data();
  auto* end = begin + it->second.size();
  auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end) {
    return std::nullopt;
  }

  return value;
}

std::optional<double> Config::get_double(const std::string& key) const {
  auto it = values_.find(normalize_key(key));
  if (it == values_.end()) {
    return std::nullopt;
  }

  try {
    std::size_t idx = 0;
    double value = std::stod(it->second, &idx);
    if (idx != it->second.size()) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

std::string Config::normalize_key(const std::string& key) {
  std::string normalized = key;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return normalized;
}

} // namespace gbemu::common
