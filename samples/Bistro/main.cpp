#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <fast_obj.h>
#include <meshoptimizer.h>
#include <taskflow/taskflow.hpp>

#include <shared/Bitmap.h>
#include <shared/Camera.h>
#include <shared/UtilsCubemap.h>

#include <lvk/HelpersImGuiMetal.h>
#include <lvk/LVK-Metal.h>

#include "app_common.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

using glm::mat3;
using glm::mat4;
using glm::vec2;
using glm::vec3;
using glm::vec4;

static const uint32_t kMeshCacheVersion = 0xC0DE000A;

static const char* kShaderMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexData {
  packed_float3 position;
  uint uv;
  ushort normal;
  ushort mtlIndex;
};

struct PerFrame {
  float4x4 proj;
  float4x4 view;
  float4x4 light;
  uint texSkyboxRadiance;
  uint texSkyboxIrradiance;
  uint texShadow;
  uint sampler0;
  uint samplerShadow0;
  uint drawNormals;
};

struct PerObject {
  float4x4 model;
  float4x4 normalMatrix;
};

struct Material {
  float4 ambient;
  float4 diffuse;
  uint texAmbient;
  uint texDiffuse;
  uint texAlpha;
  uint padding;
};

struct PC {
  device const PerFrame* perFrame;
  device const PerObject* perObject;
  device const Material* materials;
  device const VertexData* verts;
};

struct SkyPC {
  device const PerFrame* perFrame;
};

struct FullscreenPC {
  uint texture;
};

float2 unpackSnorm2x8(uint d) {
  return float2(uint2(d, d >> 8) & 255u) / 127.5 - 1.0;
}

float3 unpackOctahedral16(uint data) {
  float2 v = unpackSnorm2x8(data);
  float3 n = float3(v, 1.0 - abs(v.x) - abs(v.y));
  float t = max(-n.z, 0.0);
  n.x += (n.x > 0.0) ? -t : t;
  n.y += (n.y > 0.0) ? -t : t;
  return normalize(n);
}

float3x3 normalMat3(float4x4 m) {
  return float3x3(m[0].xyz, m[1].xyz, m[2].xyz);
}

struct MeshOut {
  float4 position [[position]];
  float2 uv;
  float3 normal;
  float4 shadowCoords;
  uint mtlIndex [[flat]];
};

vertex MeshOut meshVertexMain(uint vid [[vertex_id]], constant PC& pc [[buffer(0)]]) {
  const VertexData v = pc.verts[vid];
  const float4x4 model = pc.perObject->model;
  const float4 pos = float4(float3(v.position), 1.0);
  MeshOut o;
  o.position = pc.perFrame->proj * pc.perFrame->view * model * pos;
  o.normal = normalize(normalMat3(pc.perObject->normalMatrix) * unpackOctahedral16(uint(v.normal)));
  o.uv = float2(as_type<half2>(v.uv));
  o.shadowCoords = pc.perFrame->light * model * pos;
  o.mtlIndex = uint(v.mtlIndex);
  return o;
}

fragment float4 meshFragmentMain(MeshOut in [[stage_in]], constant PC& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  const Material m = pc.materials[in.mtlIndex];
  const uint smp = pc.perFrame->sampler0;
  if (m.texAlpha > 0 && textureBindless2D(m.texAlpha, smp, in.uv).r < 0.5)
    discard_fragment();
  const float4 Ka = m.ambient * textureBindless2D(m.texAmbient, smp, in.uv);
  const float4 Kd = m.diffuse * textureBindless2D(m.texDiffuse, smp, in.uv);
  if (Kd.a < 0.5)
    discard_fragment();
  const float3 n = normalize(in.normal);
  if (pc.perFrame->drawNormals != 0)
    return float4(0.5 * (n + 1.0), 1.0);

  float sh = 1.0;
  const float4 s = in.shadowCoords / in.shadowCoords.w;
  if (s.z > -1.0 && s.z < 1.0) {
    const uint texShadow = pc.perFrame->texShadow;
    const uint smpShadow = pc.perFrame->samplerShadow0;
    const float3 uvw = float3(s.x, 1.0 - s.y, s.z - 0.00005);
    const float sz = 1.0 / 4096.0;
    float acc = 0.0;
    for (int v = -1; v <= 1; ++v)
      for (int u = -1; u <= 1; ++u)
        acc += textureBindless2DShadow(texShadow, smpShadow, uvw + sz * float3(u, v, 0));
    sh = mix(0.3, 1.0, acc / 9.0);
  }

  const float4 f0 = float4(0.04);
  const float4 diffuse = textureBindlessCube(pc.perFrame->texSkyboxIrradiance, smp, n) * Kd * (float4(1.0) - f0);
  return Ka + diffuse * sh;
}

struct WireOut {
  float4 position [[position]];
};

vertex WireOut wireVertexMain(uint vid [[vertex_id]], constant PC& pc [[buffer(0)]]) {
  WireOut o;
  o.position = pc.perFrame->proj * pc.perFrame->view * pc.perObject->model * float4(float3(pc.verts[vid].position), 1.0);
  return o;
}

fragment float4 wireFragmentMain() {
  return float4(1.0);
}

vertex WireOut shadowVertexMain(uint vid [[vertex_id]], constant PC& pc [[buffer(0)]]) {
  WireOut o;
  o.position = pc.perFrame->proj * pc.perFrame->view * pc.perObject->model * float4(float3(pc.verts[vid].position), 1.0);
  return o;
}

fragment void shadowFragmentMain() {
}

struct SkyOut {
  float4 position [[position]];
  float3 dir;
};

