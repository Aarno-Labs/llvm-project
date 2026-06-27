//===--- RefoldIncludeReplayProof.h ---------------------------*- C++ -*-===//
//
// This file declares the include/include_next replay proof layer used by
// clang-refold include materialization.  The proof context is read-only: it
// consumes immutable model/source inputs plus explicit service callbacks for
// the few engine-owned queries that still belong to RefoldEngine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEREPLAYPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEREPLAYPROOF_H

#include "LineDirectiveInserter.h"
#include "RefoldLog.h"
#include "RefoldModel.h"
#include "RefoldProofTypes.h"
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

/// Immutable data needed by include replay proof.  These references are owned
/// by RefoldEngine and must outlive the short-lived proof context constructed
/// for one materialization operation.
struct IncludeReplayProofInputs {
  const RefoldModel &Model;
  StringRef ASource;
  const LineDirectiveInserter &LineDirs;
  const std::optional<FinalReplaySurface> &FinalReplaySurface;
};

/// Read-only engine services used by include replay proof.  The function_ref
/// callables are deliberately non-owning so this layer cannot retain mutable
/// engine state beyond the caller's materialization scope.
struct IncludeReplayProofServices {
  llvm::function_ref<StringRef(uint64_t Begin, uint64_t End)> SliceASource;

  llvm::function_ref<bool(StringRef CandidatePath,
                          const RefoldModel::IncludeItem &Include)>
      SamePhysicalIncludeFile;

  llvm::function_ref<const RefoldModel::MacroInvocation *(
      const RefoldModel::MacroInvocation &Macro)>
      LineStateObservableMacroSite;

  llvm::function_ref<bool(const RefoldModel::MacroInvocation &Macro)>
      LineStateBuiltinInvocationIsPreservedObserver;

  llvm::function_ref<LineStateObserverDemand(uint64_t IncludeId)>
      IncludeSubtreeLineStateObserverDemand;
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
    // angled ordinary include, while the older quoted-child relocation repair
    // remains quoted by construction.
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
      : services_(services), model_(inputs.Model), aSource_(inputs.ASource),
        lineDirs_(inputs.LineDirs),
        finalReplaySurface_(inputs.FinalReplaySurface) {}

  CleanChildIncludeReplayPlan planCleanChildIncludeReplayFromMaterializedParent(
      const RefoldModel::IncludeItem &child) const;

  static const char *ordinaryIncludeDelimiterName(
      OrdinaryIncludeDelimiterKind kind) {
    switch (kind) {
    case OrdinaryIncludeDelimiterKind::Quoted:
      return "quote";
    case OrdinaryIncludeDelimiterKind::Angled:
      return "angle";
    }
    llvm_unreachable("Invalid OrdinaryIncludeDelimiterKind");
  }

  static char ordinaryIncludeDelimiterOpen(OrdinaryIncludeDelimiterKind kind) {
    switch (kind) {
    case OrdinaryIncludeDelimiterKind::Quoted:
      return '"';
    case OrdinaryIncludeDelimiterKind::Angled:
      return '<';
    }
    llvm_unreachable("Invalid OrdinaryIncludeDelimiterKind");
  }

  static char ordinaryIncludeDelimiterClose(OrdinaryIncludeDelimiterKind kind) {
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
    std::filesystem::path PhysicalPath;
    std::string EnteredFileSpelling;
    std::string EnteredFileName;

    // Ordinary include replay has two independent facts to preserve:
    //
    //   * the syntax context that started lookup (quoted vs angled vs direct
    //     source-relative / absolute operand), and
    //   * for search-chain hits, the producer-normalized HeaderSearch entry
    //     that selected the file.
    //
    // The old implementation collapsed all search-dir hits into broad
    // QuoteSearchDir/AngleSearchDir buckets.  That was enough for physical-file
    // equality, but it loses the selected search-chain index that later
    // include-next proof needs.  Keep the replay candidate tied to the actual
    // search entry kind and index whenever lookup came from
    // pp_ctx.include_search_chain.
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

    LookupKind Kind = LookupKind::Unknown;

    // Present only when Kind names a producer/legacy include-search entry.
    // New-schema candidates use the exact pp_ctx.include_search_chain index;
    // legacy argv-reconstructed candidates intentionally leave this empty so
    // future include-next proof cannot mistake argv inference for producer
    // cursor provenance.
    std::optional<uint32_t> SearchChainIndex;
  };

