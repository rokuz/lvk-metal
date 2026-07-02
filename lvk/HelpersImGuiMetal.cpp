#include "lvk/HelpersImGuiMetal.h"

#include <lvk/LVK-Metal.h>

#if !defined(LVK_METAL_IMGUI_EXTERNAL)
#include "imgui/backends/imgui_impl_glfw.cpp"
#include "imgui/imgui.cpp"
#include "imgui/imgui_draw.cpp"
#include "imgui/imgui_tables.cpp"
#include "imgui/imgui_widgets.cpp"
#else // LVK_METAL_IMGUI_EXTERNAL
#include "imgui/backends/imgui_impl_glfw.h"
#include <GLFW/glfw3.h>
#endif // !defined(LVK_METAL_IMGUI_EXTERNAL)

#include <math.h>
#include <string.h>

static const char* kImGuiMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct ImVtx {
  float x;
  float y;
  float u;
  float v;
  uint rgba;
};

struct ImPush {
  float4 LRTB;
  device const ImVtx* vb;
  uint textureId;
  uint samplerId;
};

struct ImOut {
  float4 position [[position]];
  float4 color;
  float2 uv;
};

vertex ImOut imguiVertexMain(uint vid [[vertex_id]], constant ImPush& pc [[buffer(0)]]) {
  const ImVtx v = pc.vb[vid];
  const float L = pc.LRTB.x;
  const float R = pc.LRTB.y;
  const float T = pc.LRTB.z;
  const float B = pc.LRTB.w;
  const float4x4 proj = float4x4(
    float4(2.0 / (R - L),             0.0,             0.0, 0.0),
    float4(0.0,             2.0 / (T - B),             0.0, 0.0),
    float4(0.0,                       0.0,            -1.0, 0.0),
    float4((R + L) / (L - R), (T + B) / (B - T),       0.0, 1.0));
  ImOut o;
  o.position = proj * float4(v.x, v.y, 0.0, 1.0);
  o.color = unpack_unorm4x8_to_float(v.rgba);
  o.uv = float2(v.u, v.v);
  return o;
}

fragment float4 imguiFragmentMain(ImOut in [[stage_in]], constant ImPush& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  return in.color * textureBindless2D(pc.textureId, pc.samplerId, in.uv);
}
)";

namespace lvk::metal {

ImGuiRenderer::ImGuiRenderer(lvk::IContext& ctx, GLFWwindow* window, const char* defaultFontTTF, float fontSizePixels)
: ImGuiRenderer(ctx, window, nullptr, 0, fontSizePixels) {
  if (defaultFontTTF) {
    ImGui::GetIO().Fonts->Clear();
    updateFont(defaultFontTTF, nullptr, 0, fontSizePixels);
  }
}

ImGuiRenderer::ImGuiRenderer(lvk::IContext& ctx, GLFWwindow* window, const void* fontData, size_t fontDataSize, float fontSizePixels)
: ctx_(ctx)
, window_(window) {
  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.BackendRendererName = "imgui-lvk-metal";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

  if (window_) {
    ImGui_ImplGlfw_InitForOther(window_, true);
  }

  updateFont(nullptr, fontData, fontDataSize, fontSizePixels);

  vert_ = ctx_.createShaderModule({kImGuiMSL, "imguiVertexMain", lvk::Stage_Vert, "imgui.vert"});
  frag_ = ctx_.createShaderModule({kImGuiMSL, "imguiFragmentMain", lvk::Stage_Frag, "imgui.frag"});
  samplerClamp_ = ctx_.createSampler({
      .wrapU = lvk::SamplerWrap_Clamp,
      .wrapV = lvk::SamplerWrap_Clamp,
      .wrapW = lvk::SamplerWrap_Clamp,
      .debugName = "Sampler: imgui",
  });
}

ImGuiRenderer::~ImGuiRenderer() {
  ImGui::GetIO().Fonts->TexRef = ImTextureRef();
  if (window_) {
    ImGui_ImplGlfw_Shutdown();
  }
  ImGui::DestroyContext();
}

void ImGuiRenderer::updateFont(const char* defaultFontTTF, const void* fontData, size_t fontDataSize, float fontSizePixels) {
  ImGuiIO& io = ImGui::GetIO();

  ImFontConfig cfg = ImFontConfig();
  cfg.RasterizerMultiply = 1.5f;
  cfg.SizePixels = ceilf(fontSizePixels);
  cfg.PixelSnapH = true;
  cfg.OversampleH = 4;
  cfg.OversampleV = 4;

  ImFont* font = nullptr;
  if (defaultFontTTF) {
    cfg.FontDataOwnedByAtlas = true;
    font = io.Fonts->AddFontFromFileTTF(defaultFontTTF, cfg.SizePixels, &cfg);
  } else if (fontData && fontDataSize) {
    cfg.FontDataOwnedByAtlas = false;
    font = io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(fontData), int(fontDataSize), cfg.SizePixels, &cfg);
  } else {
    font = io.Fonts->AddFontDefault(&cfg);
  }

  io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
  io.FontDefault = font;
}

