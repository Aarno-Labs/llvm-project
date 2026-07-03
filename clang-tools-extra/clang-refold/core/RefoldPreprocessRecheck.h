//===--- RefoldPreprocessRecheck.h -----------------------------*- C++ -*-===//
//
// Preprocessing-recheck primitives for clang-refold.
//
// This module owns the generic "re-invoke the producer's preprocessor on a
// candidate source and compare the resulting tokens" surface used by every
// verification path in the driver:
//
//   - `preprocessToBytes`: re-invokes Clang's preprocessor on a candidate
//     source using the producer-recorded `PreprocessContext` and returns the
//     `-E -P` output bytes.
//   - `compareTokens`: compares two `PPTok` streams for byte-exact agreement,
//     returning a descriptive `llvm::Error` at the first mismatch.
//
// These primitives are intentionally generic — the `--no-lines` mode's
// ignore-mask construction and the line-control validation callback layer
// sit on top of this surface.  The raw lexer that produces a `PPTok` stream
// from a preprocessed byte buffer lives next to `PPTok` itself in
// `source/RefoldToken.h`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPREPROCESSRECHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPREPROCESSRECHECK_H

#include "core/RefoldModel.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>

namespace clang {
namespace refold {

/// Re-invoke the producer-recorded preprocessor on \p inputPath under \p ctx
/// and return the resulting `-E -P` output bytes.
///
/// The returned bytes are the canonical "preprocessed source" used by every
/// downstream verification path.  Errors are surfaced as `llvm::Error` rather
/// than fatal-erroring so callers can attach contextual diagnostics.
llvm::Expected<std::string>
preprocessToBytes(llvm::StringRef inputPath,
                  const RefoldModel::PreprocessContext &ctx);

/// Compare two preprocessed-token streams for `--check` mode.
///
/// Returns `Error::success()` on full agreement.  Any divergence is returned
/// as a descriptive `llvm::Error` carrying the first mismatch's position.  This
/// routine performs byte/token comparison only; caller-provided line-control
/// ignore masks and `--no-lines` validation are layered above it.
llvm::Error compareTokens(llvm::ArrayRef<PPTok> aToks,
                          llvm::ArrayRef<PPTok> bToks);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPREPROCESSRECHECK_H
