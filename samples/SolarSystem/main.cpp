#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <fast_obj.h>

#include <lmath/GeometryShapes.h>
#include <lmath/Random.h>
#include <shared/Bitmap.h>
#include <shared/UtilsCubemap.h>
#include <shared/Camera.h>
#include <shared/Trackball.h>

#include <lvk/LVK-Metal.h>
#include <lvk/HelpersImGuiMetal.h>

#include "app_common.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

using glm::mat3;
using glm::mat4;
using glm::vec2;
using glm::vec3;
using glm::vec4;

static const char* kShaderMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct Vertex {
  packed_float3 pos;
  packed_float2 uv;
  packed_float3 normal;
};

struct PerFrame {
  float4x4 proj;
  float4x4 view;
  float time;
};

struct Material {
  float4 emissive;
  float4 diffuse;
  uint texEmissive;
  uint texDiffuse;
  uint pad0;
  uint pad1;
};

struct DrawData {
  uint idMatrix;
  uint idMaterial;
};

struct PC {
  device const Vertex* verts;
  device const float4x4* model;
  device const float4x4* normalMat;
  device const PerFrame* perFrame;
  device const DrawData* drawData;
  device const Material* materials;
  uint firstDrawData;
};

struct SkyPC {
  device const PerFrame* perFrame;
  uint texCube;
};

struct DefaultOut {
  float4 position [[position]];
  float2 uv;
  float3 worldPos;
  float3 worldNormal;
  uint materialIndex [[flat]];
};

vertex DefaultOut defaultVertexMain(uint vid [[vertex_id]], uint iid [[instance_id]], constant PC& pc [[buffer(0)]]) {
  const DrawData dd = pc.drawData[pc.firstDrawData + iid];
  const Vertex v = pc.verts[vid];
  const float4x4 model = pc.model[dd.idMatrix];
  const float4x4 nrm = pc.normalMat[dd.idMatrix];
  const float4 wp = model * float4(float3(v.pos), 1.0);
  DefaultOut o;
  o.position = pc.perFrame->proj * pc.perFrame->view * wp;
  o.worldPos = wp.xyz;
  o.worldNormal = float3x3(nrm[0].xyz, nrm[1].xyz, nrm[2].xyz) * float3(v.normal);
  o.uv = float2(v.uv);
  o.materialIndex = dd.idMaterial;
  return o;
}

fragment float4 defaultFragmentMain(DefaultOut in [[stage_in]], constant PC& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  const Material m = pc.materials[in.materialIndex];
  const float4 Ke = m.emissive * textureBindless2D(m.texEmissive, 0, in.uv);
  const float4 Kd = m.diffuse * textureBindless2D(m.texDiffuse, 0, in.uv);
  const float3 lightPos = float3(0.0);
  const float lightRadius = 10.0;
  const bool twoSided = m.diffuse.w > 0.5;
  const float3 toLight = lightPos - in.worldPos;
  const float s = length(toLight) / lightRadius;
  const float atten = max(1.0 - s * s, 0.0);
  float d = dot(normalize(toLight), normalize(in.worldNormal));
  d = clamp(twoSided ? max(d, -d) : d, 0.0, 1.0);
  float4 color = Ke + Kd * atten * d;
  color.a = Ke.a;
  return color;
}

struct SunOut {
  float4 position [[position]];
  float2 uv;
  float time;
  uint tex0 [[flat]];
  uint tex1 [[flat]];
};

vertex SunOut sunVertexMain(uint vid [[vertex_id]], uint iid [[instance_id]], constant PC& pc [[buffer(0)]]) {
  const DrawData dd = pc.drawData[pc.firstDrawData + iid];
  const Vertex v = pc.verts[vid];
  const Material m = pc.materials[dd.idMaterial];
  SunOut o;
  o.position = pc.perFrame->proj * pc.perFrame->view * pc.model[dd.idMatrix] * float4(float3(v.pos), 1.0);
  o.uv = float2(v.uv) + 0.05 * float3(v.normal).xz;
  o.time = pc.perFrame->time;
  o.tex0 = m.texEmissive;
  o.tex1 = m.texDiffuse;
  return o;
}

fragment float4 sunFragmentMain(SunOut in [[stage_in]], LVK_BINDLESS_ARGS) {
  const float2 t = float2(cos(0.05 * in.time));
  const float3 K1 = textureBindless2D(in.tex0, 0, sin(in.uv + t)).rgb;
  const float3 K2 = textureBindless2D(in.tex1, 0, in.uv - 2.0 * t).rgb;
  return float4(K1 * K2, 1.0);
}

struct CoronaOut {
  float4 position [[position]];
  float2 uv;
  uint tex0 [[flat]];
};

vertex CoronaOut coronaVertexMain(uint vid [[vertex_id]], uint iid [[instance_id]], constant PC& pc [[buffer(0)]]) {
  const DrawData dd = pc.drawData[pc.firstDrawData + iid];
  const float2 uv = float2(pc.verts[vid].uv);
  const float4x4 mv = pc.perFrame->view * pc.model[dd.idMatrix];
  const float3 x = float3(mv[0][0], mv[1][0], mv[2][0]);
  const float3 y = float3(mv[0][1], mv[1][1], mv[2][1]);
  const float2 coef = 2.0 * (uv - 0.5) * float2(0.28);
  const float3 v = coef.x * x + coef.y * y;
  CoronaOut o;
  o.position = pc.perFrame->proj * mv * float4(v, 1.0);
  o.uv = uv;
  o.tex0 = pc.materials[dd.idMaterial].texEmissive;
  return o;
}

fragment float4 coronaFragmentMain(CoronaOut in [[stage_in]], LVK_BINDLESS_ARGS) {
  float4 K1 = textureBindless2D(in.tex0, 0, in.uv);
  K1.a = dot(K1.xyz, float3(0.333, 0.333, 0.333));
  return K1;
}

struct OrbitOut {
  float4 position [[position]];
};

vertex OrbitOut orbitVertexMain(uint vid [[vertex_id]], uint iid [[instance_id]], constant PC& pc [[buffer(0)]]) {
  const DrawData dd = pc.drawData[pc.firstDrawData + iid];
  OrbitOut o;
  o.position = pc.perFrame->proj * pc.perFrame->view * pc.model[dd.idMatrix] * float4(float3(pc.verts[vid].pos), 1.0);
  return o;
}

