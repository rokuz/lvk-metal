#include <cstdint>
#include <memory>

#include <lvk/HelpersImGuiMetal.h>
#include <lvk/LVK-Metal.h>

#include "app_common.h"

class ImGuiDemo final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow* window, uint32_t width, uint32_t height, float displayScale) override {
    width_ = width;
    height_ = height;
    imgui_ = std::make_unique<lvk::metal::ImGuiRenderer>(ctx, window);
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float time) override {
    const lvk::Framebuffer fb = {.color = {{.texture = target}}};

    imgui_->beginFrame(fb);

    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(float(width_) - 80, float(height_) - 80), ImGuiCond_Always);
    ImGui::Begin("lvk-metal + Dear ImGui");
    ImGui::Text("Hello from Metal 4!");
    ImGui::Separator();
    ImGui::Text("Bindless heaps, buffer-pointer vertices,");
    ImGui::Text("alpha blending, per-draw scissor.");
    ImGui::Spacing();
    ImGui::Button("A button");
    ImGui::ProgressBar(0.6f);
    ImGui::End();

    cmd.cmdBeginRendering(
        {.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0.10f, 0.12f, 0.15f, 1.0f}}}}, fb);
    imgui_->endFrame(cmd);
    cmd.cmdEndRendering();
  }

  void destroy() override {
    imgui_.reset();
  }

 private:
  std::unique_ptr<lvk::metal::ImGuiRenderer> imgui_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(ImGuiDemo)
