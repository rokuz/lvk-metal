#pragma once

#if !defined(IMGUI_DEFINE_MATH_OPERATORS)
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <imgui.h>

#include <lvk/LVK.h>

#include <vector>

struct GLFWwindow;

namespace lvk::metal {

class ImGuiRenderer {
 public:
  explicit ImGuiRenderer(lvk::IContext& ctx, GLFWwindow* window, const char* defaultFontTTF = nullptr, float fontSizePixels = 24.0f);
  explicit ImGuiRenderer(lvk::IContext& ctx,
                         GLFWwindow* window,
                         const void* fontData,
                         size_t fontDataSize,
                         float fontSizePixels = 24.0f);
  ~ImGuiRenderer();

  ImGuiRenderer(const ImGuiRenderer&) = delete;
  ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

  void updateFont(const char* defaultFontTTF, const void* fontData, size_t fontDataSize, float fontSizePixels);

  void beginFrame(const lvk::Framebuffer& desc);
  void endFrame(lvk::ICommandBuffer& cmd);

 private:
  lvk::Holder<lvk::RenderPipelineHandle> createNewPipelineState(const lvk::Framebuffer& desc);

  lvk::IContext& ctx_;
  GLFWwindow* window_ = nullptr;
  lvk::Holder<lvk::ShaderModuleHandle> vert_;
  lvk::Holder<lvk::ShaderModuleHandle> frag_;
  lvk::Holder<lvk::RenderPipelineHandle> pipeline_;
  lvk::Format pipelineColorFormat_ = lvk::Format_Invalid;
  lvk::Holder<lvk::SamplerHandle> samplerClamp_;
  std::vector<lvk::Holder<lvk::TextureHandle>> textures_;

  uint32_t frameIndex_ = 0;

  struct DrawableData {
    lvk::Holder<lvk::BufferHandle> vb_;
    lvk::Holder<lvk::BufferHandle> ib_;
    uint32_t numAllocatedIndices_ = 0;
    uint32_t numAllocatedVertices_ = 0;
  };

  DrawableData drawables_[3] = {};
};

} // namespace lvk::metal
