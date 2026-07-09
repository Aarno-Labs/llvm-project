//===--- RefoldFormatProviders.h --------------------------------*- C++ -*-===//
//
// Custom `llvm::format_provider` specializations for the refolding tools.
//
// Overview
// --------
// This header collects the project-wide `llvm::format_provider<>`
// specializations used by `llvm::formatv` throughout clang-refold:
//
//   • `std::optional<T>`                — prints "(none)" or delegates to T.
//   • any type with a `ToString()`      — member-function stringification
//     member (excluding `Hunk`, which     (`Hunk` has its own explicit provider
//     is special-cased elsewhere).         in `source/DiffAlgorithms.h`).
//   • any enum with an ADL `toString()` — enum stringification.
//   • `llvm::cl::opt<T>`                — delegates to the provider for T.
//
// Why this lives in its own header
// --------------------------------
// A `format_provider` partial specialization must be visible *before* the
// first point at which `formatv` instantiates it for a matching type.  GCC
// enforces this strictly: a specialization seen *after* an implicit
// instantiation is ill-formed ("partial specialization ... after
// instantiation"), whereas Clang is more permissive.  Several low-level
// headers (e.g. `core/RefoldModel.h`) format `std::optional<...>` from
// non-template inline members, so the providers must be declared ahead of
// those uses.  Keeping them in this standalone, dependency-light header lets
// every translation unit pull them in early and in a consistent order.
//
// This header is intentionally self-contained: it only forward-declares the
// `Hunk` type it needs and does not include the heavier refold headers, so it
// can be included first from anywhere.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDFORMATPROVIDERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDFORMATPROVIDERS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <type_traits>
#include <utility>

namespace clang {
namespace refold {
namespace diffutils {
// Forward declaration only: the generic `ToString()` provider below excludes
// `Hunk` (which has its own explicit provider in `source/DiffAlgorithms.h`),
// and `std::is_same_v` needs only an incomplete type.
struct Hunk;
} // namespace diffutils
} // namespace refold
} // namespace clang

namespace llvm {

// General provider for `std::optional<>`
template <typename T> struct format_provider<std::optional<T>, void> {
  static void format(const std::optional<T> &opt, raw_ostream &os,
                     StringRef style) {
    if (!opt) {
      // Default "none" text; adjust to taste ("" for empty, etc.)
      os << "(none)";
      return;
    }
    // Delegate formatting of the contained value, honoring any :Style
    format_provider<T>::format(*opt, os, style);
  }
};

// Detector for class/structs with a `ToString()` member function.
template <typename T, typename = void> struct HasToString : std::false_type {};

template <typename T>
struct HasToString<T, std::void_t<decltype(std::declval<T>().ToString())>>
    : std::is_same<decltype(std::declval<T>().ToString()), std::string> {};

// General `ToString()` provider, excluding Hunk (we special case this)
template <typename T>
struct format_provider<
    T, std::enable_if_t<HasToString<T>::value &&
                        !std::is_same_v<T, clang::refold::diffutils::Hunk>>> {
  static void format(const T &val, raw_ostream &os, StringRef style) {
    os << val.ToString();
  }
};

// Detector for enums that have a 'toString' function available via ADL
template <typename T, typename = void>
struct HasEnumToString : std::false_type {};

template <typename T>
struct HasEnumToString<T, std::void_t<decltype(toString(std::declval<T>()))>>
    : std::is_enum<T> {};

// The General Enum Provider
template <typename T>
struct format_provider<T, std::enable_if_t<HasEnumToString<T>::value>> {
  static void format(const T &val, llvm::raw_ostream &os, StringRef style) {
    // This calls the toString(T) function found via Argument Dependent Lookup
    os << toString(val);
  }
};

// This handles cl::opt wrapper specifically
template <typename T>
struct format_provider<
    llvm::cl::opt<T>,
    std::enable_if_t<!llvm::support::detail::use_string_formatter<
        llvm::cl::opt<T>>::value>> {
  static void format(const llvm::cl::opt<T> &val, llvm::raw_ostream &os,
                     StringRef style) {
    // We delegate to the provider for the underlying type T
    // Note: We use val.getValue() because operator* doesn't exist
    format_provider<T>::format(val.getValue(), os, style);
  }
};

} // namespace llvm

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDFORMATPROVIDERS_H
