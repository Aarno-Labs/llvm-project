//===--- RefoldDiffTypes.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Edit-script value types shared by the alignment algorithms and every
// consumer of their results: the SES `Step` and the coalesced edit `Hunk`.
// Code that only handles hunks includes this header instead of
// `DiffAlgorithms.h`, which declares the alignment algorithms themselves.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDDIFFTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDDIFFTYPES_H

#include "support/RefoldFormatProviders.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace clang {
namespace refold {
namespace diffutils {

enum class Op { Equal, Insert, Delete };

inline llvm::StringRef toString(Op op) {
  switch (op) {
  case Op::Equal:
    return "Equal";
  case Op::Insert:
    return "Insert";
  case Op::Delete:
    return "Delete";
  }
  llvm_unreachable("Invalid op");
}

/// \brief Immutable atom in the shortest edit script (SES).
///
/// Each `Step` represents one maximal run of a single edit operation over
/// half-open index ranges in the left (`A`) and right (`B`) sequences:
/// `A[aLo,aHi)` and `B[bLo,bHi)`.
///
/// ### Semantics & invariants
/// * **EQUAL** — consumes elements from both sides:
///   * `aHi > aLo`, `bHi > bLo`
///   * Length equality holds: `(aHi - aLo) == (bHi - bLo)`
/// * **INSERT** — inserts `B[bLo,bHi)` with no elements consumed from `A`:
///   * `aHi == aLo`, `bHi > bLo`
/// * **DELETE** — deletes `A[aLo,aHi)` with no elements consumed from `B`:
///   * `aHi > aLo`, `bHi == bLo`
///
/// Steps are emitted in order, non-overlapping, and collectively transform
/// `A` into `B`. Replacements are represented as an adjacent **DELETE**
/// followed by **INSERT** (no separate REPLACE operation).
///
/// ### Notes
/// * Indices are zero-based; ranges are half-open `[lo,hi)`.
/// * “Maximal run” means contiguous operations of the same kind are
///   coalesced into a single `Step`.
struct Step {
  Op op;
  uint64_t aLo, aHi; // indices in A
  uint64_t bLo, bHi; // indices in B

  std::string ToString() const {
    std::string opStr;
    switch (op) {
    case Op::Equal:
      opStr = "EQUAL";
      break;
    case Op::Insert:
      opStr = "INSERT";
      break;
    case Op::Delete:
      opStr = "DELETE";
      break;
    }
    return llvm::formatv("{0} A[{1},{2}) -> B[{3},{4})", opStr, aLo, aHi, bLo,
                         bHi);
  }
};

/// \brief Coalesced edit region spanning contiguous non-EQUAL steps in the SES.
///
/// A `Hunk` summarizes one contiguous *edit run* between the original
/// sequence `A` and the new sequence `B`:
/// `A[aStart,aEnd)` is replaced by `B[bStart,bEnd)`.
///
/// **Special cases:**
/// * **INSERT:** `aStart == aEnd && bStart < bEnd`
/// * **DELETE:** `aStart < aEnd && bStart == bEnd`
/// * **REPLACE:** `aStart < aEnd && bStart < bEnd`
/// * **EQUAL (degenerate):** `aStart == aEnd && bStart == bEnd`
///   *(Not normally emitted by `coalesce()`.)*
///
/// All indices are half-open and refer to element positions in the diff
/// input sequences (not byte offsets).
/// Instances are immutable and safe to reuse across passes.
struct Hunk {
  uint64_t aStart, aEnd;
  uint64_t bStart, bEnd;

  bool isInsertOnly() const { return (aStart == aEnd) && (bStart < bEnd); }
  bool isDeleteOnly() const { return (aStart < aEnd) && (bStart == bEnd); }
  bool isReplace() const { return (aStart < aEnd) && (bStart < bEnd); }
  bool isEqual() const { return (aStart == aEnd) && (bStart == bEnd); }

  bool operator==(const Hunk &other) const {
    return aStart == other.aStart && aEnd == other.aEnd &&
           bStart == other.bStart && bEnd == other.bEnd;
  }

  bool operator!=(const Hunk &other) const { return !(*this == other); }

  template <bool kVerbose = false> std::string ToString() const {
    if (kVerbose) {
      std::string kind;
      if (isInsertOnly()) {
        kind.assign("INS");
      } else if (isDeleteOnly()) {
        kind.assign("DEL");
      } else if (isReplace()) {
        kind.assign("REP");
      } else if (isEqual()) {
        kind.assign("EQL");
      }
      assert(!kind.empty() && "invalid hunk");
      return llvm::formatv("{0} HUNK A[{1},{2}) -> B[{3},{4})", kind, aStart,
                           aEnd, bStart, bEnd);
    } else {
      return llvm::formatv("HUNK A[{0},{1}) -> B[{2},{3})", aStart, aEnd,
                           bStart, bEnd);
    }
  }
};

} // namespace diffutils
} // namespace refold
} // namespace clang

namespace llvm {
/// Format `Hunk` through its `ToString()`; style "v" or "verbose" selects the
/// verbose form.
template <> struct format_provider<clang::refold::diffutils::Hunk> {
  static void format(const clang::refold::diffutils::Hunk &hunk,
                     raw_ostream &os, StringRef style) {
    std::string hunkStr;
    if (style.equals_insensitive("v") || style.equals_insensitive("verbose")) {
      // Eat the "style" and call the appropriate "to string" function.
      hunkStr.assign(hunk.ToString</*kVerbose*/ true>());
      style = "";
    } else {
      hunkStr.assign(hunk.ToString</*kVerbose*/ false>());
    }
    format_provider<StringRef>::format(hunkStr, os, style);
  }
};
} // namespace llvm

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDDIFFTYPES_H
