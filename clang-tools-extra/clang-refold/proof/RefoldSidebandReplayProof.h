//===--- RefoldSidebandReplayProof.h --------------------------*- C++ -*-===//
//
// Sideband pragma replay proof carriers, replay-envelope helpers, and
// validation reporting.
//
// Sideband pragmas are zero-normal-token artifacts: Clang may print their
// directive text in the raw preprocessed replay stream even though the ordinary
// PP-token model has deliberately removed those tokens before structural
// diffing.  This module owns the proof objects, pure replay helpers, and shared
// validation reporter that binds such source-side pragma edits to explicit
// raw-B byte witnesses.
//
// Callers remain responsible for edit emission, line-resync application,
// and accepted-result attachment.  Terminal-fallback requests for failed
// sideband proof validation are reported through RefoldTerminalProofSink so
// engine and include-materialization paths share the same fail-closed policy.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSIDEBANDREPLAYPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSIDEBANDREPLAYPROOF_H

#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace clang {
namespace refold {

class RefoldTerminalProofSink;

using llvm::ArrayRef;
using llvm::StringRef;

/// Source site together with the closure proof that made it legal.
///
/// This is the complete source-side proof object for one owner-local sideband
/// edit.  `path` names the owning source file, `[begin,end)` is the half-open
/// physical byte range in that file, `ownerIncludeId` records the concrete
/// include occurrence when the owner is not the emitted TU, and `closureKind`
/// records which source-closure proof made that site legal.  Ordered-anchor
/// obligations are discharged before this object can be constructed.
struct OwnerLocalSourceEditProof {
  enum class ClosureKind {
    Unknown,
    /// Zero-width source insertion.
    ZeroWidthInsertion,
    /// One physical source atom is consumed.
    SingleSourceAtom,
    /// Multiple source atoms form a whitespace-separated owner-local run.
    WhitespaceSeparatedRun
  };

private:
  std::string path;
  uint64_t begin = 0;
  uint64_t end = 0;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
  ClosureKind closureKind = ClosureKind::Unknown;

  OwnerLocalSourceEditProof(std::string path, uint64_t begin, uint64_t end,
                            std::optional<uint64_t> ownerIncludeId,
                            ClosureKind closureKind)
      : path(std::move(path)), begin(begin), end(end),
        ownerIncludeId(ownerIncludeId), closureKind(closureKind) {}

public:
  /// Build the source proof for a zero-width insertion site.
  static OwnerLocalSourceEditProof
  ZeroWidthInsertion(StringRef path, uint64_t byte,
                     std::optional<uint64_t> ownerIncludeId) {
    return OwnerLocalSourceEditProof(path.str(), byte, byte, ownerIncludeId,
                                     ClosureKind::ZeroWidthInsertion);
  }

  /// Build the source proof for consuming exactly one source atom.
  static OwnerLocalSourceEditProof
  SourceAtom(StringRef path, uint64_t begin, uint64_t end,
             std::optional<uint64_t> ownerIncludeId) {
    return OwnerLocalSourceEditProof(path.str(), begin, end, ownerIncludeId,
                                     ClosureKind::SingleSourceAtom);
  }

  /// Build the source proof for a whitespace-separated owner-local run.
  static OwnerLocalSourceEditProof
  WhitespaceSeparatedRun(StringRef path, uint64_t begin, uint64_t end,
                         std::optional<uint64_t> ownerIncludeId) {
    return OwnerLocalSourceEditProof(path.str(), begin, end, ownerIncludeId,
                                     ClosureKind::WhitespaceSeparatedRun);
  }

  /// Build the source proof for a consumed non-empty source-atom run.
  static OwnerLocalSourceEditProof
  ConsumedSourceRun(StringRef path, uint64_t begin, uint64_t end,
                    std::optional<uint64_t> ownerIncludeId,
                    uint64_t atomCount) {
    if (atomCount == 0)
      return OwnerLocalSourceEditProof(path.str(), begin, end, ownerIncludeId,
                                       ClosureKind::Unknown);
    if (atomCount == 1)
      return SourceAtom(path, begin, end, ownerIncludeId);
    return WhitespaceSeparatedRun(path, begin, end, ownerIncludeId);
  }

  /// Return the physical source owner path carried by this proof.
  StringRef SourcePath() const { return path; }