fragment float4 orbitFragmentMain() {
  return float4(1.0, 1.0, 1.0, 0.2);
}

struct SkyOut {
  float4 position [[position]];
  float3 dir;
};

vertex SkyOut skyVertexMain(uint vid [[vertex_id]], constant SkyPC& pc [[buffer(0)]]) {
  const float3 verts[8] = {
    float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),
    float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0)
  };
  const uint idx[36] = {0,1,2, 2,3,0, 1,5,6, 6,2,1, 7,6,5, 5,4,7, 4,0,3, 3,7,4, 4,5,1, 1,0,4, 3,2,6, 6,7,3};
  const float3 p = verts[idx[vid]];
  const float4x4 view = pc.perFrame->view;
  const float3x3 r = float3x3(view[0].xyz, view[1].xyz, view[2].xyz);
  const float4x4 vr = float4x4(float4(r[0], 0.0), float4(r[1], 0.0), float4(r[2], 0.0), float4(0.0, 0.0, 0.0, 1.0));
  SkyOut o;
  o.position = pc.perFrame->proj * vr * float4(50.0 * p, 1.0);
  o.dir = p.zxy;
  return o;
}

fragment float4 skyFragmentMain(SkyOut in [[stage_in]], constant SkyPC& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  return pow(textureBindlessCube(pc.texCube, 0, in.dir), float4(1.5, 1.5, 1.5, 1.5));
}
)";

struct Planet {
  float radius;
  float orbitalRadius;
  float globalOrbitalSpeed;
  float localOrbitalSpeed;
  float axialTilt;
  float orbitalInclination;
  const char* textureName;
};

static const float g_Scale = 0.001f;

static const std::vector<Planet> kPlanets = {
    {1.5f * g_Scale * 110.f, g_Scale * 0, 0.000f, 0.000f, 0.00f, 0.00f, "2k_sun.jpg"},
    {1.5f * g_Scale * 34.5f, g_Scale * 250, 0.650f, 0.650f, 0.03f, 7.01f, "2k_mercury.jpg"},
    {1.5f * g_Scale * 54.3f, g_Scale * 420, 1.969f, 1.969f, 2.64f, 3.39f, "2k_venus_surface.jpg"},
    {1.5f * g_Scale * 44.5f, g_Scale * 600, 0.881f, 0.881f, 23.44f, 0.00f, "2k_earth_daymap.jpg"},
    {1.5f * g_Scale * 36.5f, g_Scale * 800, 1.543f, 1.543f, 25.19f, 1.85f, "2k_mars.jpg"},
    {1.5f * g_Scale * 67.3f, g_Scale * 1350, 2.978f, 2.978f, 3.13f, 1.31f, "2k_jupiter.jpg"},
    {1.5f * g_Scale * 56.1f, g_Scale * 1650, 0.800f, 0.800f, 26.73f, 2.49f, "2k_saturn.jpg"},
    {1.5f * g_Scale * 55.3f, g_Scale * 2000, 0.607f, 0.607f, 82.23f, 0.77f, "2k_uranus.jpg"},
    {1.5f * g_Scale * 54.4f, g_Scale * 2200, 1.700f, 1.700f, 28.32f, 1.77f, "2k_neptune.jpg"},
    {2.0f * g_Scale * 12.5f, g_Scale * 100.0f, 2.843f, 2.843f * 10.0f, 6.68f, 0.0f, "2k_moon.jpg"},
};

enum { Sun, Mercury, Venus, Earth, Mars, Jupiter, Saturn, Uranus, Neptune, Moon, TotalPlanets };

static const size_t kNumAsteroidsInner = 1500;
static const size_t kNumAsteroidsOuter = 500;

static bool g_Paused = false;
static bool g_Wireframe = false;
static bool g_DrawPlanetOrbits = true;
static bool g_UseTrackball = false;
static VirtualTrackball g_Trackball;

struct PerFrame {
  mat4 proj;
  mat4 view;
  float time;
};

struct GpuMaterial {
  vec4 emissive;
  vec4 diffuse;
  uint32_t texEmissive;
  uint32_t texDiffuse;
  uint32_t pad0 = 0;
  uint32_t pad1 = 0;
};

struct DrawData {
  uint32_t idMatrix = 0;
  uint32_t idMaterial = 0;
};

struct SceneNode {
  SceneNode* parent = nullptr;
  mat4 local = mat4(1.0f);
  mat4 global = mat4(1.0f);
  int materialIdx = -1;
  std::vector<std::shared_ptr<SceneNode>> children;

  int getMaterialIndexOrParent() const {
    if (materialIdx >= 0) {
      return materialIdx;
    }
    return parent ? parent->getMaterialIndexOrParent() : -1;
  }

  SceneNode* createNode(const mat4& m = mat4(1.0f)) {
    children.push_back(std::make_shared<SceneNode>(SceneNode{this, m}));
    return children.back().get();
  }

  void updateGlobalFromLocal(const mat4& parentTransform) {
    global = parentTransform * local;
    for (std::shared_ptr<SceneNode>& c : children) {
      c->updateGlobalFromLocal(global);
    }
  }
};

struct OrbitAnimator {
  OrbitAnimator(const vec3& axis, float angle, float speed, float radius = 0.0f)
  : normalizedRotationAxis(glm::normalize(axis))
  , orbitalRadius(radius)
  , rotationSpeed(speed)
  , rotationAngle(angle) {}

  void update(float deltaSeconds) {
    rotationAngle = fmodf(rotationAngle + rotationSpeed * deltaSeconds, 360.0f);
    transform = glm::rotate(mat4(1.0f), glm::radians(rotationAngle), normalizedRotationAxis);
  }

  vec3 normalizedRotationAxis = vec3(0, 1, 0);
  float orbitalRadius = 0.0f;
  float rotationSpeed = 0.0f;
  float rotationAngle = 0.0f;
  mat4 transform = mat4(1.0f);
};

struct OrbitAnimationGroup {
  SceneNode* targetNode = nullptr;
  std::vector<OrbitAnimator> group;

