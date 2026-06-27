//===--- RefoldDenseMapInfo.h -----------------------------------*- C++ -*-===//
//
// Project-local LLVM DenseMap key-policy specializations.
//
// DenseMapInfo specializations must be visible in every translation unit that
// instantiates DenseMap operations for the corresponding key type.  Keep these
// small container-policy definitions here instead of relying on unrelated
// subsystem headers such as RefoldEngine.h to supply them accidentally.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDDENSEMAPINFO_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDDENSEMAPINFO_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/Hashing.h"

#include <cstdint>
#include <optional>

namespace llvm {

template <> struct DenseMapInfo<std::optional<uint64_t>> {
  static inline std::optional<uint64_t> getEmptyKey() {
    return std::optional<uint64_t>(~0ULL);
  }

  static inline std::optional<uint64_t> getTombstoneKey() {
    return std::optional<uint64_t>(~1ULL);
  }

  static unsigned getHashValue(const std::optional<uint64_t> &val) {
    // `std::nullopt` is a real owner key in clang-refold; it must hash to a
    // stable value distinct from the DenseMap empty/tombstone sentinels above.
    return val ? static_cast<unsigned>(llvm::hash_value(*val)) : 0u;
  }

  static bool isEqual(const std::optional<uint64_t> &lhs,
                      const std::optional<uint64_t> &rhs) {
    return lhs == rhs;
  }
};

} // namespace llvm

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDDENSEMAPINFO_H