  /// Return the concrete include owner, when this proof is header-owned.
  std::optional<uint64_t> OwnerIncludeId() const { return ownerIncludeId; }

  /// Return the owner-local source byte where this proof begins.
  uint64_t SourceBegin() const { return begin; }

  /// Return the owner-local source byte where this proof ends.
  uint64_t SourceEnd() const { return end; }

  bool IsValid() const { return begin <= end; }

  /// Return the owner-local source byte range carried by this proof.
  std::pair<uint64_t, uint64_t> SourceByteRange() const { return {begin, end}; }

  /// Return true when the proof targets a concrete include owner.
  bool HasConcreteIncludeOwner() const { return ownerIncludeId.has_value(); }

  /// Return true when this proof targets the requested include owner.
  bool TargetsInclude(uint64_t includeId) const {
    return ownerIncludeId && *ownerIncludeId == includeId;
  }

  /// Return true when the next source atom is on the same owner-local surface
  /// and is ordered after the current source proof.  The caller remains
  /// responsible for proving that the intervening bytes are whitespace-only.
  bool
  CanExtendThroughSourceAtom(StringRef nextPath, uint64_t nextBegin,
                             uint64_t nextEnd,
                             std::optional<uint64_t> nextOwnerIncludeId) const {
    return IsValid() && StringRef(path) == nextPath &&
           ownerIncludeId == nextOwnerIncludeId && end <= nextBegin &&
           nextBegin <= nextEnd;
  }

  /// Extend this proof through the next source atom after the caller has
  /// discharged the whitespace-only gap proof.
  bool ExtendThroughSourceAtom(StringRef nextPath, uint64_t nextBegin,
                               uint64_t nextEnd,
                               std::optional<uint64_t> nextOwnerIncludeId) {
    if (!CanExtendThroughSourceAtom(nextPath, nextBegin, nextEnd,
                                    nextOwnerIncludeId))
      return false;
    end = nextEnd;
    return true;
  }

  /// Return true when the proved source range is valid in an owner byte
  /// buffer of size \p ownerSize.  This is the owner-local range gate used
  /// by both TU and include-owned sideband emission.
  bool IsWithinOwnerBytes(uint64_t ownerSize) const {
    return begin <= end && end <= ownerSize;
  }

  bool HasClosureProof() const { return closureKind != ClosureKind::Unknown; }

  /// Return true when the proof is exactly a zero-width insertion site.
  ///
  /// Boundary ownership decisions use this to distinguish a genuine B-only
  /// sideband insertion from a sideband replacement/deletion that merely
  /// happens to target the same include owner.  Only a zero-width insertion may
  /// pull an ordinary B insertion island onto the header surface for joint
  /// replay.
  bool IsZeroWidthInsertion() const {
    return closureKind == ClosureKind::ZeroWidthInsertion && begin == end;
  }

  bool IsComplete() const { return IsValid() && HasClosureProof(); }
};

/// Materialized B-side replay proof for one owner-local sideband edit.
///
/// Sideband pragmas are removed from the ordinary token streams before the
/// structural diff runs, so their edit-map provenance must be carried as raw
/// B bytes.  The replacement payload and its raw-B byte witness range are one
/// proof fact: `text` is what will be emitted, and `[begin,end)` is the
/// B-side byte range that witnessed that emission.
struct OwnerLocalBReplayProof {
private:
  std::string text;
  uint64_t begin = 0;
  uint64_t end = 0;

  OwnerLocalBReplayProof(std::string text, uint64_t begin, uint64_t end)
      : text(std::move(text)), begin(begin), end(end) {}

public:
  /// Build a replay proof from the exact B-side bytes that will be emitted.
  static OwnerLocalBReplayProof FromText(StringRef text, uint64_t begin,
                                         uint64_t end) {
    return OwnerLocalBReplayProof(text.str(), begin, end);
  }

  /// Build the zero-width B-side replay proof used for sideband deletions.
  static OwnerLocalBReplayProof EmptyAt(uint64_t byte) {
    return OwnerLocalBReplayProof("", byte, byte);
  }

  bool IsValid(uint64_t bSize) const { return begin <= end && end <= bSize; }

