#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gbemu::core::state_io {

inline void write_u8(std::vector<std::uint8_t>& out, std::uint8_t v) {
  out.push_back(v);
}

inline void write_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

inline void write_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
  }
}

inline void write_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
  }
}

inline void write_i64(std::vector<std::uint8_t>& out, std::int64_t v) {
  write_u64(out, static_cast<std::uint64_t>(v));
}

inline void write_bool(std::vector<std::uint8_t>& out, bool v) {
  out.push_back(v ? 1 : 0);
}

inline void write_f64(std::vector<std::uint8_t>& out, double v) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  write_u64(out, bits);
}

inline void write_bytes(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& data) {
  write_u32(out, static_cast<std::uint32_t>(data.size()));
  out.insert(out.end(), data.begin(), data.end());
}

inline void write_bytes_fixed(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t size) {
  out.insert(out.end(), data, data + size);
}

inline bool read_u8(const std::vector<std::uint8_t>& data, std::size_t& offset, std::uint8_t& out) {
  if (offset + 1 > data.size()) return false;
  out = data[offset++];
  return true;
}

inline bool read_u16(const std::vector<std::uint8_t>& data, std::size_t& offset, std::uint16_t& out) {
  if (offset + 2 > data.size()) return false;
  out = static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
  offset += 2;
  return true;
}

inline bool read_u32(const std::vector<std::uint8_t>& data, std::size_t& offset, std::uint32_t& out) {
  if (offset + 4 > data.size()) return false;
  out = 0;
  for (int i = 0; i < 4; ++i) {
    out |= static_cast<std::uint32_t>(data[offset + i]) << (i * 8);
  }
  offset += 4;
  return true;
}

inline bool read_u64(const std::vector<std::uint8_t>& data, std::size_t& offset, std::uint64_t& out) {
  if (offset + 8 > data.size()) return false;
  out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= static_cast<std::uint64_t>(data[offset + i]) << (i * 8);
  }
  offset += 8;
  return true;
}

inline bool read_i64(const std::vector<std::uint8_t>& data, std::size_t& offset, std::int64_t& out) {
  std::uint64_t tmp = 0;
  if (!read_u64(data, offset, tmp)) return false;
  out = static_cast<std::int64_t>(tmp);
  return true;
}

inline bool read_bool(const std::vector<std::uint8_t>& data, std::size_t& offset, bool& out) {
  std::uint8_t v = 0;
  if (!read_u8(data, offset, v)) return false;
  out = v != 0;
  return true;
}

inline bool read_f64(const std::vector<std::uint8_t>& data, std::size_t& offset, double& out) {
  std::uint64_t bits = 0;
  if (!read_u64(data, offset, bits)) return false;
  std::memcpy(&out, &bits, sizeof(out));
  return true;
}

inline bool read_bytes(const std::vector<std::uint8_t>& data, std::size_t& offset, std::vector<std::uint8_t>& out) {
  std::uint32_t size = 0;
  if (!read_u32(data, offset, size)) return false;
  if (offset + size > data.size()) return false;
  out.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
             data.begin() + static_cast<std::ptrdiff_t>(offset + size));
  offset += size;
  return true;
}

inline bool read_bytes_fixed(const std::vector<std::uint8_t>& data, std::size_t& offset,
                             std::uint8_t* out, std::size_t size) {
  if (offset + size > data.size()) return false;
  std::memcpy(out, data.data() + offset, size);
  offset += size;
  return true;
}

} // namespace gbemu::core::state_io
