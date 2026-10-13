#pragma once

#include <esp_heap_caps.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <cstdlib>
#include <vector>

namespace metalio_memory {

// std::vector-compatible allocator for bulk audio payloads. Queue/control
// objects stay in normal heap/internal RAM; only the vector payload is placed
// in octal PSRAM.
template <typename T>
class PsramAllocator {
 public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  PsramAllocator() noexcept = default;

  template <typename U>
  PsramAllocator(const PsramAllocator<U>&) noexcept {}

  [[nodiscard]] T* allocate(size_type count) {
    if (count > max_size()) std::abort();

    void* ptr = heap_caps_malloc(
        count * sizeof(T),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == nullptr) std::abort();
    return static_cast<T*>(ptr);
  }

  void deallocate(T* ptr, size_type) noexcept {
    heap_caps_free(ptr);
  }

  constexpr size_type max_size() const noexcept {
    return std::numeric_limits<size_type>::max() / sizeof(T);
  }

  template <typename U>
  bool operator==(const PsramAllocator<U>&) const noexcept { return true; }

  template <typename U>
  bool operator!=(const PsramAllocator<U>&) const noexcept { return false; }
};

template <typename T>
using PsramVector = std::vector<T, PsramAllocator<T>>;

}  // namespace metalio_memory
