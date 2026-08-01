//===--- RefoldMacroDefinitionTapeSolver.h --------------------*- C++ -*-===//
//
// Definition replacement-list tape replay for macro args-only patch
// construction.
//
// This service owns the proof path for empty actuals, zero-token formal
// occurrences, and __VA_OPT__ branch flips. It parses the macro definition's
// replacement-token tape and searches for a B-token assignment to each formal
// that reproduces the producer-observed expansion.  A miss is non-terminal
// (the caller continues with paste-aware and standard-formal replay paths).
//
// Proof carrier attachment is performed inside this service through the
// borrowed RefoldProofLattice; the planner does not need to certify anything
// after the returned MacroPatch.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODEFINITIONTAPESOLVER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODEFINITIONTAPESOLVER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class RefoldProofLattice;
class RefoldSourceMapper;

/// Replays macro definition replacement-list text for definition-tape based
/// candidates.  The solver is used when a patch can be proven from the macro
/// definition surface rather than from direct invocation argument replay.
class RefoldMacroDefinitionTapeSolver {
public:
  struct Dependencies {
    const RefoldModel *model = nullptr;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    const RefoldSourceMapper *sourceMapper = nullptr;
    RefoldProofLattice *proofLattice = nullptr;
    const clang::LangOptions *lexLang = nullptr;
  };

  explicit RefoldMacroDefinitionTapeSolver(Dependencies deps)
      : deps_(std::move(deps)) {}

  /// Try the definition replacement-list replay proof used for empty actuals,
  /// zero-token formal occurrences, and __VA_OPT__ branch flips.  A miss here
  /// is non-terminal: the caller continues with paste-aware and standard
  /// formal replay strategies.
  std::optional<MacroPatch> TryDefinitionTapeReplayArgsOnlyPatch(
      const RefoldModel::MacroInvocation &invocation,
      const diffutils::Hunk &hunk, llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invArgRanges) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODEFINITIONTAPESOLVER_H
