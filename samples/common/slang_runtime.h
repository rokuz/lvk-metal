#pragma once

#include <lvk/LVK.h>

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

  std::string compileToMSL(char const* moduleName, char const* entryPoint) noexcept;

  lvk::Holder<lvk::ShaderModuleHandle> createShaderModule(lvk::IContext& ctx,
                                                          char const* moduleName,
                                                          char const* entryPoint,
                                                          lvk::ShaderStage stage,
                                                          char const* debugName) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
