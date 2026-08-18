//===--- RefoldTokenlessSourceProjection.cpp --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "source/RefoldTokenlessSourceProjection.h"

#include <algorithm>

namespace clang {
namespace refold {

namespace {

/// Return whether the mapped token's source bytes end at or before the queried
/// interval begins.
bool mappedTokenPrecedes(const RefoldModel::TokMapEntry &entry,
                         uint64_t sourceBegin) {
  return entry.e <= sourceBegin;
}

/// Return whether the mapped token's source bytes begin at or after the queried
/// interval ends.
bool mappedTokenFollows(const RefoldModel::TokMapEntry &entry,
                        uint64_t sourceEnd) {
  return entry.b >= sourceEnd;
}

} // namespace

std::optional<uint64_t> projectTokenlessSourceIntervalToATokenFrontier(
    llvm::ArrayRef<const RefoldModel::TokMapEntry *> mappedTokens,
    llvm::ArrayRef<const RefoldModel::IncludeItem *> childIncludes,
    uint64_t aTokenCount, uint64_t intervalBegin, uint64_t intervalEnd) {
  uint64_t lowerBoundary = 0;
  uint64_t upperBoundary = aTokenCount;
  bool overlapsMappedToken = false;

  for (const RefoldModel::TokMapEntry *entry : mappedTokens) {
    if (mappedTokenPrecedes(*entry, intervalBegin)) {
      lowerBoundary = std::max(lowerBoundary, entry->pp + 1);
    } else if (mappedTokenFollows(*entry, intervalEnd)) {
      upperBoundary = std::min(upperBoundary, entry->pp);
    } else {
      overlapsMappedToken = true;
    }
  }

  // Tokmap names the file that spelled a token, while an exact child include
  // cover fixes where a nested occurrence entered and left the owning source
  // stream. Both forms are producer-backed coordinates.
  for (const RefoldModel::IncludeItem *include : childIncludes) {
    if (include->siteE <= intervalBegin) {
      lowerBoundary = std::max(lowerBoundary, include->cover.end);
    } else if (include->siteB >= intervalEnd) {
      upperBoundary = std::min(upperBoundary, include->cover.begin);
    }
  }

  if (overlapsMappedToken || lowerBoundary != upperBoundary)
    return std::nullopt;
  return lowerBoundary;
}

} // namespace refold
} // namespace clang