vertex SkyOut skyVertexMain(uint vid [[vertex_id]], constant SkyPC& pc [[buffer(0)]]) {
  const float3 positions[8] = {
    float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),
    float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0)
  };
  const int indices[36] = {0,1,2, 2,3,0, 1,5,6, 6,2,1, 7,6,5, 5,4,7, 4,0,3, 3,7,4, 4,5,1, 1,0,4, 3,2,6, 6,7,3};
  float4x4 view = pc.perFrame->view;
  view[3] = float4(0.0, 0.0, 0.0, 1.0);
  const float3 pos = positions[indices[vid]];
  SkyOut o;
  o.position = (pc.perFrame->proj * view * float4(pos, 1.0)).xyww;
  o.dir = pos;
  return o;
}

fragment float4 skyFragmentMain(SkyOut in [[stage_in]], constant SkyPC& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  return textureBindlessCube(pc.perFrame->texSkyboxRadiance, pc.perFrame->sampler0, in.dir);
}

struct FsOut {
  float4 position [[position]];
  float2 uv;
};

vertex FsOut fullscreenVertexMain(uint vid [[vertex_id]]) {
  FsOut o;
  o.uv = float2((vid << 1) & 2, vid & 2);
  o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0.0, 1.0);
  return o;
}

fragment float4 fullscreenFragmentMain(FsOut in [[stage_in]], constant FullscreenPC& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  return textureBindless2D(pc.texture, 0, in.uv);
}

struct GrayscalePC {
  uint tex;
  uint width;
  uint height;
};

[[clang::annotate("lvk_numthreads(16,16,1)")]]
kernel void grayscaleMain(uint2 gid [[thread_position_in_grid]], constant GrayscalePC& pc [[buffer(0)]], LVK_BINDLESS_COMPUTE_ARGS) {
  if (gid.x < pc.width && gid.y < pc.height) {
    const float4 px = kImages2D.data[pc.tex].read(gid);
    const float l = dot(px, float4(0.299, 0.587, 0.114, 0.0));
    kImages2D.data[pc.tex].write(float4(l, l, l, 1.0), gid);
  }
}
)MSL";

struct VertexData {
  vec3 position;
  uint32_t uv;
  uint16_t normal;
  uint16_t mtlIndex;
};
static_assert(sizeof(VertexData) == 5 * sizeof(uint32_t));

struct PerFrame {
  mat4 proj;
  mat4 view;
  mat4 light;
  uint32_t texSkyboxRadiance = 0;
  uint32_t texSkyboxIrradiance = 0;
  uint32_t texShadow = 0;
  uint32_t sampler0 = 0;
  uint32_t samplerShadow0 = 0;
  uint32_t drawNormals = 0;
};

struct PerObject {
  mat4 model;
  mat4 normalMatrix;
};

struct GpuMaterial {
  vec4 ambient = vec4(0.0f);
  vec4 diffuse = vec4(0.0f);
  uint32_t texAmbient = 0;
  uint32_t texDiffuse = 0;
  uint32_t texAlpha = 0;
  uint32_t padding = 0;
};
static_assert(sizeof(GpuMaterial) % 16 == 0);

#define MAX_MATERIAL_NAME 128
struct CachedMaterial {
  char name[MAX_MATERIAL_NAME] = {};
  vec3 ambient = vec3(0.0f);
  vec3 diffuse = vec3(0.0f);
  char ambient_texname[MAX_MATERIAL_NAME] = {};
  char diffuse_texname[MAX_MATERIAL_NAME] = {};
  char alpha_texname[MAX_MATERIAL_NAME] = {};
};

static vec2 msign(vec2 v) {
  return vec2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}
static uint16_t octPackSnorm2x8(vec2 v) {
  glm::uvec2 d = glm::uvec2(glm::round(127.5f + v * 127.5f));
  return uint16_t(d.x | (d.y << 8u));
}
static uint16_t packOctahedral16(vec3 n) {
  n /= (glm::abs(n.x) + glm::abs(n.y) + glm::abs(n.z));
  return octPackSnorm2x8((n.z >= 0.0f) ? vec2(n.x, n.y) : (vec2(1.0f) - glm::abs(vec2(n.y, n.x))) * msign(vec2(n)));
}

static std::string contentDir() {
  return std::string(LVK_METAL_SAMPLE_CONTENT_DIR) + "/";
}

struct LoadedImage {
  uint32_t w = 0;
  uint32_t h = 0;
  uint32_t channels = 0;
  uint8_t* pixels = nullptr;
  std::string debugName;
};

struct LoadedMaterial {
  size_t idx = 0;
  LoadedImage ambient;
  LoadedImage diffuse;
  LoadedImage alpha;
};

class Bistro final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow* window, uint32_t width, uint32_t height, float, uint32_t samplesCount) override {
    ctx_ = &ctx;
    width_ = width;
    height_ = height;
    window_ = window;
    samplesCount_ = samplesCount > 1 ? samplesCount : 1;
    positioner_ = CameraPositioner_FirstPerson(vec3(-100, 40, -47), vec3(0, 35, 0), vec3(0, 1, 0));

    {
      const uint32_t white = 0xFFFFFFFF;
      textureDummyWhite_ = ctx.createTexture({
          .type = lvk::TextureType_2D,
          .format = lvk::Format_RGBA_UN8,
          .dimensions = {1, 1},
          .usage = lvk::TextureUsageBits_Sampled,
          .storage = lvk::StorageType_Device,
          .data = &white,
          .debugName = "dummy white",
      });
    }

    sampler_ = ctx.createSampler(
        {.mipMap = lvk::SamplerMip_Linear, .wrapU = lvk::SamplerWrap_Repeat, .wrapV = lvk::SamplerWrap_Repeat, .debugName = "linear"});
    samplerShadow_ = ctx.createSampler({.wrapU = lvk::SamplerWrap_Clamp,
                                        .wrapV = lvk::SamplerWrap_Clamp,
                                        .depthCompareOp = lvk::CompareOp_LessEqual,
                                        .depthCompareEnabled = true,
                                        .debugName = "shadow"});

    createShadowMap(ctx);
    createOffscreen(ctx);
    loadShaders(ctx);
    createPipelines(ctx);

    for (uint32_t i = 0; i < kRing; ++i) {
      ubPerFrame_[i] = makeUniform(ctx, sizeof(PerFrame), "perFrame");
      ubPerFrameShadow_[i] = makeUniform(ctx, sizeof(PerFrame), "perFrameShadow");
      ubPerObject_[i] = makeUniform(ctx, sizeof(PerObject), "perObject");
    }

    if (window_) {
      glfwSetWindowUserPointer(window_, this);
      glfwSetKeyCallback(window_, keyCallback);
      glfwSetMouseButtonCallback(window_, mouseButtonCallback);
      glfwSetCursorPosCallback(window_, cursorPosCallback);
    }

    imgui_ = std::make_unique<lvk::metal::ImGuiRenderer>(ctx, window_);