  /// Return the raw-B byte witness range carried by this replay proof.
  std::pair<uint64_t, uint64_t> MaterializedBByteRange() const {
    return {begin, end};
  }

  /// Return the emitted replacement text carried by this replay proof.
  StringRef ReplacementText() const { return text; }

  /// Return the emitted replacement-text length carried by this replay proof.
  uint64_t ReplacementTextSize() const {
    return static_cast<uint64_t>(text.size());
  }

  /// Return the emitted replacement-text range witnessed by this replay proof.
  std::pair<uint64_t, uint64_t> MaterializedOutputTextRange() const {
    return {0, ReplacementTextSize()};
  }

  /// Return true when the replay proof emits sideband-owned trailing blank
  /// line material.  This is the B-side replay predicate used by header-local
  /// sideband resync: a plain directive line owns one terminator, while an
  /// additional trailing terminator means the B replay envelope also owned a
  /// blank line before copied header suffix bytes.
  bool OwnsTrailingReplayBlankLine() const {
    unsigned trailingLineTerminators = 0;
    size_t i = text.size();
    while (i > 0) {
      const char c = text[i - 1];
      if (stringutils::isNonNewlineWs(c)) {
        --i;
        continue;
      }
      if (c == '\n') {
        --i;
        if (i > 0 && text[i - 1] == '\r')
          --i;
        ++trailingLineTerminators;
        continue;
      }
      if (c == '\r') {
        --i;
        ++trailingLineTerminators;
        continue;
      }
      break;
    }
    return trailingLineTerminators > 1;
  }

  bool EmitsVisibleText() const { return !text.empty(); }
};

/// Describes a source-side edit for a preserved sideband `#pragma` line
/// printed in the raw `.i` replay surface but excluded from the modeled
/// preprocessor-token stream.
///
/// Unknown pragmas are special because Clang may preserve their directive
/// text in `-E -P` output even though the producer's token count describes
/// only ordinary PP tokens. The driver removes such sideband directive tokens
/// from the A/B token streams before structural diffing, then passes the
/// corresponding source directive edits here so the engine can delete or
/// replace the original `DirectivePragmaItem` without falling back to raw B.
struct SidebandPragmaEdit {
private:
  OwnerLocalSourceEditProof source;
  OwnerLocalBReplayProof replay;

  SidebandPragmaEdit(OwnerLocalSourceEditProof source,
                     OwnerLocalBReplayProof replay)
      : source(std::move(source)), replay(std::move(replay)) {}

public:
  /// Return the physical source owner path carried by this sideband proof.
  StringRef SourcePath() const { return source.SourcePath(); }

  std::optional<uint64_t> OwnerIncludeId() const {
    return source.OwnerIncludeId();
  }

  bool HasConcreteIncludeOwner() const {
    return source.HasConcreteIncludeOwner();
  }

  std::pair<uint64_t, uint64_t> SourceByteRange() const {
    return source.SourceByteRange();
  }

  bool SourceHasClosureProof() const { return source.HasClosureProof(); }

  bool SourceIsValid() const { return source.IsValid(); }

  bool SourceIsWithinOwnerBytes(uint64_t ownerSize) const {
    return source.IsWithinOwnerBytes(ownerSize);
  }

  bool TargetsInclude(uint64_t includeId) const {
    return source.TargetsInclude(includeId);
  }

  /// Return the emitted replacement text carried by this complete sideband
  /// proof.  Emission paths consume the sideband edit as one source+replay
  /// proof object instead of opening the nested replay proof directly.
  StringRef ReplacementText() const { return replay.ReplacementText(); }

  /// Return true when the B replay proof owns trailing blank-line material
  /// after the visible sideband line.
  bool OwnsTrailingReplayBlankLine() const {
    return replay.OwnsTrailingReplayBlankLine();
  }

  /// Return the raw-B byte witness range for this sideband replay.
  std::pair<uint64_t, uint64_t> MaterializedBByteRange() const {
    return replay.MaterializedBByteRange();
  }

  /// Return the replacement-text range witnessed by this sideband replay.
  std::pair<uint64_t, uint64_t> MaterializedOutputTextRange() const {
    return replay.MaterializedOutputTextRange();
  }

  /// Return true when this sideband edit emits visible replay text.
  bool EmitsVisibleReplayText() const { return replay.EmitsVisibleText(); }

