//===--- RefoldNeutralityProof.h -------------------------------*- C++ -*-===//
//
// Source-neutrality proof subsystem for clang-refold — public surface.
//
// Consolidates the three interfaces that together prove a preserved
// source span or zero-token macro invocation is observationally
// neutral:
//   - the shared zero-token macro-neutrality proof,
//   - the conditional-island and balanced diagnostic-pragma neutrality
//     proofs, and
//   - the TU/header policy adapter that wires the above into
//     RefoldEngine callers.
//
// The structural proof algorithms (templates and their lexical helpers) are
// implementation details owned entirely by RefoldNeutralityProof.cpp; they
// are not part of this header's contract.  Generic source-envelope tiling
// helpers (edit/RefoldSourceEnvelopeTiling.h) live in their own header
// because they are caller-policy gap-discharge mechanics, not a neutrality
// proof.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDNEUTRALITYPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDNEUTRALITYPROOF_H

#include "core/RefoldModel.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace refold {

class RefoldMacroStateProof;
class RefoldPathIdentity;

/// Return true iff a source-gap byte slice is ignorable preprocessing trivia.
///
/// Mixed-owner tiling uses this predicate only for bytes that sit between
/// modeled zero-token state owners.  The raw lexer accepts whitespace, complete
/// comments, and physical escaped-newline splices according to the producer
/// language mode; any real token/directive-looking byte means the source gap is
/// not fully covered by modeled owners and therefore cannot be silently crossed
/// by a mixed-owner tiling proof.
bool sourceTextIsOnlyIgnorableGapTrivia(llvm::StringRef text,
                                        const clang::LangOptions &lang);

//===----------------------------------------------------------------------===//
// Conditional-island and balanced diagnostic-pragma neutrality proofs.
//===----------------------------------------------------------------------===//

/// Controls which conditional arms must have no materialized PP tokens for a
/// preserved conditional island to be source-neutral.
enum class NeutralConditionalArmSpanMode { AllArms, SelectedArmsOnly };

/// One complete locally-neutral diagnostic pragma-state island.
///
/// `#pragma clang/GCC diagnostic push/pop` forms a stack discipline: pushes
/// save the current diagnostic mapping, settings mutate only the top frame,
/// and pops restore the previous mapping.  A fully balanced island that
/// starts and ends at stack depth zero, contains only diagnostic settings
/// while depth is positive, and crosses only trivia has identity net state
/// at its boundaries.  Such an island may be carried through a source gap
/// without changing the preprocessing token stream or the diagnostic state
/// observed by preserved suffix source.
struct BalancedDiagnosticPragmaStateIsland {
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t id = 0;
};

/// Collect top-level balanced diagnostic pragma-state islands in a source
/// gap.
///
/// This is the pragma/state-effect invariant in mechanical form.  A pragma
/// sequence can be treated as source-neutral only when:
///
///  * every directive belongs to the caller's current owner surface;
///  * every directive parses as `#pragma clang/GCC diagnostic ...`;
///  * all directives in one island use the same diagnostic namespace;
///  * stack depth never goes negative and returns to zero;
///  * settings occur only while a pushed frame is active; and
///  * the bytes crossed between directives are trivia only.
///
/// The island is not deleted by this helper; callers preserve the original
/// source bytes as an explicit gap piece.  The proof is therefore about the
/// net boundary state, not about reconstructing or normalizing pragma
/// spelling.
void collectBalancedDiagnosticPragmaStateIslands(
    const RefoldModel &model, llvm::StringRef ownerBytes, uint64_t gapBegin,
    uint64_t gapEnd,
    llvm::function_ref<bool(const RefoldModel::PragmaDirective &)>
        pragmaBelongs,
    llvm::SmallVectorImpl<BalancedDiagnosticPragmaStateIsland> &out,
    const clang::LangOptions &lang);

/// Return true iff \p replacement already contains the same locally balanced
/// diagnostic pragma-state island as \p sourceIslandText.
///
/// Balanced pragma islands are zero-normal-token state artifacts: after
/// sideband normalization, the ordinary B replacement bytes may still
/// contain their raw directive spellings even though the structural token
/// diff does not.  When the B surface carries the island, copying the
/// original source island as a preserved gap would duplicate `#pragma`
/// directives and change validation.  The proof is deliberately narrow: only
/// a complete canonical island match suppresses source-gap emission;
/// otherwise the caller preserves the source island or fails through the
/// existing owner proof.
bool balancedDiagnosticPragmaStateIslandIsCarriedByReplacement(
    llvm::StringRef sourceIslandText, llvm::StringRef replacement,
    const clang::LangOptions &lang);

