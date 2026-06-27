//===--- RefoldIncludeReplayProof.h ---------------------------*- C++ -*-===//
//
// Include/include_next replay proof layer for clang-refold.
//
// The proof context is read-only: it consumes immutable model/source inputs plus
// explicit service callbacks for owner-local queries, then returns whether an
// include edge may be preserved, rewritten, or must be materialized.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEREPLAYPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEREPLAYPROOF_H

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "proof/RefoldProofTypes.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <filesystem>
#include <optional>
#include <string>

namespace clang {
namespace refold {

using llvm::DenseMap;
using llvm::DenseSet;
using llvm::SmallVector;
using llvm::StringRef;

/// Build the final-source include replay surface used by include replay proof.
///
/// The driver checks the emitted source by preprocessing the final `--out`
/// path, not the producer TU path recorded in the map.  This helper models
/// that final quoted-include lookup surface without mutating RefoldEngine.
std::optional<FinalReplaySurface>
buildFinalReplaySurface(const RefoldModel &model, StringRef finalOutputPath);

/// Immutable data needed by include replay proof.  These references are
/// borrowed from the current refold run and must outlive the short-lived proof
/// context constructed for one materialization operation.
struct IncludeReplayProofInputs {
  const RefoldModel &model;
  StringRef aSource;
  const LineDirectiveInserter &lineDirs;
  const std::optional<FinalReplaySurface> &finalReplaySurface;
};

/// Read-only services used by include replay proof.  The function_ref callables
/// are deliberately non-owning so this layer cannot retain mutable state beyond
/// the caller's materialization scope.
struct IncludeReplayProofServices {
  llvm::function_ref<StringRef(uint64_t Begin, uint64_t End)> SliceASource;

  llvm::function_ref<bool(StringRef CandidatePath,
                          const RefoldModel::IncludeItem &Include)>
      samePhysicalIncludeFile;

  llvm::function_ref<const RefoldModel::MacroInvocation *(
      const RefoldModel::MacroInvocation &Macro)>
      lineStateObservableMacroSite;

  llvm::function_ref<bool(const RefoldModel::MacroInvocation &Macro)>
      lineStateBuiltinInvocationIsPreservedObserver;

  llvm::function_ref<LineStateObserverDemand(uint64_t IncludeId)>
      includeSubtreeLineStateObserverDemand;
};

class IncludeReplayProofContext {
public:
  enum class OrdinaryIncludeDelimiterKind { Quoted, Angled };

  enum class CleanChildIncludeReplayAction { None, RewriteOperand, Materialize };

  struct CleanChildIncludeReplayPlan {
    CleanChildIncludeReplayAction action = CleanChildIncludeReplayAction::None;
    std::string rewrittenOperand;

    // RewriteOperand plans can preserve an include directive with either
    // header-name delimiter.  Direct relocated #include_next repair may prove an
    // angled ordinary include; quoted child-relocation repairs remain quoted by
    // construction.
    OrdinaryIncludeDelimiterKind rewrittenDelimiterKind =
        OrdinaryIncludeDelimiterKind::Quoted;
    std::string reason;

    // Set when the materialization decision was forced by descendant
    // #include_next state.  The recursive materialization of this child must
    // realize nested include_next directives as concrete text rather than
    // rewriting them as ordinary includes; otherwise the final output would
    // still depend on replaying a search-stack-sensitive directive from a
    // relocated context.
    bool forceMaterializeDescendantIncludeNext = false;
  };

  IncludeReplayProofContext(IncludeReplayProofInputs inputs,
                            IncludeReplayProofServices services)
      : services_(services), model_(inputs.model), aSource_(inputs.aSource),
        lineDirs_(inputs.lineDirs),
        finalReplaySurface_(inputs.finalReplaySurface) {}

  CleanChildIncludeReplayPlan PlanCleanChildIncludeReplayFromMaterializedParent(
      const RefoldModel::IncludeItem &child) const;

  static const char *OrdinaryIncludeDelimiterName(
      OrdinaryIncludeDelimiterKind kind) {
    switch (kind) {
    case OrdinaryIncludeDelimiterKind::Quoted:
      return "quote";
    case OrdinaryIncludeDelimiterKind::Angled:
      return "angle";
    }
    llvm_unreachable("Invalid OrdinaryIncludeDelimiterKind");
  }

  static char OrdinaryIncludeDelimiterOpen(OrdinaryIncludeDelimiterKind kind) {
    switch (kind) {
    case OrdinaryIncludeDelimiterKind::Quoted:
      return '"';
    case OrdinaryIncludeDelimiterKind::Angled:
      return '<';
    }
    llvm_unreachable("Invalid OrdinaryIncludeDelimiterKind");
  }

  static char OrdinaryIncludeDelimiterClose(OrdinaryIncludeDelimiterKind kind) {
    switch (kind) {
    case OrdinaryIncludeDelimiterKind::Quoted:
      return '"';
    case OrdinaryIncludeDelimiterKind::Angled:
      return '>';
    }
    llvm_unreachable("Invalid OrdinaryIncludeDelimiterKind");
  }

private:
  struct IncludeReplayCandidate {
    std::filesystem::path physicalPath;
    std::string enteredFileSpelling;
    std::string enteredFileName;

