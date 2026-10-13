#pragma once

#include <esp_heap_caps.h>

#include <cstddef>
#include <cstdlib>
#include <vector>

template <typename T>
class MetalioPsramAllocator {
 public:
  using value_type = T;

  MetalioPsramAllocator() noexcept = default;

  template <typename U>
  MetalioPsramAllocator(const MetalioPsramAllocator<U>&) noexcept {}

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
    using other = MetalioPsramAllocator<U>;
  };
};

template <typename T, typename U>
bool operator==(const MetalioPsramAllocator<T>&,
                const MetalioPsramAllocator<U>&) noexcept {
  return true;
}

template <typename T, typename U>
bool operator!=(const MetalioPsramAllocator<T>&,
                const MetalioPsramAllocator<U>&) noexcept {
  return false;
}

template <typename T>
using MetalioPsramVector = std::vector<T, MetalioPsramAllocator<T>>;
