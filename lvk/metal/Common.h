#pragma once

#include <Foundation/Foundation.hpp>
#include <Metal/MTL4AccelerationStructure.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

namespace lvk::metal::ns {

template<typename T>
[[nodiscard]] inline NS::SharedPtr<T> make() {
  return NS::TransferPtr(T::alloc()->init());
}

template<typename T>
[[nodiscard]] inline NS::SharedPtr<T> transfer(T* p) {
  return NS::TransferPtr(p);
}

template<typename T>
[[nodiscard]] inline NS::SharedPtr<T> retain(T* p) {
  return NS::RetainPtr(p);
}

[[nodiscard]] inline NS::String* string(const char* s) {
  return NS::String::string(s ? s : "", NS::UTF8StringEncoding);
}

template<typename T>
inline void setLabel(T* obj, const char* label) {
  if (obj && label && label[0])
    obj->setLabel(string(label));
}

} // namespace lvk::metal::ns
