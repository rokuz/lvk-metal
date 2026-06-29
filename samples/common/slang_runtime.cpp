#include "slang_runtime.h"

#include <slang-com-ptr.h>
#include <slang.h>

#include <vector>

struct SlangRuntime::Impl {
  std::string moduleDir;
  Slang::ComPtr<slang::IGlobalSession> globalSession;
  Slang::ComPtr<slang::ISession> session;
};

namespace {
void logDiagnostics(slang::IBlob* diag, char const* context) {
  if (diag && diag->getBufferSize() > 0)
    LLOGE("Slang [%s]: %s", context, static_cast<char const*>(diag->getBufferPointer()));
}
} // namespace

SlangRuntime::SlangRuntime(std::filesystem::path moduleDir) noexcept {
  std::unique_ptr<Impl> impl = std::make_unique<Impl>();
  impl->moduleDir = moduleDir.string();

  if (SLANG_FAILED(slang::createGlobalSession(impl->globalSession.writeRef())) || !impl->globalSession) {
    LLOGE("Slang: failed to create global session");
    return;
  }

  slang::TargetDesc target = {};
  target.structureSize = sizeof(slang::TargetDesc);
  target.format = SLANG_METAL;

  std::vector<char const*> searchPaths;
  searchPaths.push_back(impl->moduleDir.c_str());
#ifdef LVK_METAL_SAMPLE_COMMON_DIR
  searchPaths.push_back(LVK_METAL_SAMPLE_COMMON_DIR);
#endif

  slang::SessionDesc sd = {};
  sd.structureSize = sizeof(slang::SessionDesc);
  sd.targets = &target;
  sd.targetCount = 1;
  sd.searchPaths = searchPaths.data();
  sd.searchPathCount = SlangInt(searchPaths.size());

  if (SLANG_FAILED(impl->globalSession->createSession(sd, impl->session.writeRef())) || !impl->session) {
    LLOGE("Slang: failed to create session");
    return;
  }

  impl_ = std::move(impl);
}

SlangRuntime::~SlangRuntime() = default;

bool SlangRuntime::valid() const noexcept {
  return impl_ != nullptr;
}

std::string SlangRuntime::compileToMSL(char const* moduleName, char const* entryPoint, uint32_t outThreadgroupSize[3]) noexcept {
  if (outThreadgroupSize) {
    outThreadgroupSize[0] = outThreadgroupSize[1] = outThreadgroupSize[2] = 1;
  }
  if (!impl_)
    return {};

  Slang::ComPtr<slang::IBlob> diag;
  slang::IModule* module = impl_->session->loadModule(moduleName, diag.writeRef());
  logDiagnostics(diag, "loadModule");
  if (!module) {
    LLOGE("Slang: failed to load module '%s'", moduleName);
    return {};
  }

  Slang::ComPtr<slang::IEntryPoint> ep;
  if (SLANG_FAILED(module->findEntryPointByName(entryPoint, ep.writeRef())) || !ep) {
    LLOGE("Slang: entry point not found: %s::%s", moduleName, entryPoint);
    return {};
  }

  slang::IComponentType* components[] = {module, ep.get()};
  Slang::ComPtr<slang::IComponentType> composite;
  if (SLANG_FAILED(impl_->session->createCompositeComponentType(components, 2, composite.writeRef(), diag.writeRef())) || !composite) {
    logDiagnostics(diag, "compose");
    return {};
  }

  Slang::ComPtr<slang::IComponentType> linked;
  if (SLANG_FAILED(composite->link(linked.writeRef(), diag.writeRef())) || !linked) {
    logDiagnostics(diag, "link");
    return {};
  }

  Slang::ComPtr<slang::IBlob> code;
  Slang::ComPtr<slang::IBlob> codeDiag;
  if (SLANG_FAILED(linked->getEntryPointCode(0, 0, code.writeRef(), codeDiag.writeRef())) || !code) {
    logDiagnostics(codeDiag, "codegen");
    LLOGE("Slang: MSL codegen failed: %s::%s", moduleName, entryPoint);
    return {};
  }

  if (outThreadgroupSize) {
    if (slang::ProgramLayout* layout = linked->getLayout(0, nullptr)) {
      if (layout->getEntryPointCount() > 0) {
        SlangUInt sizes[3] = {1, 1, 1};
        layout->getEntryPointByIndex(0)->getComputeThreadGroupSize(3, sizes);
        outThreadgroupSize[0] = uint32_t(sizes[0] ? sizes[0] : 1);
        outThreadgroupSize[1] = uint32_t(sizes[1] ? sizes[1] : 1);
        outThreadgroupSize[2] = uint32_t(sizes[2] ? sizes[2] : 1);
      }
    }
  }

  return std::string(static_cast<char const*>(code->getBufferPointer()), code->getBufferSize());
}

lvk::Holder<lvk::ShaderModuleHandle> SlangRuntime::createShaderModule(lvk::metal::IMetalContext& ctx,
                                                                      char const* moduleName,
                                                                      char const* entryPoint,
                                                                      lvk::ShaderStage stage,
                                                                      char const* debugName) noexcept {
  uint32_t threadgroupSize[3] = {1, 1, 1};
  std::string const msl = compileToMSL(moduleName, entryPoint, threadgroupSize);
  if (msl.empty())
    return {};
  lvk::Holder<lvk::ShaderModuleHandle> handle = ctx.createShaderModule({msl.c_str(), entryPoint, stage, debugName});
  if (handle.valid())
    ctx.setShaderModuleMetadata(handle, {.threadgroupSize = {threadgroupSize[0], threadgroupSize[1], threadgroupSize[2]}});
  return handle;
}
