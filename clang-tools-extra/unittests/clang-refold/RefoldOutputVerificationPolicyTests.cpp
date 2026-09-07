//===-- RefoldOutputVerificationPolicyTests.cpp ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the closing check's mode/verdict contract.
//
// `Inconclusive` is not reachable from a source file, and that is a property
// of the pipeline rather than of the shapes tried.  The verdict means the
// finished assembly could not be preprocessed, and the refolder refuses to
// emit such an assembly in the first place: materializing a header inlines
// the includes nested inside it rather than relocating them unresolved, an
// arm whose condition used `__has_include` is not source-replayable from a
// relocated header at all (`cond_uses_has_include`), and an edit that would
// activate a preserved `#error` fails its macro-state obligation before any
// text is written.  What remains are environmental failures -- a scratch file
// that cannot be created, a header gone between production and verification
// -- which a lit test cannot stage.
//
// So the contract is pinned here instead: which disposition each mode
// requires for each verdict, as one table, so no caller can implement half of
// it and no new mode or verdict can be added without deciding its cell.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldFinalAssemblyVerifier.h"

#include "gtest/gtest.h"

using namespace clang::refold;

namespace {

/// Every `OutputVerificationMode`, so a new mode cannot be added without
/// deciding what it does with each verdict.
constexpr OutputVerificationMode AllOutputVerificationModes[] = {
    OutputVerificationMode::Off,
    OutputVerificationMode::Repair,
    OutputVerificationMode::Fatal,
};

/// Every `FinalAssemblyVerdictKind`, for the same reason.
constexpr FinalAssemblyVerdictKind AllFinalAssemblyVerdictKinds[] = {
    FinalAssemblyVerdictKind::Verified,
    FinalAssemblyVerdictKind::Diverged,
    FinalAssemblyVerdictKind::Inconclusive,
};

TEST(RefoldOutputVerificationPolicy, VerifiedIsAcceptedInEveryMode) {
  // A verified assembly is the one outcome no mode may reject: the check it
  // asked for ran and passed.
  for (OutputVerificationMode mode : AllOutputVerificationModes) {
    EXPECT_EQ(DispositionForVerdict(mode, FinalAssemblyVerdictKind::Verified),
              FinalAssemblyDisposition::Accept);
  }
}

TEST(RefoldOutputVerificationPolicy, FatalRejectsInconclusive) {
  // The defect this test exists for.  `fatal` asks for an assembly the check
  // has verified; a comparison that could not be performed does not produce
  // one, and reporting it as verified would let a run that explicitly asked
  // for verification ship a result nothing checked.
  EXPECT_EQ(DispositionForVerdict(OutputVerificationMode::Fatal,
                                  FinalAssemblyVerdictKind::Inconclusive),
            FinalAssemblyDisposition::Fail);
}

TEST(RefoldOutputVerificationPolicy, RepairKeepsInconclusive) {
  // The other half of the same decision, and it must not move with it: an
  // unperformed comparison names no diverging region, so the repair ladder
  // has nothing to rule out and re-assemble around.  Failing here would trade
  // a real refold for a missing measurement rather than for a proven defect.
  EXPECT_EQ(DispositionForVerdict(OutputVerificationMode::Repair,
                                  FinalAssemblyVerdictKind::Inconclusive),
            FinalAssemblyDisposition::Accept);
}

TEST(RefoldOutputVerificationPolicy, OnlyRepairRepairsADivergence) {
  // A proven divergence is the only verdict carrying a region to narrow, and
  // `repair` is the only mode that opts into spending it.  `fatal` reports
  // instead, because repairing silently costs completeness in a way nothing
  // observes.
  EXPECT_EQ(DispositionForVerdict(OutputVerificationMode::Repair,
                                  FinalAssemblyVerdictKind::Diverged),
            FinalAssemblyDisposition::Repair);
  EXPECT_EQ(DispositionForVerdict(OutputVerificationMode::Fatal,
                                  FinalAssemblyVerdictKind::Diverged),
            FinalAssemblyDisposition::Fail);
}

TEST(RefoldOutputVerificationPolicy, RepairIsNeverDemandedWithoutADivergence) {
  // Only `Diverged` may ask for a repair.  A caller reading any other
  // disposition as "narrow and retry" would loop on a verdict that names
  // nothing to narrow.
  for (OutputVerificationMode mode : AllOutputVerificationModes) {
    for (FinalAssemblyVerdictKind kind : AllFinalAssemblyVerdictKinds) {
      if (kind == FinalAssemblyVerdictKind::Diverged)
        continue;
      EXPECT_NE(DispositionForVerdict(mode, kind),
                FinalAssemblyDisposition::Repair)
          << "mode=" << static_cast<int>(mode)
          << " kind=" << static_cast<int>(kind);
    }
  }
}

TEST(RefoldOutputVerificationPolicy, OffNeverFails) {
  // `off` does not build the check, so it holds no verdict to act on.  The
  // table answers for completeness, and its answer may never be a rejection:
  // a run that declined verification must not be failed by it.
  for (FinalAssemblyVerdictKind kind : AllFinalAssemblyVerdictKinds) {
    EXPECT_EQ(DispositionForVerdict(OutputVerificationMode::Off, kind),
              FinalAssemblyDisposition::Accept);
  }
}

} // namespace
