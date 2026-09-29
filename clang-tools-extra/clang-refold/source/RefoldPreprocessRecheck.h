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
//   - `buildFinalSourcePreprocessCallback` and
//     `buildFinalLineControlValidationCallback`: the executable oracles that
//     final verification and final line-control pruning consume.
//
// These primitives are intentionally generic — the `--no-lines` mode's
// ignore-mask construction sits on top of this surface.  The raw lexer that
// produces a `PPTok` stream from a preprocessed byte buffer lives next to
// `PPTok` itself in `model/RefoldToken.h`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPREPROCESSRECHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPREPROCESSRECHECK_H

#include "line-control/FinalLineControlModel.h"
#include "model/RefoldModel.h"
#include "model/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>

namespace clang {
namespace refold {

/// Whether a replay enters the producer's command-line forced includes.
///
/// `-include <file>` enters <file> ahead of the first byte of the input, so the
/// producer's preprocessed stream -- and every edited stream derived from it --
/// already carries that header's tokens.
enum class ForcedIncludeReplay : uint8_t {
  /// Enter each forced include ahead of the input, as the producer did.  This
  /// is the replay of source: the producer's own translation unit, or a
  /// refold of it.
  Enter,

  /// Enter none.  This is the replay of an edited preprocessed stream, which
  /// already holds every forced include's expansion.  Entering them again
  /// would emit their tokens a second time, and would re-read the header's
  /// own declarations under the macros it defined: a header may declare a
  /// function and then define a function-like macro of the same name, and the
  /// second pass reads that declaration as a malformed invocation.
  ///
  /// `-imacros`, `-D` and `-U` contribute macro state but no tokens, so they
  /// are replayed on both sides as before.
  Omit
};

/// Re-invoke the producer-recorded preprocessor on \p inputPath under \p ctx
/// and return the resulting `-E -P` output bytes.
///
/// The returned bytes are the canonical "preprocessed source" used by every
/// downstream verification path.  Errors are surfaced as `llvm::Error` rather
/// than fatal-erroring so callers can attach contextual diagnostics.
///
/// \p extraArgs are appended after the producer's own arguments, so a search
/// path supplied there is consulted only once every producer-recorded path has
/// missed.  The ordering is the contract: a replay resolves a header the way the
/// producer resolved it whenever it can, and reaches a caller-declared directory
/// only for headers the producer never saw.
///
/// \p forcedIncludes selects whether the producer's `-include` files are
/// entered; see `ForcedIncludeReplay`.
llvm::Expected<std::string> preprocessToBytes(
    llvm::StringRef inputPath, const RefoldModel::PreprocessContext &ctx,
    llvm::ArrayRef<std::string> extraArgs = {},
    ForcedIncludeReplay forcedIncludes = ForcedIncludeReplay::Enter);

/// Build a callback that preprocesses an assembled final source through the
/// producer-recorded context, using one stable temporary path beside \p
/// anchorPath so `__FILE__` and quoted-include lookup stay comparable.
///
/// \p anchorPath must name the producer's own source, not the refold output;
/// see `producerSourceAnchorPath()` for why the distinction is load-bearing.
///
///
/// \p verifyIncludeDirs are caller-declared last-resort include directories.
/// An edited stream may name a header that did not exist when the producer ran
/// -- a transform that hoists globals into a new header, for instance -- so no
/// producer-recorded search path can find it.  Where such a header lives is not
/// derivable from the refold map, so it is declared rather than guessed; each
/// directory is searched only after every producer-recorded path has missed.
///
/// \p forcedIncludes is passed to every `preprocessToBytes()` call the
/// callback makes: `Omit` for a callback that replays an edited stream.
FinalSourcePreprocessCallback buildFinalSourcePreprocessCallback(
    llvm::StringRef anchorPath, const RefoldModel::PreprocessContext &ctx,
    llvm::ArrayRef<std::string> verifyIncludeDirs = {},
    ForcedIncludeReplay forcedIncludes = ForcedIncludeReplay::Enter);

/// Build the executable oracle that validates one proposed final-stream
/// `#line` deletion.
///
/// The returned callback is consumed by `refoldTranslationUnit` and ultimately
/// by the final-line-control pruner.  It re-invokes the producer-recorded
/// preprocessor on the current accepted final source and the candidate final
/// source using one stable temporary path located beside \p outputPath, so
/// `__FILE__` and quoted-include lookup remain comparable across both inputs.
/// Byte-for-byte preprocessor equivalence is accepted first; otherwise the
/// callback falls back to token-sequence equality, which is the same oracle
/// `--check` uses.
FinalLineControlValidationCallback buildFinalLineControlValidationCallback(
    llvm::StringRef outputPath, const RefoldModel::PreprocessContext &ctx);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPREPROCESSRECHECK_H
