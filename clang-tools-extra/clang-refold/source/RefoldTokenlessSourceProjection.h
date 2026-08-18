//===--- RefoldTokenlessSourceProjection.h ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Producer-backed projection of a tokenless physical source interval onto the
// A-token stream.
//
// A directive occupies source bytes but contributes no preprocessed token, so
// its position in A is not observable from the token stream itself.  It is
// recoverable exactly from producer coordinates: the tokmap says which source
// bytes spelled each A token, and an include cover says where a nested
// occurrence entered and left the owning stream.  Together they bound the
// interval from both sides, and the projection is admissible only when those
// bounds meet at one frontier.
//
// This is a coordinate theorem, not a policy: it decides where a tokenless
// interval sits, never whether crossing, moving, or preserving it is sound.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENLESSSOURCEPROJECTION_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENLESSSOURCEPROJECTION_H

#include "core/RefoldModel.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace clang {
namespace refold {

/// Return the unique A-token frontier of a tokenless physical source interval.
///
/// `mappedTokens` are the producer tokmap entries whose spelling file is the
/// file containing `[intervalBegin, intervalEnd)`, and `childIncludes` the
/// include occurrences that file entered directly.  Both surfaces must already
/// be restricted to the queried owner; this function applies no path or
/// ownership filtering of its own.
///
/// The result is the single index `frontier` for which every recorded A token
/// below it is spelled before `intervalBegin` and every recorded A token from
/// it upward is spelled at or after `intervalEnd`.  Half-open A ranges and
/// half-open source ranges are both preserved.
///
/// The projection fails closed and returns std::nullopt when
///
///   * a mapped token overlaps the interval, so the interval is not tokenless
///     -- a pass-through pragma whose spelling survives into the preprocessed
///     stream is rejected here rather than by a separate emptiness test; or
///   * the surrounding evidence leaves more than one admissible frontier,
///     which happens when no recorded coordinate separates the interval from
///     the tokens around it.
std::optional<uint64_t> projectTokenlessSourceIntervalToATokenFrontier(
    llvm::ArrayRef<const RefoldModel::TokMapEntry *> mappedTokens,
    llvm::ArrayRef<const RefoldModel::IncludeItem *> childIncludes,
    uint64_t aTokenCount, uint64_t intervalBegin, uint64_t intervalEnd);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENLESSSOURCEPROJECTION_H
