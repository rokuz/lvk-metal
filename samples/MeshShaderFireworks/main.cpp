#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <GLFW/glfw3.h>

#include <lmath/Random.h>

#include <lvk/HelpersImGuiMetal.h>
#include <lvk/LVK-Metal.h>

#include "app_common.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

using glm::mat4;
using glm::vec2;
using glm::vec3;
using glm::vec4;

static const char* kShaderMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Vertex {
  packed_float3 position;
  packed_float3 color;
  float flare;
};

struct PerFrame {
  float4x4 proj;
  float4x4 view;
};

struct PushConstants {
  device const PerFrame* perFrame;
  device const Vertex* vb;
  uint texture;
  uint sampler;
};

struct VertexOut {
  float4 position [[position]];
  float3 color;
  float2 uv;
};

using FireworksMesh = metal::mesh<VertexOut, void, 128, 64, metal::topology::triangle>;

constant float2 kOffs[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};

[[mesh, max_total_threads_per_threadgroup(32), clang::annotate("lvk_numthreads(32,1,1)")]]
void meshMain(FireworksMesh outMesh,
              constant PushConstants& pc [[buffer(0)]],
              uint3 gid [[threadgroup_position_in_grid]],
              uint3 tid [[thread_position_in_threadgroup]]) {
  outMesh.set_primitive_count(64);

  const uint particleIdx = gid.x * 32 + tid.x;
  const uint vertOff = tid.x * 4;
  const uint triOff = tid.x * 2;

  const float4x4 proj = pc.perFrame->proj;
  const float4x4 view = pc.perFrame->view;
  const Vertex v = pc.vb[particleIdx];
  const float4 center = view * float4(float3(v.position), 1.0);

  const float2 size = v.flare > 0.5 ? float2(0.08, 0.4) : float2(0.2, 0.2);
  const float3 color = v.flare > 0.5 ? 0.5 * float3(v.color) : float3(v.color);

  for (uint i = 0; i < 4; i++) {
    const float4 offset = float4(size * kOffs[i], 0, 0);
    VertexOut o;
    o.position = proj * (center + offset);
    o.color = color;
    o.uv = (kOffs[i] + 1.0) * 0.5;
    outMesh.set_vertex(vertOff + i, o);
  }

  outMesh.set_index((triOff + 0) * 3 + 0, vertOff + 0);
  outMesh.set_index((triOff + 0) * 3 + 1, vertOff + 1);
  outMesh.set_index((triOff + 0) * 3 + 2, vertOff + 2);
  outMesh.set_index((triOff + 1) * 3 + 0, vertOff + 2);
  outMesh.set_index((triOff + 1) * 3 + 1, vertOff + 1);
  outMesh.set_index((triOff + 1) * 3 + 2, vertOff + 3);
}

fragment float4 fragmentMain(VertexOut in [[stage_in]], constant PushConstants& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  const float alpha = textureBindless2D(pc.texture, pc.sampler, in.uv).r;
  return float4(in.color, alpha);
}
)MSL";

LRandom g_rng;

float random(float x) {
  return g_rng.randomInRange(0.0f, x);
}

float randomRange(float lo, float hi) {
  return g_rng.randomInRange(lo, hi);
}

const int kMaxParticles = 50000;
const int kStackSize = 50000;
const int kParticlesPerWorkgroup = 32;
const int kMaxParticlesPadded = ((kMaxParticles + kParticlesPerWorkgroup - 1) / kParticlesPerWorkgroup) * kParticlesPerWorkgroup;

vec3 g_Gravity = {0, -0.001, 0};
float g_AirResistance = 0.98f;
bool g_Pause = false;

struct ColorPalette {
  vec3 primary;
  vec3 secondary;
  vec3 sparkle;
};

const ColorPalette g_Palettes[] = {
    {{1.0f, 0.3f, 0.1f}, {1.0f, 0.6f, 0.2f}, {1.0f, 1.0f, 0.4f}},
    {{0.2f, 0.4f, 1.0f}, {0.4f, 0.7f, 1.0f}, {0.4f, 0.9f, 1.0f}},
    {{0.1f, 0.9f, 0.3f}, {0.3f, 1.0f, 0.5f}, {0.3f, 1.0f, 0.8f}},
    {{0.7f, 0.2f, 0.9f}, {0.9f, 0.4f, 1.0f}, {1.0f, 0.4f, 1.0f}},
    {{1.0f, 0.8f, 0.2f}, {1.0f, 0.9f, 0.5f}, {1.0f, 1.0f, 0.5f}},
    {{1.0f, 0.2f, 0.6f}, {1.0f, 0.5f, 0.8f}, {1.0f, 0.5f, 1.0f}},
    {{0.1f, 0.9f, 0.9f}, {0.3f, 1.0f, 1.0f}, {0.4f, 1.0f, 1.0f}},
    {{0.7f, 0.2f, 0.1f}, {0.9f, 0.5f, 0.1f}, {1.0f, 0.5f, 0.3f}},
};

enum ExplosionType {
  ExplosionType_Sphere,
  ExplosionType_Ring,
  ExplosionType_DoubleRing,
  ExplosionType_Willow,
  ExplosionType_Chrysanthemum,
  ExplosionType_Crackle,
  ExplosionType_Palm,
  ExplosionType_Crossette,
  NumExplosionTypes,
};

enum ParticleStateMessage {
  PSM_None = 0,
  PSM_Kill = 1,
  PSM_Emission = 2,
};

struct Particle {
  vec3 pos = vec3(0.0f);
  vec3 velocity = vec3(0.0f);
  vec3 baseColor = vec3(0.0f);
  vec3 currentColor = vec3(0.0f);

  int TTL = 0;
  int initialLT = 1;

  bool alive = false;
  bool flare = false;
  bool spawnExplosion = false;

  bool fadingOut = false;
  bool emission = false;

  float brightness = 1.0f;
  int explosionType = ExplosionType_Sphere;
  int paletteIndex = 0;
  int secondaryExplosionTimer = -1;

  ParticleStateMessage update() {
    pos += velocity;
    velocity *= g_AirResistance;
    velocity += g_Gravity;
    TTL--;

    if (fadingOut) {
      const float t = static_cast<float>(TTL) / initialLT;
      currentColor = baseColor * t * t * brightness;
    }

    if (secondaryExplosionTimer > 0) {
      secondaryExplosionTimer--;
      if (secondaryExplosionTimer == 0) {
        spawnExplosion = true;
        return PSM_Kill;
      }
    }

    if (TTL < 0) {
      return PSM_Kill;
    }

    return emission ? PSM_Emission : PSM_None;
  }
};

struct ParticleSystem {
  Particle particles[kMaxParticles];
  Particle particlesStack[kStackSize];
  int totalParticles = 0;
  int queuedParticles = 0;
  vec3 viewerPos = vec3(0.0f);
  bool useViewerFacingExplosions = false;

  void nextFrame() {
    int processedParticles = 0;

    for (int i = 0; i < kMaxParticles; i++) {
      if (particles[i].alive) {
        processedParticles++;

        switch (particles[i].update()) {
        case PSM_None:
          break;
        case PSM_Kill:
          if (particles[i].spawnExplosion) {
            addExplosion(particles[i].pos, particles[i].explosionType, particles[i].paletteIndex);
          }
          particles[i].alive = false;
          totalParticles--;
          break;
        case PSM_Emission:
          if (random(1.0f) < 0.7f) {
            const vec3 trailColor = particles[i].currentColor * 0.6f;
            const int trailTTL = particles[i].TTL >> 3;
            addParticle({
                .pos = particles[i].pos,
                .velocity = particles[i].velocity * randomRange(0.02f, 0.1f),
                .baseColor = trailColor,
                .currentColor = trailColor,
                .TTL = trailTTL,
                .initialLT = trailTTL,
                .alive = true,
                .fadingOut = true,
                .brightness = 0.5f,
            });
          }
          break;
        }
      } else if (queuedParticles > 0) {
        particles[i] = particlesStack[--queuedParticles];
        totalParticles++;
      } else if (processedParticles >= totalParticles) {
        return;
      }
    }
  }

  void addParticle(const Particle& particle) {
    if (queuedParticles < kStackSize) {
      particlesStack[queuedParticles++] = particle;
    }
  }