    // Ordinary include replay has two independent facts to preserve:
    //
    //   * the syntax context that started lookup (quoted vs angled vs direct
    //     source-relative / absolute operand), and
    //   * for search-chain hits, the producer-normalized HeaderSearch entry
    //     that selected the file.
    //
    // Keep the replay candidate tied to the actual search entry kind and index
    // whenever lookup came from pp_ctx.include_search_chain.  Physical-file
    // equality alone is not enough for #include_next proof, which also needs
    // the producer-selected search-chain cursor.
    enum class LookupKind {
      DirectSourceRelative,
      QuoteDir,
      UserI,
      System,
      IdirAfter,
      Framework,
      Builtin,
      AbsoluteOperand,
      Unknown
    };

    LookupKind kind = LookupKind::Unknown;

    // Present only when Kind names a producer/legacy include-search entry.
    // New-schema candidates use the exact pp_ctx.include_search_chain index;
    // legacy argv-reconstructed candidates intentionally leave this empty so
    // future include-next proof cannot mistake argv inference for producer
    // cursor provenance.
    std::optional<uint32_t> searchChainIndex;
  };

  struct IncludeNextReplayCandidate {
    std::filesystem::path physicalPath;
    std::string enteredFileSpelling;
    std::string enteredFileName;

    // The resume cursor is the first producer search-chain index examined by
    // #include_next replay.  Unlike ordinary includes, include_next never
    // starts from source-relative lookup or from the beginning of the search
    // chain; it resumes immediately after the search entry that selected the
    // containing file.
    uint32_t resumeSearchChainIndex = 0;

    // The producer search-chain entry that replay selected.  This must be an
    // actual pp_ctx.include_search_chain index, not a legacy argv-derived
    // approximation, because descendant #include_next proof is a proof about
    // HeaderSearch cursor state.
    uint32_t selectedSearchChainIndex = 0;
    IncludeLookupKind selectedKind = IncludeLookupKind::Unknown;
  };

  struct IncludeReplaySearchDir {
    std::filesystem::path lookupPath;
    std::string enteredSpellingPrefix;
    IncludeReplayCandidate::LookupKind kind =
        IncludeReplayCandidate::LookupKind::Unknown;
    std::optional<uint32_t> searchChainIndex;

    // True for producer search-chain entries that this consumer deliberately
    // does not model as ordinary directories.  Such entries are not skipped:
    // if lookup reaches one before finding the requested header, the replay
    // result is unknown because the unmodeled entry may have selected or
    // shadowed the target.
    bool isUnsupportedBarrier = false;
  };

  struct OrdinaryIncludeReplayResult {
    std::filesystem::path physicalPath;
    std::string enteredFileSpelling;
    std::string enteredFileName;
    IncludeReplayCandidate::LookupKind lookupKind =
        IncludeReplayCandidate::LookupKind::Unknown;

    // Present only for results selected by a modeled producer search-chain
    // entry.  Source-relative and absolute-operand hits have no HeaderSearch
    // cursor; legacy argv-reconstructed hits intentionally leave this empty.
    std::optional<uint32_t> searchChainIndex;
  };

  enum class OrdinaryIncludeReplayFailureKind {
    None,
    Unresolved,
    UnknownSearchChainEntry
  };

  struct IncludeReplaySurface {
    std::filesystem::path sourceDirectoryPath;
    std::string sourceDirectorySpelling;
  };

  struct RecordedIncludeSearchDirs {
    // Direct source-relative lookup is handled before these lists.  The quoted
    // list then contains the exact directories that quoted include lookup may
    // search, while the angled list contains only entries valid for angled
    // lookup.  New maps populate both lists from pp_ctx.include_search_chain,
    // preserving the producer's effective HeaderSearch order and entry index.
    SmallVector<IncludeReplaySearchDir, 48> quotedLookupDirs;
    SmallVector<IncludeReplaySearchDir, 32> angledLookupDirs;
  };

  /// Convert producer lookup provenance to the ordinary directory replay kinds
  /// modeled by include replay proof.  Keeping this as a private member avoids
  /// exposing the private IncludeReplayCandidate carrier outside this proof
  /// context while still sharing the conversion across ordinary include and
  /// include-next replay checks.
  static std::optional<IncludeReplayCandidate::LookupKind>
  ReplayableDirectoryLookupKind(IncludeLookupKind kind);

  mutable std::optional<RecordedIncludeSearchDirs>
      recordedIncludeSearchDirsCache_;

  IncludeReplayProofServices services_;
  const RefoldModel &model_;
  StringRef aSource_;
  const LineDirectiveInserter &lineDirs_;
  const std::optional<FinalReplaySurface> &finalReplaySurface_;

};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEREPLAYPROOF_H