lvk::Holder<lvk::RenderPipelineHandle> ImGuiRenderer::createNewPipelineState(const lvk::Framebuffer& desc) {
  return ctx_.createRenderPipeline({
      .smVert = vert_,
      .smFrag = frag_,
      .color = {{
          .format = ctx_.getFormat(desc.color[0].texture),
          .blendEnabled = true,
          .srcRGBBlendFactor = lvk::BlendFactor_SrcAlpha,
          .dstRGBBlendFactor = lvk::BlendFactor_OneMinusSrcAlpha,
          .dstAlphaBlendFactor = lvk::BlendFactor_OneMinusSrcAlpha,
      }},
      .depthFormat = desc.depthStencil.texture ? ctx_.getFormat(desc.depthStencil.texture) : lvk::Format_Invalid,
      .cullMode = lvk::CullMode_None,
      .debugName = "Pipeline: imgui",
  });
}

void ImGuiRenderer::beginFrame(const lvk::Framebuffer& desc) {
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;

  const lvk::Format colorFormat = ctx_.getFormat(desc.color[0].texture);
  const lvk::Format depthFormat = desc.depthStencil.texture ? ctx_.getFormat(desc.depthStencil.texture) : lvk::Format_Invalid;
  if (pipeline_.empty() || pipelineColorFormat_ != colorFormat || pipelineDepthFormat_ != depthFormat) {
    pipeline_ = createNewPipelineState(desc);
    pipelineColorFormat_ = colorFormat;
    pipelineDepthFormat_ = depthFormat;
  }

  const lvk::Dimensions dim = ctx_.getDimensions(desc.color[0].texture);
  if (window_) {
    ImGui_ImplGlfw_NewFrame();
    const float sx = io.DisplaySize.x > 0.0f ? float(dim.width) / io.DisplaySize.x : 1.0f;
    const float sy = io.DisplaySize.y > 0.0f ? float(dim.height) / io.DisplaySize.y : 1.0f;
    io.DisplaySize = ImVec2(float(dim.width), float(dim.height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    if (glfwGetWindowAttrib(window_, GLFW_HOVERED)) {
      double cursorX = 0.0;
      double cursorY = 0.0;
      glfwGetCursorPos(window_, &cursorX, &cursorY);
      io.AddMousePosEvent(float(cursorX) * sx, float(cursorY) * sy);
    }
  } else {
    io.DisplaySize = ImVec2(float(dim.width), float(dim.height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;
  }

  ImGui::NewFrame();
}

void ImGuiRenderer::endFrame(lvk::ICommandBuffer& cmd) {
  static_assert(sizeof(ImDrawIdx) == 2);

  ImGui::EndFrame();
  ImGui::Render();

  ImDrawData* dd = ImGui::GetDrawData();

  const float fbWidth = dd->DisplaySize.x * dd->FramebufferScale.x;
  const float fbHeight = dd->DisplaySize.y * dd->FramebufferScale.y;
  if (fbWidth <= 0 || fbHeight <= 0 || dd->CmdListsCount == 0) {
    return;
  }

  if (dd->Textures) {
    for (ImTextureData* tex : *dd->Textures) {
      switch (tex->Status) {
      case ImTextureStatus_WantCreate:
        textures_.emplace_back(ctx_.createTexture({
            .type = lvk::TextureType_2D,
            .format = lvk::Format_RGBA_UN8,
            .dimensions = {uint32_t(tex->Width), uint32_t(tex->Height)},
            .usage = lvk::TextureUsageBits_Sampled,
            .data = tex->Pixels,
            .debugName = "ImGuiTexture",
        }));
        tex->SetTexID(ImTextureID(textures_.back().index()));
        tex->BackendUserData = textures_.back().handleAsVoid();
        tex->SetStatus(ImTextureStatus_OK);
        break;
      case ImTextureStatus_WantUpdates:
        ctx_.upload(lvk::TextureHandle(tex->BackendUserData),
                    {.offset = {tex->UpdateRect.x, tex->UpdateRect.y, 0},
                     .dimensions = {uint32_t(tex->UpdateRect.w), uint32_t(tex->UpdateRect.h), 1}},
                    tex->GetPixelsAt(tex->UpdateRect.x, tex->UpdateRect.y),
                    uint32_t(tex->Width));
        tex->SetStatus(ImTextureStatus_OK);
        break;
      case ImTextureStatus_WantDestroy:
        for (lvk::Holder<lvk::TextureHandle>& holder : textures_) {
          if (holder.handleAsVoid() == tex->BackendUserData) {
            holder = std::move(textures_.back());
            textures_.pop_back();
            break;
          }
        }
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
        tex->BackendUserData = nullptr;
        break;
      default:
        break;
      }
    }
  }

  cmd.cmdPushDebugGroupLabel("ImGui Rendering");
  cmd.cmdBindDepthState({});
  cmd.cmdBindViewport({.x = 0.0f, .y = 0.0f, .width = fbWidth, .height = fbHeight});

  const float L = dd->DisplayPos.x;
  const float R = dd->DisplayPos.x + dd->DisplaySize.x;
  const float T = dd->DisplayPos.y;
  const float B = dd->DisplayPos.y + dd->DisplaySize.y;
  const ImVec2 clipOff = dd->DisplayPos;
  const ImVec2 clipScale = dd->FramebufferScale;

  DrawableData& drawableData = drawables_[frameIndex_];
  frameIndex_ = (frameIndex_ + 1) % LVK_ARRAY_NUM_ELEMENTS(drawables_);

  const auto alignTo4 = [](size_t n) -> size_t { return (n + 3) & ~size_t(3); };

  size_t idxBufBytes = 0;
  for (const ImDrawList* cmdList : dd->CmdLists) {
    idxBufBytes += alignTo4(size_t(cmdList->IdxBuffer.Size) * sizeof(ImDrawIdx));
  }

  if (drawableData.indexBufferSize_ < idxBufBytes) {
    drawableData.ib_ = ctx_.createBuffer({
        .usage = lvk::BufferUsageBits_Index,
        .storage = lvk::StorageType_HostVisible,
        .size = idxBufBytes,
        .debugName = "ImGui: ib",
    });
    drawableData.indexBufferSize_ = idxBufBytes;
  }
  const auto vertexBufferSize = uint32_t(dd->TotalVtxCount) * sizeof(ImDrawVert);
  if (drawableData.vertexBufferSize_ < vertexBufferSize) {
    drawableData.vb_ = ctx_.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_HostVisible,
        .size = vertexBufferSize,
        .debugName = "ImGui: vb",
    });
    drawableData.vertexBufferSize_ = vertexBufferSize;
  }

  {
    size_t vtxBytesOffset = 0;
    size_t idxBytesOffset = 0;
    for (const ImDrawList* cmdList : dd->CmdLists) {
      const size_t vtxBytes = size_t(cmdList->VtxBuffer.Size) * sizeof(ImDrawVert);
      const size_t idxBytes = size_t(cmdList->IdxBuffer.Size) * sizeof(ImDrawIdx);
      ctx_.upload(drawableData.vb_, cmdList->VtxBuffer.Data, vtxBytes, vtxBytesOffset);
      ctx_.upload(drawableData.ib_, cmdList->IdxBuffer.Data, idxBytes, idxBytesOffset);
      vtxBytesOffset += vtxBytes;
      idxBytesOffset += alignTo4(idxBytes);
    }
  }

  uint32_t idxOffset = 0;
  uint32_t vtxOffset = 0;

  cmd.cmdBindIndexBuffer(drawableData.ib_, lvk::IndexFormat_UI16);
  cmd.cmdBindRenderPipeline(pipeline_);

  for (const ImDrawList* cmdList : dd->CmdLists) {
    for (int i = 0; i < cmdList->CmdBuffer.Size; ++i) {
      const ImDrawCmd cmd_ = cmdList->CmdBuffer[i];
      if (cmd_.UserCallback) {
        if (cmd_.UserCallback == ImDrawCallback_ResetRenderState) {
          cmd.cmdBindViewport({.x = 0.0f, .y = 0.0f, .width = fbWidth, .height = fbHeight});
          cmd.cmdBindIndexBuffer(drawableData.ib_, lvk::IndexFormat_UI16);
          cmd.cmdBindRenderPipeline(pipeline_);
        } else {
          cmd_.UserCallback(cmdList, &cmd_);
        }
        continue;
      }

      ImVec2 clipMin((cmd_.ClipRect.x - clipOff.x) * clipScale.x, (cmd_.ClipRect.y - clipOff.y) * clipScale.y);
      ImVec2 clipMax((cmd_.ClipRect.z - clipOff.x) * clipScale.x, (cmd_.ClipRect.w - clipOff.y) * clipScale.y);
      if (clipMin.x < 0.0f) {
        clipMin.x = 0.0f;
      }
      if (clipMin.y < 0.0f) {
        clipMin.y = 0.0f;
      }
      if (clipMax.x > fbWidth) {
        clipMax.x = fbWidth;
      }
      if (clipMax.y > fbHeight) {
        clipMax.y = fbHeight;
      }
      if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) {
        continue;
      }
      const struct {
        float LRTB[4];
        uint64_t vb;
        uint32_t textureId;
        uint32_t samplerId;
      } pc = {
          .LRTB = {L, R, T, B},
          .vb = ctx_.gpuAddress(drawableData.vb_),
          .textureId = uint32_t(cmd_.GetTexID()),
          .samplerId = samplerClamp_.index(),
      };
      cmd.cmdPushConstants(pc);
      cmd.cmdBindScissorRect({uint32_t(clipMin.x), uint32_t(clipMin.y), uint32_t(clipMax.x - clipMin.x), uint32_t(clipMax.y - clipMin.y)});
      cmd.cmdDrawIndexed(cmd_.ElemCount, 1u, idxOffset + cmd_.IdxOffset, int32_t(vtxOffset + cmd_.VtxOffset));
    }
    idxOffset += uint32_t(alignTo4(size_t(cmdList->IdxBuffer.Size) * sizeof(ImDrawIdx)) / sizeof(ImDrawIdx));
    vtxOffset += cmdList->VtxBuffer.Size;
  }

  cmd.cmdPopDebugGroupLabel();
}

} // namespace lvk::metal
