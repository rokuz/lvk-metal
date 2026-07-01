#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <lvk/LVK-Metal.h>

#include "app_common.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

static const char* kShaderMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct QuadOut {
  float4 position [[position]];
  float2 uv;
};

vertex QuadOut yuvVertexMain(uint vid [[vertex_id]]) {
  const float2 pos[4] = { float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0) };
  QuadOut o;
  o.position = float4(pos[vid], 0.0, 1.0);
  o.uv = pos[vid] * 0.5 + 0.5;
  o.uv.y = 1.0 - o.uv.y;
  return o;
}

struct YUVPush {
  uint texId;
  uint smpId;
};

fragment float4 yuvFragmentMain(QuadOut in [[stage_in]], constant YUVPush& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  return textureBindlessYUV(pc.texId, pc.smpId, in.uv);
}
)";

namespace {

std::vector<uint8_t> loadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    return {};
  const std::streamsize size = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  f.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

struct YUVDemo {
  const char* name;
  lvk::Format format;
  lvk::Holder<lvk::TextureHandle> texture;
};

} // namespace

class YUV final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow* window, uint32_t width, uint32_t height, float, uint32_t) override {
    width_ = width;
    height_ = height;
    window_ = window;

    sampler_ = ctx.createSampler({
        .minFilter = lvk::SamplerFilter_Linear,
        .magFilter = lvk::SamplerFilter_Linear,
        .wrapU = lvk::SamplerWrap_Clamp,
        .wrapV = lvk::SamplerWrap_Clamp,
        .debugName = "Sampler: YUV linear",
    });

    lvk::Holder<lvk::ShaderModuleHandle> vert;
    lvk::Holder<lvk::ShaderModuleHandle> frag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      vert = slang.createShaderModule(ctx, "yuv", "vertexMain", lvk::Stage_Vert, "yuv.vert");
      frag = slang.createShaderModule(ctx, "yuv", "fragmentMain", lvk::Stage_Frag, "yuv.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      vert = ctx.createShaderModule({kShaderMSL, "yuvVertexMain", lvk::Stage_Vert, "yuv.vert"});
      frag = ctx.createShaderModule({kShaderMSL, "yuvFragmentMain", lvk::Stage_Frag, "yuv.frag"});
    }

    pipeline_ = ctx.createRenderPipeline({
        .topology = lvk::Topology_TriangleStrip,
        .smVert = vert,
        .smFrag = frag,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .debugName = "Pipeline: YUV",
    });

    createDemo(ctx, "YUV NV12", lvk::Format_YUV_NV12, "output_frame_900.nv12.yuv");
    createDemo(ctx, "YUV 420p", lvk::Format_YUV_420p, "output_frame_900.420p.yuv");

    if (window_) {
      glfwSetWindowUserPointer(window_, this);
      glfwSetKeyCallback(window_, keyCallback);
      LLOGL("YUV: press SPACE to switch between NV12 and 420p");
    }
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float) override {
    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0, 0, 0, 1}}}},
                          {.color = {{.texture = target}}});
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    if (!demos_.empty()) {
      cmd.cmdBindRenderPipeline(pipeline_);
      const struct {
        uint32_t texId;
        uint32_t smpId;
      } pc = {.texId = demos_[current_].texture.index(), .smpId = sampler_.index()};
      cmd.cmdPushConstants(pc);
      cmd.cmdDraw(4);
    }
    cmd.cmdEndRendering();
  }

 private:
  void createDemo(lvk::metal::IMetalContext& ctx, const char* name, lvk::Format format, const char* fileName) {
    const std::string path = (std::filesystem::path(LVK_METAL_SAMPLE_CONTENT_DIR) / fileName).string();
    const std::vector<uint8_t> pixels = loadFile(path);
    if (pixels.size() != size_t(kWidth) * kHeight * 3 / 2) {
      LLOGE("YUV: cannot load '%s' (got %zu bytes, expected %d). Run lvk's deploy_content.py.",
            path.c_str(),
            pixels.size(),
            kWidth * kHeight * 3 / 2);
      return;
    }
    demos_.push_back({
        .name = name,
        .format = format,
        .texture = ctx.createTexture({
            .type = lvk::TextureType_2D,
            .format = format,
            .dimensions = {kWidth, kHeight},
            .usage = lvk::TextureUsageBits_Sampled,
            .data = pixels.data(),
            .debugName = name,
        }),
    });
  }

  static void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    YUV* self = static_cast<YUV*>(glfwGetWindowUserPointer(window));
    if (self && key == GLFW_KEY_SPACE && action == GLFW_PRESS && !self->demos_.empty())
      self->current_ = (self->current_ + 1) % self->demos_.size();
  }

  static constexpr uint32_t kWidth = 1920;
  static constexpr uint32_t kHeight = 1080;

  lvk::Holder<lvk::RenderPipelineHandle> pipeline_;
  lvk::Holder<lvk::SamplerHandle> sampler_;
  std::vector<YUVDemo> demos_;
  size_t current_ = 0;
  GLFWwindow* window_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(YUV)