  /// Return true when the source-side proof is a zero-width insertion.
  bool SourceIsZeroWidthInsertion() const {
    return source.IsZeroWidthInsertion();
  }

  /// Return true when the B replay proof range is valid in the edited
  /// preprocessed B buffer.
  bool ReplayIsValid(uint64_t bSize) const { return replay.IsValid(bSize); }

  /// Return true when visible header-owned sideband replay forces a real
  /// include owner transition.  This is derived from the proved edit facts
  /// rather than stored as mutable proof state: a sideband edit needs
  /// wrappers precisely when it targets a concrete include and emits
  /// non-empty text.
  bool ForcesIncludeLineDirectiveWrappers() const {
    return OwnerIncludeId() && EmitsVisibleReplayText();
  }

  bool IsComplete(uint64_t bSize) const {
    return source.IsComplete() && ReplayIsValid(bSize);
  }

  /// Build a complete sideband edit from already-proved source and replay
  /// facts.  This keeps the final construction gate with the proof object:
  /// callers may supply only a discharged source proof, a B replay proof, and
  /// the B-buffer size used to validate the replay witness range.
  static std::optional<SidebandPragmaEdit>
  Create(std::optional<OwnerLocalSourceEditProof> source,
         OwnerLocalBReplayProof replay, uint64_t bSize) {
    if (!source)
      return std::nullopt;
    SidebandPragmaEdit edit(std::move(*source), std::move(replay));
    if (!edit.IsComplete(bSize))
      return std::nullopt;
    return edit;
  }
};

/// Return an implementation-local validation failure for a complete sideband
/// proof, or std::nullopt when the proof is structurally valid for the B
/// buffer.
///
/// This helper is intentionally side-effect free: it checks the source closure
/// proof and B replay envelope without mutating the terminal fallback ledger.
std::optional<StringRef>
validateSidebandPragmaEditProof(const SidebandPragmaEdit &edit, uint64_t bSize);

/// Validate one sideband pragma proof and report a classified terminal fallback
/// request when validation fails.
///
/// This is the shared fail-closed reporting gate for TU and include-owned
/// sideband edits.  Keeping the predicate above pure and the failure
/// translation here prevents each caller from hand-encoding the same
/// terminal-fallback obligation, reason, and diagnostic detail.  `traceSuccess`
/// preserves the engine-level owner-local trace without forcing include
/// materialization to add new success logs.
bool validateAndReportSidebandPragmaEditProof(
    const SidebandPragmaEdit &edit, uint64_t bSize,
    const RefoldTerminalProofSink &terminalSink, llvm::StringRef stage,
    bool traceSuccess);

/// Remove visible sideband replay bytes from an ordinary replay payload when
/// those bytes are owned by separate, non-insertion sideband source edits.
///
/// B-only sideband insertions may be carried by the surrounding ordinary
/// insertion island.  Sideband replacements/deletions have their own source
/// edit and must be removed from the ordinary replay payload to avoid
/// duplicating the emitted pragma text.
std::string stripSeparatelyOwnedSidebandReplay(
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits, StringRef replayText,
    std::optional<uint64_t> replayBByteBegin,
    std::optional<uint64_t> replayBByteEnd);

/// Return the union of B-byte envelopes contributed by sideband pragma edits
/// owned by one materialized include.
///
/// The helper is a pure proof-envelope query: it validates each candidate
/// replay range against the B buffer size and returns std::nullopt when no
/// complete sideband witness exists for the include.
std::optional<std::pair<uint64_t, uint64_t>>
sidebandPragmaMaterializedBByteRangeForInclude(
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits, uint64_t includeId,
    uint64_t bSize);

/// Return true when a zero-width sideband insertion's raw-B byte witness is
/// already covered by an ordinary include insertion patch envelope.
///
/// This is the pure proof predicate used before staging header-owned sideband
/// source edits: when the ordinary patch already owns the visible B replay
/// bytes, separately staging the sideband edit would duplicate the pragma text.
bool sidebandInsertionReplayIsCoveredByPatchEnvelope(
    const SidebandPragmaEdit &sideband, uint64_t patchBByteBegin,
    uint64_t patchBByteEnd);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSIDEBANDREPLAYPROOF_H
