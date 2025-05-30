// Copyright 2020 The TensorStore Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TENSORSTORE_UTIL_ENDIAN_H_
#define TENSORSTORE_UTIL_ENDIAN_H_

#include <stdint.h>

#include <cstring>
#include <ostream>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/nullability.h"

namespace tensorstore {

/// Indicates platform endianness, as added in C++20.
///
/// See https://en.cppreference.com/w/cpp/types/endian.
enum class endian {
#ifdef _WIN32
  little = 0,
  big = 1,
  native = little
#else
  little = __ORDER_LITTLE_ENDIAN__,
  big = __ORDER_BIG_ENDIAN__,
  native = __BYTE_ORDER__
#endif
};

inline std::ostream& operator<<(std::ostream& os, endian e) {
  return os << (e == endian::little ? '<' : '>');
}

namespace endian_internal {

// clang-format off
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint64_t gbswap_64(uint64_t x) {
#if ABSL_HAVE_BUILTIN(__builtin_bswap64) || defined(__GNUC__)
  return __builtin_bswap64(x);
#else
  return (((x & uint64_t{0xFF}) << 56) |
          ((x & uint64_t{0xFF00}) << 40) |
          ((x & uint64_t{0xFF0000}) << 24) |
          ((x & uint64_t{0xFF000000}) << 8) |
          ((x & uint64_t{0xFF00000000}) >> 8) |
          ((x & uint64_t{0xFF0000000000}) >> 24) |
          ((x & uint64_t{0xFF000000000000}) >> 40) |
          ((x & uint64_t{0xFF00000000000000}) >> 56));
#endif
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t gbswap_32(uint32_t x) {
#if ABSL_HAVE_BUILTIN(__builtin_bswap32) || defined(__GNUC__)
  return __builtin_bswap32(x);
#else
  return (((x & uint32_t{0xFF}) << 24) |
          ((x & uint32_t{0xFF00}) << 8) |
          ((x & uint32_t{0xFF0000}) >> 8) |
          ((x & uint32_t{0xFF000000}) >> 24));
#endif
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint16_t gbswap_16(uint16_t x) {
#if ABSL_HAVE_BUILTIN(__builtin_bswap16) || defined(__GNUC__)
  return __builtin_bswap16(x);
#else
  return (((x & uint16_t{0xFF}) << 8) | ((x & uint16_t{0xFF00}) >> 8));
#endif
}
// clang-format on

}  // namespace endian_internal

// Utilities to convert numbers between the current hosts's native byte
// order and little-endian / big-endian byte order
//
// Load/Store methods are alignment safe
#ifdef ABSL_IS_LITTLE_ENDIAN
namespace little_endian {
#else
namespace big_endian {
#endif

inline uint16_t FromHost16(uint16_t x) { return x; }
inline uint16_t ToHost16(uint16_t x) { return x; }

inline uint32_t FromHost32(uint32_t x) { return x; }
inline uint32_t ToHost32(uint32_t x) { return x; }

inline uint64_t FromHost64(uint64_t x) { return x; }
inline uint64_t ToHost64(uint64_t x) { return x; }

// Functions to do unaligned loads and stores.
inline uint16_t Load16(const void* absl_nonnull p) {
  uint16_t v;
  memcpy(&v, p, sizeof v);
  return ToHost16(v);
}

inline void Store16(void* absl_nonnull p, uint16_t v) {
  v = FromHost16(v);
  memcpy(p, &v, sizeof v);
}

inline uint32_t Load32(const void* absl_nonnull p) {
  uint32_t v;
  memcpy(&v, p, sizeof v);
  return ToHost32(v);
}

inline void Store32(void* absl_nonnull p, uint32_t v) {
  v = FromHost32(v);
  memcpy(p, &v, sizeof v);
}

inline uint64_t Load64(const void* absl_nonnull p) {
  uint64_t v;
  memcpy(&v, p, sizeof v);
  return ToHost64(v);
}

inline void Store64(void* absl_nonnull p, uint64_t v) {
  v = FromHost64(v);
  memcpy(p, &v, sizeof v);
}

}  // namespace little_endian/big_endian

// Utilities to convert numbers between the current hosts's native byte
// order and little-endian / big-endian byte order
//
// Load/Store methods are alignment safe
#ifdef ABSL_IS_LITTLE_ENDIAN
namespace big_endian {
#else
namespace little_endian {
#endif

inline uint16_t FromHost16(uint16_t x) { return endian_internal::gbswap_16(x); }
inline uint16_t ToHost16(uint16_t x) { return endian_internal::gbswap_16(x); }

inline uint32_t FromHost32(uint32_t x) { return endian_internal::gbswap_32(x); }
inline uint32_t ToHost32(uint32_t x) { return endian_internal::gbswap_32(x); }

inline uint64_t FromHost64(uint64_t x) { return endian_internal::gbswap_64(x); }
inline uint64_t ToHost64(uint64_t x) { return endian_internal::gbswap_64(x); }

// Functions to do unaligned loads and stores.
inline uint16_t Load16(const void* absl_nonnull p) {
  uint16_t v;
  memcpy(&v, p, sizeof v);
  return ToHost16(v);
}

inline void Store16(void* absl_nonnull p, uint16_t v) {
  v = FromHost16(v);
  memcpy(p, &v, sizeof v);
}

inline uint32_t Load32(const void* absl_nonnull p) {
  uint32_t v;
  memcpy(&v, p, sizeof v);
  return ToHost32(v);
}

inline void Store32(void* absl_nonnull p, uint32_t v) {
  v = FromHost32(v);
  memcpy(p, &v, sizeof v);
}

inline uint64_t Load64(const void* absl_nonnull p) {
  uint64_t v;
  memcpy(&v, p, sizeof v);
  return ToHost64(v);
}

inline void Store64(void* absl_nonnull p, uint64_t v) {
  v = FromHost64(v);
  memcpy(p, &v, sizeof v);
}

}  // namespace little_endian/big_endian

namespace internal {

/// Swaps endianness of a single 1-, 2-, 4-, or 8-byte value.
///
/// There is no alignment requirement on `source` or `dest`.
///
/// \tparam ElementSize Size in bytes of the value.
/// \param source Pointer to source element of `ElementSize` bytes.
/// \param dest Pointer to destination element of `ElementSize` bytes.
template <size_t ElementSize>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void SwapEndianUnaligned(
    const void* absl_nonnull source, void* absl_nonnull dest) {
  static_assert((ElementSize == 1 || ElementSize == 2 || ElementSize == 4 ||
                 ElementSize == 8),
                "ElementSize must be 1, 2, 4, or 8.");
  if constexpr (ElementSize == 1) {
    std::memcpy(dest, source, 1);
  } else if constexpr (ElementSize == 2) {
    uint16_t temp;
    std::memcpy(&temp, source, ElementSize);
    temp = endian_internal::gbswap_16(temp);
    std::memcpy(dest, &temp, ElementSize);
  } else if constexpr (ElementSize == 4) {
    uint32_t temp;
    std::memcpy(&temp, source, ElementSize);
    temp = endian_internal::gbswap_32(temp);
    std::memcpy(dest, &temp, ElementSize);
  } else if constexpr (ElementSize == 8) {
    uint64_t temp;
    std::memcpy(&temp, source, ElementSize);
    temp = endian_internal::gbswap_64(temp);
    std::memcpy(dest, &temp, ElementSize);
  }
}

/// Swaps endianness for a contiguous array of `Count` sub-elements.
///
/// This is used for swapping the endianness of data types like
/// `std::complex<T>`; in that case, `SubElementSize = sizeof(T)` and
/// `Count = 2`.
///
/// In generic code, this can also be used with `SubElementSize = 1` as an
/// equivalent for `std::memcpy(dest, source, Count)`.
///
/// \tparam SubElementSize Size in bytes of each sub-element.
/// \tparam Count Number of elements.
/// \param source Pointer to source array of `SubElementSize*Count` bytes.
/// \param dest Pointer to destination array of `SubElementSize*Count` bytes.
template <size_t SubElementSize, size_t Count>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void SwapEndianUnaligned(
    const void* absl_nonnull source, void* absl_nonnull dest) {
  if constexpr (SubElementSize == 1) {
    std::memcpy(dest, source, Count);
  } else {
    for (size_t i = 0; i < Count; ++i) {
      SwapEndianUnaligned<SubElementSize>(source, dest);
      source = reinterpret_cast<const char*>(source) + SubElementSize;
      dest = reinterpret_cast<char*>(dest) + SubElementSize;
    }
  }
}

/// Swaps endianness in-place.
///
/// There is no alignment requirement on `data`.
///
/// This is equivalent to `SwapEndianUnaligned<ElementSize>(data, data)`.
template <size_t ElementSize>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void SwapEndianUnalignedInplace(
    void* absl_nonnull data) {
  SwapEndianUnaligned<ElementSize>(data, data);
}

/// Swaps endianness in-place for a contiguous array of `Count` elements.
///
/// There is no alignment requirement on `data`.
///
/// This is equivalent to
/// `SwapEndianUnaligned<SubElementSize, Count>(data, data)`.
template <size_t SubElementSize, size_t Count>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void SwapEndianUnalignedInplace(
    void* absl_nonnull data) {
  SwapEndianUnaligned<SubElementSize, Count>(data, data);
}

}  // namespace internal
}  // namespace tensorstore

#endif  // TENSORSTORE_UTIL_ENDIAN_H_
