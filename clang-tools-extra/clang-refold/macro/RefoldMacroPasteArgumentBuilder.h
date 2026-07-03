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

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

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
  std::optional<std::vector<PasteArgEdit>>
  DerivePasteArgEdits(const RefoldModel::MacroInvocation &m,
                      const diffutils::Hunk &h) const;

  /// Split a pasted A token and its B replacement into per-argument segments
  /// when the fixed non-argument slices give a deterministic segmentation.
  static std::optional<std::vector<std::string>>
  SegmentPastedTokenArgsByFixedSlices(
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