    initModel(ctx);
    loadSkybox(ctx);
    loadMaterials();
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float time) override {
    const float delta = lastTime_ < 0.0f ? 1.0f / 60.0f : time - lastTime_;
    lastTime_ = time;

    if (window_ && !ImGui::GetIO().WantCaptureMouse) {
      positioner_.update(delta, mousePos_, mousePressedLeft_);
    } else {
      positioner_.update(delta, mousePos_, false);
    }

    const uint32_t r = frameIdx_ % kRing;
    frameIdx_++;

    const float fov = glm::radians(45.0f);
    const float aspect = float(width_) / float(height_);
    const mat4 shadowProj = glm::perspective(glm::radians(60.0f), 1.0f, 10.0f, 4000.0f);
    const mat4 shadowView = mat4(vec4(0.772608519f, 0.532385886f, -0.345892131f, 0),
                                 vec4(0, 0.544812560f, 0.838557839f, 0),
                                 vec4(0.634882748f, -0.647876859f, 0.420926809f, 0),
                                 vec4(-58.9244843f, -30.4530792f, -508.410126f, 1.0f));
    const mat4 scaleBias = mat4(0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 1.0, 0, 0.5, 0.5, 0, 1.0);

    PerFrame pf = {
        .proj = glm::perspective(fov, aspect, 0.5f, 500.0f),
        .view = camera_.getViewMatrix(),
        .light = scaleBias * shadowProj * shadowView,
        .texSkyboxRadiance = skyboxRadiance_.index(),
        .texSkyboxIrradiance = skyboxIrradiance_.index(),
        .texShadow = shadowMap_.index(),
        .sampler0 = sampler_.index(),
        .samplerShadow0 = samplerShadow_.index(),
        .drawNormals = drawNormals_ ? 1u : 0u,
    };
    std::memcpy(ctx_->getMappedPtr(ubPerFrame_[r]), &pf, sizeof(pf));

    const mat4 model = glm::scale(mat4(1.0f), vec3(0.05f));
    const PerObject perObject = {.model = model, .normalMatrix = glm::transpose(glm::inverse(model))};
    std::memcpy(ctx_->getMappedPtr(ubPerObject_[r]), &perObject, sizeof(perObject));

    PerFrame pfShadow = pf;
    pfShadow.proj = shadowProj;
    pfShadow.view = shadowView;
    std::memcpy(ctx_->getMappedPtr(ubPerFrameShadow_[r]), &pfShadow, sizeof(pfShadow));

    if (!window_ && loaderPool_) {
      loaderPool_->wait_for_all();
    }
    processLoadedMaterials();

    struct ScenePC {
      uint64_t perFrame;
      uint64_t perObject;
      uint64_t materials;
      uint64_t verts;
    };

    if (shadowMapDirty_) {
      const ScenePC pc = {ctx_->gpuAddress(ubPerFrameShadow_[r]), ctx_->gpuAddress(ubPerObject_[r]), 0, vbAddress_};
      cmd.cmdBeginRendering({.depth = {.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearDepth = 1.0f}},
                            {.depthStencil = {.texture = shadowMap_}});
      cmd.cmdBindRenderPipeline(pipelineShadow_);
      cmd.cmdBindDepthState({.compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = true});
      cmd.cmdBindViewport({.x = 0, .y = 0, .width = float(kShadowSize), .height = float(kShadowSize)});
      cmd.cmdBindScissorRect({.x = 0, .y = 0, .width = kShadowSize, .height = kShadowSize});
      cmd.cmdPushConstants(pc);
      cmd.cmdBindIndexBuffer(ib0_, lvk::IndexFormat_UI32);
      cmd.cmdDrawIndexed(numIndices_);
      cmd.cmdEndRendering();
      shadowMapDirty_ = false;
    }

    const ScenePC scenePC = {
        ctx_->gpuAddress(ubPerFrame_[r]), ctx_->gpuAddress(ubPerObject_[r]), ctx_->gpuAddress(sbMaterials_), vbAddress_};

    const bool msaa = samplesCount_ > 1;
    const lvk::TextureHandle msaaColorTex = offscreenColorMSAA_;
    const lvk::TextureHandle resolvedColorTex = offscreenColor_;
    const lvk::TextureHandle sceneColorTex = msaa ? msaaColorTex : resolvedColorTex;
    const lvk::TextureHandle sceneResolveTex = msaa ? resolvedColorTex : lvk::TextureHandle{};
    cmd.cmdBeginRendering(
        {.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = msaa ? lvk::StoreOp_DontCare : lvk::StoreOp_Store, .clearColor = {0, 0, 0, 1}}},
         .depth = {.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_DontCare, .clearDepth = 1.0f}},
        {.color = {{.texture = sceneColorTex, .resolveTexture = sceneResolveTex}}, .depthStencil = {.texture = offscreenDepth_}},
        {.sampledImages = {shadowMap_}});
    cmd.cmdBindViewport({.x = 0, .y = 0, .width = float(width_), .height = float(height_)});
    cmd.cmdBindScissorRect({.x = 0, .y = 0, .width = width_, .height = height_});

    cmd.cmdBindRenderPipeline(pipelineMesh_);
    cmd.cmdBindDepthState({.compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = true});
    cmd.cmdPushConstants(scenePC);
    cmd.cmdBindIndexBuffer(ib0_, lvk::IndexFormat_UI32);
    cmd.cmdDrawIndexed(numIndices_);
    if (wireframe_) {
      cmd.cmdBindRenderPipeline(pipelineWireframe_);
      cmd.cmdPushConstants(scenePC);
      cmd.cmdDrawIndexed(numIndices_);
    }

    cmd.cmdBindRenderPipeline(pipelineSkybox_);
    cmd.cmdBindDepthState({.compareOp = lvk::CompareOp_LessEqual, .isDepthWriteEnabled = true});
    const struct {
      uint64_t perFrame;
    } skyPC = {ctx_->gpuAddress(ubPerFrame_[r])};
    cmd.cmdPushConstants(skyPC);
    cmd.cmdDraw(36);
    cmd.cmdEndRendering();

    if (compute_) {
      cmd.cmdBindComputePipeline(pipelineGrayscale_);
      const struct {
        uint32_t tex;
        uint32_t width;
        uint32_t height;
      } gpc = {offscreenColor_.index(), width_, height_};
      cmd.cmdPushConstants(gpc);
      cmd.cmdDispatch({1 + width_ / 16, 1 + height_ / 16, 1}, {.storageImages = {offscreenColor_}});
    }

    const lvk::Framebuffer fbMain = {.color = {{.texture = target}}};
    imgui_->beginFrame(fbMain);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 200), ImGuiCond_FirstUseEver);
    ImGui::Begin("Bistro", nullptr, ImGuiWindowFlags_NoNavInputs);
    ImGui::Text("W/S/A/D - camera movement");
    ImGui::Text("1/2 - camera up/down, Shift - fast");
    ImGui::Checkbox("Compute postprocess (C)", &compute_);
    ImGui::Checkbox("Draw normals (N)", &drawNormals_);
    ImGui::Checkbox("Wireframe (T)", &wireframe_);
    const uint32_t remaining = remainingMaterials_.load(std::memory_order_acquire);
    if (remaining) {
      ImGui::Separator();
      ImGui::Text("Loading textures: %u left", remaining);
    }
    ImGui::End();

    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0, 0, 0, 1}}}},
                          fbMain,
                          {.sampledImages = {offscreenColor_}});
    cmd.cmdBindViewport({.x = 0, .y = 0, .width = float(width_), .height = float(height_)});
    cmd.cmdBindScissorRect({.x = 0, .y = 0, .width = width_, .height = height_});
    cmd.cmdBindRenderPipeline(pipelineFullscreen_);
    const struct {
      uint32_t texture;
    } fsPC = {offscreenColor_.index()};
    cmd.cmdPushConstants(fsPC);
    cmd.cmdDraw(3);
    imgui_->endFrame(cmd);
    cmd.cmdEndRendering();
  }

  bool isReadyForScreenshot() const override {
    return remainingMaterials_.load(std::memory_order_acquire) == 0;
  }

  void destroy() override {
    if (loaderPool_) {
      loaderShouldExit_.store(true, std::memory_order_release);
      loaderPool_->wait_for_all();
      loaderPool_.reset();
    }
    imgui_.reset();
    texturesCache_.clear();
    for (LoadedImage& img : ownedPixels_) {
      if (img.pixels) {
        stbi_image_free(img.pixels);
      }
    }
    ownedPixels_.clear();
  }

 private:
  static constexpr uint32_t kRing = 3;
  static constexpr uint32_t kShadowSize = 4096;

  lvk::Holder<lvk::BufferHandle> makeUniform(lvk::metal::IMetalContext& ctx, size_t size, const char* name) {
    return ctx.createBuffer(
        {.usage = lvk::BufferUsageBits_Storage, .storage = lvk::StorageType_HostVisible, .size = size, .debugName = name});
  }

  void createShadowMap(lvk::metal::IMetalContext& ctx) {
    shadowMap_ = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_Z_F32,
        .dimensions = {kShadowSize, kShadowSize},
        .usage = lvk::TextureUsageBits_Attachment | lvk::TextureUsageBits_Sampled,
        .storage = lvk::StorageType_Device,
        .debugName = "Shadow map",
    });
  }

  void createOffscreen(lvk::metal::IMetalContext& ctx) {
    offscreenColor_ = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_RGBA_UN8,
        .dimensions = {width_, height_},
        .usage = lvk::TextureUsageBits_Attachment | lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .debugName = "Offscreen color",
    });
    if (samplesCount_ > 1) {
      offscreenColorMSAA_ = ctx.createTexture({
          .type = lvk::TextureType_2D,
          .format = lvk::Format_RGBA_UN8,
          .dimensions = {width_, height_},
          .numSamples = samplesCount_,
          .usage = lvk::TextureUsageBits_Attachment,
          .storage = lvk::StorageType_Device,
          .debugName = "Offscreen color MSAA",
      });
    }
    offscreenDepth_ = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_Z_F32,
        .dimensions = {width_, height_},
        .numSamples = samplesCount_,
        .usage = lvk::TextureUsageBits_Attachment,
        .storage = lvk::StorageType_Device,
        .debugName = "Offscreen depth",
    });
  }

  void loadShaders(lvk::metal::IMetalContext& ctx) {
#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      meshVert_ = slang.createShaderModule(ctx, "mesh", "meshVertexMain", lvk::Stage_Vert, "mesh.vert");
      meshFrag_ = slang.createShaderModule(ctx, "mesh", "meshFragmentMain", lvk::Stage_Frag, "mesh.frag");
      wireVert_ = slang.createShaderModule(ctx, "wireframe", "wireVertexMain", lvk::Stage_Vert, "wire.vert");
      wireFrag_ = slang.createShaderModule(ctx, "wireframe", "wireFragmentMain", lvk::Stage_Frag, "wire.frag");
      shadowVert_ = slang.createShaderModule(ctx, "shadow", "shadowVertexMain", lvk::Stage_Vert, "shadow.vert");
      shadowFrag_ = slang.createShaderModule(ctx, "shadow", "shadowFragmentMain", lvk::Stage_Frag, "shadow.frag");
      skyVert_ = slang.createShaderModule(ctx, "skybox", "skyVertexMain", lvk::Stage_Vert, "sky.vert");
      skyFrag_ = slang.createShaderModule(ctx, "skybox", "skyFragmentMain", lvk::Stage_Frag, "sky.frag");
      fsVert_ = slang.createShaderModule(ctx, "fullscreen", "fullscreenVertexMain", lvk::Stage_Vert, "fs.vert");
      fsFrag_ = slang.createShaderModule(ctx, "fullscreen", "fullscreenFragmentMain", lvk::Stage_Frag, "fs.frag");
      grayscaleComp_ = slang.createShaderModule(ctx, "grayscale", "grayscaleMain", lvk::Stage_Comp, "grayscale.comp");
      return;
    }