  void addSphereExplosion(vec3 pos, const ColorPalette& pal, int count, float speed, int lifetime) {
    for (int i = 0; i < count; i++) {
      const float theta = random(float(M_PI) * 2.0f);
      const float phi = acosf(randomRange(-1.0f, 1.0f));
      const float r = speed * powf(random(1.0f), 0.33f);

      const vec3 velocity(r * sinf(phi) * cosf(theta), r * sinf(phi) * sinf(theta), r * cosf(phi));

      vec3 color = glm::mix(pal.primary, pal.secondary, random(1.0f));
      color += vec3(randomRange(-0.1f, 0.1f));
      color = glm::clamp(color, vec3(0.0f), vec3(1.0f));

      const int ttl = lifetime + int(random(30));
      const bool core = random(1.0f) < 0.15f;
      const vec3 finalColor = core ? pal.sparkle : color;
      addParticle({
          .pos = pos,
          .velocity = velocity,
          .baseColor = finalColor,
          .currentColor = finalColor,
          .TTL = ttl,
          .initialLT = ttl,
          .alive = true,
          .fadingOut = true,
          .emission = random(1.0f) < 0.3f,
          .brightness = core ? 1.5f : 1.0f,
      });
    }
  }

  void addRingExplosion(vec3 pos, const ColorPalette& pal, int count, float speed, int lifetime, float tiltX = 0, float tiltZ = 0) {
    vec3 right(1, 0, 0);
    vec3 up(0, 1, 0);
    vec3 viewDir(0, 0, 1);
    if (useViewerFacingExplosions) {
      const vec3 toViewer = viewerPos - pos;
      const float dist = glm::length(toViewer);
      viewDir = dist > 0.001f ? toViewer / dist : vec3(0, 0, 1);
      const vec3 ref = fabsf(viewDir.y) < 0.99f ? vec3(0, 1, 0) : vec3(1, 0, 0);
      right = glm::normalize(glm::cross(ref, viewDir));
      up = glm::cross(viewDir, right);
    }

    for (int i = 0; i < count; i++) {
      const float angle = (float(i) / count) * float(M_PI) * 2.0f + random(0.2f);
      const float r = speed * randomRange(0.9f, 1.1f);

      vec3 velocity = right * (r * cosf(angle)) + up * (r * sinf(angle)) + viewDir * random(0.02f);

      const float cx = cosf(tiltX);
      const float sx = sinf(tiltX);
      const float cz = cosf(tiltZ);
      const float sz = sinf(tiltZ);
      const vec3 v1(velocity.x * cx - velocity.y * sx, velocity.x * sx + velocity.y * cx, velocity.z);
      velocity = vec3(v1.x, v1.y, v1.z * cz - v1.y * sz);

      const vec3 color = glm::mix(pal.primary, pal.secondary, random(1.0f));

      const int ttl = lifetime + int(random(20));
      addParticle({
          .pos = pos,
          .velocity = velocity,
          .baseColor = color,
          .currentColor = color,
          .TTL = ttl,
          .initialLT = ttl,
          .alive = true,
          .fadingOut = true,
          .emission = true,
      });
    }
  }

  void addWillowExplosion(vec3 pos, const ColorPalette& pal, int count) {
    for (int i = 0; i < count; i++) {
      const float theta = random(float(M_PI) * 2.0f);
      const float phi = randomRange(0.3f, float(M_PI) * 0.7f);
      const float r = randomRange(0.06f, 0.12f);

      const vec3 velocity(r * sinf(phi) * cosf(theta), r * cosf(phi) + 0.05f, r * sinf(phi) * sinf(theta));
      const vec3 color = glm::mix(pal.primary, pal.sparkle, random(0.5f));

      const int ttl = 150 + int(random(50));
      addParticle({
          .pos = pos,
          .velocity = velocity,
          .baseColor = color,
          .currentColor = color,
          .TTL = ttl,
          .initialLT = ttl,
          .alive = true,
          .fadingOut = true,
          .emission = true,
          .brightness = randomRange(0.8f, 1.2f),
      });
    }
  }

