#pragma once

#include <esp_heap_caps.h>

#include <cstddef>
#include <cstdlib>
#include <vector>

namespace metalio_psram {

template <typename T>
class Allocator {
 public:
  using value_type = T;

  Allocator() noexcept = default;

  template <typename U>
  Allocator(const Allocator<U>&) noexcept {}

  T* allocate(std::size_t count) {
    if (count == 0) return nullptr;

    void* ptr = heap_caps_malloc(
        count * sizeof(T),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (ptr == nullptr) {
      std::abort();
    }

    return static_cast<T*>(ptr);
  }

  void deallocate(T* ptr, std::size_t) noexcept {
    if (ptr != nullptr) {
      heap_caps_free(ptr);
    }
  }

  template <typename U>
  struct rebind {
    using other = Allocator<U>;
  };
};

template <typename T, typename U>
bool operator==(const Allocator<T>&, const Allocator<U>&) noexcept {
  return true;
}

template <typename T, typename U>
bool operator!=(const Allocator<T>&, const Allocator<U>&) noexcept {
  return false;
}

template <typename T>
using Vector = std::vector<T, Allocator<T>>;

}  // namespace metalio_psram
