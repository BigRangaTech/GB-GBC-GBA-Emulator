#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "config.h"
#include "core.h"
#include "rom.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cout << "FAIL: " << message << "\n";
    ++failures;
  }
}

void test_read_file_missing() {
  std::vector<std::uint8_t> data;
  std::string error;
  bool ok = gbemu::common::read_file("/no/such/file.bin", &data, &error);
  expect(!ok, "read_file should fail for missing file");
  expect(!error.empty(), "read_file should return an error message");
}

void test_read_file_roundtrip() {
  std::filesystem::path path = std::filesystem::temp_directory_path() / "gbemu_test_rom.bin";
  {
    std::ofstream out(path, std::ios::binary);
    std::vector<std::uint8_t> bytes = {0x01, 0x02, 0x03, 0x04};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  std::vector<std::uint8_t> data;
  std::string error;
  bool ok = gbemu::common::read_file(path.string(), &data, &error);
  expect(ok, "read_file should succeed for valid file");
  expect(data.size() == 4, "read_file should read full file size");
  expect(data.size() >= 4 && data[0] == 0x01 && data[3] == 0x04, "read_file content should match");

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void test_config_parse() {
  std::filesystem::path path = std::filesystem::temp_directory_path() / "gbemu_test_config.conf";
  {
    std::ofstream out(path);
    out << "# Sample config\n";
    out << "system = gbc\n";
    out << "fps = 59.7275\n";
    out << "scale = 3\n";
    out << "video_driver = wayland\n";
  }

  gbemu::common::Config config;
  std::string error;
  bool ok = config.load_file(path.string(), &error);
  expect(ok, "config should load successfully");
  expect(config.get_string("system", "") == "gbc", "config should parse system");
  expect(config.get_int("scale").value_or(0) == 3, "config should parse scale");
  double fps = config.get_double("fps").value_or(0.0);
  expect(fps > 59.7 && fps < 59.8, "config should parse fps");
  expect(config.get_string("video_driver", "") == "wayland", "config should parse video_driver");

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void test_ppu_sizes() {
  gbemu::core::EmulatorCore core;
  core.set_system(gbemu::core::System::GB);
  expect(core.framebuffer_width() == 160 && core.framebuffer_height() == 144,
         "GB framebuffer should be 160x144");

  core.set_system(gbemu::core::System::GBC);
  expect(core.framebuffer_width() == 160 && core.framebuffer_height() == 144,
         "GBC framebuffer should be 160x144");

  core.set_system(gbemu::core::System::GBA);
  expect(core.framebuffer_width() == 240 && core.framebuffer_height() == 160,
         "GBA framebuffer should be 240x160");
}

} // namespace

int main() {
  test_read_file_missing();
  test_read_file_roundtrip();
  test_config_parse();
  test_ppu_sizes();

  if (failures == 0) {
    std::cout << "All tests passed.\n";
    return 0;
  }

  std::cout << failures << " test(s) failed.\n";
  return 1;
}