#endif
    LLOGL("[lvk-metal] shaders: native MSL");
    meshVert_ = ctx.createShaderModule({kShaderMSL, "meshVertexMain", lvk::Stage_Vert, "mesh.vert"});
    meshFrag_ = ctx.createShaderModule({kShaderMSL, "meshFragmentMain", lvk::Stage_Frag, "mesh.frag"});
    wireVert_ = ctx.createShaderModule({kShaderMSL, "wireVertexMain", lvk::Stage_Vert, "wire.vert"});
    wireFrag_ = ctx.createShaderModule({kShaderMSL, "wireFragmentMain", lvk::Stage_Frag, "wire.frag"});
    shadowVert_ = ctx.createShaderModule({kShaderMSL, "shadowVertexMain", lvk::Stage_Vert, "shadow.vert"});
    shadowFrag_ = ctx.createShaderModule({kShaderMSL, "shadowFragmentMain", lvk::Stage_Frag, "shadow.frag"});
    skyVert_ = ctx.createShaderModule({kShaderMSL, "skyVertexMain", lvk::Stage_Vert, "sky.vert"});
    skyFrag_ = ctx.createShaderModule({kShaderMSL, "skyFragmentMain", lvk::Stage_Frag, "sky.frag"});
    fsVert_ = ctx.createShaderModule({kShaderMSL, "fullscreenVertexMain", lvk::Stage_Vert, "fs.vert"});
    fsFrag_ = ctx.createShaderModule({kShaderMSL, "fullscreenFragmentMain", lvk::Stage_Frag, "fs.frag"});
    grayscaleComp_ = ctx.createShaderModule({kShaderMSL, "grayscaleMain", lvk::Stage_Comp, "grayscale.comp"});
  }

  void createPipelines(lvk::metal::IMetalContext& ctx) {
    const lvk::Format color = ctx.getFormat(offscreenColor_);
    pipelineMesh_ = ctx.createRenderPipeline({
        .smVert = meshVert_,
        .smFrag = meshFrag_,
        .color = {{.format = color}},
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_Back,
        .frontFace = lvk::WindingMode_CCW,
        .samplesCount = samplesCount_,
        .debugName = "Pipeline: mesh",
    });
    lvk::RenderPipelineDesc wire = {
        .smVert = wireVert_,
        .smFrag = wireFrag_,
        .color = {{.format = color}},
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_None,
        .polygonMode = lvk::PolygonMode_Line,
        .samplesCount = samplesCount_,
        .debugName = "Pipeline: wireframe",
    };
    pipelineWireframe_ = ctx.createRenderPipeline(wire);
    pipelineShadow_ = ctx.createRenderPipeline({
        .smVert = shadowVert_,
        .smFrag = shadowFrag_,
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_None,
        .debugName = "Pipeline: shadow",
    });
    pipelineSkybox_ = ctx.createRenderPipeline({
        .smVert = skyVert_,
        .smFrag = skyFrag_,
        .color = {{.format = color}},
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_Front,
        .frontFace = lvk::WindingMode_CCW,
        .samplesCount = samplesCount_,
        .debugName = "Pipeline: skybox",
    });
    pipelineFullscreen_ = ctx.createRenderPipeline({
        .smVert = fsVert_,
        .smFrag = fsFrag_,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .cullMode = lvk::CullMode_None,
        .debugName = "Pipeline: fullscreen",
    });
    pipelineGrayscale_ = ctx.createComputePipeline({.smComp = grayscaleComp_});
  }

  bool loadFromCache(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
      return false;
    }
    bool ok = false;
    uint32_t version = 0, numMat = 0, numVert = 0, numIdx = 0;
    if (fread(&version, sizeof(version), 1, f) == 1 && version == kMeshCacheVersion && fread(&numMat, 4, 1, f) == 1 &&
        fread(&numVert, 4, 1, f) == 1 && fread(&numIdx, 4, 1, f) == 1) {
      cachedMaterials_.resize(numMat);
      vertexData_.resize(numVert);
      indexData_.resize(numIdx);
      ok = fread(cachedMaterials_.data(), sizeof(CachedMaterial), numMat, f) == numMat &&
           fread(vertexData_.data(), sizeof(VertexData), numVert, f) == numVert &&
           fread(indexData_.data(), sizeof(uint32_t), numIdx, f) == numIdx;
    }
    fclose(f);
    if (ok) {
      for (CachedMaterial& m : cachedMaterials_) {
        for (char* s : {m.ambient_texname, m.diffuse_texname, m.alpha_texname}) {
          for (char* c = s; *c; ++c) {
            if (*c == '\\') {
              *c = '/';
            }
          }
        }
      }
    }
    return ok;
  }

  bool loadAndCache(const char* path) {
    LLOGL("Bistro: loading exterior.obj (slow on first run)...");
    fastObjMesh* mesh = fast_obj_read((contentDir() + "src/bistro/Exterior/exterior.obj").c_str());
    if (!mesh) {
      LLOGE("Bistro: failed to read exterior.obj");
      return false;
    }
    uint32_t vertexIndex = 0;
    for (uint32_t face = 0; face < mesh->face_count; ++face) {
      for (uint32_t v = 0; v < mesh->face_vertices[face]; ++v) {
        const fastObjIndex gi = mesh->indices[vertexIndex++];
        const float* p = &mesh->positions[gi.p * 3];
        const float* n = &mesh->normals[gi.n * 3];
        const float* t = &mesh->texcoords[gi.t * 2];
        vertexData_.push_back({
            .position = vec3(p[0], p[1], p[2]),
            .uv = glm::packHalf2x16(vec2(t[0], t[1])),
            .normal = packOctahedral16(vec3(n[0], n[1], n[2])),
            .mtlIndex = uint16_t(mesh->face_materials[face]),
        });
      }
    }
    {
      const size_t indexCount = vertexData_.size();
      std::vector<uint32_t> remap(indexCount);
      const size_t vcount =
          meshopt_generateVertexRemap(remap.data(), nullptr, indexCount, vertexData_.data(), indexCount, sizeof(VertexData));
      std::vector<VertexData> remapped(vcount);
      indexData_.resize(indexCount);
      meshopt_remapIndexBuffer(indexData_.data(), nullptr, indexCount, remap.data());
      meshopt_remapVertexBuffer(remapped.data(), vertexData_.data(), indexCount, sizeof(VertexData), remap.data());
      vertexData_ = remapped;
      meshopt_optimizeVertexCache(indexData_.data(), indexData_.data(), indexCount, vcount);
      meshopt_optimizeOverdraw(
          indexData_.data(), indexData_.data(), indexCount, &vertexData_[0].position.x, vcount, sizeof(VertexData), 1.05f);
      meshopt_optimizeVertexFetch(vertexData_.data(), indexData_.data(), indexCount, vertexData_.data(), vcount, sizeof(VertexData));
    }
    for (uint32_t i = 0; i < mesh->material_count; ++i) {
      const fastObjMaterial& m = mesh->materials[i];
      CachedMaterial mtl;
      mtl.ambient = vec3(m.Ka[0], m.Ka[1], m.Ka[2]);
      mtl.diffuse = vec3(m.Kd[0], m.Kd[1], m.Kd[2]);
      copyName(mtl.ambient_texname, m.map_Ka < mesh->texture_count ? mesh->textures[m.map_Ka].name : nullptr);
      copyName(mtl.diffuse_texname, m.map_Kd < mesh->texture_count ? mesh->textures[m.map_Kd].name : nullptr);
      copyName(mtl.alpha_texname, m.map_d < mesh->texture_count ? mesh->textures[m.map_d].name : nullptr);
      cachedMaterials_.push_back(mtl);
    }
    fast_obj_destroy(mesh);

    FILE* f = fopen(path, "wb");
    if (f) {
      const uint32_t numMat = uint32_t(cachedMaterials_.size());
      const uint32_t numVert = uint32_t(vertexData_.size());
      const uint32_t numIdx = uint32_t(indexData_.size());
      fwrite(&kMeshCacheVersion, 4, 1, f);
      fwrite(&numMat, 4, 1, f);
      fwrite(&numVert, 4, 1, f);
      fwrite(&numIdx, 4, 1, f);
      fwrite(cachedMaterials_.data(), sizeof(CachedMaterial), numMat, f);
      fwrite(vertexData_.data(), sizeof(VertexData), numVert, f);
      fwrite(indexData_.data(), sizeof(uint32_t), numIdx, f);
      fclose(f);
    }
    return true;
  }

  static void copyName(char* dst, const char* src) {
    if (src && src[0]) {
      std::strncpy(dst, src, MAX_MATERIAL_NAME - 1);
    }
  }

  void initModel(lvk::metal::IMetalContext& ctx) {
    const std::string cacheFile = contentDir() + "cache.data";
    if (!loadFromCache(cacheFile.c_str())) {
      cachedMaterials_.clear();
      vertexData_.clear();
      indexData_.clear();
      loadAndCache(cacheFile.c_str());
    }
    numIndices_ = uint32_t(indexData_.size());

    materials_.resize(cachedMaterials_.size());
    for (size_t i = 0; i < cachedMaterials_.size(); ++i) {
      materials_[i] = GpuMaterial{vec4(cachedMaterials_[i].ambient, 1.0f),
                                  vec4(cachedMaterials_[i].diffuse, 1.0f),
                                  textureDummyWhite_.index(),
                                  textureDummyWhite_.index(),
                                  0u,
                                  0u};
    }
    sbMaterials_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage,
                                     .storage = lvk::StorageType_HostVisible,
                                     .size = sizeof(GpuMaterial) * std::max<size_t>(materials_.size(), 1),
                                     .data = materials_.data(),
                                     .debugName = "materials"});
    vb0_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage,
                             .storage = lvk::StorageType_Device,
                             .size = sizeof(VertexData) * vertexData_.size(),
                             .data = vertexData_.data(),
                             .debugName = "vertices"});
    vbAddress_ = ctx.gpuAddress(vb0_);
    ib0_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Index,
                             .storage = lvk::StorageType_Device,
                             .size = sizeof(uint32_t) * indexData_.size(),
                             .data = indexData_.data(),
                             .debugName = "indices"});
  }

  lvk::Holder<lvk::TextureHandle> makeCube(lvk::metal::IMetalContext& ctx, const Bitmap& equirect, const char* name) {
    const Bitmap cube = convertEquirectangularMapToCubeMapFaces(equirect);
    const uint32_t fw = uint32_t(cube.w_);
    const uint32_t fh = uint32_t(cube.h_);
    lvk::Holder<lvk::TextureHandle> tex = ctx.createTexture({
        .type = lvk::TextureType_Cube,
        .format = lvk::Format_RGBA_F32,
        .dimensions = {fw, fh},
        .usage = lvk::TextureUsageBits_Sampled,
        .storage = lvk::StorageType_Device,
        .debugName = name,
    });
    const vec3* src = reinterpret_cast<const vec3*>(cube.data_.data());
    std::vector<vec4> face(size_t(fw) * fh);
    for (uint32_t f = 0; f < 6; ++f) {
      for (size_t i = 0; i < size_t(fw) * fh; ++i) {
        face[i] = vec4(src[f * size_t(fw) * fh + i], 1.0f);
      }
      ctx.upload(tex, {.dimensions = {fw, fh, 1}, .layer = f}, face.data());
    }
    return tex;
  }

  void loadSkybox(lvk::metal::IMetalContext& ctx) {
    const std::string hdr = contentDir() + "src/skybox_hdr/immenstadter_horn_2k.hdr";
    int w = 0, h = 0;
    float* pxs = stbi_loadf(hdr.c_str(), &w, &h, nullptr, 3);
    if (!pxs) {
      LLOGE("Bistro: failed to load skybox HDR %s", hdr.c_str());
      return;
    }
    skyboxRadiance_ = makeCube(ctx, Bitmap(w, h, 3, eBitmapFormat_Float, pxs), "skybox radiance");
    const int dstW = 256;
    const int dstH = 128;
    std::vector<vec3> irr(size_t(dstW) * dstH);
    convolveDiffuse(reinterpret_cast<vec3*>(pxs), w, h, dstW, dstH, irr.data(), 1024);
    skyboxIrradiance_ = makeCube(ctx, Bitmap(dstW, dstH, 3, eBitmapFormat_Float, irr.data()), "skybox irradiance");
    stbi_image_free(pxs);
  }

  LoadedImage loadImage(const std::string& path, int channels) {
    int w = 0, h = 0;
    uint8_t* pixels = stbi_load(path.c_str(), &w, &h, nullptr, channels);
    return LoadedImage{uint32_t(w), uint32_t(h), uint32_t(channels), pixels, path};
  }

  void loadMaterials() {
    stbi_set_flip_vertically_on_load(1);
    textures_.resize(cachedMaterials_.size());
    remainingMaterials_ = uint32_t(cachedMaterials_.size());
    const std::string prefix = contentDir() + "src/bistro/Exterior/";
    for (size_t i = 0; i < cachedMaterials_.size(); ++i) {
      loaderPool_->silent_async([this, i, prefix]() {
        if (loaderShouldExit_.load(std::memory_order_acquire)) {
          return;
        }
        const CachedMaterial& cm = cachedMaterials_[i];
        LoadedMaterial mtl;
        mtl.idx = i;
        if (cm.ambient_texname[0]) {
          mtl.ambient = loadImage(prefix + cm.ambient_texname, 4);
        }
        if (cm.diffuse_texname[0]) {
          mtl.diffuse = loadImage(prefix + cm.diffuse_texname, 4);
        }
        if (cm.alpha_texname[0]) {
          mtl.alpha = loadImage(prefix + cm.alpha_texname, 1);
        }
        std::lock_guard<std::mutex> guard(loadedMutex_);
        loadedMaterials_.push_back(mtl);
      });
    }
  }

  lvk::TextureHandle createTexture(lvk::metal::IMetalContext& ctx, const LoadedImage& img) {
    if (!img.pixels) {
      return {};
    }
    const auto it = texturesCache_.find(img.debugName);
    if (it != texturesCache_.end()) {
      return it->second;
    }
    lvk::Holder<lvk::TextureHandle> tex = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = img.channels == 1 ? lvk::Format_R_UN8 : lvk::Format_RGBA_UN8,
        .dimensions = {img.w, img.h},
        .usage = lvk::TextureUsageBits_Sampled,
        .storage = lvk::StorageType_Device,
        .data = img.pixels,
        .debugName = img.debugName.c_str(),
    });
    const lvk::TextureHandle handle = tex;
    texturesCache_[img.debugName] = std::move(tex);
    return handle;
  }

  void processLoadedMaterials() {
    std::vector<LoadedMaterial> batch;
    {
      std::lock_guard<std::mutex> guard(loadedMutex_);
      batch.swap(loadedMaterials_);
    }
    if (batch.empty()) {
      return;
    }
    for (LoadedMaterial& mtl : batch) {
      const lvk::TextureHandle ambient = createTexture(*ctx_, mtl.ambient);
      const lvk::TextureHandle diffuse = createTexture(*ctx_, mtl.diffuse);
      const lvk::TextureHandle alpha = createTexture(*ctx_, mtl.alpha);
      GpuMaterial& m = materials_[mtl.idx];
      if (!ambient.empty()) {
        m.texAmbient = ambient.index();
      }
      if (!diffuse.empty()) {
        m.texDiffuse = diffuse.index();
      }
      m.texAlpha = alpha.empty() ? 0u : alpha.index();
      ownedPixels_.push_back(mtl.ambient);
      ownedPixels_.push_back(mtl.diffuse);
      ownedPixels_.push_back(mtl.alpha);
      remainingMaterials_.fetch_sub(1u, std::memory_order_release);
    }
    std::memcpy(ctx_->getMappedPtr(sbMaterials_), materials_.data(), sizeof(GpuMaterial) * materials_.size());
  }

  static void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    Bistro* self = static_cast<Bistro*>(glfwGetWindowUserPointer(window));
    if (!self || ImGui::GetIO().WantCaptureKeyboard) {
      return;
    }
    const bool pressed = action != GLFW_RELEASE;
    CameraPositioner_FirstPerson::Movement& m = self->positioner_.movement_;
    switch (key) {
    case GLFW_KEY_W:
      m.forward_ = pressed;
      break;
    case GLFW_KEY_S:
      m.backward_ = pressed;
      break;
    case GLFW_KEY_A:
      m.left_ = pressed;
      break;
    case GLFW_KEY_D:
      m.right_ = pressed;
      break;
    case GLFW_KEY_1:
      m.up_ = pressed;
      break;
    case GLFW_KEY_2:
      m.down_ = pressed;
      break;
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT:
      m.fastSpeed_ = pressed;
      break;
    case GLFW_KEY_C:
      if (action == GLFW_PRESS)
        self->compute_ = !self->compute_;
      break;
    case GLFW_KEY_N:
      if (action == GLFW_PRESS)
        self->drawNormals_ = !self->drawNormals_;
      break;
    case GLFW_KEY_T:
      if (action == GLFW_PRESS)
        self->wireframe_ = !self->wireframe_;
      break;
    default:
      break;
    }
  }
  static void mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    Bistro* self = static_cast<Bistro*>(glfwGetWindowUserPointer(window));
    if (self && button == GLFW_MOUSE_BUTTON_LEFT) {
      self->mousePressedLeft_ = action == GLFW_PRESS;
    }
  }
  static void cursorPosCallback(GLFWwindow* window, double x, double y) {
    Bistro* self = static_cast<Bistro*>(glfwGetWindowUserPointer(window));
    if (!self) {
      return;
    }
    int w = 0, h = 0;
    glfwGetWindowSize(window, &w, &h);
    self->mousePos_ = vec2(w > 0 ? float(x) / w : 0.0f, h > 0 ? 1.0f - float(y) / h : 0.0f);
  }

  lvk::metal::IMetalContext* ctx_ = nullptr;
  GLFWwindow* window_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t samplesCount_ = 1;
  float lastTime_ = -1.0f;
  uint32_t frameIdx_ = 0;
  uint32_t numIndices_ = 0;
  bool shadowMapDirty_ = true;
  bool compute_ = false;
  bool drawNormals_ = false;
  bool wireframe_ = false;

  vec2 mousePos_ = vec2(0);
  bool mousePressedLeft_ = false;
  CameraPositioner_FirstPerson positioner_;
  Camera camera_ = Camera(positioner_);

  std::vector<VertexData> vertexData_;
  std::vector<uint32_t> indexData_;
  std::vector<CachedMaterial> cachedMaterials_;
  std::vector<GpuMaterial> materials_;
  std::vector<lvk::TextureHandle> textures_;

  std::unique_ptr<tf::Executor> loaderPool_ = std::make_unique<tf::Executor>(std::max(2u, std::thread::hardware_concurrency() / 2));
  std::mutex loadedMutex_;
  std::vector<LoadedMaterial> loadedMaterials_;
  std::vector<LoadedImage> ownedPixels_;
  std::atomic<bool> loaderShouldExit_ = false;
  std::atomic<uint32_t> remainingMaterials_ = 0;
  std::unordered_map<std::string, lvk::Holder<lvk::TextureHandle>> texturesCache_;

  lvk::Holder<lvk::ShaderModuleHandle> meshVert_, meshFrag_, wireVert_, wireFrag_, shadowVert_, shadowFrag_, skyVert_, skyFrag_, fsVert_,
      fsFrag_, grayscaleComp_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineMesh_, pipelineWireframe_, pipelineShadow_, pipelineSkybox_, pipelineFullscreen_;
  lvk::Holder<lvk::ComputePipelineHandle> pipelineGrayscale_;

  lvk::Holder<lvk::SamplerHandle> sampler_, samplerShadow_;
  lvk::Holder<lvk::TextureHandle> textureDummyWhite_, shadowMap_, offscreenColor_, offscreenColorMSAA_, offscreenDepth_, skyboxRadiance_,
      skyboxIrradiance_;
  lvk::Holder<lvk::BufferHandle> vb0_, ib0_, sbMaterials_;
  lvk::Holder<lvk::BufferHandle> ubPerFrame_[kRing], ubPerFrameShadow_[kRing], ubPerObject_[kRing];
  uint64_t vbAddress_ = 0;

  std::unique_ptr<lvk::metal::ImGuiRenderer> imgui_;
};

DESKTOP_MAIN(Bistro)