  void update(float deltaSeconds) {
    mat4 transform = mat4(1.0f);
    for (OrbitAnimator& anim : group) {
      anim.update(deltaSeconds);
      const mat4 translation =
          anim.orbitalRadius != 0.0f ? glm::translate(mat4(1.0f), vec3(0.0f, anim.orbitalRadius, 0.0f)) : mat4(1.0f);
      transform = translation * anim.transform * transform;
    }
    targetNode->local = transform;
  }
};

struct Mesh {
  std::vector<GeometryShapes::Vertex> vertices;
  int firstVertex = -1;
};

struct MeshComponent {
  SceneNode* sceneNode = nullptr;
  std::shared_ptr<Mesh> mesh;
};

struct Material {
  vec4 emissive = vec4(0.2f, 0.2f, 0.2f, 1.0f);
  vec4 diffuse = vec4(0.8f, 0.8f, 0.8f, 1.0f);
  lvk::TextureHandle texEmissive;
  lvk::TextureHandle texDiffuse;
  bool isTransparent = false;
  bool twoSided = false;
  lvk::RenderPipelineHandle pipeline;
  lvk::RenderPipelineHandle pipelineW;
};

struct Scene {
  std::vector<MeshComponent> meshes;
  std::vector<Material> materials;
  std::vector<OrbitAnimationGroup> animators;
  SceneNode root;

  void updateAnimations(float deltaSeconds) {
    for (OrbitAnimationGroup& a : animators) {
      a.update(deltaSeconds);
    }
  }
  void createMesh(SceneNode* node, std::shared_ptr<Mesh> mesh) {
    meshes.push_back(MeshComponent{node, mesh});
  }
  void createMaterial(SceneNode* node, const Material& mat) {
    materials.push_back(mat);
    node->materialIdx = int(materials.size()) - 1;
  }
};

struct RenderOp {
  lvk::RenderPipelineHandle pipeline;
  lvk::RenderPipelineHandle pipelineW;
  uint32_t firstVertex = 0;
  uint32_t numVertices = 0;
  uint32_t numInstances = 1;
  DrawData drawData;
  uint32_t idDrawData = 0;
};

class SolarSystem final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow* window, uint32_t width, uint32_t height, float displayScale) override {
    width_ = width;
    height_ = height;
    window_ = window;
    positioner_ = CameraPositioner_FirstPerson(vec3(0.8f, -1.6f, 0.6f), vec3(-1.5f, 1.2f, -0.6f), vec3(0, 0, 1));
    positioner_.maxSpeed_ = 0.1f;

    loadShaders(ctx);
    createPipelines(ctx);

    skyTex_ = loadTextureCube(ctx, "starmap_4k.jpg");
    sampler_ = ctx.createSampler({.minFilter = lvk::SamplerFilter_Linear, .magFilter = lvk::SamplerFilter_Linear, .debugName = "linear"});

    buildScene(ctx);
    buildBuffers(ctx);