  void addChrysanthemumExplosion(vec3 pos, const ColorPalette& pal, int count) {
    for (int i = 0; i < count; i++) {
      const float theta = random(float(M_PI) * 2.0f);
      const float phi = acosf(randomRange(-1.0f, 1.0f));
      const float r = randomRange(0.08f, 0.14f);

      const vec3 velocity(r * sinf(phi) * cosf(theta), r * sinf(phi) * sinf(theta), r * cosf(phi));
      const vec3 color = (random(1.0f) < 0.3f) ? pal.sparkle : pal.primary;

      const int ttl = 100 + int(random(40));
      addParticle({
          .pos = pos,
          .velocity = velocity,
          .baseColor = color,
          .currentColor = color,
          .TTL = ttl,
          .initialLT = ttl,
          .alive = true,
          .fadingOut = true,
          .emission = true,
      });
    }

    for (int i = 0; i < 30; i++) {
      const float theta = random(float(M_PI) * 2.0f);
      const float phi = acosf(randomRange(-1.0f, 1.0f));
      const float r = randomRange(0.03f, 0.05f);

      const vec3 velocity(r * sinf(phi) * cosf(theta), r * sinf(phi) * sinf(theta), r * cosf(phi));

      const int sparkleTTL = 30 + int(random(20));
      addParticle({
          .pos = pos,
          .velocity = velocity,
          .baseColor = pal.sparkle,
          .currentColor = pal.sparkle,
          .TTL = sparkleTTL,
          .initialLT = sparkleTTL,
          .alive = true,
          .fadingOut = true,
          .brightness = 2.0f,
      });
    }
  }

  void addCrackleExplosion(vec3 pos, const ColorPalette& pal, int count) {
    addSphereExplosion(pos, pal, count / 2, 0.08f, 60);

    for (int i = 0; i < 25; i++) {
      const float theta = random(float(M_PI) * 2.0f);
      const float phi = acosf(randomRange(-1.0f, 1.0f));
      const float r = randomRange(0.06f, 0.1f);

      const vec3 velocity(r * sinf(phi) * cosf(theta), r * sinf(phi) * sinf(theta), r * cosf(phi));

      const int ttl = 50 + int(random(30));
      addParticle({
          .pos = pos,
          .velocity = velocity,
          .baseColor = pal.sparkle,
          .currentColor = pal.sparkle,
          .TTL = ttl,
          .initialLT = ttl,
          .alive = true,
          .spawnExplosion = true,
          .brightness = 1.5f,
          .explosionType = ExplosionType_Sphere,
          .paletteIndex = static_cast<int>(random(LVK_ARRAY_NUM_ELEMENTS(g_Palettes))),
      });
    }
  }

  void addPalmExplosion(vec3 pos, const ColorPalette& pal, int count) {
    for (int i = 0; i < count; i++) {
      const float angle = random(float(M_PI) * 2.0f);
      const float spread = randomRange(0.02f, 0.06f);
      const float upward = randomRange(0.1f, 0.15f);

      const vec3 velocity(spread * cosf(angle), upward, spread * sinf(angle));
      const vec3 color = glm::mix(pal.primary, pal.secondary, random(1.0f));

      const int ttl = 120 + int(random(40));
      addParticle({
          .pos = pos,
          .velocity = velocity,
          .baseColor = color,
          .currentColor = color,
          .TTL = ttl,
          .initialLT = ttl,
          .alive = true,
          .fadingOut = true,
          .emission = true,
      });
    }

    for (int i = 0; i < 50; i++) {
      const float angle = random(float(M_PI) * 2.0f);
      const float spread = randomRange(0.03f, 0.08f);

      const vec3 velocity(spread * cosf(angle), 0.08f + random(0.04f), spread * sinf(angle));

      const int tipTTL = 80 + int(random(30));
      addParticle({
          .pos = pos,
          .velocity = velocity,
          .baseColor = pal.sparkle,
          .currentColor = pal.sparkle,
          .TTL = tipTTL,
          .initialLT = tipTTL,
          .alive = true,
          .fadingOut = true,
          .brightness = 1.3f,
      });
    }
  }

  void addCrossetteExplosion(vec3 pos, const ColorPalette& pal, int count) {
    for (int i = 0; i < count; i++) {
      const float theta = (float(i) / count) * float(M_PI) * 2.0f;
      const float r = randomRange(0.08f, 0.12f);

      const vec3 velocity(r * cosf(theta), random(0.04f), r * sinf(theta));

      addParticle({
          .pos = pos,
          .velocity = velocity,
          .baseColor = pal.primary,
          .currentColor = pal.primary,
          .TTL = 80,
          .initialLT = 80,
          .alive = true,
          .spawnExplosion = true,
          .fadingOut = true,
          .emission = true,
          .explosionType = ExplosionType_Sphere,
          .paletteIndex = static_cast<int>(random(LVK_ARRAY_NUM_ELEMENTS(g_Palettes))),
          .secondaryExplosionTimer = 30 + int(random(20)),
      });
    }
  }