//===----------------------------------------------------------------------===//
// TU/header source-neutrality policy adapter.
//===----------------------------------------------------------------------===//

/// Source-neutrality context for TU-owned fallback gaps.
///
/// The context binds the immutable producer model, TU source bytes, path
/// identity service, and macro-state proof used to prove zero-token source
/// islands neutral without reintroducing RefoldEngine-local policy lambdas.
struct TUSourceNeutralityContext {
  const RefoldModel &model;
  const RefoldMacroStateProof &macroStateProof;
  const RefoldPathIdentity &paths;
  llvm::StringRef tuBytes;
  llvm::StringRef tuPath;
  bool (*isNeutralTrivia)(llvm::StringRef);
};

/// Source-neutrality context for one materialized include expansion.
///
/// The include id narrows ownership so neutral header gaps cannot borrow TU
/// source policy or unrelated include-instance facts.
struct HeaderSourceNeutralityContext {
  const RefoldModel &model;
  const RefoldMacroStateProof &macroStateProof;
  const RefoldPathIdentity &paths;
  llvm::StringRef headerBytes;
  llvm::StringRef headerPath;
  bool (*isNeutralTrivia)(llvm::StringRef);
  uint64_t includeId = 0;
};

/// Owner-neutral conditional-island policy knobs shared by TU and header
/// uses.
///
/// The remaining owner policy hooks are derived from
/// TUSourceNeutralityContext or HeaderSourceNeutralityContext inside
/// RefoldSourceNeutralityProof so callers do not duplicate the same
/// path/owner matching lambdas.
struct NeutralConditionalIslandContext {
  bool requireGroupBeginAtLineStart = false;
  NeutralConditionalArmSpanMode armSpanMode =
      NeutralConditionalArmSpanMode::SelectedArmsOnly;
};

/// Named adapter service for source-neutral zero-token proof wiring.
struct RefoldSourceNeutralityProof {
  /// Build the context used by TU fallback source-neutrality checks.
  static TUSourceNeutralityContext BuildTUSourceNeutralityContext(
      const RefoldModel &model, const RefoldMacroStateProof &macroStateProof,
      const RefoldPathIdentity &paths, llvm::StringRef tuBytes,
      llvm::StringRef tuPath, bool (*isNeutralTrivia)(llvm::StringRef));

  /// Build the context used by materialized-header source-neutrality checks.
  static HeaderSourceNeutralityContext BuildHeaderSourceNeutralityContext(
      const RefoldModel &model, const RefoldMacroStateProof &macroStateProof,
      const RefoldPathIdentity &paths, llvm::StringRef headerBytes,
      llvm::StringRef headerPath, uint64_t includeId,
      bool (*isNeutralTrivia)(llvm::StringRef));

  /// Return true iff the recorded macro invocation emitted material PP
  /// tokens.
  static bool MacroInvocationHasMaterializedPPTokens(
      const RefoldModel::MacroInvocation &invocation);

  /// Prove that a TU-spelled macro invocation is recursively zero-token
  /// neutral.
  static bool MacroInvocationIsSourceNeutralZeroToken(
      const TUSourceNeutralityContext &context,
      const RefoldModel::MacroInvocation &invocation);

  /// Prove that a header-spelled macro invocation is recursively zero-token
  /// neutral, preserving the header materializer's whole-definition-list
  /// tiling policy.
  static bool MacroInvocationIsSourceNeutralZeroToken(
      const HeaderSourceNeutralityContext &context,
      const RefoldModel::MacroInvocation &invocation);

  /// Prove a TU-owned conditional group under the shared neutral-island
  /// proof.
  static bool ConditionalGroupIsNeutralIsland(
      const TUSourceNeutralityContext &context,
      const RefoldModel::CondGroup &group, uint64_t gapBegin, uint64_t gapEnd,
      const NeutralConditionalIslandContext &islandContext,
      llvm::function_ref<bool(const RefoldModel::IncludeItem &)>
          includeIsNeutral);

  /// Prove a materialized-header conditional group under the shared
  /// neutral-island proof.
  static bool ConditionalGroupIsNeutralIsland(
      const HeaderSourceNeutralityContext &context,
      const RefoldModel::CondGroup &group, uint64_t gapBegin, uint64_t gapEnd,
      const NeutralConditionalIslandContext &islandContext,
      llvm::function_ref<bool(const RefoldModel::IncludeItem &)>
          includeIsNeutral);
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDNEUTRALITYPROOF_H
