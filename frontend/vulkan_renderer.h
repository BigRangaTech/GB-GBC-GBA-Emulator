#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;

namespace gbemu::frontend {

class VulkanRenderer {
 public:
  VulkanRenderer() = default;
  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  bool init(SDL_Window* window, int initial_width, int initial_height, std::string* error);
  void shutdown();

  bool draw_frame(const std::uint32_t* pixels, int width, int height, std::string* error);
  void notify_resize();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace gbemu::frontend