  void addExplosion(vec3 pos, int type = -1, int paletteIdx = -1) {
    if (type < 0) {
      type = static_cast<int>(random(NumExplosionTypes));
    }
    if (paletteIdx < 0) {
      paletteIdx = static_cast<int>(random(LVK_ARRAY_NUM_ELEMENTS(g_Palettes)));
    }

    const ColorPalette& pal = g_Palettes[paletteIdx % LVK_ARRAY_NUM_ELEMENTS(g_Palettes)];

    switch (type) {
    case ExplosionType_Sphere:
      addSphereExplosion(pos, pal, 350, 0.1f, 80);
      break;
    case ExplosionType_Ring:
      addRingExplosion(pos, pal, 120, 0.12f, 100, random(1.0f), random(1.0f));
      break;
    case ExplosionType_DoubleRing:
      addRingExplosion(pos, pal, 100, 0.1f, 90, 0.5f, 0.0f);
      addRingExplosion(pos, pal, 100, 0.1f, 90, -0.5f, 0.8f);
      addSphereExplosion(pos, {pal.sparkle, pal.sparkle, pal.sparkle}, 50, 0.04f, 40);
      break;
    case ExplosionType_Willow:
      addWillowExplosion(pos, pal, 400);
      break;
    case ExplosionType_Chrysanthemum:
      addChrysanthemumExplosion(pos, pal, 500);
      break;
    case ExplosionType_Crackle:
      addCrackleExplosion(pos, pal, 300);
      break;
    case ExplosionType_Palm:
      addPalmExplosion(pos, pal, 250);
      break;
    case ExplosionType_Crossette:
      addCrossetteExplosion(pos, pal, 12);
      addSphereExplosion(pos, pal, 100, 0.05f, 40);
      break;
    }

    for (int i = 0; i < 20; i++) {
      const int flashTTL = 5 + int(random(5));
      addParticle({
          .pos = pos,
          .velocity = vec3(randomRange(-0.01f, 0.01f), randomRange(-0.01f, 0.01f), randomRange(-0.01f, 0.01f)),
          .baseColor = vec3(1.0f),
          .currentColor = vec3(1.0f),
          .TTL = flashTTL,
          .initialLT = flashTTL,
          .alive = true,
          .fadingOut = true,
          .brightness = 3.0f,
      });
    }
  }

  void launchFirework(vec3 position) {
    const int explosionType = static_cast<int>(random(NumExplosionTypes));
    const int paletteIdx = static_cast<int>(random(LVK_ARRAY_NUM_ELEMENTS(g_Palettes)));
    const ColorPalette& pal = g_Palettes[paletteIdx];

    const vec3 velocity(randomRange(-0.03f, 0.03f), randomRange(0.22f, 0.32f), randomRange(-0.03f, 0.03f));

    const int flareTTL = 30 + int(random(20));
    addParticle({
        .pos = position,
        .velocity = velocity,
        .baseColor = pal.sparkle,
        .currentColor = pal.sparkle,
        .TTL = flareTTL,
        .initialLT = flareTTL,
        .alive = true,
        .flare = true,
        .spawnExplosion = true,
        .brightness = 1.5f,
        .explosionType = explosionType,
        .paletteIndex = paletteIdx,
    });

    const vec3 trailColor = pal.primary * 0.5f;
    for (int i = 0; i < 5; i++) {
      const int trailTTL = 10 + int(random(10));
      addParticle({
          .pos = position,
          .velocity = velocity * randomRange(0.1f, 0.3f),
          .baseColor = trailColor,
          .currentColor = trailColor,
          .TTL = trailTTL,
          .initialLT = trailTTL,
          .alive = true,
          .flare = true,
          .fadingOut = true,
          .brightness = 0.7f,
      });
    }
  }
};

ParticleSystem g_Points;

struct Vertex {
  vec3 pos;
  vec4 color;
};

struct PerFrame {
  mat4 proj;
  mat4 view;
};