    depth_ = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_Z_F32,
        .dimensions = {width_, height_},
        .usage = lvk::TextureUsageBits_Attachment,
        .storage = lvk::StorageType_Device,
        .debugName = "depth",
    });

    imgui_ = std::make_unique<lvk::metal::ImGuiRenderer>(ctx, window_);

    if (window_) {
      glfwSetWindowUserPointer(window_, this);
      glfwSetKeyCallback(window_, keyCallback);
      glfwSetMouseButtonCallback(window_, mouseButtonCallback);
      glfwSetCursorPosCallback(window_, cursorPosCallback);
    }
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float time) override {
    const float delta = lastTime_ < 0.0f ? 0.0f : time - lastTime_;
    lastTime_ = time;

    scene_.updateAnimations(g_Paused ? 0.0f : delta);
    scene_.root.updateGlobalFromLocal(mat4(1.0f));

    const uint32_t r = frameIdx_ % kRing;
    frameIdx_++;

    mat4* model = reinterpret_cast<mat4*>(ptrModel_[r]);
    mat4* normal = reinterpret_cast<mat4*>(ptrNormal_[r]);
    for (size_t i = 0; i != scene_.meshes.size(); ++i) {
      model[i] = scene_.meshes[i].sceneNode->global;
      normal[i] = glm::transpose(glm::inverse(model[i]));
    }

    const float aspect = float(width_) / float(height_);
    const mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.01f, 100.0f);
    const mat4 view = computeView(delta);

    PerFrame* pf = reinterpret_cast<PerFrame*>(ptrPerFrame_[r]);
    pf->proj = proj;
    pf->view = view;
    pf->time = time;

    struct {
      uint64_t verts;
      uint64_t model;
      uint64_t normalMat;
      uint64_t perFrame;
      uint64_t drawData;
      uint64_t materials;
      uint32_t firstDrawData;
    } pc = {
        .verts = addrVertices_,
        .model = addrModel_[r],
        .normalMat = addrNormal_[r],
        .perFrame = addrPerFrame_[r],
        .drawData = addrDrawData_,
        .materials = addrMaterials_,
        .firstDrawData = 0,
    };

    const lvk::Framebuffer fb = {.color = {{.texture = target}}, .depthStencil = {.texture = depth_}};
    cmd.cmdBeginRendering(
        {.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0, 0, 0, 1}}},
         .depth = {.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_DontCare, .clearDepth = 1.0f}},
        fb);
    cmd.cmdBindViewport({.x = 0.0f, .y = 0.0f, .width = float(width_), .height = float(height_)});
    cmd.cmdBindScissorRect({.x = 0, .y = 0, .width = width_, .height = height_});

    cmd.cmdBindDepthState({.compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = true});
    for (const RenderOp& op : renderQueueOpaque_) {
      cmd.cmdBindRenderPipeline(g_Wireframe ? op.pipelineW : op.pipeline);
      pc.firstDrawData = op.idDrawData;
      cmd.cmdPushConstants(pc);
      cmd.cmdDraw(op.numVertices, op.numInstances, op.firstVertex);
    }

    cmd.cmdBindRenderPipeline(pipelineSky_);
    cmd.cmdBindDepthState({.compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = false});
    const struct {
      uint64_t perFrame;
      uint32_t texCube;
    } skyPush = {.perFrame = addrPerFrame_[r], .texCube = skyTex_.index()};
    cmd.cmdPushConstants(skyPush);
    cmd.cmdDraw(36);

    cmd.cmdBindDepthState({.compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = false});
    for (const RenderOp& op : renderQueueTransparent_) {
      cmd.cmdBindRenderPipeline(g_Wireframe ? op.pipelineW : op.pipeline);
      pc.firstDrawData = op.idDrawData;
      cmd.cmdPushConstants(pc);
      cmd.cmdDraw(op.numVertices, op.numInstances, op.firstVertex);
    }

    imgui_->beginFrame(fb);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 320), ImGuiCond_FirstUseEver);
    ImGui::Begin("Keyboard hints:", nullptr, ImGuiWindowFlags_NoNavInputs);
    ImGui::Text("W/S/A/D - camera movement");
    ImGui::Text("1/2 - camera up/down");
    ImGui::Text("Shift - fast movement");
    ImGui::Text("Space - reset camera");
    ImGui::Separator();
    ImGui::Checkbox("Pause animation (P)", &g_Paused);
    ImGui::Checkbox("Wireframe (X)", &g_Wireframe);
    ImGui::Checkbox("Use trackball navigation", &g_UseTrackball);
    if (window_) {
      ImGui::Separator();
      ImGui::Text("FPS: %.1f", delta > 0.0f ? 1.0f / delta : 0.0f);
    }
    ImGui::End();

    imgui_->endFrame(cmd);
    cmd.cmdEndRendering();
  }

 private:
  static constexpr uint32_t kRing = 3;

  mat4 computeView(float delta) {
    if (g_UseTrackball) {
      if (!ImGui::GetIO().WantCaptureMouse) {
        g_Trackball.dragTo(vec2(1.0f - mouseX_, mouseY_), 10.0f, mousePressedLeft_);
      }
      return glm::lookAt(vec3(0.0f, 0.0f, 4.0f), vec3(0.0f), vec3(0.0f, -1.0f, 0.0f)) * g_Trackball.getRotationMatrix();
    }
    if (window_ && !ImGui::GetIO().WantCaptureMouse) {
      positioner_.update(delta, vec2(mouseX_, mouseY_), mousePressedLeft_);
    } else {
      positioner_.update(delta, vec2(mouseX_, mouseY_), false);
    }
    return positioner_.getViewMatrix();
  }

  void loadShaders(lvk::metal::IMetalContext& ctx) {
#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      defaultVert_ = slang.createShaderModule(ctx, "default", "defaultVertexMain", lvk::Stage_Vert, "default.vert");
      defaultFrag_ = slang.createShaderModule(ctx, "default", "defaultFragmentMain", lvk::Stage_Frag, "default.frag");
      sunVert_ = slang.createShaderModule(ctx, "sun", "sunVertexMain", lvk::Stage_Vert, "sun.vert");
      sunFrag_ = slang.createShaderModule(ctx, "sun", "sunFragmentMain", lvk::Stage_Frag, "sun.frag");
      coronaVert_ = slang.createShaderModule(ctx, "corona", "coronaVertexMain", lvk::Stage_Vert, "corona.vert");
      coronaFrag_ = slang.createShaderModule(ctx, "corona", "coronaFragmentMain", lvk::Stage_Frag, "corona.frag");
      orbitVert_ = slang.createShaderModule(ctx, "orbit", "orbitVertexMain", lvk::Stage_Vert, "orbit.vert");
      orbitFrag_ = slang.createShaderModule(ctx, "orbit", "orbitFragmentMain", lvk::Stage_Frag, "orbit.frag");
      skyVert_ = slang.createShaderModule(ctx, "skybox", "skyVertexMain", lvk::Stage_Vert, "skybox.vert");
      skyFrag_ = slang.createShaderModule(ctx, "skybox", "skyFragmentMain", lvk::Stage_Frag, "skybox.frag");
      return;
    }
