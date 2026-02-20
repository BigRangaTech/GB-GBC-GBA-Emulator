#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace gbemu::common {

class Config {
 public:
  bool load_file(const std::string& path, std::string* error);
  const std::unordered_map<std::string, std::string>& values() const;

  bool has(const std::string& key) const;
  std::string get_string(const std::string& key, const std::string& default_value) const;
  std::optional<int> get_int(const std::string& key) const;
  std::optional<double> get_double(const std::string& key) const;

 private:
  static std::string normalize_key(const std::string& key);

  std::unordered_map<std::string, std::string> values_;
};

} // namespace gbemu::common