void generateParticleTexture(uint8_t* image, int size) {
  const float center = 0.5f * (size - 1);
  const float maxDist = center;

  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      const float dx = x - center;
      const float dy = y - center;
      const float dist = sqrtf(dx * dx + dy * dy);

      const float normalizedDist = dist < maxDist ? dist / maxDist : 1.0f;
      const float falloff = 1.0f - normalizedDist;
      const float value = falloff * falloff * falloff * 255.0f;

      image[y * size + x] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, value)));
    }
  }
}

class MeshShaderFireworks final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow* window, uint32_t width, uint32_t height, float, uint32_t) override {
    ctx_ = &ctx;
    width_ = width;
    height_ = height;
    window_ = window;

    for (lvk::Holder<lvk::BufferHandle>& vb : vb_) {
      vb = ctx.createBuffer({
          .usage = lvk::BufferUsageBits_Storage,
          .storage = lvk::StorageType_HostVisible,
          .size = sizeof(Vertex) * kMaxParticlesPadded,
          .debugName = "Buffer: vertices",
      });
    }

    for (lvk::Holder<lvk::BufferHandle>& buf : bufPerFrame_) {
      buf = ctx.createBuffer({
          .usage = lvk::BufferUsageBits_Storage,
          .storage = lvk::StorageType_HostVisible,
          .size = sizeof(PerFrame),
          .debugName = "Buffer: per frame",
      });
    }

    sampler_ = ctx.createSampler({.debugName = "Sampler: linear"});

    uint8_t particleTextureData[64 * 64];
    generateParticleTexture(particleTextureData, 64);