  struct IncludeNextReplayCandidate {
    std::filesystem::path PhysicalPath;
    std::string EnteredFileSpelling;
    std::string EnteredFileName;

    // The resume cursor is the first producer search-chain index examined by
    // #include_next replay.  Unlike ordinary includes, include_next never
    // starts from source-relative lookup or from the beginning of the search
    // chain; it resumes immediately after the search entry that selected the
    // containing file.
    uint32_t ResumeSearchChainIndex = 0;

    // The producer search-chain entry that replay selected.  This must be an
    // actual pp_ctx.include_search_chain index, not a legacy argv-derived
    // approximation, because descendant #include_next proof is a proof about
    // HeaderSearch cursor state.
    uint32_t SelectedSearchChainIndex = 0;
    IncludeLookupKind SelectedKind = IncludeLookupKind::Unknown;
  };

  struct IncludeReplaySearchDir {
    std::filesystem::path LookupPath;
    std::string EnteredSpellingPrefix;
    IncludeReplayCandidate::LookupKind Kind =
        IncludeReplayCandidate::LookupKind::Unknown;
    std::optional<uint32_t> SearchChainIndex;

    // True for producer search-chain entries that this consumer deliberately
    // does not model as ordinary directories.  Such entries are not skipped:
    // if lookup reaches one before finding the requested header, the replay
    // result is unknown because the unmodeled entry may have selected or
    // shadowed the target.
    bool IsUnsupportedBarrier = false;
  };

  struct OrdinaryIncludeReplayResult {
    std::filesystem::path PhysicalPath;
    std::string EnteredFileSpelling;
    std::string EnteredFileName;
    IncludeReplayCandidate::LookupKind LookupKind =
        IncludeReplayCandidate::LookupKind::Unknown;

    // Present only for results selected by a modeled producer search-chain
    // entry.  Source-relative and absolute-operand hits have no HeaderSearch
    // cursor; legacy argv-reconstructed hits intentionally leave this empty.
    std::optional<uint32_t> SearchChainIndex;
  };

  enum class OrdinaryIncludeReplayFailureKind {
    None,
    Unresolved,
    UnknownSearchChainEntry
  };

  struct IncludeReplaySurface {
    std::filesystem::path SourceDirectoryPath;
    std::string SourceDirectorySpelling;
  };

  struct RecordedIncludeSearchDirs {
    // Direct source-relative lookup is handled before these lists.  The quoted
    // list then contains the exact directories that quoted include lookup may
    // search, while the angled list contains only entries valid for angled
    // lookup.  New maps populate both lists from pp_ctx.include_search_chain,
    // preserving the producer's effective HeaderSearch order and entry index.
    SmallVector<IncludeReplaySearchDir, 48> QuotedLookupDirs;
    SmallVector<IncludeReplaySearchDir, 32> AngledLookupDirs;
  };

  /// Convert producer lookup provenance to the ordinary directory replay kinds
  /// modeled by include replay proof.  Keeping this as a private member avoids
  /// exposing the private IncludeReplayCandidate carrier outside this proof
  /// context while still sharing the conversion across ordinary include and
  /// include-next replay checks.
  static std::optional<IncludeReplayCandidate::LookupKind>
  replayableDirectoryLookupKind(IncludeLookupKind kind);

  mutable std::optional<RecordedIncludeSearchDirs>
      recordedIncludeSearchDirsCache_;

  IncludeReplayProofServices services_;
  const RefoldModel &model_;
  StringRef aSource_;
  const LineDirectiveInserter &lineDirs_;
  const std::optional<FinalReplaySurface> &finalReplaySurface_;

  StringRef SliceASource(uint64_t begin, uint64_t end) const {
    return services_.SliceASource(begin, end);
  }

  const RefoldModel::MacroInvocation *LineStateObservableMacroSite(
      const RefoldModel::MacroInvocation &macro) const {
    return services_.LineStateObservableMacroSite(macro);
  }

  bool LineStateBuiltinInvocationIsPreservedObserver(
      const RefoldModel::MacroInvocation &macro) const {
    return services_.LineStateBuiltinInvocationIsPreservedObserver(macro);
  }

  LineStateObserverDemand
  IncludeSubtreeLineStateObserverDemand(uint64_t includeId) const {
    return services_.IncludeSubtreeLineStateObserverDemand(includeId);
  }

  bool samePhysicalIncludeFile(
      StringRef candidatePath, const RefoldModel::IncludeItem &include) const {
    return services_.SamePhysicalIncludeFile(candidatePath, include);
  }
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEREPLAYPROOF_H