#endif
    LLOGL("[lvk-metal] shaders: native MSL");
    defaultVert_ = ctx.createShaderModule({kShaderMSL, "defaultVertexMain", lvk::Stage_Vert, "default.vert"});
    defaultFrag_ = ctx.createShaderModule({kShaderMSL, "defaultFragmentMain", lvk::Stage_Frag, "default.frag"});
    sunVert_ = ctx.createShaderModule({kShaderMSL, "sunVertexMain", lvk::Stage_Vert, "sun.vert"});
    sunFrag_ = ctx.createShaderModule({kShaderMSL, "sunFragmentMain", lvk::Stage_Frag, "sun.frag"});
    coronaVert_ = ctx.createShaderModule({kShaderMSL, "coronaVertexMain", lvk::Stage_Vert, "corona.vert"});
    coronaFrag_ = ctx.createShaderModule({kShaderMSL, "coronaFragmentMain", lvk::Stage_Frag, "corona.frag"});
    orbitVert_ = ctx.createShaderModule({kShaderMSL, "orbitVertexMain", lvk::Stage_Vert, "orbit.vert"});
    orbitFrag_ = ctx.createShaderModule({kShaderMSL, "orbitFragmentMain", lvk::Stage_Frag, "orbit.frag"});
    skyVert_ = ctx.createShaderModule({kShaderMSL, "skyVertexMain", lvk::Stage_Vert, "skybox.vert"});
    skyFrag_ = ctx.createShaderModule({kShaderMSL, "skyFragmentMain", lvk::Stage_Frag, "skybox.frag"});
  }

  void createPipelines(lvk::metal::IMetalContext& ctx) {
    const lvk::Format color = ctx.getSwapchainFormat();

    lvk::RenderPipelineDesc def = {
        .smVert = defaultVert_,
        .smFrag = defaultFrag_,
        .color = {{.format = color}},
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_Back,
        .frontFace = lvk::WindingMode_CCW,
        .debugName = "Pipeline: default",
    };
    pipelineDefault_ = ctx.createRenderPipeline(def);
    def.polygonMode = lvk::PolygonMode_Line;
    pipelineDefaultW_ = ctx.createRenderPipeline(def);

    pipelineSaturnRings_ = ctx.createRenderPipeline({
        .topology = lvk::Topology_TriangleStrip,
        .smVert = defaultVert_,
        .smFrag = defaultFrag_,
        .color = {{.format = color,
                   .blendEnabled = true,
                   .srcRGBBlendFactor = lvk::BlendFactor_SrcAlpha,
                   .dstRGBBlendFactor = lvk::BlendFactor_OneMinusSrcAlpha}},
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_None,
        .debugName = "Pipeline: Saturn rings",
    });
    pipelineOrbit_ = ctx.createRenderPipeline({
        .topology = lvk::Topology_LineStrip,
        .smVert = orbitVert_,
        .smFrag = orbitFrag_,
        .color = {{.format = color,
                   .blendEnabled = true,
                   .srcRGBBlendFactor = lvk::BlendFactor_SrcAlpha,
                   .dstRGBBlendFactor = lvk::BlendFactor_OneMinusSrcAlpha}},
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_None,
        .debugName = "Pipeline: orbit",
    });

    lvk::RenderPipelineDesc sun = {
        .smVert = sunVert_,
        .smFrag = sunFrag_,
        .color = {{.format = color}},
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_Back,
        .frontFace = lvk::WindingMode_CCW,
        .debugName = "Pipeline: Sun",
    };
    pipelineSun_ = ctx.createRenderPipeline(sun);
    sun.polygonMode = lvk::PolygonMode_Line;
    pipelineSunW_ = ctx.createRenderPipeline(sun);

    pipelineSunCorona_ = ctx.createRenderPipeline({
        .topology = lvk::Topology_TriangleStrip,
        .smVert = coronaVert_,
        .smFrag = coronaFrag_,
        .color = {{.format = color,
                   .blendEnabled = true,
                   .srcRGBBlendFactor = lvk::BlendFactor_SrcAlpha,
                   .dstRGBBlendFactor = lvk::BlendFactor_OneMinusSrcAlpha}},
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_None,
        .debugName = "Pipeline: Sun corona",
    });
    pipelineSky_ = ctx.createRenderPipeline({
        .smVert = skyVert_,
        .smFrag = skyFrag_,
        .color = {{.format = color}},
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_None,
        .debugName = "Pipeline: sky box",
    });
  }

  void buildScene(lvk::metal::IMetalContext& ctx) {
    const vec3 X = vec3(1, 0, 0);
    const vec3 Z = vec3(0, 0, 1);

    SceneNode* allPlanets[TotalPlanets] = {};
    allPlanets[Sun] = scene_.root.createNode();

    for (size_t i = 0; i < kPlanets.size(); ++i) {
      const Material planetMaterial = {
          .emissive = vec4(0.5f, 0.5f, 0.5f, 1.0f),
          .diffuse = vec4(0.8f),
          .texEmissive = loadTexture(ctx, kPlanets[i].textureName),
          .texDiffuse = loadTexture(ctx, kPlanets[i].textureName),
          .pipeline = pipelineDefault_,
          .pipelineW = pipelineDefaultW_,
      };
      const bool isSun = strstr(kPlanets[i].textureName, "_sun") != nullptr;
      const bool isMoon = strstr(kPlanets[i].textureName, "_moon") != nullptr;
      if (isSun) {
        Material sunMaterial = planetMaterial;
        sunMaterial.twoSided = true;
        sunMaterial.pipeline = pipelineSun_;
        sunMaterial.pipelineW = pipelineSunW_;
        allPlanets[i] = allPlanets[Sun];
        scene_.createMaterial(allPlanets[i], sunMaterial);
        scene_.createMesh(allPlanets[i], std::make_shared<Mesh>(Mesh{GeometryShapes::createIcoSphere(vec3(0), kPlanets[i].radius, 4)}));
        SceneNode* sunCorona = allPlanets[i]->createNode(glm::rotate(mat4(1.0f), glm::radians(90.0f), vec3(1, 0, 0)));
        scene_.createMesh(sunCorona, std::make_shared<Mesh>(Mesh{GeometryShapes::createQuad2D(vec2(-0.8f), vec2(0.8f), 0.0f)}));
        scene_.createMaterial(sunCorona,
                              Material{
                                  .texEmissive = loadTexture(ctx, "768_sun_corona.jpg"),
                                  .isTransparent = true,
                                  .pipeline = pipelineSunCorona_,
                              });
      } else if (isMoon) {
        allPlanets[i] = allPlanets[Earth]->createNode();
        scene_.animators.push_back(OrbitAnimationGroup{allPlanets[i],
                                                       {
                                                           {Z, 0.0f, kPlanets[i].globalOrbitalSpeed, kPlanets[i].orbitalRadius},
                                                           {Z, 0.0f, kPlanets[i].localOrbitalSpeed, 0.0f},
                                                           {X, kPlanets[i].axialTilt, 0.0f, 0.0f},
                                                           {X, kPlanets[i].orbitalInclination, 0.0f, 0.0f},
                                                       }});
        scene_.createMesh(allPlanets[i], std::make_shared<Mesh>(Mesh{GeometryShapes::createIcoSphere(vec3(0), kPlanets[i].radius, 3)}));
        scene_.createMaterial(allPlanets[i], planetMaterial);
      } else {
        allPlanets[i] = allPlanets[Sun]->createNode(glm::translate(mat4(1.0f), vec3(0.0f, kPlanets[i].orbitalRadius, 0.0f)));
        scene_.animators.push_back(OrbitAnimationGroup{allPlanets[i],
                                                       {
                                                           {Z, 0.0f, kPlanets[i].localOrbitalSpeed, 0.0f},
                                                           {X, kPlanets[i].axialTilt, 0.0f, kPlanets[i].orbitalRadius},
                                                           {Z, 0.0f, kPlanets[i].globalOrbitalSpeed, 0.0f},
                                                           {X, kPlanets[i].orbitalInclination, 0.0f, 0.0f},
                                                       }});
        scene_.createMaterial(allPlanets[i], planetMaterial);
        scene_.createMesh(allPlanets[i], std::make_shared<Mesh>(Mesh{GeometryShapes::createIcoSphere(vec3(0), kPlanets[i].radius, 3)}));
      }
    }

    if (g_DrawPlanetOrbits) {
      const Material orbitMaterial = {.isTransparent = true, .pipeline = pipelineOrbit_};
      for (size_t i = 0; i < kPlanets.size(); ++i) {
        SceneNode* orbit = allPlanets[Sun]->createNode(glm::rotate(mat4(1.0f), glm::radians(kPlanets[i].orbitalInclination), X));
        scene_.createMaterial(orbit, orbitMaterial);
        scene_.createMesh(orbit, std::make_shared<Mesh>(Mesh{GeometryShapes::createOrbit(kPlanets[i].orbitalRadius, 128)}));
      }
      SceneNode* orbit = allPlanets[Earth]->createNode();
      scene_.createMaterial(orbit, orbitMaterial);
      scene_.createMesh(orbit, std::make_shared<Mesh>(Mesh{GeometryShapes::createOrbit(kPlanets[Moon].orbitalRadius, 64)}));
    }

    {
      const Material diskMaterial = {
          .emissive = vec4(0.7f, 0.7f, 0.7f, 0.3f),
          .diffuse = vec4(1.0f, 1.0f, 1.0f, 0.3f),
          .texEmissive = loadTexture(ctx, "1k_saturn_rings.jpg"),
          .texDiffuse = loadTexture(ctx, "1k_saturn_rings.jpg"),
          .isTransparent = true,
          .pipeline = pipelineSaturnRings_,
      };
      SceneNode* disk = allPlanets[Saturn]->createNode();
      scene_.createMaterial(disk, diskMaterial);
      scene_.createMesh(disk,
                        std::make_shared<Mesh>(Mesh{GeometryShapes::createDisk(1.5f * g_Scale * 70.0f, 1.5f * g_Scale * 130.0f, 100)}));
    }

    {
      const Material asteroidMat1 = {
          .emissive = vec4(0.3f, 0.3f, 0.3f, 1.0f),
          .diffuse = vec4(0.5f, 0.5f, 0.5f, 1.0f),
          .texEmissive = loadTexture(ctx, "2k_makemake_fictional.jpg"),
          .texDiffuse = loadTexture(ctx, "2k_makemake_fictional.jpg"),
          .pipeline = pipelineDefault_,
          .pipelineW = pipelineDefaultW_,
      };
      const Material asteroidMat2 = {
          .emissive = vec4(0.4f, 0.3f, 0.3f, 1.0f),
          .diffuse = vec4(0.45f, 0.45f, 0.55f, 1.0f),
          .texEmissive = loadTexture(ctx, "2k_makemake_fictional.jpg"),
          .texDiffuse = loadTexture(ctx, "2k_makemake_fictional.jpg"),
          .pipeline = pipelineDefault_,
          .pipelineW = pipelineDefaultW_,
      };
      SceneNode* parentAsteroid = allPlanets[Sun]->createNode();
      const std::shared_ptr<Mesh> asteroidMesh = std::make_shared<Mesh>(Mesh{loadMesh("deimos.obj")});

      LRandom rng;
      auto randomFloat = [&rng](float lo, float hi) { return rng.randomInRange(lo, hi); };
      auto randomVec = [&rng](const vec3& lo, const vec3& hi) { return rng.randomVector3InRange(lo, hi); };

      for (size_t i = 0; i < kNumAsteroidsInner; ++i) {
        const float radius = randomFloat(g_Scale * 900.0f, g_Scale * 1250.0f);
        const float globalSpeed = randomFloat(0.5f, 2.5f);
        const float localSpeed = randomFloat(1.0f, 5.5f);
        const float gRotAngle = randomFloat(0.0f, 360.0f);
        const float lRotAngle = randomFloat(0.0f, 360.0f);
        const vec3 randomRotAxis = glm::normalize(randomVec(vec3(0), vec3(1)));
        SceneNode* node = parentAsteroid->createNode();
        scene_.animators.push_back(OrbitAnimationGroup{node,
                                                       {
                                                           {randomRotAxis, lRotAngle, 20.0f * localSpeed, 0.0f},
                                                           {Z, 0.0f, 0.0f, radius},
                                                           {Z, gRotAngle, globalSpeed, 0.0f},
                                                           {Z, 0.0f, 0.0f, 0.0f},
                                                       }});
        SceneNode* subNode = node->createNode(glm::scale(mat4(1.0f), g_Scale * vec3(randomFloat(0.05f, 0.15f))));
        scene_.createMesh(subNode, asteroidMesh);
        scene_.createMaterial(subNode, randomFloat(0.0f, 1.0f) < 0.75f ? asteroidMat1 : asteroidMat2);
      }
      for (size_t i = 0; i < kNumAsteroidsOuter; ++i) {
        const float radius = randomFloat(g_Scale * 1700.0f, g_Scale * 1900.0f);
        const float globalSpeed = randomFloat(0.7f, 3.4f);
        const float localSpeed = randomFloat(0.5f, 2.5f);
        const float gRotAngle = randomFloat(0.0f, 360.0f);
        const float lRotAngle = randomFloat(0.0f, 360.0f);
        const vec3 randomRotAxis = glm::normalize(randomVec(vec3(0), vec3(1)));
        SceneNode* node = parentAsteroid->createNode();
        scene_.animators.push_back(OrbitAnimationGroup{node,
                                                       {
                                                           {randomRotAxis, lRotAngle, 10.0f * localSpeed, 0.0f},
                                                           {Z, 0.0f, 0.0f, radius},
                                                           {Z, gRotAngle, globalSpeed, 0.0f},
                                                           {Z, 0.0f, 0.0f, 0.0f},
                                                       }});
        SceneNode* subNode = node->createNode(glm::scale(mat4(1.0f), g_Scale * vec3(randomFloat(0.1f, 0.3f))));
        scene_.createMesh(subNode, asteroidMesh);
        scene_.createMaterial(subNode, randomFloat(0.0f, 1.0f) < 0.25f ? asteroidMat1 : asteroidMat2);
      }
    }

    scene_.updateAnimations(150.0f);
  }

  void buildBuffers(lvk::metal::IMetalContext& ctx) {
    std::vector<RenderOp> flat;
    flat.reserve(scene_.meshes.size());
    std::vector<GeometryShapes::Vertex> allVertices;

    for (size_t i = 0; i != scene_.meshes.size(); ++i) {
      MeshComponent& mc = scene_.meshes[i];
      const int materialIdx = mc.sceneNode->getMaterialIndexOrParent();
      if (mc.mesh->firstVertex == -1) {
        mc.mesh->firstVertex = int(allVertices.size());
        allVertices.insert(allVertices.end(), mc.mesh->vertices.begin(), mc.mesh->vertices.end());
      }
      const Material& m = scene_.materials[materialIdx];
      flat.push_back(RenderOp{
          .pipeline = m.pipeline,
          .pipelineW = m.pipelineW ? m.pipelineW : m.pipeline,
          .firstVertex = uint32_t(mc.mesh->firstVertex),
          .numVertices = uint32_t(mc.mesh->vertices.size()),
          .drawData = {.idMatrix = uint32_t(i), .idMaterial = uint32_t(materialIdx)},
      });
    }

    bufVertices_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = sizeof(GeometryShapes::Vertex) * allVertices.size(),
        .data = allVertices.data(),
        .debugName = "Buffer: vertices",
    });
    addrVertices_ = ctx.gpuAddress(bufVertices_);

    std::sort(flat.begin(), flat.end(), [&mm = scene_.materials](const RenderOp& a, const RenderOp& b) {
      if (mm[a.drawData.idMaterial].isTransparent != mm[b.drawData.idMaterial].isTransparent) {
        return mm[a.drawData.idMaterial].isTransparent > mm[b.drawData.idMaterial].isTransparent;
      }
      if (a.pipeline.index() != b.pipeline.index()) {
        return a.pipeline.index() < b.pipeline.index();
      }
      return a.firstVertex < b.firstVertex;
    });

    for (const RenderOp& op : flat) {
      if (scene_.materials[op.drawData.idMaterial].isTransparent) {
        renderQueueTransparent_.push_back(op);
      } else {
        renderQueueOpaque_.push_back(op);
      }
    }

    std::vector<DrawData> drawData;
    for (RenderOp& op : renderQueueOpaque_) {
      op.idDrawData = uint32_t(drawData.size());
      drawData.push_back(op.drawData);
    }
    for (RenderOp& op : renderQueueTransparent_) {
      op.idDrawData = uint32_t(drawData.size());
      drawData.push_back(op.drawData);
    }
    bufDrawData_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = sizeof(DrawData) * drawData.size(),
        .data = drawData.data(),
        .debugName = "Buffer: drawData",
    });
    addrDrawData_ = ctx.gpuAddress(bufDrawData_);

    renderQueueOpaque_ = batch(renderQueueOpaque_);
    renderQueueTransparent_ = batch(renderQueueTransparent_);

    std::vector<GpuMaterial> materials;
    materials.reserve(scene_.materials.size());
    for (const Material& m : scene_.materials) {
      materials.push_back(GpuMaterial{
          .emissive = m.emissive,
          .diffuse = vec4(vec3(m.diffuse), m.twoSided ? 1.0f : 0.0f),
          .texEmissive = m.texEmissive.index(),
          .texDiffuse = m.texDiffuse.index(),
      });
    }
    bufMaterials_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = sizeof(GpuMaterial) * materials.size(),
        .data = materials.data(),
        .debugName = "Buffer: materials",
    });
    addrMaterials_ = ctx.gpuAddress(bufMaterials_);

    const uint64_t matBytes = uint64_t(scene_.meshes.size()) * sizeof(mat4);
    for (uint32_t i = 0; i < kRing; ++i) {
      bufModel_[i] = ctx.createBuffer(
          {.usage = lvk::BufferUsageBits_Storage, .storage = lvk::StorageType_HostVisible, .size = matBytes, .debugName = "Buffer: model"});
      bufNormal_[i] = ctx.createBuffer(
          {.usage = lvk::BufferUsageBits_Storage, .storage = lvk::StorageType_HostVisible, .size = matBytes, .debugName = "Buffer: normal"});
      bufPerFrame_[i] = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage,
                                          .storage = lvk::StorageType_HostVisible,
                                          .size = sizeof(PerFrame) + 16,
                                          .debugName = "Buffer: perFrame"});
      addrModel_[i] = ctx.gpuAddress(bufModel_[i]);
      addrNormal_[i] = ctx.gpuAddress(bufNormal_[i]);
      addrPerFrame_[i] = ctx.gpuAddress(bufPerFrame_[i]);
      ptrModel_[i] = ctx.getMappedPtr(bufModel_[i]);
      ptrNormal_[i] = ctx.getMappedPtr(bufNormal_[i]);
      ptrPerFrame_[i] = ctx.getMappedPtr(bufPerFrame_[i]);
    }
  }

  static std::vector<RenderOp> batch(const std::vector<RenderOp>& ops) {
    if (ops.size() < 2) {
      return ops;
    }
    std::vector<RenderOp> out;
    out.reserve(ops.size());
    out.push_back(ops[0]);
    for (size_t i = 1; i != ops.size(); ++i) {
      const RenderOp& prev = ops[i - 1];
      const RenderOp& cur = ops[i];
      const bool samePipeline = prev.pipeline == cur.pipeline;
      const bool sameMesh = prev.firstVertex == cur.firstVertex;
      const bool sequential = prev.idDrawData + 1 == cur.idDrawData;
      if (samePipeline && sameMesh && sequential) {
        out.back().numInstances++;
      } else {
        out.push_back(cur);
      }
    }
    return out;
  }

  lvk::TextureHandle loadTexture(lvk::metal::IMetalContext& ctx, const char* name) {
    const std::unordered_map<std::string, lvk::Holder<lvk::TextureHandle>>::iterator it = textures_.find(name);
    if (it != textures_.end()) {
      return it->second;
    }
    const std::string path = std::string(LVK_METAL_SAMPLE_CONTENT_DIR) + "/" + name;
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!pixels) {
      LLOGE("SolarSystem: failed to load %s", path.c_str());
      return {};
    }
    lvk::Holder<lvk::TextureHandle> tex = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_RGBA_UN8,
        .dimensions = {uint32_t(w), uint32_t(h)},
        .usage = lvk::TextureUsageBits_Sampled,
        .storage = lvk::StorageType_Device,
        .data = pixels,
        .debugName = name,
    });
    stbi_image_free(pixels);
    const lvk::TextureHandle handle = tex;
    textures_[name] = std::move(tex);
    return handle;
  }

  lvk::Holder<lvk::TextureHandle> loadTextureCube(lvk::metal::IMetalContext& ctx, const char* name) {
    const std::string path = std::string(LVK_METAL_SAMPLE_CONTENT_DIR) + "/" + name;
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!pixels) {
      LLOGE("SolarSystem: failed to load %s", path.c_str());
      return {};
    }
    const Bitmap in(w, h, 4, eBitmapFormat_UnsignedByte, pixels);
    stbi_image_free(pixels);
    const Bitmap cube = convertEquirectangularMapToCubeMapFaces(in);
    const uint32_t fw = uint32_t(cube.w_);
    const uint32_t fh = uint32_t(cube.h_);
    lvk::Holder<lvk::TextureHandle> tex = ctx.createTexture({
        .type = lvk::TextureType_Cube,
        .format = lvk::Format_RGBA_UN8,
        .dimensions = {fw, fh},
        .usage = lvk::TextureUsageBits_Sampled,
        .storage = lvk::StorageType_Device,
        .debugName = name,
    });
    const size_t faceBytes = size_t(fw) * fh * 4;
    for (uint32_t face = 0; face < 6; ++face) {
      ctx.upload(tex, {.dimensions = {fw, fh, 1}, .layer = face}, cube.data_.data() + face * faceBytes);
    }
    return tex;
  }

  static std::vector<GeometryShapes::Vertex> loadMesh(const char* name) {
    const std::string path = std::string(LVK_METAL_SAMPLE_CONTENT_DIR) + "/" + name;
    fastObjMesh* mesh = fast_obj_read(path.c_str());
    std::vector<GeometryShapes::Vertex> result;
    if (!mesh) {
      LLOGE("SolarSystem: failed to load mesh %s", path.c_str());
      return result;
    }
    uint32_t vertexIndex = 0;
    for (uint32_t face = 0; face < mesh->face_count; ++face) {
      for (uint32_t v = 0; v < mesh->face_vertices[face]; ++v) {
        const fastObjIndex gi = mesh->indices[vertexIndex++];
        const float* p = &mesh->positions[gi.p * 3];
        const float* n = &mesh->normals[gi.n * 3];
        const float* t = &mesh->texcoords[gi.t * 2];
        result.push_back(GeometryShapes::Vertex{
            .pos = vec3(p[0], p[1], p[2]),
            .uv = vec2(t[0], t[1]),
            .normal = vec3(n[0], n[1], n[2]),
        });
      }
    }
    fast_obj_destroy(mesh);
    return result;
  }

  static void keyCallback(GLFWwindow* window, int key, int, int action, int mods) {
    SolarSystem* self = static_cast<SolarSystem*>(glfwGetWindowUserPointer(window));
    if (!self) {
      return;
    }
    if (ImGui::GetIO().WantCaptureKeyboard) {
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
    case GLFW_KEY_SPACE:
      if (action == GLFW_PRESS) {
        self->positioner_.lookAt(vec3(0.8f, -1.6f, 0.6f), vec3(-1.5f, 1.2f, -0.6f), vec3(0, 0, 1));
      }
      break;
    case GLFW_KEY_X:
      if (action == GLFW_PRESS) {
        g_Wireframe = !g_Wireframe;
      }
      break;
    case GLFW_KEY_P:
      if (action == GLFW_PRESS) {
        g_Paused = !g_Paused;
      }
      break;
    default:
      break;
    }
  }

  static void mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    SolarSystem* self = static_cast<SolarSystem*>(glfwGetWindowUserPointer(window));
    if (self && button == GLFW_MOUSE_BUTTON_LEFT) {
      self->mousePressedLeft_ = action == GLFW_PRESS;
    }
  }

  static void cursorPosCallback(GLFWwindow* window, double x, double y) {
    SolarSystem* self = static_cast<SolarSystem*>(glfwGetWindowUserPointer(window));
    if (!self) {
      return;
    }
    int w = 0, h = 0;
    glfwGetWindowSize(window, &w, &h);
    self->mouseX_ = w > 0 ? float(x) / float(w) : 0.0f;
    self->mouseY_ = h > 0 ? 1.0f - float(y) / float(h) : 0.0f;
  }

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  GLFWwindow* window_ = nullptr;
  float lastTime_ = -1.0f;
  uint32_t frameIdx_ = 0;

  float mouseX_ = 0.0f;
  float mouseY_ = 0.0f;
  bool mousePressedLeft_ = false;
  CameraPositioner_FirstPerson positioner_;

  Scene scene_;
  std::vector<RenderOp> renderQueueOpaque_;
  std::vector<RenderOp> renderQueueTransparent_;
  std::unordered_map<std::string, lvk::Holder<lvk::TextureHandle>> textures_;

  lvk::Holder<lvk::ShaderModuleHandle> defaultVert_, defaultFrag_;
  lvk::Holder<lvk::ShaderModuleHandle> sunVert_, sunFrag_;
  lvk::Holder<lvk::ShaderModuleHandle> coronaVert_, coronaFrag_;
  lvk::Holder<lvk::ShaderModuleHandle> orbitVert_, orbitFrag_;
  lvk::Holder<lvk::ShaderModuleHandle> skyVert_, skyFrag_;

  lvk::Holder<lvk::RenderPipelineHandle> pipelineDefault_, pipelineDefaultW_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineSaturnRings_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineOrbit_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineSun_, pipelineSunW_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineSunCorona_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineSky_;

  lvk::Holder<lvk::SamplerHandle> sampler_;
  lvk::Holder<lvk::TextureHandle> skyTex_;
  lvk::Holder<lvk::TextureHandle> depth_;

  lvk::Holder<lvk::BufferHandle> bufVertices_;
  lvk::Holder<lvk::BufferHandle> bufDrawData_;
  lvk::Holder<lvk::BufferHandle> bufMaterials_;
  lvk::Holder<lvk::BufferHandle> bufModel_[kRing];
  lvk::Holder<lvk::BufferHandle> bufNormal_[kRing];
  lvk::Holder<lvk::BufferHandle> bufPerFrame_[kRing];

  uint64_t addrVertices_ = 0;
  uint64_t addrDrawData_ = 0;
  uint64_t addrMaterials_ = 0;
  uint64_t addrModel_[kRing] = {};
  uint64_t addrNormal_[kRing] = {};
  uint64_t addrPerFrame_[kRing] = {};
  uint8_t* ptrModel_[kRing] = {};
  uint8_t* ptrNormal_[kRing] = {};
  uint8_t* ptrPerFrame_[kRing] = {};

  std::unique_ptr<lvk::metal::ImGuiRenderer> imgui_;
};

DESKTOP_MAIN(SolarSystem)