    texture_ = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_R_UN8,
        .dimensions = {64, 64},
        .usage = lvk::TextureUsageBits_Sampled,
        .data = particleTextureData,
        .debugName = "Particle",
    });

    lvk::Holder<lvk::ShaderModuleHandle> mesh;
    lvk::Holder<lvk::ShaderModuleHandle> frag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      mesh = slang.createShaderModule(ctx, "fireworks", "meshMain", lvk::Stage_Mesh, "fireworks.mesh");
      frag = slang.createShaderModule(ctx, "fireworks", "fragmentMain", lvk::Stage_Frag, "fireworks.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      mesh = ctx.createShaderModule({kShaderMSL, "meshMain", lvk::Stage_Mesh, "mesh"});
      frag = ctx.createShaderModule({kShaderMSL, "fragmentMain", lvk::Stage_Frag, "frag"});
    }

    pipeline_ = ctx.createRenderPipeline({
        .smMesh = mesh,
        .smFrag = frag,
        .color = {{
            .format = ctx.getSwapchainFormat(),
            .blendEnabled = true,
            .rgbBlendOp = lvk::BlendOp_Add,
            .alphaBlendOp = lvk::BlendOp_Add,
            .srcRGBBlendFactor = lvk::BlendFactor_SrcAlpha,
            .srcAlphaBlendFactor = lvk::BlendFactor_SrcAlpha,
            .dstRGBBlendFactor = lvk::BlendFactor_One,
            .dstAlphaBlendFactor = lvk::BlendFactor_One,
        }},
        .cullMode = lvk::CullMode_None,
        .debugName = "Pipeline: mesh",
    });

    if (window_) {
      glfwSetWindowUserPointer(window_, this);
      glfwSetKeyCallback(window_, keyCallback);
    }

    imgui_ = std::make_unique<lvk::metal::ImGuiRenderer>(ctx, window_);

    vertices_.reserve(kMaxParticles);
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float time) override {
    const float delta = time - lastTime_;
    lastTime_ = time;

    if (!g_Pause) {
      accTime_ += delta;
    }

    uint32_t steps = 0;
    while (accTime_ >= kTimeQuantum) {
      accTime_ -= kTimeQuantum;
      launchTimer_ += kTimeQuantum;
      g_Points.nextFrame();
      if (launchTimer_ >= nextLaunch_) {
        launchTimer_ = 0.0f;
        nextLaunch_ = randomRange(0.7f, 1.8f);
        const vec3 position(randomRange(-5.0f, 5.0f), -6.0f, randomRange(-2.0f, 2.0f));
        g_Points.launchFirework(position);
      }
      ++steps;
    }

    if (steps > 0) {
      vertices_.clear();
      for (int i = 0; i < kMaxParticles; i++) {
        if (g_Points.particles[i].alive) {
          const Particle& p = g_Points.particles[i];
          vertices_.push_back(Vertex{
              .pos = p.pos,
              .color = vec4(p.currentColor, p.flare ? 1.0f : 0.0f),
          });
        }
      }
      if (!vertices_.empty()) {
        const size_t paddedSize = (vertices_.size() + kParticlesPerWorkgroup - 1) & ~size_t(kParticlesPerWorkgroup - 1);
        vertices_.resize(paddedSize, Vertex{.pos = vec3(0), .color = vec4(0)});
        bufferIndex_ = (bufferIndex_ + 1) % LVK_ARRAY_NUM_ELEMENTS(vb_);
        std::memcpy(ctx_->getMappedPtr(vb_[bufferIndex_]), vertices_.data(), sizeof(Vertex) * vertices_.size());
      }
    }

    const uint32_t r = frameRing_;
    frameRing_ = (frameRing_ + 1) % LVK_ARRAY_NUM_ELEMENTS(bufPerFrame_);

    const PerFrame perFrame = {
        .proj = glm::perspective(glm::radians(90.0f), float(width_) / float(height_), 0.1f, 100.0f),
        .view = glm::translate(mat4(1.0f), vec3(0.0f, 0.0f, -8.0f)),
    };
    std::memcpy(ctx_->getMappedPtr(bufPerFrame_[r]), &perFrame, sizeof(perFrame));

    const lvk::Framebuffer framebuffer = {.color = {{.texture = target}}};

    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}}}},
                          framebuffer);
    cmd.cmdBindRenderPipeline(pipeline_);
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdPushDebugGroupLabel("Render Mesh", 0xff0000ff);
    const struct {
      uint64_t perFrame;
      uint64_t vb;
      uint32_t texture;
      uint32_t sampler;
    } bindings = {
        .perFrame = ctx_->gpuAddress(bufPerFrame_[r]),
        .vb = ctx_->gpuAddress(vb_[bufferIndex_]),
        .texture = texture_.index(),
        .sampler = sampler_.index(),
    };
    cmd.cmdPushConstants(bindings);
    if (!vertices_.empty()) {
      cmd.cmdDrawMeshTasks({uint32_t(vertices_.size() / kParticlesPerWorkgroup), 1, 1});
    }
    cmd.cmdPopDebugGroupLabel();

    imgui_->beginFrame(framebuffer);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 120), ImGuiCond_FirstUseEver);
    ImGui::Begin("Info", nullptr, ImGuiWindowFlags_NoNavInputs);
    ImGui::Text("Particles: %i", g_Points.totalParticles);
    ImGui::Text("1/2 - gravity, Space - pause");
    ImGui::End();

    imgui_->endFrame(cmd);
    cmd.cmdEndRendering();
  }

 private:
  static void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    if (action != GLFW_PRESS || ImGui::GetIO().WantCaptureKeyboard) {
      return;
    }
    if (key == GLFW_KEY_1) {
      g_Gravity.x += 0.001f;
    } else if (key == GLFW_KEY_2) {
      g_Gravity.x -= 0.001f;
    } else if (key == GLFW_KEY_SPACE) {
      g_Pause = !g_Pause;
    }
  }

  static constexpr float kTimeQuantum = 0.02f;

  lvk::metal::IMetalContext* ctx_ = nullptr;
  GLFWwindow* window_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  lvk::Holder<lvk::BufferHandle> vb_[3];
  lvk::Holder<lvk::BufferHandle> bufPerFrame_[3];
  lvk::Holder<lvk::SamplerHandle> sampler_;
  lvk::Holder<lvk::TextureHandle> texture_;
  lvk::Holder<lvk::RenderPipelineHandle> pipeline_;
  std::unique_ptr<lvk::metal::ImGuiRenderer> imgui_;

  std::vector<Vertex> vertices_;
  double accTime_ = 0.0;
  float launchTimer_ = 0.0f;
  float nextLaunch_ = 1.0f;
  float lastTime_ = 0.0f;
  uint32_t bufferIndex_ = 0;
  uint32_t frameRing_ = 0;
};

DESKTOP_MAIN(MeshShaderFireworks)
