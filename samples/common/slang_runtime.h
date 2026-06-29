#pragma once

#include <lvk/LVK-Metal.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

class SlangRuntime {
 public:
  explicit SlangRuntime(std::filesystem::path moduleDir) noexcept;
  ~SlangRuntime();

  SlangRuntime(SlangRuntime const&) = delete;
  SlangRuntime& operator=(SlangRuntime const&) = delete;

  bool valid() const noexcept;

  std::string compileToMSL(char const* moduleName, char const* entryPoint, uint32_t outThreadgroupSize[3] = nullptr) noexcept;

  lvk::Holder<lvk::ShaderModuleHandle> createShaderModule(lvk::metal::IMetalContext& ctx,
                                                          char const* moduleName,
                                                          char const* entryPoint,
                                                          lvk::ShaderStage stage,
                                                          char const* debugName) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
