//===--- RefoldNoLinesPruning.h --------------------------------*- C++ -*-===//
//
// `--no-lines` pruning recheck pipeline for clang-refold.
//
// This module owns the `--no-lines`-specific pieces of the `--check` recheck
// flow.  The generic recheck primitives `preprocessToBytes` and `compareTokens`
// live in `core/RefoldPreprocessRecheck.h`; `lexPPTokens` lives next to
// `PPTok` in `source/RefoldToken.h`; refold-map JSON extraction
// (`PreprocessContext`, source-path lookup) lives on `RefoldModel`; and the
// final-line-control validation callback factory lives in
// `line-control/FinalLineControlModel.h`.  This header exposes only:
//
//   - `buildNoLinesIgnoreMask`: computes the per-B-token ignore mask that
//     relaxes token comparison for `__LINE__`/`__FILE__`-sensitive predefined
//     macro spans whose values change under `--no-lines`.
//
//   - `compareTokensNoLinesAware`: ignore-mask-aware token comparison used by
//     `--check --no-lines` mode.
//
// `PPCtx` is retained as a using-alias for `RefoldModel::PreprocessContext`
// so existing recheck-pipeline call sites keep their established spelling.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDNOLINESPRUNING_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDNOLINESPRUNING_H

#include "core/RefoldModel.h"
#include "core/RefoldPreprocessRecheck.h"
#include "line-control/FinalLineControlModel.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <string>
#include <vector>

namespace clang {
namespace refold {

/// Convenience alias for the producer-recorded preprocessing context.
///
/// The canonical home is `RefoldModel::PreprocessContext`; this alias keeps
/// the established short spelling used by the recheck pipeline.
using PPCtx = RefoldModel::PreprocessContext;

/// Build the per-B-token ignore mask that relaxes token comparison under
/// `--check --no-lines`.
///
/// The mask marks every B-token whose corresponding A-token comes from a
/// location-sensitive predefined macro (`__LINE__`, `__FILE__`,
/// `__FILE_NAME__`, `__BASE_FILE__`) so that values which legitimately differ
/// after pruning line directives do not fail validation.  Returns an `Error`
/// when the recheck preprocessing cannot be performed or the producer
/// metadata is internally inconsistent.
llvm::Expected<std::vector<uint8_t>>
buildNoLinesIgnoreMask(const llvm::json::Object &rootJson, const PPCtx &ctx,
                       llvm::ArrayRef<PPTok> bPPToks);

/// Build the per-B-token ignore mask that relaxes token comparison for
/// stringified-argument observers under a relaxed (non-`--strict`) `--check`.
///
/// The mask marks every B-token that came from argument stringification (`#x`)
/// *and* that B left equal to its original spelling.  In relaxed mode the refold
/// pipeline may fold an argument edit while leaving a stringified occurrence
/// stale, so re-expanding the refolded source regenerates a different `#arg`
/// than B carried; that difference is tolerated only at these stale positions.
/// A stringified occurrence that B independently edited is not aligned EQUAL to
/// its original A token, so it is never masked and must still match exactly.
/// Returns an `Error` when recheck preprocessing fails or the producer metadata
/// is internally inconsistent.
llvm::Expected<std::vector<uint8_t>>
buildRelaxedStringifyIgnoreMask(const llvm::json::Object &rootJson,
                                const PPCtx &ctx,
                                llvm::ArrayRef<PPTok> bPPToks);

/// Compare two preprocessed-token streams while honoring an ignore mask.
///
/// Used by `--check --no-lines` mode.  Token-position mismatches whose B-side
/// index is set in \p ignoreMask are skipped (the producer-known
/// line-sensitive predefined macros are allowed to differ).  All other
/// divergences are returned as a descriptive `llvm::Error`.
llvm::Error compareTokensNoLinesAware(llvm::ArrayRef<PPTok> aToks,
                                      llvm::ArrayRef<PPTok> bToks,
                                      llvm::ArrayRef<uint8_t> ignoreMask);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDNOLINESPRUNING_H
