//===--- RefoldMacroPasteArgumentBuilder.h --------------------*- C++ -*-===//
//
// Paste-aware argument derivation for macro args-only patch construction.
//
// This service isolates the paste-segment recovery and validation predicates
// used when an edit touches a token-paste contribution inside a macro
// invocation. It decides whether the edit can be expressed as an args-only
// rewrite (single or multiple paste-derived argument segments). The service
// does not own proof-carrier certifying: the planner attaches PasteWitness
// records to the resulting MacroPatch after consuming the returned edits.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPASTEARGUMENTBUILDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPASTEARGUMENTBUILDER_H

#include "edit/RefoldPatchTypes.h"
#include "model/RefoldModel.h"
#include "model/RefoldToken.h"
#include "source/RefoldDiffTypes.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class RefoldSourceMapper;

/// Why a paste-token argument derivation produced no argument rewrites.
///
/// The distinction is load-bearing rather than descriptive. `NotApplicable`
/// says only that this derivation does not explain the hunk, so a caller may
/// go on to try a different one. `AmbiguousOrigin` is a *proof* that the hunk
/// is a paste-token edit whose argument origin is not unique: more than one
/// assignment of argument spellings reproduces the edited token, and the token
/// alone does not choose among them. Re-deciding that question with a weaker
/// rule would pick one origin without evidence, so a caller must fail closed
/// instead of falling back.
enum class PasteArgDerivation {
  NotApplicable,
  Derived,
  AmbiguousOrigin,
};

/// Argument rewrites required by the paste tokens a hunk touches, together
/// with why there are none when `edits` is empty.
///
/// `edits` is non-empty exactly when `kind` is `Derived`.
struct PasteArgEditsResult {
  PasteArgDerivation kind = PasteArgDerivation::NotApplicable;
  std::vector<PasteArgEdit> edits;
};

/// Per-argument segments of one edited pasted token, together with why there
/// are none when `segments` is empty.
///
/// `segments` is non-empty exactly when `kind` is `Derived`.
struct PasteTokenSegmentation {
  PasteArgDerivation kind = PasteArgDerivation::NotApplicable;
  std::vector<std::string> segments;
};

/// Paste-aware argument derivation for macro args-only patch construction.
///
/// Constructed on demand by the macro patch planner from borrowed source/model
/// state. Methods are pure functions over their inputs except for token-source
/// queries through the sourceMapper.
class RefoldMacroPasteArgumentBuilder {
public:
  struct Dependencies {
    const RefoldSourceMapper *sourceMapper = nullptr;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    const clang::LangOptions *lexLang = nullptr;
  };

  explicit RefoldMacroPasteArgumentBuilder(Dependencies deps)
      : deps_(std::move(deps)) {}

  /// Return whether the hunk intersects any token-paste provenance span for
  /// the invocation, forcing paste-aware argument derivation.
  static bool HunkTouchesAnyPasteToken(const RefoldModel::MacroInvocation &m,
                                       const diffutils::Hunk &h);

  /// Derive the single argument rewrite implied by a hunk touching one pasted
  /// token, or fail closed when the pasted segment cannot be isolated.
  std::optional<PasteArgEdit>
  DerivePasteArgEdit(const RefoldModel::MacroInvocation &m,
                     const diffutils::Hunk &h) const;

  /// Derive all argument rewrites required by paste-token edits in the hunk,
  /// preserving the unique-segment proof requirement for each paste site.
  ///
  /// Returns `AmbiguousOrigin` rather than an empty result when a touched
  /// paste token has more than one argument origin, so that a caller cannot
  /// mistake a refusal for "this derivation does not apply" and re-decide it
  /// with a weaker rule.
  PasteArgEditsResult
  DerivePasteArgEdits(const RefoldModel::MacroInvocation &m,
                      const diffutils::Hunk &h) const;

  /// Split a pasted A token and its B replacement into per-argument segments
  /// when the fixed non-argument slices force exactly one segmentation.
  ///
  /// Reports `AmbiguousOrigin` when two anchor occurrences both replay to
  /// different segments; the fixed slices then do not determine the split and
  /// the caller must not choose one.
  static PasteTokenSegmentation SegmentPastedTokenArgsByFixedSlices(
      llvm::StringRef aTok, llvm::StringRef bTok,
      llvm::ArrayRef<const RefoldModel::PPArgSpan *> spansAsc);

  /// Return the replacement spelling segment corresponding to an old pasted
  /// argument segment inside a rewritten argument spelling.
  static llvm::StringRef DeriveNewPasteSegmentFromSpellingReplacement(
      llvm::StringRef baseArg, llvm::StringRef newArg, llvm::StringRef oldSeg);

  /// Verify that the derived argument replacements reproduce every touched
  /// pasted token on the B side before accepting an args-only patch.
  bool PasteArgReplacementsMatchAllPasteTokensInB(
      const RefoldModel::MacroInvocation &m, llvm::StringRef baseInvText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invArgRanges,
      const llvm::DenseMap<uint32_t, std::string> &replByArgIdx) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPASTEARGUMENTBUILDER_H
