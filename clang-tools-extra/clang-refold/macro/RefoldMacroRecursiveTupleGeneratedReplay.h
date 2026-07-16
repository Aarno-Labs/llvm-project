//===--- RefoldMacroRecursiveTupleGeneratedReplay.h ------------*- C++ -*-===//
//
// Recursive tuple-generated-callee replay boundary for clang-refold.
//
// This resolver is the focused entry point for the recursive theorem in which a
// root macro forwards a selector formal and a tuple formal through
// producer-recorded macro ancestry until one terminal generated function-like
// callee consumes exact elements of the root tuple.  The implementation is kept
// separate from the standard args-only builder so forwarding-graph composition
// can remain private while tuple-slice proof, generated-callee replay, and
// tuple edit application live behind one deterministic boundary without
// creating a broad public service.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRORECURSIVETUPLEGENERATEDREPLAY_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRORECURSIVETUPLEGENERATEDREPLAY_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace clang {
namespace refold {

class RefoldMacroGeneratedCalleeReplayEngine;
class RefoldMacroPatchProofCertifier;
class RefoldMacroTopology;
class RefoldSourceMapper;

/// Request for one recursive tuple-generated-callee replay attempt.
///
/// The request names the already-selected root invocation and its args-only
/// replay envelope.  Later implementation steps will prove a unique
/// caller_macro_id path from this root to one terminal generated callee and then
/// rewrite only exact slices of one source-spelled root tuple argument.
struct RecursiveTupleGeneratedReplayRequest {
  /// Root macro invocation considered for source-preserving tuple repair.
  const RefoldModel::MacroInvocation &rootInvocation;

  /// Macro definition used by the root invocation.
  const RefoldModel::MacroDirective &rootDefinition;

  /// Complete source spelling of the root invocation.
  llvm::StringRef baseInvocationText;

  /// Formal-content byte ranges inside `baseInvocationText`.
  llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;

  /// Whole-cover A-token envelope governed by the root invocation.
  const std::pair<uint64_t, uint64_t> &wholeCoverATokens;

  /// Edited B-token envelope that the generated-callee replay must realize.
  const std::pair<size_t, size_t> &bTokenEnvelope;
};

/// Focused resolver for recursive tuple-generated-callee replay.
///
/// This class is intentionally narrower than the existing generated-callee
/// replay engine.  It owns only the consumer-side composition theorem: use
/// producer-recorded ancestry and forwarding evidence to reduce a recursive
/// tuple-forwarding case to the existing terminal generated-callee solver.
/// Forwarding graph, exact whole-formal composition, terminal-edge
/// recognition, and tuple-slice derivation are behavior-neutral.  Candidate
/// construction is added by later replay/edit steps.
class RefoldMacroRecursiveTupleGeneratedReplay {
public:
  /// Borrowed services needed by the recursive replay resolver.
  ///
  /// All references must outlive the resolver.  Keeping the dependency set
  /// explicit prevents hidden planner access and makes the later integration
  /// point testable without introducing a new global service.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldSourceMapper &sourceMapper;
    const RefoldMacroTopology &macroTopology;
    const RefoldMacroGeneratedCalleeReplayEngine &generatedCalleeReplayEngine;
    RefoldMacroPatchProofCertifier &proofCertifier;
    /// Lexing options used for structural tuple element splitting.
    const clang::LangOptions &lexLang;
  };

  explicit RefoldMacroRecursiveTupleGeneratedReplay(Dependencies deps);

  /// Try to construct a direct paste-derived tuple-generated-callee candidate.
  ///
  /// This covers non-recursive roots shaped like `a##b t`, where the generated
  /// callee token is produced by token pasting over root actuals and the callee
  /// arguments are supplied by one parenthesized tuple actual.  The method owns
  /// only the tuple/generator theorem gate; terminal replay and root invocation
  /// rebuilding stay delegated to the generated-callee engine.
  std::optional<MacroPatch> BuildPasteTupleCandidate(
      const RecursiveTupleGeneratedReplayRequest &request) const;

  /// Try to construct a recursive tuple-generated-callee replay candidate.
  ///
  /// The method emits a patch only after proving the unique forwarding path,
  /// terminal generated-callee replay, exact root tuple slices, and root tuple
  /// edit.  Any missing or ambiguous obligation returns nullopt so existing
  /// fallback behavior remains available.
  std::optional<MacroPatch> BuildCandidate(
      const RecursiveTupleGeneratedReplayRequest &request) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRORECURSIVETUPLEGENERATEDREPLAY_H
