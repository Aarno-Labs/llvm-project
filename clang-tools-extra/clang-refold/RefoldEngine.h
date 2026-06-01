//===--- RefoldEngine.h -----------------------------------------*- C++ -*-===//
//
// This component implements the deterministic “refolding” engine that projects
// edits made to a raw preprocessed stream (B) back onto the original, partially
// expanded translation unit (TU) described by the refold map.
//
// Overview
// --------
// RefoldEngine consumes:
//   • A: original preprocessed bytes and tokens
//   • B: edited preprocessed bytes and tokens
//   • M: RefoldModel (parsed from the JSON refold map)
//
// It aligns A↔B token streams, derives edit hunks, classifies each hunk as
// TU-owned / include-owned / macro-invocation–owned, and materializes a new
// TU that incorporates edits while preserving original structure and semantics.
//
// Strict-Domain Theorem Contract
// ------------------------------
// clang-refold's theorem-facing contract is intentionally narrower than “all
// possible C preprocessor edits.”  It is complete for finite deterministic
// owner-closed edit tilings whose state-transition summaries compose and whose
// preserved suffix observers see preprocessing state equivalent to the edited
// preprocessed stream B.
//
// In that domain, the engine must emit source S' such that preprocessing S'
// produces exactly B.  It enforces the contract by requiring every emitted
// accepted edit to normalize to one final TheoremProofClass and every rejected or
// raw-B result to carry a declared TerminalFallbackProofFailure.
//
// A hunk is in-domain only when its proof discharges all of the following:
//   • each owner consumes a closed source interval and exactly the A-token
//     envelope it produced;
//   • the owner emits, preserves, or realizes exactly the B-token envelope
//     assigned to it;
//   • zero-token state transitions (`#define`, `#undef`, `#line`, include
//     guard effects, conditionals, pragmas, builtin location/counter state,
//     etc.) are preserved, repaired, widened into the closure, materialized,
//     or proven unobserved by the preserved suffix;
//   • mixed-owner hunks tile deterministically, gap-free, and in source order
//     over both A tokens and B tokens, including zero-token state gaps; and
//   • no upstream source-state mutation is reverse-solved from a downstream
//     expansion unless the directive itself lies inside the proven edited
//     source interval.
//
// Inputs outside this contract are not completeness failures.  They must be
// represented by an explicit failed proof obligation, materialized as a closed
// owner realization when possible, or rejected/fallen back in a way that
// preserves token soundness rather than emitting a speculative partial
// refolding.  The terminology in comments, theorem-audit logs, proof enums,
// and tests is expected to match this contract.
//
// Responsibilities
// ----------------
//   • Compute LCS-based A→B anchors and contiguous edit hunks.
//   • Attribute hunks to includes or macro call sites using M’s coverage data.
//   • Normalize and coalesce include insertions (line-local, boundary safe).
//   • Realize include expansions bottom-up, applying macro patches in-owner.
//   • Apply TU-level replacements with boundary hygiene (no token gluing).
//
// Determinism & Policy
// --------------------
//   • All iteration and sorting are stable; edits apply high→low to avoid
//   drift. • Boundary padding inserts at most one space locally when needed by
//     maximal-munch rules; internal whitespace is preserved verbatim.
//   • Errors are reported via the Logging subsystem (`fatal()/error()/...`).
//
// Public Surface
// --------------
//   • std::string Refold(...): orchestrates the end-to-end refolding and
//     returns the refolded TU text.
//   • Helper utilities: token/byte mapping, hunk builders, include realization,
//     macro-patch construction, and line-local boundary checks.
//
// Notes
// -----
//   • No RTTI or exceptions required; mirrors LLVM/Clang style.
//   • Paths are compared via canonicalization (see RefoldEngine::PathsEqual()).
//   • All indices are half-open where applicable: tokens [lo,hi), bytes [b,e).
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H

#include "DiffAlgorithms.h"
#include "FinalLineControlModel.h"
#include "LineDirectiveInserter.h"
#include "RefoldModel.h"
#include "StringUtils.h"
#include "clang/Basic/LangOptions.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

struct PPTok {
  // Clang token kind (e.g. "identifier", "numeric_constant", "string_literal",
  // "l_paren"). This is sourced from the token dump and is intentionally a
  // simple string.
  std::string kind;
  std::string spelling;
};

/// Half-open byte mapping for one edit that survived final TU emission.
///
/// `modifiedPreprocessed*` is expressed in the edited preprocessed stream B
/// passed via `--pp-mod`. `refoldedSource*` is expressed in the final refolded
/// C source emitted to `--out`. All ranges are byte offsets and use the same
/// half-open `[begin,end)` convention as the rest of the engine.
///
/// The two sides are materialization envelopes, not necessarily byte-for-byte
/// textual correspondences. For example, a macro-argument rewrite may map a
/// whole B-side macro expansion envelope to the compact argument text in the
/// preserved source invocation that regenerates that expansion.
struct MaterializedEditMapping {
  uint64_t modifiedPreprocessedBegin = 0;
  uint64_t modifiedPreprocessedEnd = 0;
  uint64_t refoldedSourceBegin = 0;
  uint64_t refoldedSourceEnd = 0;
};

/// One modified include owner emitted next to the refolded TU.
///
/// The normal clang-refold backend is a single-output backend: dirty include
/// owners are materialized into the TU.  A strictly narrower proof class can
/// preserve the include edge when the dirty header's active source `#line`
/// directive depends on macro state supplied by the immediate includer.  In
/// that case, materializing the header into the TU is token-sound but loses the
/// source-graph fact that the edit belongs to the header.  This side channel
/// writes the modified header bytes under the same quoted relative include path
/// beside the `.c.mod` output, so replay sees the edited owner while the TU
/// include spelling remains unchanged.
struct SourceGraphOutput {
  uint64_t includeId = 0;
  std::string relativePath;
  std::string originalTarget;
  std::string resolvedPath;
  std::string bytes;

  /// True when this entry is not an emitted sidecar, but a cleanup proof for a
  /// stale sidecar from an earlier run.  Source-graph selection is deliberately
  /// dynamic: a path that was admissible in one run can become inadmissible once
  /// another same-spelling include survives.  The driver may remove the stale
  /// file only if its bytes still exactly match \c bytes and it is not the
  /// producer-resolved input header.
  bool cleanupOnly = false;
};


/// \brief Deterministic refolder that projects edits made to raw preprocessed
/// C back onto the original translation unit (TU) without re-running the
/// preprocessor.
///
/// Inputs:
///   - A: original raw preprocessed text (e.g., clang -E -P output).
///   - B: edited raw preprocessed text.
///   - M: refold map JSON produced by a modified Clang preprocessor
///     (--refold-map=...). The map records provenance for includes,
///     conditionals, and macro expansions in PP-token coordinates, plus TU
///     file/byte ranges for patch sites.
///
/// Goal:
///
/// Reconstruct a TU that preserves the original preprocessor structure as much
/// as possible while still realizing the user's edits. Only constructs whose
/// expanded bytes were edited in B are forced to materialize in the output; all
/// others remain in their original form.
///
/// Policy and determinism:
///   - Token alignment uses an owner-aware LCS. Each PP-token gap in A is
///     assigned an ownerDepthGap derived from include depth and conditional
///     depth; LCS tie-breaks prefer edits to land at shallower ownership
///     boundaries (e.g., TU over deep includes).
///   - All selection among multiple valid owners is stable: smallest-covering
///     constructs win, ties break by id, and text edits apply right-to-left to
///     avoid offset drift.
///   - The engine fails fast when provenance is insufficient; it does not
///     guess.
///
/// Macro handling:
///
/// Macro edits are expressed as call-site patches:
///   - Whole-cover patches: replace an invocation span with edited expansion
///     bytes.
///   - Argument patches: when an edit is confined to invocation arguments,
///     rewrite the invocation text in-place (keeping the macro call in the
///     output).
///
/// Some expansions (notably builtin/object-like macros such as __FILE__ or
/// __LINE__) may appear inside another macro's replacement list and lack an
/// invocation-site span in M. Such edits are attributed to the nearest
/// enclosing patchable macro invocation (one with inv_b/inv_e/inv_text) so the
/// outer call site becomes the unit of change.
///
/// Stringification (#param):
///
/// Stringified arguments expand to string literal tokens in B and often do not
/// produce arg_spans in M. The engine recovers a mapping by parsing the active
/// #define preceding the invocation and locating #param occurrences in the
/// replacement list. If a string-literal edit can be safely inverted to a
/// single invocation argument (no commas/newlines; simple escape handling),
/// the invocation is rewritten; otherwise the invocation is expanded via a
/// whole-cover patch.
///
/// High-level pipeline:
///  1. Load M and the TU source.
///  2. Lex A and B into comparable PP-token sequences and build token-to-byte
///     offset tables.
///  3. Compute ownerDepthGap and an A->B token mapping via weighted LCS.
///  4. Derive edit hunks from the A->B mapping.
///  5. Classify each hunk deterministically as TU-owned, include-owned, or
///     macro-owned and build per-owner patch plans (including nested macro
///     patch baking).
///  6. Realize includes bottom-up along the recorded conditional arms and
///     apply patches to produce the final refolded TU.
///
/// Unsupported / out of scope:
///
/// The engine intentionally refuses macro-preserving rewrites unless the
/// producer map supplies enough local proof for the relevant feature (including
/// paste, stringify, variadic forwarding, and wrapper chains). When such proof
/// is unavailable, it prefers deterministic expansion of the affected macro
/// instance over speculative rewriting.
///
/// \author jeikenberry
class RefoldEngine {
struct TextEdit;
public:

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
    std::pair<uint64_t, uint64_t> SourceByteRange() const {
      return {begin, end};
    }

    /// Return true when the proof targets a concrete include owner.
    bool HasConcreteIncludeOwner() const { return ownerIncludeId.has_value(); }

    /// Return true when this proof targets the requested include owner.
    bool TargetsInclude(uint64_t includeId) const {
      return ownerIncludeId && *ownerIncludeId == includeId;
    }

    /// Return true when the next source atom is on the same owner-local surface
    /// and is ordered after the current source proof.  The caller remains
    /// responsible for proving that the intervening bytes are whitespace-only.
    bool CanExtendThroughSourceAtom(
        StringRef nextPath, uint64_t nextBegin, uint64_t nextEnd,
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

    bool HasClosureProof() const {
      return closureKind != ClosureKind::Unknown;
    }

    /// Return true when the proof is exactly a zero-width insertion site.
    ///
    /// Boundary ownership decisions use this to distinguish a genuine B-only
    /// sideband insertion from a sideband replacement/deletion that merely
    /// happens to target the same include owner.  Only the former may pull an
    /// ordinary B insertion island onto the header surface for joint replay.
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

    /// Return the emitted replacement-text length carried by this sideband
    /// proof.
    uint64_t ReplacementTextSize() const {
      return replay.ReplacementTextSize();
    }

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
    /// rather than stored as mutable proof state: a sideband edit needs wrappers
    /// precisely when it targets a concrete include and emits non-empty text.
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

  /// \brief Perform the end-to-end refolding process for a translation unit.
  ///
  /// This method takes the original preprocessed text *A* (e.g. `test.c.i`),
  /// the edited preprocessed text *B* (e.g. `test.c.i.mod`), and the refold
  /// map JSON produced by the modified Clang preprocessor. It re-lexes the
  /// edited text, aligns A↔B token streams, projects edits back through the
  /// preprocessor structure (macros, includes, and conditional arms) using
  /// the refold map, and emits a partially expanded C source file that
  /// preserves the original semantics while incorporating edits
  /// deterministically.
  ///
  /// #### Determinism
  /// * All decisions are derived from explicit byte/token spans in the refold
  ///   map; no heuristics cross physical newlines.
  /// * Ambiguities are treated as hard errors with precise diagnostics.
  ///
  /// #### Behavioral Guarantees
  /// * Macro edits are applied at the *invocation site*, never at the
  ///   definition.
  /// * `#include` / `#include_next` edits are realized *in place* within the
  ///   included file and recursively folded back.
  /// * Conditional blocks are re-emitted with the correct selected arm, and
  ///   edits land in the corresponding arm.
  /// * Token boundary hygiene prevents “glued” tokens; spaces are inserted only
  ///   when required by maximal-munch rules.
  ///
  /// \param root       Parsed refold map JSON (immutable model root).
  /// \param aSource    Original preprocessed text A (e.g., `test.c.i`).
  /// \param aToks      Tokens of A.
  /// \param aTokOff    Byte offsets for A tokens (size = |A| + 1).
  /// \param bSource    Edited preprocessed text B (e.g., `test.c.i.mod`).
  /// \param bToks      Tokens of B.
  /// \param bTokOff    Byte offsets for B tokens (size = |B| + 1).
  /// \param noLines    If true, then do not inject #line.
  /// \param strict     If true, then make stringified args significant.
  /// \returns          The refolded, partially expanded C source.
  static Expected<std::string>
  Refold(const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
         ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
         ArrayRef<size_t> bTokOff, bool noLines, bool strict,
         ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits = {},
         std::vector<MaterializedEditMapping> *materializedEditMappings =
             nullptr,
         FinalLineControlValidationCallback finalLineControlValidationCallback =
             FinalLineControlValidationCallback(),
         std::vector<SourceGraphOutput> *sourceGraphOutputs = nullptr);

  /// Build lexer language options from the producer-captured language name.
  static clang::LangOptions MakeLexLangOptions(llvm::StringRef langName);

private:
  const RefoldModel model_;
  StringRef aSource_, bSource_;
  ArrayRef<PPTok> aToks_, bToks_;
  ArrayRef<size_t> aTokOff_, bTokOff_;
  LineDirectiveInserter lineDirs_;
  bool strict_;
  LangOptions lexLang_;
  std::vector<MaterializedEditMapping> *materializedEditMappings_ = nullptr;
  std::vector<SourceGraphOutput> *sourceGraphOutputs_ = nullptr;
  FinalLineControlValidationCallback finalLineControlValidationCallback_;

  /// Final-output byte ranges for synthetic `#line` directives that the local
  /// emitters have explicitly made eligible for the final fixed-point pruner.
  /// The vector is cleared at the start of each top-level refold attempt and is
  /// passed to the final pruner only after structural emission has selected the
  /// actual output stream.
  mutable std::vector<FinalLineControlPruneCandidate>
      finalLineControlPruneCandidates_;

  /// Final-output byte ranges that were copied byte-for-byte from a physical
  /// source owner.  The final line-control scanner uses these mappings to bind
  /// final `#line` directive bytes back to producer-recorded LineControlEvent
  /// records without guessing through materialized B replay or synthetic text.
  mutable std::vector<FinalLineControlSourceMapping>
      finalLineControlSourceMappings_;

  /// Source edits for sideband pragma directive lines that were removed from
  /// the lexed A/B token streams before diffing. These are applied as ordinary
  /// TU byte edits only when their source path is the emitted TU; non-TU
  /// occurrences are rejected by the structural pass because this single-file
  /// output cannot directly edit an arbitrary header.
  std::vector<SidebandPragmaEdit> sidebandPragmaEdits_;

  /// \brief Phase-0A vocabulary for behaviorally legacy emission paths.
  ///
  /// A path is legacy only when emitted behavior is justified by one of these
  /// implementation-local mechanisms instead of by the proof lattice.  This is
  /// intentionally a definition layer, not an audit: no caller is rejected merely
  /// by naming a kind here.  Phase 0B audit sites should use this shared
  /// vocabulary so every diagnostic answers the same question: which old
  /// behavior must be represented by which proof-lattice invariant before it is
  /// allowed to survive to selection or emission?
#define REFOLD_LEGACY_PATH_KIND_LIST(REFOLD_X)                                \
  REFOLD_X(Unknown)                                                           \
  REFOLD_X(PathSpecificProofMirror)                                           \
  REFOLD_X(PostSummaryProofKindDecision)                                      \
  REFOLD_X(StructurePreservingProofBit)                                       \
  REFOLD_X(UnclassifiedFallbackBranch)                                       \
  REFOLD_X(StateCheckOutsideGateway)                                          \
  REFOLD_X(FinalLineControlLivenessWithoutObligation)

  enum class LegacyPathKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_LEGACY_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(LegacyPathKind value) {
    switch (value) {
#define REFOLD_X(name) case LegacyPathKind::name: return #name;
      REFOLD_LEGACY_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }

  /// \brief Canonical Phase-0A definition for one legacy-path category.
  ///
  /// `definition` states the forbidden legacy dependency.  `requiredClosure`
  /// names the proof-lattice representation that must replace that dependency
  /// before the path can be theorem-facing.  Keeping the prose centralized keeps
  /// audit output, comments, and future CI checks synchronized instead of
  /// allowing six slightly different definitions of "legacy path" to drift.
  struct LegacyPathDefinition {
    LegacyPathKind kind = LegacyPathKind::Unknown;
    StringRef definition;
    StringRef requiredClosure;
  };

  static LegacyPathDefinition DescribeLegacyPathKind(LegacyPathKind kind) {
    switch (kind) {
    case LegacyPathKind::Unknown:
      return {kind, "unclassified legacy dependency",
              "classify the dependency before it can be audited"};
    case LegacyPathKind::PathSpecificProofMirror:
      return {kind,
              "path-specific proof booleans or witness flags mirror "
              "ProofSummary",
              "move the fact into ProofSummary / the canonical emitted proof "
              "carrier"};
    case LegacyPathKind::PostSummaryProofKindDecision:
      return {kind,
              "proofKind or accepted-path provenance decides behavior after "
              "ProofSummary / TheoremProofClass should dominate",
              "normalize to exactly one TheoremProofClass before selection or "
              "emission"};
    case LegacyPathKind::StructurePreservingProofBit:
      return {kind,
              "structurePreserving is treated as proof authority rather than "
              "construction metadata",
              "represent preservation with an explicit theorem proof class, "
              "typed witness, and lattice preference"};
    case LegacyPathKind::UnclassifiedFallbackBranch:
      return {kind,
              "fallback behavior is not represented by a classified "
              "TerminalFallbackProofFailure",
              "build a TerminalFallbackProofFailure / TerminalFallbackWitness "
              "or convert the path to an in-domain lattice proof"};
    case LegacyPathKind::StateCheckOutsideGateway:
      return {kind,
              "preprocessor-state acceptance is checked outside "
              "OwnerStateDelta, OwnerStateGraph, or the state-transition "
              "gateway",
              "route the state fact through OwnerStateDelta / OwnerStateGraph "
              "/ gateway witnesses"};
    case LegacyPathKind::FinalLineControlLivenessWithoutObligation:
      return {kind,
              "final #line liveness is decided without a compact line-control "
              "obligation or removal proof",
              "represent liveness as a typed final-line-control obligation / "
              "proof before replacing the operational scanner"};
    }
    return {LegacyPathKind::Unknown, "unclassified legacy dependency",
            "classify the dependency before it can be audited"};
  }

  static bool IsDefinedLegacyPathKind(LegacyPathKind kind) {
    return kind != LegacyPathKind::Unknown;
  }

  /// \brief One semantic no-legacy audit finding.
  ///
  /// The audit names the legacy category, the engine boundary that observed it,
  /// and a deterministic detail string.  During strict/theorem validation it
  /// becomes theorem state and may force the same explicit terminal fallback as
  /// other audit violations.
  struct LegacyAuditEvidence {
    LegacyPathKind kind = LegacyPathKind::Unknown;
    std::string role;
    std::string detail;
  };

  /// Return whether semantic no-legacy auditing is active for this run.
  ///
  /// Step 6 removes the temporary environment-variable controls.  The audit is
  /// now tied directly to strict/theorem validation: when this returns true,
  /// findings are theorem state and may force the same fail-closed terminal
  /// result as other undischarged emission-boundary obligations.
  bool IsNoLegacyAuditEnabled() const;

  /// Build and emit a deterministic no-legacy audit finding.
  static LegacyAuditEvidence
  MakeLegacyAuditEvidence(LegacyPathKind kind, llvm::StringRef role,
                          llvm::StringRef detail = llvm::StringRef());

  /// Emit one no-legacy finding and attach it to the theorem audit.
  ///
  /// Step 6 makes no-legacy cleanliness part of the strict/theorem invariant:
  /// a strict engine run may not merely print findings and still report a
  /// satisfied theorem audit.  Keeping this as the single reporting entry point
  /// avoids duplicated counter/violation policy at individual audit sites.
  void ReportNoLegacyAuditFinding(const LegacyAuditEvidence &evidence) const;

#undef REFOLD_LEGACY_PATH_KIND_LIST

  /// \brief Named proof obligation that forced the terminal raw-B result.
  ///
  /// Terminal fallback is part of the proof system, not a convenience escape.
  /// Every request must name the theorem obligation that failed.  Keep these
  /// obligations close to the closed-domain contract: owner closure,
  /// deterministic tiling, state stability, producer-fact availability, and
  /// final validation are the only acceptable reasons to emit raw B.
  enum class TerminalFallbackObligationKind : uint8_t {
    Unknown,
    OwnerClosedCover,
    DeterministicMixedOwnerTiling,
    StateTransitionClosure,
    PragmaBoundaryKnown,
    LineControlStateProducerProven,
    CounterStateStabilizable,
    MacroStateStabilizable,
    IncludeGuardStateStabilizable,
    ConditionalStateStabilizable,
    ReverseSolvedDirectiveForbidden,
    InvocationPreservationWellFormed,
    ProducerFactsAvailable,
    FinalValidationSucceeded,
    IncludeRealizationBEnvelopeMapped,
    EmissionArtifactDischarged,
    EmissionEditSetComposable,
    TheoremAuditInvariantSatisfied,
  };

  friend inline StringRef toString(TerminalFallbackObligationKind obligation) {
    switch (obligation) {
    case TerminalFallbackObligationKind::Unknown:
      return "Unknown";
    case TerminalFallbackObligationKind::OwnerClosedCover:
      return "OwnerClosedCover";
    case TerminalFallbackObligationKind::DeterministicMixedOwnerTiling:
      return "DeterministicMixedOwnerTiling";
    case TerminalFallbackObligationKind::StateTransitionClosure:
      return "StateTransitionClosure";
    case TerminalFallbackObligationKind::PragmaBoundaryKnown:
      return "PragmaBoundaryKnown";
    case TerminalFallbackObligationKind::LineControlStateProducerProven:
      return "LineControlStateProducerProven";
    case TerminalFallbackObligationKind::CounterStateStabilizable:
      return "CounterStateStabilizable";
    case TerminalFallbackObligationKind::MacroStateStabilizable:
      return "MacroStateStabilizable";
    case TerminalFallbackObligationKind::IncludeGuardStateStabilizable:
      return "IncludeGuardStateStabilizable";
    case TerminalFallbackObligationKind::ConditionalStateStabilizable:
      return "ConditionalStateStabilizable";
    case TerminalFallbackObligationKind::ReverseSolvedDirectiveForbidden:
      return "ReverseSolvedDirectiveForbidden";
    case TerminalFallbackObligationKind::InvocationPreservationWellFormed:
      return "InvocationPreservationWellFormed";
    case TerminalFallbackObligationKind::ProducerFactsAvailable:
      return "ProducerFactsAvailable";
    case TerminalFallbackObligationKind::FinalValidationSucceeded:
      return "FinalValidationSucceeded";
    case TerminalFallbackObligationKind::IncludeRealizationBEnvelopeMapped:
      return "IncludeRealizationBEnvelopeMapped";
    case TerminalFallbackObligationKind::EmissionArtifactDischarged:
      return "EmissionArtifactDischarged";
    case TerminalFallbackObligationKind::EmissionEditSetComposable:
      return "EmissionEditSetComposable";
    case TerminalFallbackObligationKind::TheoremAuditInvariantSatisfied:
      return "TheoremAuditInvariantSatisfied";
    }
    return "Unknown";
  }

  /// \brief Concrete reason the terminal-fallback obligation failed.
  ///
  /// This implementation-local reason preserves precise diagnostics before
  /// normalization to the final Phase-2A theorem vocabulary below.  The reason
  /// names the local domain wall; the obligation names the theorem
  /// condition.  Keeping both makes terminal fallback auditable without having
  /// to reverse-engineer the implementation phase that requested it.
  enum class TerminalFallbackFailureReason : uint8_t {
    Unknown,
    NoOwnerClosedCover,
    NoDeterministicMixedOwnerTiling,
    AmbiguousMixedOwnerTiling,
    StateTransitionConsumedAndObserved,
    UnknownPragmaCrossesBoundary,
    LineControlStateNotProducerProven,
    CounterStateNotStabilizable,
    MacroStateNotStabilizable,
    IncludeGuardStateNotStabilizable,
    ConditionalStateNotStabilizable,
    ReverseSolvedDirectiveRequired,
    MalformedInvocationPreservation,
    MissingProducerFacts,
    NoCanonicalSuffixOrder,
    ValidationFailure,
    NoTUAnchorForUnresolvedOwner,
    UnmappableIncludeBEnvelope,
    UndischargedEmissionArtifact,
    UncomposableEmissionEditSet,
    TheoremAuditInvariantViolation,
    UnclassifiedTerminalFallback,
  };

  friend inline StringRef toString(TerminalFallbackFailureReason reason) {
    switch (reason) {
    case TerminalFallbackFailureReason::Unknown:
      return "Unknown";
    case TerminalFallbackFailureReason::NoOwnerClosedCover:
      return "NoOwnerClosedCover";
    case TerminalFallbackFailureReason::NoDeterministicMixedOwnerTiling:
      return "NoDeterministicMixedOwnerTiling";
    case TerminalFallbackFailureReason::AmbiguousMixedOwnerTiling:
      return "AmbiguousMixedOwnerTiling";
    case TerminalFallbackFailureReason::StateTransitionConsumedAndObserved:
      return "StateTransitionConsumedAndObserved";
    case TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary:
      return "UnknownPragmaCrossesBoundary";
    case TerminalFallbackFailureReason::LineControlStateNotProducerProven:
      return "LineControlStateNotProducerProven";
    case TerminalFallbackFailureReason::CounterStateNotStabilizable:
      return "CounterStateNotStabilizable";
    case TerminalFallbackFailureReason::MacroStateNotStabilizable:
      return "MacroStateNotStabilizable";
    case TerminalFallbackFailureReason::IncludeGuardStateNotStabilizable:
      return "IncludeGuardStateNotStabilizable";
    case TerminalFallbackFailureReason::ConditionalStateNotStabilizable:
      return "ConditionalStateNotStabilizable";
    case TerminalFallbackFailureReason::ReverseSolvedDirectiveRequired:
      return "ReverseSolvedDirectiveRequired";
    case TerminalFallbackFailureReason::MalformedInvocationPreservation:
      return "MalformedInvocationPreservation";
    case TerminalFallbackFailureReason::MissingProducerFacts:
      return "MissingProducerFacts";
    case TerminalFallbackFailureReason::NoCanonicalSuffixOrder:
      return "NoCanonicalSuffixOrder";
    case TerminalFallbackFailureReason::ValidationFailure:
      return "ValidationFailure";
    case TerminalFallbackFailureReason::NoTUAnchorForUnresolvedOwner:
      return "NoTUAnchorForUnresolvedOwner";
    case TerminalFallbackFailureReason::UnmappableIncludeBEnvelope:
      return "UnmappableIncludeBEnvelope";
    case TerminalFallbackFailureReason::UndischargedEmissionArtifact:
      return "UndischargedEmissionArtifact";
    case TerminalFallbackFailureReason::UncomposableEmissionEditSet:
      return "UncomposableEmissionEditSet";
    case TerminalFallbackFailureReason::TheoremAuditInvariantViolation:
      return "TheoremAuditInvariantViolation";
    case TerminalFallbackFailureReason::UnclassifiedTerminalFallback:
      return "UnclassifiedTerminalFallback";
    }
    return "Unknown";
  }

  /// \brief Final theorem-facing terminal failure vocabulary.
  ///
  /// Older/local failure reasons may remain for diagnostics, but every live
  /// terminal fallback must normalize to one of these Phase-2A failure kinds.
  /// This is the canonical answer to: "which strict-domain obligation failed
  /// strongly enough to justify the terminal raw-B carrier?"
  enum class TheoremFallbackFailureKind : uint8_t {
    Unknown,
    NoOwnerClosedCover,
    NoDeterministicMixedOwnerTiling,
    AmbiguousMixedOwnerTiling,
    StateTransitionConsumedAndObserved,
    UnknownPragmaCrossesBoundary,
    LineControlStateNotProducerProven,
    CounterStateNotStabilizable,
    MacroStateNotStabilizable,
    IncludeGuardStateNotStabilizable,
    ConditionalStateNotStabilizable,
    ReverseSolvedDirectiveRequired,
    MalformedInvocationPreservation,
    MissingProducerFacts,
    ValidationFailure,
  };

  friend inline StringRef toString(TheoremFallbackFailureKind kind) {
    switch (kind) {
    case TheoremFallbackFailureKind::Unknown:
      return "Unknown";
    case TheoremFallbackFailureKind::NoOwnerClosedCover:
      return "NoOwnerClosedCover";
    case TheoremFallbackFailureKind::NoDeterministicMixedOwnerTiling:
      return "NoDeterministicMixedOwnerTiling";
    case TheoremFallbackFailureKind::AmbiguousMixedOwnerTiling:
      return "AmbiguousMixedOwnerTiling";
    case TheoremFallbackFailureKind::StateTransitionConsumedAndObserved:
      return "StateTransitionConsumedAndObserved";
    case TheoremFallbackFailureKind::UnknownPragmaCrossesBoundary:
      return "UnknownPragmaCrossesBoundary";
    case TheoremFallbackFailureKind::LineControlStateNotProducerProven:
      return "LineControlStateNotProducerProven";
    case TheoremFallbackFailureKind::CounterStateNotStabilizable:
      return "CounterStateNotStabilizable";
    case TheoremFallbackFailureKind::MacroStateNotStabilizable:
      return "MacroStateNotStabilizable";
    case TheoremFallbackFailureKind::IncludeGuardStateNotStabilizable:
      return "IncludeGuardStateNotStabilizable";
    case TheoremFallbackFailureKind::ConditionalStateNotStabilizable:
      return "ConditionalStateNotStabilizable";
    case TheoremFallbackFailureKind::ReverseSolvedDirectiveRequired:
      return "ReverseSolvedDirectiveRequired";
    case TheoremFallbackFailureKind::MalformedInvocationPreservation:
      return "MalformedInvocationPreservation";
    case TheoremFallbackFailureKind::MissingProducerFacts:
      return "MissingProducerFacts";
    case TheoremFallbackFailureKind::ValidationFailure:
      return "ValidationFailure";
    }
    return "Unknown";
  }

  /// Normalize an implementation-local terminal reason to the frozen theorem
  /// failure vocabulary.  This is the Phase-2A gate that lets legacy/local
  /// diagnostics coexist with a small final theorem language.
  static std::optional<TheoremFallbackFailureKind>
  NormalizeTerminalFallbackFailureReason(
      TerminalFallbackFailureReason reason) {
    switch (reason) {
    case TerminalFallbackFailureReason::NoOwnerClosedCover:
    case TerminalFallbackFailureReason::NoTUAnchorForUnresolvedOwner:
      return TheoremFallbackFailureKind::NoOwnerClosedCover;
    case TerminalFallbackFailureReason::NoDeterministicMixedOwnerTiling:
      return TheoremFallbackFailureKind::NoDeterministicMixedOwnerTiling;
    case TerminalFallbackFailureReason::AmbiguousMixedOwnerTiling:
      return TheoremFallbackFailureKind::AmbiguousMixedOwnerTiling;
    case TerminalFallbackFailureReason::StateTransitionConsumedAndObserved:
      return TheoremFallbackFailureKind::StateTransitionConsumedAndObserved;
    case TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary:
      return TheoremFallbackFailureKind::UnknownPragmaCrossesBoundary;
    case TerminalFallbackFailureReason::LineControlStateNotProducerProven:
      return TheoremFallbackFailureKind::LineControlStateNotProducerProven;
    case TerminalFallbackFailureReason::CounterStateNotStabilizable:
      return TheoremFallbackFailureKind::CounterStateNotStabilizable;
    case TerminalFallbackFailureReason::MacroStateNotStabilizable:
      return TheoremFallbackFailureKind::MacroStateNotStabilizable;
    case TerminalFallbackFailureReason::IncludeGuardStateNotStabilizable:
      return TheoremFallbackFailureKind::IncludeGuardStateNotStabilizable;
    case TerminalFallbackFailureReason::ConditionalStateNotStabilizable:
      return TheoremFallbackFailureKind::ConditionalStateNotStabilizable;
    case TerminalFallbackFailureReason::ReverseSolvedDirectiveRequired:
      return TheoremFallbackFailureKind::ReverseSolvedDirectiveRequired;
    case TerminalFallbackFailureReason::MalformedInvocationPreservation:
      return TheoremFallbackFailureKind::MalformedInvocationPreservation;
    case TerminalFallbackFailureReason::MissingProducerFacts:
    case TerminalFallbackFailureReason::NoCanonicalSuffixOrder:
    case TerminalFallbackFailureReason::UnmappableIncludeBEnvelope:
    case TerminalFallbackFailureReason::UndischargedEmissionArtifact:
    case TerminalFallbackFailureReason::TheoremAuditInvariantViolation:
      return TheoremFallbackFailureKind::MissingProducerFacts;
    case TerminalFallbackFailureReason::UncomposableEmissionEditSet:
      return TheoremFallbackFailureKind::NoDeterministicMixedOwnerTiling;
    case TerminalFallbackFailureReason::ValidationFailure:
      return TheoremFallbackFailureKind::ValidationFailure;
    case TerminalFallbackFailureReason::Unknown:
    case TerminalFallbackFailureReason::UnclassifiedTerminalFallback:
      return std::nullopt;
    }
    return std::nullopt;
  }

  /// \brief Structured local facts attached to a terminal proof failure.
  ///
  /// Phase 2C makes terminal fallback context data, not prose.  Most fallback
  /// sites cannot populate every field yet, but the carrier is intentionally
  /// stable: later phases can attach owner, hunk, state-component, source-span,
  /// token-envelope, and first-observer evidence without changing the terminal
  /// witness format again.  Logs are derived from this structure; callers
  /// should not encode theorem facts only inside a free-form detail string.
  struct TerminalFallbackFailureContext {
    std::optional<std::string> owner;
    std::optional<uint64_t> hunk;
    std::optional<std::string> stateComponent;

    std::optional<std::string> sourcePath;
    std::optional<uint64_t> sourceBegin;
    std::optional<uint64_t> sourceEnd;

    std::optional<uint64_t> aTokenBegin;
    std::optional<uint64_t> aTokenEnd;
    std::optional<uint64_t> bTokenBegin;
    std::optional<uint64_t> bTokenEnd;


    static TerminalFallbackFailureContext ForStateComponent(StringRef name) {
      TerminalFallbackFailureContext context;
      context.stateComponent = name.str();
      return context;
    }

    static TerminalFallbackFailureContext ForHunkTokenEnvelope(
        uint64_t hunkIndex, uint64_t aBegin, uint64_t aEnd, uint64_t bBegin,
        uint64_t bEnd) {
      TerminalFallbackFailureContext context;
      context.hunk = hunkIndex;
      context.aTokenBegin = aBegin;
      context.aTokenEnd = aEnd;
      context.bTokenBegin = bBegin;
      context.bTokenEnd = bEnd;
      return context;
    }

    bool Empty() const {
      return !owner && !hunk && !stateComponent && !sourcePath &&
             !sourceBegin && !sourceEnd && !aTokenBegin && !aTokenEnd &&
             !bTokenBegin && !bTokenEnd;
    }

  };

  /// \brief Normalized proof failure attached to terminal fallback.
  struct TerminalFallbackProofFailure {
    TerminalFallbackObligationKind obligation =
        TerminalFallbackObligationKind::Unknown;
    TerminalFallbackFailureReason reason = TerminalFallbackFailureReason::Unknown;
    TheoremFallbackFailureKind theoremFailure =
        TheoremFallbackFailureKind::Unknown;
    TerminalFallbackFailureContext context;
  };

  friend inline std::string
  toString(const TerminalFallbackProofFailure &failure) {
    return llvm::formatv("failedObligation={0} theoremFailure={1} "
                         "failureReason={2}",
                         toString(failure.obligation),
                         toString(failure.theoremFailure),
                         toString(failure.reason))
        .str();
  }

  /// \brief Return whether a fallback proof failure is theorem-facing.
  ///
  /// Phase 2 forbids terminal fallback whose proof still says `Unknown` or
  /// `Unclassified`.  The guard is deliberately small and shared by request
  /// recording, terminal-witness construction, and theorem-audit reporting so a
  /// new fallback path cannot quietly reintroduce an opaque raw-B escape.
  static bool
  IsClassifiedTerminalFallbackProofFailure(
      const TerminalFallbackProofFailure &failure) {
    return failure.obligation != TerminalFallbackObligationKind::Unknown &&
           failure.theoremFailure != TheoremFallbackFailureKind::Unknown &&
           failure.reason != TerminalFallbackFailureReason::Unknown &&
           failure.reason !=
               TerminalFallbackFailureReason::UnclassifiedTerminalFallback;
  }

  /// \brief Build a classified terminal fallback proof failure.
  ///
  /// Use this helper at every terminal fallback request site.  It documents the
  /// local proof obligation being rejected and prevents call sites from relying
  /// on terminal-fallback kind alone as an implicit classification.
  static TerminalFallbackProofFailure
  MakeTerminalFallbackProofFailure(TerminalFallbackObligationKind obligation,
                                   TerminalFallbackFailureReason reason) {
    TerminalFallbackProofFailure failure;
    failure.obligation = obligation;
    failure.reason = reason;
    failure.theoremFailure =
        NormalizeTerminalFallbackFailureReason(reason).value_or(
            TheoremFallbackFailureKind::Unknown);
    return failure;
  }

  /// Build a classified terminal fallback proof failure with structured local
  /// context.  This overload keeps Phase 2C context population centralized so
  /// call sites do not have to duplicate the obligation/reason initialization
  /// sequence before attaching owner, hunk, state, or span facts.
  static TerminalFallbackProofFailure MakeTerminalFallbackProofFailure(
      TerminalFallbackObligationKind obligation,
      TerminalFallbackFailureReason reason,
      TerminalFallbackFailureContext context) {
    TerminalFallbackProofFailure failure =
        MakeTerminalFallbackProofFailure(obligation, reason);
    failure.context = std::move(context);
    return failure;
  }

  /// \brief Compact witness describing why terminal raw-B emission was selected.
  ///
  /// This is the theorem-facing carrier for the terminal raw-B exit.  Phase 4D
  /// deliberately keeps only the ordered structured failures.  There is no
  /// parallel "primary" scalar, request counter, or branch-local boolean: the
  /// first element is the primary failed obligation and the remaining elements
  /// are secondary obligations discovered before terminal emission.
  struct TerminalFallbackWitness {
    std::vector<TerminalFallbackProofFailure> proofFailures;

    const TerminalFallbackProofFailure *PrimaryFailure() const {
      return proofFailures.empty() ? nullptr : &proofFailures.front();
    }
  };

  friend inline std::string toString(const TerminalFallbackWitness &witness) {
    const TerminalFallbackProofFailure *primary = witness.PrimaryFailure();
    return llvm::formatv("{0} failureCount={1} secondaryFailureCount={2}",
                         primary ? toString(*primary)
                                 : StringRef("<missing-terminal-failure>"),
                         witness.proofFailures.size(),
                         witness.proofFailures.size() > 1
                             ? witness.proofFailures.size() - 1
                             : 0)
        .str();
  }

  /// \brief Typed construction request for the terminal raw-B carrier.
  ///
  /// Phase 4C makes terminal fallback construction data-first.  The proof
  /// failure below is the only semantic reason that may justify selecting the
  /// terminal out-of-domain result; `phase` and `detail` are diagnostic labels
  /// used to keep traces actionable.  Keeping them in the same carrier prevents
  /// call sites from smuggling behavior through a free-form reason string while
  /// preserving the useful human explanation in debug output.
  struct TerminalFallbackRequest {
    TerminalFallbackProofFailure failure;
    std::string phase;
    std::string detail;
  };

  static TerminalFallbackRequest
  MakeTerminalFallbackRequest(TerminalFallbackProofFailure failure,
                              llvm::StringRef phase,
                              llvm::StringRef detail) {
    TerminalFallbackRequest request;
    request.failure = std::move(failure);
    request.phase = phase.str();
    request.detail = detail.str();
    return request;
  }

  friend inline std::string toString(const TerminalFallbackRequest &request) {
    return llvm::formatv("{0} terminalAction=raw-b-emission phase={1}: {2}",
                         toString(request.failure), request.phase,
                         request.detail)
        .str();
  }

  struct RefoldStats {
    uint64_t totalIncludes = 0;
    uint64_t expandedIncludes = 0;
    uint64_t totalMacros = 0;
    uint64_t expandedMacros = 0;
  };

  /// \brief Declared structural-refolding domain used by the theorem audit.
  ///
  /// The theorem/completeness target is explicit so later work does not
  /// silently move the goalposts. The engine treats an input as inside the
  /// declared structural domain iff every emitted non-terminal artifact can be
  /// represented by a normalized accepted-result carrier such that:
  ///   - the carrier belongs to a declared proof class,
  ///   - the carrier is explicit-proof-backed,
  ///   - the carrier is inside the declared domain,
  ///   - the carrier is locally discharged,
  ///   - final competition is resolved only by the shared lattice, and
  ///   - no unresolved owner / unresolved B-envelope exclusion remains except
  ///     those explicitly tracked as named out-of-domain terminal results.
  ///
  /// Internal staging objects may temporarily lack one of these properties, but
  /// they are not theorem-facing and therefore do not count toward the domain
  /// statement until they are restamped onto a concrete emitted carrier.
  struct TheoremAuditStats {
    uint64_t emittedNonTerminalEdits = 0;
    uint64_t emittedCarriers = 0;
    uint64_t emittedDeclaredClassCarriers = 0;
    uint64_t emittedDischargedCarriers = 0;
    uint64_t emittedSelectorOnlyExceptionCarriers = 0;
    uint64_t emittedTransitionalTheoremCarriers = 0;
    uint64_t emittedUndischargedCarriers = 0;
    uint64_t emittedUnknownClassCarriers = 0;
    uint64_t emittedOutOfDomainCarriers = 0;

    // Phase 1D: composite edits must be proof-composed, not just bags of
    // individually valid carriers.  These counters distinguish the accepted
    // composition laws currently enforced at the emission boundary.
    uint64_t emittedCompositeEdits = 0;
    uint64_t emittedEquivalentCompositeEdits = 0;
    uint64_t emittedOrderedCompositeEdits = 0;
    uint64_t emittedUncomposedCompositeEdits = 0;

    uint64_t selectorCompetitions = 0;
    uint64_t selectorResolutions = 0;
    uint64_t selectorNoSelectable = 0;
    uint64_t selectorUnresolvedCompetitions = 0;
    uint64_t selectorDirectBypasses = 0;

    // Phase 3D/Step 4: no-legacy audit findings are theorem-audit data, not
    // ad hoc stderr-only diagnostics.  The aggregate count covers every
    // emitted finding; the boundary/rejection counters keep the fail-closed
    // emission-boundary subset distinct from ordinary proof-discharge failures.
    uint64_t noLegacyAuditFindings = 0;
    uint64_t noLegacyEmissionBoundaryViolations = 0;
    uint64_t noLegacyStrictRejections = 0;

    uint64_t explicitTerminalExclusions = 0;
    uint64_t nonExplicitTerminalExclusions = 0;

    // Phase 2D/2E: terminal fallback must retain all failed obligations and
    // audit them before raw-B emission.
    uint64_t terminalFailureObligations = 0;
    uint64_t terminalSecondaryFailureObligations = 0;
    uint64_t terminalFailureAuditViolations = 0;

    // Phase 4G: persistent owner/state graph census.  These counters are
    // diagnostic/audit data only; they make it visible whether zero-token state
    // events and missing producer facts were modeled in the graph.
    uint64_t graphOwnerNodes = 0;
    uint64_t graphZeroTokenStateNodes = 0;
    uint64_t graphObservedStateComponents = 0;
    uint64_t graphMutatedStateComponents = 0;
    uint64_t graphIncomparableNodes = 0;
    uint64_t graphMissingProducerFacts = 0;

    // Phase 5L: state-changing edits must be audited through the
    // StateTransitionGateway before any non-terminal bytes are emitted.  These
    // counters are updated by the gateway itself, so the final emission audit
    // can reject Unknown/None/undischarged state transitions without trying to
    // rediscover state effects from emitted text.
    uint64_t stateTransitionGatewayChecks = 0;
    uint64_t stateTransitionGatewayStable = 0;
    uint64_t stateTransitionGatewayTerminalFailures = 0;
    uint64_t stateTransitionAuditViolations = 0;
    uint64_t stateTransitionUnknownComponentViolations = 0;
    uint64_t stateTransitionUnknownMutationViolations = 0;
    uint64_t stateTransitionNoneWitnessViolations = 0;

    // Phase 5A: direct state-sensitive handling inventory.  Strict/theorem
    // validation populates these counters to make the remaining component-local
    // state checks visible before later phases delete or route them through the
    // gateway.
    uint64_t directStateChecksAudited = 0;
    uint64_t directStateChecksDeltaFacts = 0;
    uint64_t directStateChecksGraphEdges = 0;
    uint64_t directStateChecksGatewayWitnesses = 0;
    uint64_t directStateChecksTerminalFailures = 0;
    uint64_t directStateChecksUnclosedLocal = 0;

    bool theoremSatisfied = true;
    std::string firstViolation;
  };

  RefoldStats lastStats_;
  mutable TheoremAuditStats lastTheoremAudit_;

  /// \brief Cache of weakly-canonicalized filesystem paths used by hot-path
  /// ownership and mapping queries.
  ///
  /// Path comparison remains semantically identical to the historical
  /// implementation: non-empty paths are still compared via
  /// `std::filesystem::weakly_canonical()`, and canonicalization failures are
  /// still fatal. The only change is that each distinct raw path spelling is
  /// canonicalized at most once per refold engine instance.
  mutable llvm::StringMap<std::string> canonicalPathCache_;

  /// Half-open physical source extent of a #define directive, including any
  /// line-spliced continuation lines.
  struct DefineDirectiveExtent {
    uint64_t begin = 0;
    uint64_t end = 0;
  };

  /// Per-engine source-text cache used while building the #define containment
  /// index. This must not be process-global because the directive list and
  /// line-directive path resolver are properties of the current refold model.
  mutable llvm::StringMap<std::string> defineFileTextCache_;

  /// Per-engine cache of widened #define directive end offsets keyed by
  /// producer directive id.
  mutable llvm::DenseMap<uint64_t, uint64_t> defineEndCache_;

  /// Per-engine index of #define directive extents keyed by absolute source
  /// path.
  mutable llvm::StringMap<std::vector<DefineDirectiveExtent>> definesByAbsPath_;

  /// True once definesByAbsPath_ has been built for this engine instance.
  mutable bool definesIndexBuilt_ = false;

  // Single-pass terminal raw-B scaffold: if any edit/patch cannot be
  // discharged into the declared proof/lattice outcomes in the current pass,
  // record the typed failed obligation and later emit the fully expanded edited
  // preprocessed stream (B).  Phase 4D intentionally has no separate
  // "we are in fallback" flag and no duplicate proof-failure mirrors: the
  // ordered typed requests are the only terminal-state carrier.
  //
  // Note: terminal requests are recorded from several helper routines that are
  // logically "const" (for example include materialization). Treat this as
  // diagnostic/proof side-channel state via `mutable`.
  mutable std::vector<TerminalFallbackRequest> terminalFallbackRequests_;

  /// Return true once the current pass has at least one typed terminal request.
  bool HasTerminalFallbackRequest() const {
    return !terminalFallbackRequests_.empty();
  }

  /// \brief Record that the current pass must fall back to the explicit
  /// terminal edited-preprocessed-stream result.
  ///
  /// Helpers call this when they cannot discharge an edit into one of the
  /// single-pass proof/lattice outcomes.
  void RequestTerminalFallback(TerminalFallbackRequest request) const;
  void RequestTerminalFallback(TerminalFallbackProofFailure failure,
                               llvm::StringRef phase,
                               llvm::StringRef detail) const;

  /// \brief Clear per-pass terminal-fallback state.
  ///
  /// This is invoked once before running the single structural pass.
  void ResetTerminalFallbackState() const {
    terminalFallbackRequests_.clear();
  }

  /// Reset the per-attempt refold statistics to a clean baseline.
  ///
  /// This clears the counters accumulated for the current refolding attempt and
  /// reinitializes the invariant totals from the loaded refold model. The
  /// include total is the size of the recorded include tree. The macro total is
  /// the number of top-level macro invocation roots recorded during
  /// preprocessing, excluding nested expansion nodes that are attributable to
  /// an outer caller via \c callerMacroId.
  void ResetAttemptStats() {
    lastStats_ = RefoldStats{};
    lastStats_.totalIncludes = model_.GetIncludes().size();
    for (const auto &mi : model_.GetMacroInvocations()) {
      if (!mi.callerMacroId)
        ++lastStats_.totalMacros;
    }
  }

  /// Reset the per-run theorem audit counters.
  void ResetTheoremAudit() const { lastTheoremAudit_ = TheoremAuditStats{}; }

  /// Record the first theorem-audit violation encountered in the current run.
  void NoteTheoremAuditViolation(llvm::StringRef detail) const {
    if (lastTheoremAudit_.theoremSatisfied)
      lastTheoremAudit_.firstViolation = detail.str();
    lastTheoremAudit_.theoremSatisfied = false;
  }

  /// The theorem audit is an invariant checker rather than only a descriptive
  /// dashboard. This helper normalizes any surviving counter-based
  /// violations onto the theorem state and, in strict mode, requests the one
  /// explicit terminal fallback instead of allowing a structurally-refolded
  /// result to escape with a violated theorem audit.
  void EnforceTheoremAuditInvariants() const;

  /// Validate that one terminal fallback proof failure is a named, structured
  /// failed obligation rather than an opaque raw-B escape.
  ///
  /// Phase 2E uses this immediately before raw-B emission.  A generic
  /// MissingProducerFacts reason must identify the missing fact through the
  /// structured context carrier; implementation-specific reasons such as an
  /// unmappable include envelope are already self-identifying.
  bool AuditTerminalFallbackProofFailure(
      const TerminalFallbackProofFailure &failure, llvm::StringRef role) const;

  struct MacroPatch;

  /// Report a no-legacy emission-boundary finding and, when strict theorem
  /// validation is active, convert it into an explicit terminal proof failure.
  ///
  /// This helper is intentionally centralized so emission sites do not each
  /// invent their own policy for theorem-boundary audit findings.  Non-strict
  /// diagnostic audit remains report-only; strict engine mode and transitional
  /// strict audit mode turn the finding into a fail-closed result.
  bool RejectNoLegacyAuditFindingIfStrict(
      const LegacyAuditEvidence &evidence,
      const TerminalFallbackProofFailure &failure) const;

  /// Build the canonical terminal failure used when a MacroPatch reaches an
  /// emission boundary without the selected accepted-result carrier that Step 1
  /// requires every emitted MacroPatch to carry.
  TerminalFallbackProofFailure
  MakeMissingSelectedMacroPatchCarrierFailure() const;

  /// Enforce the Step 3 emission-boundary invariant for a MacroPatch whose
  /// selected carrier is missing.  Strict engine runs fail closed immediately
  /// without manufacturing replacement proof authority from the raw MacroPatch.
  bool RejectMissingSelectedMacroPatchCarrier(
      const MacroPatch &patch, llvm::StringRef role,
      llvm::StringRef detail) const;

  /// Validate the Phase-5 state-transition gateway audit before bytes are
  /// emitted.  The gateway is the only place where a state-changing edit may
  /// cross into a preserved suffix; this audit ensures no routed transition was
  /// accepted with Unknown/None state proof data.
  bool AuditStateTransitionGatewayProofs(llvm::StringRef emissionPhase,
                                         llvm::StringRef emissionOwner) const;

  /// Validate that the terminal fallback state itself still classifies as the
  /// one explicit out-of-domain theorem carrier.
  ///
  /// Requesting fallback to B is not enough; the resulting witness must also
  /// restamp onto the
  /// normalized terminal explicit-out-of-domain carrier rather than escaping as
  /// an unclassified fallback side effect.
  void RecordTerminalFallbackTheoremAudit() const;

  /// Build a compact diagnostic string for a strict-mode theorem-audit
  /// fallback.
  std::string BuildTheoremAuditInvariantDetail() const;

  /// \brief Resolve the full post-structural fallback result for the current
  /// run.
  ///
  /// Phase 8 removes the legacy post-terminal macro-expansion proof-bypass
  /// path.  This resolver therefore performs no secondary owner search:
  /// in-domain macro whole-cover edits must have been discharged by
  /// OwnerRealizationProof or MixedOwnerTilingProof before fallback was
  /// requested.  Once structural emission fails closed, the only remaining
  /// result is the declared raw-B TerminalOutOfDomain carrier with its named
  /// proof obligation.
  std::string ResolvePostStructuralFallback();

  /// \brief Try to realize one unresolved edit as a declared TU include-closure.
  ///
  /// Phase 8c classifies this path as still uniquely needed, but no longer as a
  /// legacy fallback branch. The helper emits the explicit
  /// `TUIncludeClosureEdit` carrier when one PP hunk cannot be anchored as an
  /// ordinary macro/include/TU edit but can be proven as one closed replacement
  /// over top-level TU `#include` directives plus the adjacent TU source bytes
  /// that the same hunk consumes. The helper stays deliberately narrow:
  ///
  /// * only non-insertion hunks are eligible,
  /// * only top-level TU `#include` directives are considered,
  /// * the touched include run must form one contiguous A-cover with no PP
  ///   gaps,
  /// * the hunk must either match that cover exactly, be widenable to it
  ///   without absorbing any other token hunk, or form one mixed TU/include
  ///   closure with directly consumed TU tokens adjacent to the include run,
  /// * the corresponding source bytes in the TU may contain only consumed TU
  ///   token spellings, the include directives themselves, and whitespace
  ///   between those pieces,
  /// * the widened source interval must not overlap an already-staged
  ///   source edit,
  /// * and the replacement text must come from a canonical-or-consensus B
  ///   envelope for the full source closure.
  ///
  /// When those obligations hold, the result is staged as one explicit TU text
  /// edit carrying the `TUIncludeClosureEdit` proof class and later composed
  /// with ordinary TU edits and macro callsite patches. Otherwise the caller
  /// must keep the hunk out-of-domain and retain the existing terminal fallback
  /// behavior.
  std::optional<TextEdit>
  BuildTUIncludeClosureEditForUnresolvedHunk(
      const diffutils::Hunk &h, llvm::StringRef tuPath,
      llvm::StringRef tuBytes,
      llvm::ArrayRef<std::pair<uint64_t, uint64_t>> stagedSourceIntervals)
      const;

  /// Emit a compact theorem-audit summary for the current run.
  ///
  /// The counters summarize the declared-domain statement above rather than an
  /// independent ad hoc checklist: every emitted non-terminal carrier must be
  /// declared, discharged, lattice-selected, and in-domain; any transitional
  /// theorem-facing carrier or unresolved selector competition is a violation;
  /// and named terminal exclusions remain the only acceptable out-of-domain
  /// escape.
  void EmitTheoremAudit() const {
    info("theorem",
         "satisfied={0} emittedEdits={1} carriers={2} declared={3} "
         "discharged={4} "
         "selectorOnlyExceptions={5} transitional={6} undischarged={7} "
         "unknownClass={8} outOfDomain={9} "
         "composite={10} equivalentComposite={11} orderedComposite={12} "
         "uncomposedComposite={13} "
         "selectorCompetitions={14} selectorResolutions={15} "
         "selectorNoSelectable={16} selectorUnresolved={17} "
         "selectorDirectBypasses={18} explicitTerminalExclusions={19} "
         "nonExplicitTerminalExclusions={20} terminalFailures={21} "
         "terminalSecondaryFailures={22} terminalFailureAuditViolations={23} "
         "graphNodes={24} zeroTokenStateNodes={25} graphObservedComponents={26} "
         "graphMutatedComponents={27} graphIncomparableNodes={28} "
         "graphMissingProducerFacts={29} directStateChecks={30} "
         "directStateDeltaFacts={31} directStateGraphEdges={32} "
         "directStateGatewayWitnesses={33} directStateTerminalFailures={34} "
         "directStateUnclosed={35}",
         lastTheoremAudit_.theoremSatisfied ? 1 : 0,
         lastTheoremAudit_.emittedNonTerminalEdits,
         lastTheoremAudit_.emittedCarriers,
         lastTheoremAudit_.emittedDeclaredClassCarriers,
         lastTheoremAudit_.emittedDischargedCarriers,
         lastTheoremAudit_.emittedSelectorOnlyExceptionCarriers,
         lastTheoremAudit_.emittedTransitionalTheoremCarriers,
         lastTheoremAudit_.emittedUndischargedCarriers,
         lastTheoremAudit_.emittedUnknownClassCarriers,
         lastTheoremAudit_.emittedOutOfDomainCarriers,
         lastTheoremAudit_.emittedCompositeEdits,
         lastTheoremAudit_.emittedEquivalentCompositeEdits,
         lastTheoremAudit_.emittedOrderedCompositeEdits,
         lastTheoremAudit_.emittedUncomposedCompositeEdits,
         lastTheoremAudit_.selectorCompetitions,
         lastTheoremAudit_.selectorResolutions,
         lastTheoremAudit_.selectorNoSelectable,
         lastTheoremAudit_.selectorUnresolvedCompetitions,
         lastTheoremAudit_.selectorDirectBypasses,
         lastTheoremAudit_.explicitTerminalExclusions,
         lastTheoremAudit_.nonExplicitTerminalExclusions,
         lastTheoremAudit_.terminalFailureObligations,
         lastTheoremAudit_.terminalSecondaryFailureObligations,
         lastTheoremAudit_.terminalFailureAuditViolations,
         lastTheoremAudit_.graphOwnerNodes,
         lastTheoremAudit_.graphZeroTokenStateNodes,
         lastTheoremAudit_.graphObservedStateComponents,
         lastTheoremAudit_.graphMutatedStateComponents,
         lastTheoremAudit_.graphIncomparableNodes,
         lastTheoremAudit_.graphMissingProducerFacts,
         lastTheoremAudit_.directStateChecksAudited,
         lastTheoremAudit_.directStateChecksDeltaFacts,
         lastTheoremAudit_.directStateChecksGraphEdges,
         lastTheoremAudit_.directStateChecksGatewayWitnesses,
         lastTheoremAudit_.directStateChecksTerminalFailures,
         lastTheoremAudit_.directStateChecksUnclosedLocal);
    if (!lastTheoremAudit_.theoremSatisfied &&
        !lastTheoremAudit_.firstViolation.empty()) {
      info("theorem", "firstViolation={0}",
           stringutils::showWsWithClip(lastTheoremAudit_.firstViolation, 220));
    }
  }

  /// Emit a one-line summary of the final refolding statistics.
  ///
  /// The summary reports how many includes and top-level macro invocations
  /// remained expanded in the chosen refold result, relative to the total
  /// number of includes and root macro invocations recorded in the model. The
  /// line is annotated when refolding terminated by falling back to the fully
  /// expanded B-side text.
  void EmitRefoldStats() const {
    info("stats", "includes-expanded={0}/{1} macros-expanded={2}/{3}{4}",
         lastStats_.expandedIncludes, lastStats_.totalIncludes,
         lastStats_.expandedMacros, lastStats_.totalMacros,
         HasTerminalFallbackRequest() ? " terminal-raw-b=B" : "");
  }

  /// Per-gap ownership depth for insertion before PP token k (k in [0..N]).
  /// Computed once per refold run and reused to bound best-effort snapping.
  std::vector<uint32_t> ownerDepthGap_;

  /// Cached token-level hunks for the current refold invocation.
  ///
  /// These hunks are used to deterministically disambiguate A→B token
  /// envelope mapping at *span boundaries* when there are adjacent
  /// pure-insertion hunks (A-span is empty) that should not be absorbed
  /// into a larger mapped envelope (e.g., macro whole-cover replacement).
  std::vector<diffutils::Hunk> abTokHunks_;

  // Forward declarations for the Phase-7 mixed-owner proof records.
  //
  // The persistent side tables below live with the per-run caches, but the full
  // witness definitions are declared later with the rest of the theorem-facing
  // proof carriers, after OwnerClosure is available.  Forward declarations keep
  // the cache layout near the data it annotates without forcing the proof-record
  // definitions to appear before their owner/source/token-range dependencies.
  struct MixedOwnerTilingWitness;
  struct MixedOwnerTilingSegmentBinding;

  /// Persisted Phase-7 mixed-owner tiling witnesses for the current run.
  ///
  /// The mixed-owner normalizer still lowers a proved tiling to ordinary token
  /// hunks so existing macro/include/TU classifiers can operate unchanged.
  /// These side tables preserve the theorem proof that created those hunks and
  /// map each emitted token segment back to the full ordered tiling path.
  std::vector<MixedOwnerTilingWitness> mixedOwnerTilingWitnesses_;
  std::vector<MixedOwnerTilingSegmentBinding> mixedOwnerTilingSegmentBindings_;

  /// Cached token-level A/B maps for the current refold invocation.
  ///
  /// Final TU emission uses these as proof inputs when it has to decide whether
  /// an otherwise uncomposable cluster of direct TU hunk edits can be replaced
  /// by one closed source/B-token realization. The closure proof requires that
  /// every matched A token physically consumed by the source interval maps into
  /// the candidate B interval, and every matched B token emitted by that
  /// replacement maps back into the same source interval.
  std::vector<int64_t> abTokMapA2B_;
  std::vector<int64_t> abTokMapB2A_;

  std::optional<std::vector<diffutils::Hunk>> abByteHunks_;

  /// \brief Prefix-summed A->B byte-length delta for \c abByteHunks_.
  ///
  /// Entry \c i stores the cumulative `(bLen - aLen)` contributed by the
  /// first \c i byte hunks. This keeps repeated A-byte→B-byte coordinate
  /// projection at `O(log H)` after the initial binary search instead of
  /// re-walking all preceding hunks on every lookup.
  std::vector<int64_t> abByteHunkPrefixDelta_;

  // ---------------------------------------------------------------------------
  // B token provenance / ownership for pure insertions
  // ---------------------------------------------------------------------------
  //
  // Goal: enforce a global invariant that no B-only insertion segment is
  // emitted twice (e.g., once as a standalone boundary insertion patch and
  // again as part of a macro whole-cover replacement slice).
  //
  // We track token-level pure-insertion hunks (A length 0, B length > 0) and
  // allow exactly one emission site to claim each insertion.

  // Emission ownership for a pure B-only insertion segment.
  enum class BInsertionClaim : uint8_t {
    Unclaimed = 0,
    Standalone = 1 // emitted as a TU/include/arm insertion patch
  };

  // Provenance record for one token-level pure insertion hunk:
  // inserted at A-gap aGap, produced by hunkIndex, spanning B tokens [b0,b1).
  struct BInsertionProv {
    uint64_t aGap = 0;      // insertion position in A (pp token gap)
    uint64_t hunkIndex = 0; // index in abTokHunks_
    size_t b0 = 0;          // [b0,b1) in B token space
    size_t b1 = 0;
    BInsertionClaim claim = BInsertionClaim::Unclaimed;
  };

  // Master table of all tracked pure insertion segments.
  std::vector<BInsertionProv> bInsertions_;

  // Reverse map: B token index -> insertion id in bInsertions_, or -1 if this
  // B token is not part of a tracked pure insertion.
  std::vector<int32_t> bTokToInsertionId_;

  // Reverse map: token diff hunk index -> insertion id in bInsertions_, or -1
  // if the hunk is not a pure insertion.
  std::vector<int32_t> hunkToInsertionId_;

  /// \brief Build provenance indices for token-level pure insertion hunks.
  ///
  /// Scans \p hunks for token-level pure insertions (A-span empty, B-span
  /// non-empty) and populates:
  ///   - \c bInsertions_           : insertion records in B token space
  ///   - \c bTokToInsertionId_     : per-B-token map to insertion id (or -1)
  ///   - \c hunkToInsertionId_     : per-hunk map to insertion id (or -1)
  ///
  /// The resulting structures allow later phases to enforce a global “emit each
  /// B-only segment exactly once” invariant (no double-emission).
  void BuildBInsertionProvenance(ArrayRef<diffutils::Hunk> hunks);

  /// \brief Pre-claim insertions that must be emitted as standalone boundary
  /// patches.
  ///
  /// Runs as an early planning pass (before macro patch construction) so that
  /// later B-slicing codepaths (e.g. macro whole-cover replacement) can clip
  /// away B-only segments that are already committed to standalone emission.
  ///
  /// Macro call-sites take priority: if an insertion lies within a patchable
  /// macro cover, it is left unclaimed so the macro patch may absorb it.
  void PreclaimStandaloneInsertions(StringRef tuPath,
                                    ArrayRef<diffutils::Hunk> hunks);

  /// \brief Claim a pure insertion hunk for a specific emission site.
  ///
  /// Claims are used to prevent double-emission. Attempting to claim an already
  /// claimed insertion with a different claim kind is a hard error.
  ///
  /// \param insId Insertion id in \c bInsertions_.
  /// \param c     Claim kind to record.
  /// \param why   Debug string describing the claimant (for diagnostics).
  void ClaimBInsertion(size_t insId, BInsertionClaim c, llvm::StringRef why);

  /// \brief Partition a B-token interval into segments that are safe to emit.
  ///
  /// Returns a small list of sub-ranges of \c [bTokStart,bTokEnd) with any
  /// B-only insertion hunks claimed as \c Standalone removed. The returned
  /// sub-ranges are ordered and non-overlapping.
  llvm::SmallVector<std::pair<size_t, size_t>, 4>
  ClipBTokenRangeAgainstClaims(size_t bTokStart, size_t bTokEnd) const;

  /// \brief Slice \c [bTokStart,bTokEnd) from B while omitting claimed
  /// insertion segments.
  ///
  /// This concatenates the segments returned by \c ClipBTokenRangeAgainstClaims
  /// and returns the resulting byte sequence.
  std::string SliceBSourceClippedAgainstClaims(size_t bTokStart,
                                               size_t bTokEnd) const;

  /// True iff \p text contains \p name as a preprocessing identifier token.
  ///
  /// This is the shared macro-state observation predicate for object-like
  /// definitions and #undef transitions.  It intentionally lexes arbitrary
  /// replacement/source text because that text is not fully represented by the
  /// producer map once users edit B.
  bool RawIdentifierAppearsInText(StringRef name, StringRef text) const;

  /// Return the byte offset of the first identifier-token observation of
  /// \p name in \p text, if any.
  std::optional<size_t> FirstRawIdentifierObservationOffsetInText(
      StringRef name, StringRef text) const;

  /// True iff \p text contains a function-like macro observation of \p name.
  ///
  /// \p suffix is lexed after \p text so callers can prove NAME followed by `(`
  /// across a replacement/source boundary while still returning observations only
  /// whose NAME token starts in \p text.
  bool FunctionLikeInvocationAppearsInText(StringRef name, StringRef text,
                                           StringRef suffix = StringRef()) const;

  /// Return the byte offset of the NAME token that starts the first function-like
  /// invocation observation in \p text, if any.  The following `(` may be in
  /// \p text or in \p suffix.
  std::optional<size_t> FirstFunctionLikeInvocationOffsetInText(
      StringRef name, StringRef text, StringRef suffix = StringRef()) const;

  /// Macro-state observation mode for a moved or preserved #define/#undef.
  ///
  /// Object-like definitions and #undef transitions are observed by any real
  /// preprocessing identifier token with the macro name.  Function-like
  /// definitions are observed only by NAME followed by `(` as preprocessing
  /// tokens.  This shared proof primitive replaces the repeated local enums
  /// previously spread across TU carry, header materialization, include edits,
  /// and unresolved-expansion fallback.
  enum class MacroStateObservationKind {
    IdentifierToken,
    FunctionLikeInvocation,
  };

  /// Return the observation mode for \p directive when it controls \p macroName.
  MacroStateObservationKind MacroStateObservationKindForDirective(
      const RefoldModel::MacroDirective &directive,
      StringRef macroName) const;

  /// Return the first token offset in \p text that would observe \p directive,
  /// or std::nullopt if the bytes cannot observe that macro-state transition.
  std::optional<size_t> FirstMacroStateObservationOffsetInText(
      const RefoldModel::MacroDirective &directive, StringRef macroName,
      StringRef text, StringRef suffix = StringRef()) const;

  /// True iff \p replacement can observe a macro-state directive if that
  /// directive is active before the replacement payload.  Malformed producer
  /// proof data is handled by \p unprovenObserves so callers can remain
  /// fail-closed in their own proof domain.
  bool ReplacementObservesMacroStateDirective(
      const RefoldModel::MacroDirective &directive, StringRef replacement,
      bool unprovenObserves) const;

  /// True iff \p text contains a preprocessing directive line.  Macro-state
  /// carry proofs treat such chunks as observable side-effect hazards unless a
  /// more specific proof class owns the intervening directive.
  bool TextContainsDirectiveLine(StringRef text) const;

  /// True iff moving \p directive across \p chunk could change how those
  /// original source bytes preprocess.  Ordinary text is checked by macro-name
  /// observation; directive lines are conservative side-effect hazards.
  bool SourceChunkObservesMacroStateDirectiveWhenCrossed(
      const RefoldModel::MacroDirective &directive, StringRef macroName,
      StringRef chunk, StringRef following = StringRef()) const;

  /// Exact source-line interval for a producer-recorded macro-state directive.
  ///
  /// MacroDirective::siteB is anchored at the macro name, not necessarily at the
  /// beginning of the physical directive line.  This witness records the
  /// validated full-line interval recovered from the recorded directive text.
  struct MacroStateDirectiveLineInterval {
    const RefoldModel::MacroDirective *directive = nullptr;
    uint64_t begin = 0;
    uint64_t end = 0;
    StringRef name;
  };

  /// Recover and byte-verify the complete physical source line for a recorded
  /// #define/#undef directive in \p fileBytes.
  ///
  /// The helper is the single owner for the repeated proof used by TU carry,
  /// header materialization, include edits, expansion fallback, and replay
  /// stability: the directive must match \p expectedPath, match the requested
  /// include-owner instance, have a producer-recorded macro name, and its
  /// reconstructed full-line bytes must exactly equal MacroDirective::text.
  std::optional<MacroStateDirectiveLineInterval>
  RecoverMacroStateDirectiveLineInterval(
      const RefoldModel::MacroDirective &directive, StringRef expectedPath,
      StringRef fileBytes, std::optional<uint64_t> requiredOwnerIncludeId) const;

  /// File-byte interval for the replacement list of the #define that created a
  /// recorded macro invocation.
  ///
  /// The interval is expressed both in directive-text coordinates and in source
  /// file coordinates.  Source-neutral macro-gap proofs use this to tile only
  /// the replacement-list body while still translating nested callsite byte
  /// ranges back through the producer's macro-name source anchor.
  struct MacroDefinitionReplacementListInterval {
    const RefoldModel::MacroDirective *directive = nullptr;
    size_t nameTextBegin = 0;
    size_t replacementTextBegin = 0;
    uint64_t fileBase = 0;
    uint64_t fileBegin = 0;
    uint64_t fileEnd = 0;
  };

  /// Recover the source interval for the replacement list of the #define used
  /// by \p invocation, if the defining directive and invocation shape are
  /// producer-proven and byte-coordinate translation is well-formed.
  std::optional<MacroDefinitionReplacementListInterval>
  RecoverMacroDefinitionReplacementListInterval(
      const RefoldModel::MacroInvocation &invocation) const;

  // Macro invocation graph (derived from RefoldModel) used for structural
  // queries over recorded callerMacroId relationships.
  DenseMap<uint64_t, SmallVector<const RefoldModel::MacroInvocation *, 4>>
      macroChildrenById_;

  /// Construct an engine from concrete inputs. The instance method `Refold()`
  /// runs the full pipeline using these captured members.
  RefoldEngine(RefoldModel model, StringRef aSource, ArrayRef<PPTok> aToks,
               ArrayRef<size_t> aTokOff, StringRef bSource,
               ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff, bool noLines,
               bool strict, ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
               std::vector<MaterializedEditMapping> *materializedEditMappings =
                   nullptr,
               FinalLineControlValidationCallback finalLineControlValidationCallback =
                   FinalLineControlValidationCallback(),
               std::vector<SourceGraphOutput> *sourceGraphOutputs = nullptr)
      : model_(std::move(model)), aSource_(aSource), bSource_(bSource),
        aToks_(aToks), bToks_(bToks), aTokOff_(aTokOff), bTokOff_(bTokOff),
        lineDirs_(!noLines, model_.GetPPCwd()), strict_(strict),
        lexLang_(MakeLexLangOptions(model_.GetPPLang())),
        materializedEditMappings_(materializedEditMappings),
        sourceGraphOutputs_(sourceGraphOutputs),
        finalLineControlValidationCallback_(
            std::move(finalLineControlValidationCallback)),
        sidebandPragmaEdits_(sidebandPragmaEdits.begin(),
                             sidebandPragmaEdits.end()) {
    BuildMacroInvocationGraph();
  }

  // Build derived macro indices once per run.
  // Note: pointers into model_.GetMacroInvocations() remain stable.
  void BuildMacroInvocationGraph() {
    macroChildrenById_.clear();
    for (const auto &mi : model_.GetMacroInvocations()) {
      if (mi.callerMacroId) {
        macroChildrenById_[*mi.callerMacroId].push_back(&mi);
      }
    }
  }

  /// \brief Run the full refolding pipeline for the current inputs.
  ///
  /// This is the main instance entry point. It classifies token hunks, plans
  /// TU/include/macro edits under the structural policy, materializes the
  /// refolded translation unit, and either returns that single-pass result or
  /// falls back to the explicit terminal edited-preprocessed-stream outcome.
  ///
  /// \returns The refolded C/C++ source text for the translation unit.
  std::string Refold();

  /// \brief Run the single structural refold pass.
  ///
  /// The caller reacts to \c RequestTerminalFallback() by selecting the
  /// explicit terminal fallback result.
  std::string RunSinglePassRefold();

  /// Append source edits for sideband pragma directive text that was present in
  /// the raw `.i` replay surface but removed before token-level diffing.
  /// Returns false after requesting terminal fallback when an edit targets a
  /// non-TU owner or has an invalid source range.
  bool AppendSidebandPragmaSourceEdits(StringRef tuPath, StringRef tuBytes,
                                       std::vector<TextEdit> &tuEdits);

  /// Append TU realization edits for preserved source lines containing
  /// `__LINE__` observers whose B-side preprocessed layout merged that line
  /// with the previous PP line.  Such an observer cannot remain source-spelled:
  /// no `#line` directive can be inserted at the required mid-line point, so
  /// the line must be materialized as the B-side numeric token sequence.
  bool AppendLineObserverLayoutRealizationEdits(
      StringRef tuPath, StringRef tuBytes, std::vector<TextEdit> &tuEdits);

  // ---------------------------- Small Data Records ---------------------------

  /// Represents a pending resync that must be flushed at the next safe BOL.
  ///
  /// `ownerIncludeId` keeps the deferred query in the same owner domain as the
  /// edit that created the drift.  Header instances may contain conditionals
  /// with the same file spelling but different selected arms, so delayed #line
  /// recovery must carry the include-instance identity through to the eventual
  /// flush point.
  struct PendingResync {
    std::string fileSpellingForDir;
    std::optional<uint64_t> ownerIncludeId;

    // True when the pending synthetic resync was emitted for a concrete
    // line-state demand and may therefore enter the fixed-point pruning
    // candidate set.  Phase 6F validates exact deletion attempts directly
    // rather than consulting a late final-stream liveness scanner.
    bool finalLineControlPruneEligible = false;

    // Logical state proved at the original source offset where the replacement
    // rejoins untouched text.  Pending flushes may occur later, after copying a
    // prefix of that untouched text; in that case the state advances by the
    // non-spliced newlines copied between resumeOffset and the flush point.
    std::string resumeFileSpelling;
    size_t resumeLineNo = 0;
    uint64_t resumeOffset = 0;

    // True when the pending correction is dominated by a preserved conditional
    // join rather than by the next textual BOL inside the selected arm.  In
    // that case the line-state repair must be emitted by the join pass at the
    // first post-group observer, not before an intervening preprocessor
    // directive in only the selected arm.
    bool deferToConditionalJoin = false;

    explicit PendingResync(llvm::StringRef file,
                           std::optional<uint64_t> ownerInclude = std::nullopt,
                           bool pruneEligible = false,
                           llvm::StringRef resumeFile = llvm::StringRef(),
                           size_t resumeLine = 0,
                           uint64_t resumeAt = 0,
                           bool deferToJoin = false)
        : fileSpellingForDir(file.str()), ownerIncludeId(ownerInclude),
          finalLineControlPruneEligible(pruneEligible),
          resumeFileSpelling(resumeFile.empty() ? file.str()
                                                : resumeFile.str()),
          resumeLineNo(resumeLine), resumeOffset(resumeAt),
          deferToConditionalJoin(deferToJoin) {}
  };

  /// The final text and optional pending state for an edit.
  struct ResyncOutcome {
    std::string text;
    std::optional<PendingResync> pending;
    std::vector<FinalLineControlPruneCandidate> lineControlPruneCandidates;

    ResyncOutcome(std::string t, std::optional<PendingResync> p)
        : text(std::move(t)), pending(std::move(p)) {}

    ResyncOutcome(std::string t, std::optional<PendingResync> p,
                  std::vector<FinalLineControlPruneCandidate> candidates)
        : text(std::move(t)), pending(std::move(p)),
          lineControlPruneCandidates(std::move(candidates)) {}
  };


  /// \brief Closed Phase-3A inventory of concrete emission surfaces.
  ///
  /// This enum intentionally does not replace TheoremProofClass.  It answers
  /// only "which concrete artifact surface can reach emission?" so Phase 3B
  /// can force every such surface through AcceptedResultCandidate without
  /// rediscovering path names by grep.  Some entries are primary artifact
  /// surfaces, while mixed-owner tiling and owner-realization materialization
  /// are proof overlays that can coexist with a macro/include/TU primary path.
#define REFOLD_EMISSION_PATH_KIND_LIST(REFOLD_X)                              \
  REFOLD_X(Unknown)                                                           \
  REFOLD_X(MacroPatch)                                                        \
  REFOLD_X(IncludePatch)                                                      \
  REFOLD_X(TUAnchor)                                                          \
  REFOLD_X(TUTextEdit)                                                        \
  REFOLD_X(TerminalOutOfDomain)                                               \
  REFOLD_X(MixedOwnerTilingSegment)                                           \
  REFOLD_X(OwnerRealizationMaterialization)

  enum class EmissionPathKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_EMISSION_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(EmissionPathKind value) {
    switch (value) {
#define REFOLD_X(name) case EmissionPathKind::name: return #name;
      REFOLD_EMISSION_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_EMISSION_PATH_KIND_LIST

  /// \brief Canonical list of emission paths represented by one candidate.
  ///
  /// A normalized candidate always has at most one primary emitted surface
  /// (`MacroPatch`, `IncludePatch`, `TUAnchor`, `TUTextEdit`, or
  /// `TerminalOutOfDomain`).  It may also carry proof overlays, such as a
  /// mixed-owner segment witness or an owner-realization materialization
  /// witness.  Keeping those overlays in the same deduplicated inventory avoids
  /// another parallel family of booleans while preserving the distinction
  /// between construction provenance and theorem proof authority.
  struct EmissionPathInventory {
    std::vector<EmissionPathKind> paths;

    bool Contains(EmissionPathKind kind) const {
      for (EmissionPathKind existing : paths)
        if (existing == kind)
          return true;
      return false;
    }

    void Add(EmissionPathKind kind) {
      if (kind == EmissionPathKind::Unknown || Contains(kind))
        return;
      paths.push_back(kind);
    }

    bool empty() const { return paths.empty(); }
  };

  struct AcceptedResultCandidate;

  struct TextEdit {
    uint64_t start, end;
    std::string text;
    std::optional<PendingResync> pending;

    // Root macro invocation id to charge as expanded if this edit survives
    // normalization and is applied in the final chosen refold result.
    std::optional<uint64_t> expandedMacroRootId = std::nullopt;

    // Normalized accepted-result carriers for the non-terminal artifacts that
    // were composed into this final emitted edit. This is structural only: it
    // does not change emission semantics, but it makes the proof-bearing
    // source of each emitted artifact explicit at the byte-edit boundary so a
    // later universal proof gate can reason over the actual emitted surface.
    std::vector<std::shared_ptr<const AcceptedResultCandidate>>
        acceptedResults;

    // Final-output line-control pruning candidates carried by this edit, using
    // byte offsets relative to `text`.  Only synthetic directives that a local
    // emitter explicitly proves eligible are listed here; source-authored
    // directives copied through materialized text remain fail-closed until the
    // final model has producer-backed source-line-control evidence.
    std::vector<FinalLineControlPruneCandidate> lineControlPruneCandidates = {};

    // Final-output source mappings carried by replacement text, using byte
    // offsets relative to `text`.  These are present only for byte-for-byte
    // source material threaded through a replacement, such as materialized
    // include bodies.  Replayed B payloads and synthetic text remain unmapped.
    std::vector<FinalLineControlSourceMapping> lineControlSourceMappings = {};

    // Optional provenance for a TextEdit emitted directly from a single
    // token-level TU hunk. This is deliberately not inferred for macro,
    // include, or synthesized closure edits: the closed-realization resolver
    // may only coalesce edits that still have a one-to-one token-hunk witness.
    bool isDirectTUHunkEdit = false;
    std::optional<uint64_t> directTUHunkIndex = std::nullopt;
    std::optional<uint64_t> directTUHunkAStart = std::nullopt;
    std::optional<uint64_t> directTUHunkAEnd = std::nullopt;
    std::optional<uint64_t> directTUHunkBStart = std::nullopt;
    std::optional<uint64_t> directTUHunkBEnd = std::nullopt;

    // Raw and final TU byte spans for direct TU hunk edits. raw* records the
    // immediate token-map span before lexical widening/spacing repair; final*
    // records the span actually handed to the byte applicator.
    std::optional<uint64_t> directTURawStart = std::nullopt;
    std::optional<uint64_t> directTURawEnd = std::nullopt;
    std::optional<uint64_t> directTUFinalStart = std::nullopt;
    std::optional<uint64_t> directTUFinalEnd = std::nullopt;

    // Optional B-side byte range for the materialized replacement. This is set
    // by the producer of the final edit surface, then consumed only by the
    // optional sidecar mapping writer at the final TU emission boundary.
    std::optional<uint64_t> materializedBByteBegin = std::nullopt;
    std::optional<uint64_t> materializedBByteEnd = std::nullopt;

    // Optional byte range inside `text` that should be reported as the
    // refolded-output side of the materialized edit map. Most edits map to
    // their whole replacement text. Invocation-preserving macro rewrites can
    // replace a full callsite while only the rewritten argument envelope is the
    // source surface corresponding to the B-side materialization witness.
    std::optional<uint64_t> materializedOutputTextBegin = std::nullopt;
    std::optional<uint64_t> materializedOutputTextEnd = std::nullopt;
  };

  /// Replacement text produced while wrapping a materialized include together
  /// with the synthetic line-control candidates that were inserted by the
  /// wrapper.  Candidate offsets are relative to `text`; the file-level edit
  /// applicator translates them to final-output offsets only after all edit
  /// normalization and pending-resync flushing has been resolved.
  struct LineControlWrappedText {
    std::string text;
    std::vector<FinalLineControlPruneCandidate> lineControlPruneCandidates = {};
    std::vector<FinalLineControlSourceMapping> lineControlSourceMappings = {};
  };

  // Result of planning header-local edits for one include. When
  // requiresIncludeRealization is set, the caller must stop trying to anchor
  // header-local edits and realize the include directly from the edited
  // preprocessed stream B instead.
  struct IncludeTextEditPlan {
    std::vector<TextEdit> edits;
    bool requiresIncludeRealization = false;
    std::string realizationReason;
  };

  struct PasteArgEdit {
    uint32_t argIdx;
    std::string newSeg;
    std::string oldSeg;
    std::optional<uint32_t> argByteBegin;
    std::optional<uint32_t> argByteEnd;

    PasteArgEdit(uint32_t Idx, std::string New, std::string Old,
                 std::optional<uint32_t> ArgByteBegin = std::nullopt,
                 std::optional<uint32_t> ArgByteEnd = std::nullopt)
        : argIdx(Idx), newSeg(std::move(New)), oldSeg(std::move(Old)),
          argByteBegin(ArgByteBegin), argByteEnd(ArgByteEnd) {}
  };

  // ---------------------------- Ownership Helpers ----------------------------

  /// Logical owner class used by the theorem-facing closure model.
  ///
  /// Existing emission paths still materialize TU and include owners directly,
  /// while macro and conditional owners are often represented by specialized
  /// proof carriers.  The closed-domain model treats all of them as instances
  /// of the same owner identity so future tiling, state-summary composition,
  /// and terminal out-of-domain diagnostics can reason through one interface
  /// instead of through owner-specific fallback ladders.
  enum class OwnerKind {
    TU,
    Include,
    MacroInvocation,
    MacroDirective,
    LineControlIsland,
    PragmaIsland,
    ConditionalGroup,
    ConditionalArm,
    Unknown
  };

  friend inline StringRef toString(OwnerKind kind) {
    switch (kind) {
    case OwnerKind::TU:
      return "TU";
    case OwnerKind::Include:
      return "Include";
    case OwnerKind::MacroInvocation:
      return "MacroInvocation";
    case OwnerKind::MacroDirective:
      return "MacroDirective";
    case OwnerKind::LineControlIsland:
      return "LineControlIsland";
    case OwnerKind::PragmaIsland:
      return "PragmaIsland";
    case OwnerKind::ConditionalGroup:
      return "ConditionalGroup";
    case OwnerKind::ConditionalArm:
      return "ConditionalArm";
    case OwnerKind::Unknown:
      return "Unknown";
    }
    llvm_unreachable("Invalid owner kind");
  }

  // Grant access to the specific formatter specialization
  template <typename T, typename Enable> friend struct llvm::format_provider;

  /// Stable identity for one refolding owner.
  ///
  /// `Owner` is deliberately small and value-semantic: it names the owner only.
  /// Source intervals, A/B token envelopes, state summaries, and suffix
  /// observers live in `OwnerClosure` below.  Keeping identity separate from
  /// closure facts prevents call sites from accidentally treating a classified
  /// owner as a discharged proof.
  struct Owner {
    OwnerKind kind = OwnerKind::Unknown;
    // non-nullopt only when kind == Include.
    std::optional<uint64_t> includeId;
    // non-nullopt only when kind == MacroInvocation.
    std::optional<uint64_t> macroInvocationId;
    // non-nullopt only when kind == MacroDirective.
    std::optional<uint64_t> macroDirectiveId;
    // non-nullopt only when kind == LineControlIsland.
    std::optional<uint64_t> lineControlId;
    // non-nullopt only when kind == PragmaIsland.
    std::optional<uint64_t> pragmaId;
    // non-nullopt only when kind == ConditionalGroup.
    std::optional<uint64_t> condGroupId;
    // nullable; non-nullopt when the owner is in or is a specific conditional
    // arm.
    std::optional<uint64_t> condArmId;

    static Owner TU(std::optional<uint64_t> condArmId = std::nullopt) {
      Owner o;
      o.kind = OwnerKind::TU;
      o.condArmId = condArmId;
      return o;
    }

    static Owner Include(uint64_t includeId,
                         std::optional<uint64_t> condArmId = std::nullopt) {
      Owner o;
      o.kind = OwnerKind::Include;
      o.includeId = includeId;
      o.condArmId = condArmId;
      return o;
    }

    static Owner MacroInvocation(
        uint64_t macroInvocationId,
        std::optional<uint64_t> condArmId = std::nullopt) {
      Owner o;
      o.kind = OwnerKind::MacroInvocation;
      o.macroInvocationId = macroInvocationId;
      o.condArmId = condArmId;
      return o;
    }

    static Owner MacroDirective(
        uint64_t macroDirectiveId,
        std::optional<uint64_t> condArmId = std::nullopt) {
      Owner o;
      o.kind = OwnerKind::MacroDirective;
      o.macroDirectiveId = macroDirectiveId;
      o.condArmId = condArmId;
      return o;
    }

    static Owner LineControlIsland(
        uint64_t lineControlId,
        std::optional<uint64_t> condArmId = std::nullopt) {
      Owner o;
      o.kind = OwnerKind::LineControlIsland;
      o.lineControlId = lineControlId;
      o.condArmId = condArmId;
      return o;
    }

    static Owner PragmaIsland(
        uint64_t pragmaId,
        std::optional<uint64_t> condArmId = std::nullopt) {
      Owner o;
      o.kind = OwnerKind::PragmaIsland;
      o.pragmaId = pragmaId;
      o.condArmId = condArmId;
      return o;
    }

    static Owner ConditionalGroup(uint64_t condGroupId) {
      Owner o;
      o.kind = OwnerKind::ConditionalGroup;
      o.condGroupId = condGroupId;
      return o;
    }

    static Owner ConditionalArm(uint64_t condArmId) {
      Owner o;
      o.kind = OwnerKind::ConditionalArm;
      o.condArmId = condArmId;
      return o;
    }

    static Owner Unknown() { return Owner(); }

    bool IsKnown() const { return kind != OwnerKind::Unknown; }

    bool IsTU() const { return kind == OwnerKind::TU; }

    bool IsInclude() const { return kind == OwnerKind::Include; }

    bool IsMacroInvocation() const {
      return kind == OwnerKind::MacroInvocation;
    }

    bool IsMacroDirective() const {
      return kind == OwnerKind::MacroDirective;
    }

    bool IsLineControlIsland() const {
      return kind == OwnerKind::LineControlIsland;
    }

    bool IsPragmaIsland() const {
      return kind == OwnerKind::PragmaIsland;
    }

    bool IsConditionalGroup() const {
      return kind == OwnerKind::ConditionalGroup;
    }

    bool IsConditionalArm() const {
      return kind == OwnerKind::ConditionalArm;
    }

    bool HasSameIdentity(const Owner &other) const {
      return kind == other.kind && includeId == other.includeId &&
             macroInvocationId == other.macroInvocationId &&
             macroDirectiveId == other.macroDirectiveId &&
             lineControlId == other.lineControlId &&
             pragmaId == other.pragmaId && condGroupId == other.condGroupId &&
             condArmId == other.condArmId;
    }
  };

  /// Half-open token interval in either the original preprocessed token stream
  /// A or the edited preprocessed token stream B.
  struct OwnerTokenRange {
    uint64_t begin = 0;
    uint64_t end = 0;

    static OwnerTokenRange From(uint64_t begin, uint64_t end) {
      return {begin, end};
    }

    bool IsValid() const { return begin <= end; }

    bool Empty() const { return begin == end; }

    bool Contains(uint64_t tok) const { return begin <= tok && tok < end; }

    bool Contains(OwnerTokenRange other) const {
      return IsValid() && other.IsValid() && begin <= other.begin &&
             other.end <= end;
    }
  };

  /// Half-open physical source interval owned by one source surface.
  ///
  /// `path` is the owner-local source file, `[begin,end)` is byte-based in that
  /// file, and `includeId` identifies the concrete include occurrence when the
  /// source surface is header-owned.  The range is only a candidate source
  /// fact; `OwnerClosure::IsComplete()` is the theorem-facing gate that also
  /// requires owner identity and token-envelope validity.
  struct OwnerSourceRange {
    std::string path;
    uint64_t begin = 0;
    uint64_t end = 0;
    std::optional<uint64_t> includeId = std::nullopt;

    static OwnerSourceRange From(StringRef path, uint64_t begin, uint64_t end,
                                 std::optional<uint64_t> includeId =
                                     std::nullopt) {
      OwnerSourceRange r;
      r.path = path.str();
      r.begin = begin;
      r.end = end;
      r.includeId = includeId;
      return r;
    }

    bool IsValid() const { return begin <= end; }

    bool Empty() const { return begin == end; }

    bool HasPath() const { return !path.empty(); }

    bool IsComplete() const { return HasPath() && IsValid(); }
  };

  struct OwnerObserverSummary;

  /// Producer-derived identity for one macro-state fact.
  ///
  /// Phase 3B keeps macro state component-specific instead of treating the
  /// entire macro namespace as one global bit.  The identity deliberately stores
  /// only producer facts or deterministic projections of those facts; it never
  /// reparses a directive to infer missing state.  Empty optional fields mean the
  /// producer did not supply that part of the identity, in which case the owner
  /// summary also carries the appropriate conservative missing/unmodeled marker.
  struct MacroStateIdentity {
    std::string macroName;
    std::optional<uint64_t> definitionDirectiveId = std::nullopt;
    std::optional<uint64_t> undefDirectiveId = std::nullopt;
    bool functionLike = false;
    std::optional<uint32_t> arity = std::nullopt;
    bool variadic = false;
    std::string replacementTokenHash;

    bool operator==(const MacroStateIdentity &other) const {
      return macroName == other.macroName &&
             definitionDirectiveId == other.definitionDirectiveId &&
             undefDirectiveId == other.undefDirectiveId &&
             functionLike == other.functionLike && arity == other.arity &&
             variadic == other.variadic &&
             replacementTokenHash == other.replacementTokenHash;
    }
  };

  /// The kind of macro-state observation made by an owner.
  ///
  /// Phase 3C separates ordinary macro expansion from `defined(NAME)` queries
  /// and conditional-expression macro dependencies.  These observations are not
  /// interchangeable: a suffix may depend only on whether a macro is defined, on
  /// the replacement tokens used for expansion, or on conditional branch
  /// selection.
  enum class MacroObservationKind : uint8_t {
    Expansion,
    DefinedOperator,
    ConditionalEvaluation
  };

  friend inline StringRef toString(MacroObservationKind kind) {
    switch (kind) {
    case MacroObservationKind::Expansion:
      return "MacroExpansionObservation";
    case MacroObservationKind::DefinedOperator:
      return "DefinedOperatorObservation";
    case MacroObservationKind::ConditionalEvaluation:
      return "ConditionalMacroObservation";
    }
    llvm_unreachable("Invalid macro observation kind");
  }

  struct MacroStateObservation {
    MacroObservationKind kind = MacroObservationKind::Expansion;
    MacroStateIdentity identity;

    bool operator==(const MacroStateObservation &other) const {
      return kind == other.kind && identity == other.identity;
    }
  };


  /// Producer provenance for a logical line-control mutation.
  ///
  /// Phase 3E makes line/file state component-specific.  A `#line` directive is
  /// not just textual trivia: it mutates the logical line, file, and basename
  /// state later observed by `__LINE__`, `__FILE__`, and `__FILE_NAME__`.
  /// These provenance values say whether the producer, not the consumer,
  /// evaluated the directive operands and conditional activity.
  enum class LineDirectiveOperandProvenance : uint8_t {
    ProducerEvaluatedOperands,
    InactiveDirective,
    MissingProducerOperands,
    Unknown
  };

  friend inline StringRef toString(LineDirectiveOperandProvenance provenance) {
    switch (provenance) {
    case LineDirectiveOperandProvenance::ProducerEvaluatedOperands:
      return "ProducerEvaluatedOperands";
    case LineDirectiveOperandProvenance::InactiveDirective:
      return "InactiveDirective";
    case LineDirectiveOperandProvenance::MissingProducerOperands:
      return "MissingProducerOperands";
    case LineDirectiveOperandProvenance::Unknown:
      return "Unknown";
    }
    llvm_unreachable("Invalid line-directive operand provenance");
  }

  /// Producer-derived identity for one zero-token line-control state mutation.
  ///
  /// The physical directive bytes may produce no ordinary A-side tokens, but the
  /// event changes the logical source-location state observed by later owners.
  /// Empty optional fields are conservative missing facts: later theorem checks
  /// may reject the transition, but they must not infer it from downstream
  /// `__LINE__`/`__FILE__` values.
  struct LineControlStateIdentity {
    uint64_t eventId = 0;
    std::string physicalFile;
    std::optional<uint64_t> siteBegin = std::nullopt;
    std::optional<uint64_t> siteEnd = std::nullopt;
    bool active = false;
    bool producerProven = false;
    uint64_t logicalLineAfter = 0;
    std::string logicalFileAfter;
    std::optional<uint64_t> ownerIncludeId = std::nullopt;
    LineDirectiveOperandProvenance operandProvenance =
        LineDirectiveOperandProvenance::Unknown;

    bool operator==(const LineControlStateIdentity &other) const {
      return eventId == other.eventId && physicalFile == other.physicalFile &&
             siteBegin == other.siteBegin && siteEnd == other.siteEnd &&
             active == other.active && producerProven == other.producerProven &&
             logicalLineAfter == other.logicalLineAfter &&
             logicalFileAfter == other.logicalFileAfter &&
             ownerIncludeId == other.ownerIncludeId &&
             operandProvenance == other.operandProvenance;
    }
  };

  /// Location-state component observed by a builtin token.
  enum class BuiltinLocationObservationKind : uint8_t {
    LineState,
    FileState,
    FileNameState
  };

  friend inline StringRef toString(BuiltinLocationObservationKind kind) {
    switch (kind) {
    case BuiltinLocationObservationKind::LineState:
      return "LineState";
    case BuiltinLocationObservationKind::FileState:
      return "FileState";
    case BuiltinLocationObservationKind::FileNameState:
      return "FileNameState";
    }
    llvm_unreachable("Invalid builtin location observation kind");
  }

  /// Producer/source-local observation of logical line/file state.
  ///
  /// The observation may come from source text, an invocation spelling, or an
  /// already-expanded PP token.  Optional source/token anchors are evidence only;
  /// absence keeps the coarse observation bit conservative without fabricating a
  /// stronger ordering fact.
  struct BuiltinLocationObservation {
    BuiltinLocationObservationKind kind =
        BuiltinLocationObservationKind::LineState;
    std::optional<uint64_t> ownerIncludeId = std::nullopt;
    std::optional<uint64_t> sourceBegin = std::nullopt;
    std::optional<uint64_t> sourceEnd = std::nullopt;
    std::optional<uint64_t> aTokenBegin = std::nullopt;
    std::optional<uint64_t> aTokenEnd = std::nullopt;

    bool operator==(const BuiltinLocationObservation &other) const {
      return kind == other.kind && ownerIncludeId == other.ownerIncludeId &&
             sourceBegin == other.sourceBegin && sourceEnd == other.sourceEnd &&
             aTokenBegin == other.aTokenBegin && aTokenEnd == other.aTokenEnd;
    }
  };

  /// Producer-derived identity for one `__COUNTER__` event.
  ///
  /// Phase 3G makes counter state event-specific.  The identity names the
  /// producer macro invocation and concrete A-token occurrence that consumed the
  /// counter stream.  `expectedBValue` is optional because ordinary owner-state
  /// summaries are edit-independent; later stabilization code can fill or check
  /// it when an A->B alignment is available.
  struct CounterEventIdentity {
    uint64_t macroInvocationId = 0;
    uint64_t occurrenceOrdinal = 0;
    std::optional<uint64_t> ownerIncludeId = std::nullopt;
    std::optional<uint64_t> callerMacroId = std::nullopt;
    uint64_t aTokenBegin = 0;
    uint64_t aTokenEnd = 0;
    std::string expansionSiteFile;
    std::optional<uint64_t> expansionSiteBegin = std::nullopt;
    std::optional<uint64_t> expansionSiteEnd = std::nullopt;
    std::string aValue;
    std::optional<std::string> expectedBValue = std::nullopt;
    bool canStabilizeByLiteralization = false;
    bool canStabilizeByMaterialization = false;

    bool operator==(const CounterEventIdentity &other) const {
      return macroInvocationId == other.macroInvocationId &&
             occurrenceOrdinal == other.occurrenceOrdinal &&
             ownerIncludeId == other.ownerIncludeId &&
             callerMacroId == other.callerMacroId &&
             aTokenBegin == other.aTokenBegin && aTokenEnd == other.aTokenEnd &&
             expansionSiteFile == other.expansionSiteFile &&
             expansionSiteBegin == other.expansionSiteBegin &&
             expansionSiteEnd == other.expansionSiteEnd &&
             aValue == other.aValue && expectedBValue == other.expectedBValue &&
             canStabilizeByLiteralization ==
                 other.canStabilizeByLiteralization &&
             canStabilizeByMaterialization ==
                 other.canStabilizeByMaterialization;
    }
  };


  /// Producer-derived identity for a concrete include directive occurrence.
  ///
  /// Phase 3I separates include-path resolution, include file identity, include
  /// token materialization, and include-state effects.  This identity is built
  /// only from serialized producer facts: the as-written target, the resolved
  /// path when present, the includer/source site, and the concrete include
  /// instance id.  It is not an include-path resolver.
  struct IncludeStateIdentity {
    uint64_t includeId = 0;
    std::string directiveKind;
    std::string sitePath;
    uint64_t siteBegin = 0;
    uint64_t siteEnd = 0;
    std::string target;
    std::optional<std::string> resolvedPath = std::nullopt;
    bool angled = false;
    std::optional<uint64_t> parentIncludeId = std::nullopt;
    bool hasTokenMaterialization = false;

    bool operator==(const IncludeStateIdentity &other) const {
      return includeId == other.includeId &&
             directiveKind == other.directiveKind &&
             sitePath == other.sitePath && siteBegin == other.siteBegin &&
             siteEnd == other.siteEnd && target == other.target &&
             resolvedPath == other.resolvedPath && angled == other.angled &&
             parentIncludeId == other.parentIncludeId &&
             hasTokenMaterialization == other.hasTokenMaterialization;
    }
  };

  /// The producer-visible include-guard role of one include occurrence.
  ///
  /// The current map does not yet serialize a first-class include-guard oracle,
  /// so `guardMacroName` is optional.  An empty guard macro is still useful: it
  /// ties the guard-state risk to a concrete header/include identity instead of
  /// poisoning the whole include-state universe.  Later producer extensions can
  /// fill the macro name without changing the theorem-facing state shape.
  enum class IncludeGuardObservationKind : uint8_t {
    ActiveIncludeMayMutateGuard,
    SkippedIncludeMayObserveGuard,
    UnknownGuardEffect
  };

  friend inline StringRef toString(IncludeGuardObservationKind kind) {
    switch (kind) {
    case IncludeGuardObservationKind::ActiveIncludeMayMutateGuard:
      return "ActiveIncludeMayMutateGuard";
    case IncludeGuardObservationKind::SkippedIncludeMayObserveGuard:
      return "SkippedIncludeMayObserveGuard";
    case IncludeGuardObservationKind::UnknownGuardEffect:
      return "UnknownGuardEffect";
    }
    llvm_unreachable("Invalid include-guard observation kind");
  }

  struct IncludeGuardStateIdentity {
    IncludeGuardObservationKind kind =
        IncludeGuardObservationKind::UnknownGuardEffect;
    uint64_t includeId = 0;
    std::string headerPath;
    std::optional<std::string> guardMacroName = std::nullopt;
    std::optional<uint64_t> parentIncludeId = std::nullopt;
    bool producerProvenGuard = false;

    bool operator==(const IncludeGuardStateIdentity &other) const {
      return kind == other.kind && includeId == other.includeId &&
             headerPath == other.headerPath &&
             guardMacroName == other.guardMacroName &&
             parentIncludeId == other.parentIncludeId &&
             producerProvenGuard == other.producerProvenGuard;
    }
  };

  /// Producer-side pragma classification used by Phase 3J.
  ///
  /// Unknown pragmas remain fail-closed.  Known diagnostic pragmas are recorded
  /// as component-specific state so later suffix-stability code can distinguish
  /// a balanced/local pragma island from an opaque state transition.
  enum class PragmaStateClassification : uint8_t {
    KnownLocalPragmaState,
    KnownBalancedPragmaState,
    UnknownPragmaState
  };

  friend inline StringRef toString(PragmaStateClassification classification) {
    switch (classification) {
    case PragmaStateClassification::KnownLocalPragmaState:
      return "KnownLocalPragmaState";
    case PragmaStateClassification::KnownBalancedPragmaState:
      return "KnownBalancedPragmaState";
    case PragmaStateClassification::UnknownPragmaState:
      return "UnknownPragmaState";
    }
    llvm_unreachable("Invalid pragma-state classification");
  }

  struct PragmaStateIdentity {
    uint64_t pragmaId = 0;
    std::string sitePath;
    uint64_t siteBegin = 0;
    uint64_t siteEnd = 0;
    std::optional<uint64_t> ownerIncludeId = std::nullopt;
    PragmaStateClassification classification =
        PragmaStateClassification::UnknownPragmaState;
    std::string directiveFingerprint;

    bool operator==(const PragmaStateIdentity &other) const {
      return pragmaId == other.pragmaId && sitePath == other.sitePath &&
             siteBegin == other.siteBegin && siteEnd == other.siteEnd &&
             ownerIncludeId == other.ownerIncludeId &&
             classification == other.classification &&
             directiveFingerprint == other.directiveFingerprint;
    }
  };

  /// Producer-derived branch-selection state for a conditional group or arm.
  ///
  /// Phase 3K makes conditionals explicit state rather than treating a selected
  /// arm as ordinary source text.  The identity records which directive/arm was
  /// producer-selected and which condition text was observed.  It does not
  /// reverse-solve a different branch condition from downstream B-side tokens.
  enum class ConditionalStateRole : uint8_t {
    ConditionalGroup,
    ActiveArm,
    InactiveArm
  };

  friend inline StringRef toString(ConditionalStateRole role) {
    switch (role) {
    case ConditionalStateRole::ConditionalGroup:
      return "ConditionalGroup";
    case ConditionalStateRole::ActiveArm:
      return "ActiveArm";
    case ConditionalStateRole::InactiveArm:
      return "InactiveArm";
    }
    llvm_unreachable("Invalid conditional-state role");
  }

  struct ConditionalStateIdentity {
    ConditionalStateRole role = ConditionalStateRole::ConditionalGroup;
    uint64_t groupId = 0;
    std::optional<uint64_t> armId = std::nullopt;
    std::string file;
    uint64_t groupBegin = 0;
    uint64_t groupEnd = 0;
    std::optional<uint64_t> parentArmId = std::nullopt;
    std::optional<uint64_t> parentIncludeId = std::nullopt;
    std::string armKind;
    std::optional<std::string> conditionText = std::nullopt;
    bool selected = false;
    std::optional<uint64_t> aTokenBegin = std::nullopt;
    std::optional<uint64_t> aTokenEnd = std::nullopt;
    bool conditionTruthProducerProven = true;
    bool reverseSolvedDirectiveRequired = false;

    bool operator==(const ConditionalStateIdentity &other) const {
      return role == other.role && groupId == other.groupId &&
             armId == other.armId && file == other.file &&
             groupBegin == other.groupBegin && groupEnd == other.groupEnd &&
             parentArmId == other.parentArmId &&
             parentIncludeId == other.parentIncludeId &&
             armKind == other.armKind &&
             conditionText == other.conditionText && selected == other.selected &&
             aTokenBegin == other.aTokenBegin && aTokenEnd == other.aTokenEnd &&
             conditionTruthProducerProven == other.conditionTruthProducerProven &&
             reverseSolvedDirectiveRequired ==
                 other.reverseSolvedDirectiveRequired;
    }
  };



  /// Component-specific producer fact that is missing from the owner-state
  /// summary.
  ///
  /// Phase 3L replaces the old single "unmodeled state" surface with precise
  /// missing-fact markers.  Theorem-facing code can distinguish a missing macro
  /// definition identity from missing line-control operands, counter events,
  /// pragma classification, include-guard facts, conditional branch facts, or
  /// owner ordering.
  enum class MissingStateFactKind : uint8_t {
    MissingMacroFacts,
    MissingLineControlFacts,
    MissingCounterFacts,
    MissingPragmaFacts,
    MissingIncludeGuardFacts,
    MissingConditionalFacts,
    MissingOwnerOrderingFacts
  };

  friend inline StringRef toString(MissingStateFactKind kind) {
    switch (kind) {
    case MissingStateFactKind::MissingMacroFacts:
      return "MissingMacroFacts";
    case MissingStateFactKind::MissingLineControlFacts:
      return "MissingLineControlFacts";
    case MissingStateFactKind::MissingCounterFacts:
      return "MissingCounterFacts";
    case MissingStateFactKind::MissingPragmaFacts:
      return "MissingPragmaFacts";
    case MissingStateFactKind::MissingIncludeGuardFacts:
      return "MissingIncludeGuardFacts";
    case MissingStateFactKind::MissingConditionalFacts:
      return "MissingConditionalFacts";
    case MissingStateFactKind::MissingOwnerOrderingFacts:
      return "MissingOwnerOrderingFacts";
    }
    llvm_unreachable("Invalid missing state fact kind");
  }

  struct MissingStateFact {
    MissingStateFactKind kind = MissingStateFactKind::MissingOwnerOrderingFacts;
    std::string detail;

    bool operator==(const MissingStateFact &other) const {
      return kind == other.kind && detail == other.detail;
    }
  };

  /// Shared Phase-3 state-fact payload.
  ///
  /// R3 removes the old flat boolean compatibility surface.  Every fact in this
  /// bucket is now component-specific: macro facts are keyed by macro identity,
  /// line-control facts are keyed by producer events, counter facts are keyed by
  /// event identity, and missing producer information is carried explicitly as a
  /// MissingStateFact obligation.
  struct OwnerStateFacts {
    // Phase-3B/3C component-specific macro facts.
    std::vector<MacroStateIdentity> macroDefinitions;
    std::vector<MacroStateIdentity> macroUndefinitions;
    std::vector<MacroStateIdentity> macroRequirements;
    std::vector<MacroStateObservation> macroExpansionObservations;
    std::vector<MacroStateObservation> definedOperatorObservations;
    std::vector<MacroStateObservation> conditionalMacroObservations;

    // Phase-3E/3F component-specific line-control facts.  `lineControlEvents`
    // are zero-token state mutations; `builtinLocationObservations` are reads of
    // a particular logical-location component.
    std::vector<LineControlStateIdentity> lineControlEvents;
    std::vector<BuiltinLocationObservation> builtinLocationObservations;

    // Phase-3G component-specific counter events.  Each event names one
    // producer-proven occurrence instead of treating the counter stream as a
    // global text scan.
    std::vector<CounterEventIdentity> counterEvents;

    // Phase-3J component-specific pragma facts.  Unknown pragma semantics stay
    // attached to the event identity instead of a global poison bit.
    std::vector<PragmaStateIdentity> pragmaStateEvents;

    // Phase-3H/3I component-specific include facts.  These keep include path
    // resolution, file identity, token materialization, and guard effects
    // separable.
    std::vector<IncludeStateIdentity> includeStateEvents;
    std::vector<IncludeGuardStateIdentity> includeGuardStateEvents;

    // Phase-3K component-specific conditional facts.  These distinguish the
    // directive island, active arm, inactive arms, condition-expression macro
    // dependencies, and reverse-solving boundary explicitly.
    std::vector<ConditionalStateIdentity> conditionalStateEvents;

    // Phase-3L component-specific missing producer facts.  These are
    // theorem-facing obligations; incomplete facts make composition less
    // admissible, never more admissible.
    std::vector<MissingStateFact> missingStateFacts;

    bool HasMissingStateFacts() const { return !missingStateFacts.empty(); }

    bool HasMissingFactKind(MissingStateFactKind kind) const {
      return llvm::any_of(missingStateFacts, [&](const MissingStateFact &fact) {
        return fact.kind == kind;
      });
    }

    bool HasMacroDefinitions() const { return !macroDefinitions.empty(); }

    bool HasMacroUndefinitions() const { return !macroUndefinitions.empty(); }

    bool HasMacroRequirements() const { return !macroRequirements.empty(); }

    bool HasMacroExpansionObservations() const {
      return !macroExpansionObservations.empty();
    }

    bool HasDefinedOperatorObservations() const {
      return !definedOperatorObservations.empty();
    }

    bool HasConditionalMacroObservations() const {
      return !conditionalMacroObservations.empty();
    }

    bool HasLineControlEvents() const { return !lineControlEvents.empty(); }

    bool HasBuiltinLocationObservations() const {
      return !builtinLocationObservations.empty();
    }

    bool HasCounterEvents() const { return !counterEvents.empty(); }

    bool HasPragmaStateEvents() const { return !pragmaStateEvents.empty(); }

    bool HasIncludeStateEvents() const { return !includeStateEvents.empty(); }

    bool HasIncludeGuardStateEvents() const {
      return !includeGuardStateEvents.empty();
    }

    bool HasConditionalStateEvents() const {
      return !conditionalStateEvents.empty();
    }

    /// Return true if this bucket contains theorem-facing component facts.
    bool HasTheoremStateFacts() const {
      return HasMacroDefinitions() || HasMacroUndefinitions() ||
             HasMacroRequirements() || HasMacroExpansionObservations() ||
             HasDefinedOperatorObservations() ||
             HasConditionalMacroObservations() || HasLineControlEvents() ||
             HasBuiltinLocationObservations() || HasCounterEvents() ||
             HasPragmaStateEvents() || HasIncludeStateEvents() ||
             HasIncludeGuardStateEvents() || HasConditionalStateEvents() ||
             HasMissingStateFacts();
    }

    /// Copy theorem-facing component facts from `other`.
    OwnerStateFacts &MergeTheoremFactsFrom(const OwnerStateFacts &other) {
      for (const MacroStateIdentity &identity : other.macroDefinitions)
        AddMacroDefinition(identity);
      for (const MacroStateIdentity &identity : other.macroUndefinitions)
        AddMacroUndefinition(identity);
      for (const MacroStateIdentity &identity : other.macroRequirements)
        AddMacroRequirement(identity);
      for (const MacroStateObservation &observation :
           other.macroExpansionObservations)
        AddMacroObservation(observation);
      for (const MacroStateObservation &observation :
           other.definedOperatorObservations)
        AddMacroObservation(observation);
      for (const MacroStateObservation &observation :
           other.conditionalMacroObservations)
        AddMacroObservation(observation);
      for (const LineControlStateIdentity &identity :
           other.lineControlEvents)
        AddLineControlEvent(identity);
      for (const BuiltinLocationObservation &observation :
           other.builtinLocationObservations)
        AddBuiltinLocationObservation(observation);
      for (const CounterEventIdentity &identity : other.counterEvents)
        AddCounterEvent(identity);
      for (const PragmaStateIdentity &identity : other.pragmaStateEvents)
        AddPragmaStateEvent(identity);
      for (const IncludeStateIdentity &identity : other.includeStateEvents)
        AddIncludeStateEvent(identity);
      for (const IncludeGuardStateIdentity &identity :
           other.includeGuardStateEvents)
        AddIncludeGuardStateEvent(identity);
      for (const ConditionalStateIdentity &identity :
           other.conditionalStateEvents)
        AddConditionalStateEvent(identity);
      for (const MissingStateFact &fact : other.missingStateFacts)
        AddMissingStateFact(fact.kind, fact.detail);
      return *this;
    }

    bool HasTheoremUnknownPragmaState() const {
      if (HasMissingFactKind(MissingStateFactKind::MissingPragmaFacts))
        return true;
      return llvm::any_of(pragmaStateEvents,
                          [](const PragmaStateIdentity &identity) {
                            return identity.classification ==
                                   PragmaStateClassification::UnknownPragmaState;
                          });
    }

    bool MutatesMacroState() const {
      return HasMacroDefinitions() || HasMacroUndefinitions();
    }

    bool ObservesBuiltinLocationState() const {
      return HasBuiltinLocationObservations();
    }

    bool MutatesLineFileState() const { return HasLineControlEvents(); }

    bool MutatesPragmaState() const { return HasPragmaStateEvents(); }

    bool MutatesIncludeState() const { return HasIncludeStateEvents(); }

    bool ObservesIncludeState() const { return HasIncludeStateEvents(); }

    bool MutatesIncludeGuardState() const {
      return HasIncludeGuardStateEvents();
    }

    bool ObservesIncludeGuardState() const {
      return HasIncludeGuardStateEvents();
    }

    bool MutatesConditionalState() const {
      return HasConditionalStateEvents();
    }

    bool ObservesConditionalState() const {
      return HasConditionalStateEvents();
    }

    bool MutatesAnyState() const {
      return MutatesMacroState() || MutatesLineFileState() ||
             HasCounterEvents() || MutatesPragmaState() ||
             MutatesIncludeState() || MutatesIncludeGuardState() ||
             MutatesConditionalState() || HasMissingStateFacts();
    }

    bool ObservesAnyState() const {
      return HasMacroRequirements() || HasMacroExpansionObservations() ||
             HasDefinedOperatorObservations() ||
             HasConditionalMacroObservations() ||
             ObservesBuiltinLocationState() || HasCounterEvents() ||
             HasPragmaStateEvents() || ObservesIncludeState() ||
             ObservesIncludeGuardState() || ObservesConditionalState() ||
             HasMissingStateFacts();
    }

    bool Empty() const { return !MutatesAnyState() && !ObservesAnyState(); }

    template <typename T>
    static void AppendUnique(std::vector<T> &dst, ArrayRef<T> src) {
      for (const T &item : src)
        AppendUniqueOne(dst, item);
    }

    template <typename T>
    static void AppendUniqueOne(std::vector<T> &dst, const T &item) {
      if (llvm::none_of(dst, [&](const T &existing) {
            return existing == item;
          }))
        dst.push_back(item);
    }

    void AddMissingStateFact(MissingStateFactKind kind, StringRef detail) {
      MissingStateFact fact;
      fact.kind = kind;
      fact.detail = detail.str();
      AppendUniqueOne(missingStateFacts, fact);
    }

    void AddMacroDefinition(const MacroStateIdentity &identity) {
      AppendUniqueOne(macroDefinitions, identity);
    }

    void AddMacroUndefinition(const MacroStateIdentity &identity) {
      AppendUniqueOne(macroUndefinitions, identity);
    }

    void AddMacroRequirement(const MacroStateIdentity &identity) {
      AppendUniqueOne(macroRequirements, identity);
    }

    void AddMacroObservation(const MacroStateObservation &observation) {
      switch (observation.kind) {
      case MacroObservationKind::Expansion:
        AppendUniqueOne(macroExpansionObservations, observation);
        break;
      case MacroObservationKind::DefinedOperator:
        AppendUniqueOne(definedOperatorObservations, observation);
        break;
      case MacroObservationKind::ConditionalEvaluation:
        AppendUniqueOne(conditionalMacroObservations, observation);
        break;
      }
    }


    void AddLineControlEvent(const LineControlStateIdentity &identity) {
      AppendUniqueOne(lineControlEvents, identity);
    }

    void AddBuiltinLocationObservation(
        const BuiltinLocationObservation &observation) {
      AppendUniqueOne(builtinLocationObservations, observation);
    }

    void AddCounterObservation(const CounterEventIdentity &identity) {
      AppendUniqueOne(counterEvents, identity);
    }

    void AddCounterMutation(const CounterEventIdentity &identity) {
      AppendUniqueOne(counterEvents, identity);
    }

    void AddCounterEvent(const CounterEventIdentity &identity) {
      AddCounterObservation(identity);
    }

    void AddPragmaStateEvent(const PragmaStateIdentity &identity) {
      AppendUniqueOne(pragmaStateEvents, identity);
    }

    void AddIncludeStateEvent(const IncludeStateIdentity &identity) {
      AppendUniqueOne(includeStateEvents, identity);
    }

    void AddIncludeGuardStateEvent(
        const IncludeGuardStateIdentity &identity) {
      AppendUniqueOne(includeGuardStateEvents, identity);
      // Missing guard-oracle facts are represented in the identity itself.
      // Phase 3L will convert component-specific missing facts into named
      // obligations; do not collapse them back into the global unmodeled bit
      // here, or every include would conservatively poison unrelated state.
    }

    void AddConditionalStateEvent(
        const ConditionalStateIdentity &identity) {
      AppendUniqueOne(conditionalStateEvents, identity);
    }

    OwnerStateFacts &MergeFrom(const OwnerStateFacts &other) {
      AppendUnique(macroDefinitions,
                   ArrayRef<MacroStateIdentity>(other.macroDefinitions));
      AppendUnique(macroUndefinitions,
                   ArrayRef<MacroStateIdentity>(other.macroUndefinitions));
      AppendUnique(macroRequirements,
                   ArrayRef<MacroStateIdentity>(other.macroRequirements));
      AppendUnique(macroExpansionObservations,
                   ArrayRef<MacroStateObservation>(
                       other.macroExpansionObservations));
      AppendUnique(definedOperatorObservations,
                   ArrayRef<MacroStateObservation>(
                       other.definedOperatorObservations));
      AppendUnique(conditionalMacroObservations,
                   ArrayRef<MacroStateObservation>(
                       other.conditionalMacroObservations));
      AppendUnique(lineControlEvents,
                   ArrayRef<LineControlStateIdentity>(other.lineControlEvents));
      AppendUnique(builtinLocationObservations,
                   ArrayRef<BuiltinLocationObservation>(
                       other.builtinLocationObservations));
      AppendUnique(counterEvents,
                   ArrayRef<CounterEventIdentity>(other.counterEvents));
      AppendUnique(pragmaStateEvents,
                   ArrayRef<PragmaStateIdentity>(other.pragmaStateEvents));
      AppendUnique(includeStateEvents,
                   ArrayRef<IncludeStateIdentity>(other.includeStateEvents));
      AppendUnique(includeGuardStateEvents,
                   ArrayRef<IncludeGuardStateIdentity>(
                       other.includeGuardStateEvents));
      AppendUnique(conditionalStateEvents,
                   ArrayRef<ConditionalStateIdentity>(
                       other.conditionalStateEvents));
      AppendUnique(missingStateFacts,
                   ArrayRef<MissingStateFact>(other.missingStateFacts));
      return *this;
    }
  };

  /// State required to hold at owner entry before the owner's source bytes are
  /// replayed or preserved.  Phase 3B+ will make this component-specific; Phase
  /// 3A uses the existing conservative bits as the seed vocabulary.
  struct StateRequirements : OwnerStateFacts {};

  /// State actually observed by the owner while producing its A-token envelope.
  struct StateObservations : OwnerStateFacts {};

  /// State transitions performed by the owner.
  struct StateMutations : OwnerStateFacts {};

  /// State equivalences the owner requires at exit for a preserved suffix to
  /// remain valid.  Today this is projected from the mutation surface; later
  /// phases can record finer post-state guarantees here.
  struct StateGuarantees : OwnerStateFacts {};

  /// Canonical Phase-3 state-delta model for one owner.
  ///
  /// This is the theorem-facing answer to four different questions that the old
  /// boolean-only summary could not separate:
  ///
  ///   * Entry:    what state must already be true before this owner?
  ///   * Observes: what state does this owner read?
  ///   * Mutates:  what state does this owner change?
  ///   * Exit:     what state must be equivalent after this owner?
  ///
  /// The buckets are monotone.  A missing producer fact must set the relevant
  /// unmodeled bit in one or more buckets, never clear an obligation.
  struct OwnerStateDelta {
    StateRequirements Entry;
    StateObservations Observes;
    StateMutations Mutates;
    StateGuarantees Exit;

    bool Empty() const {
      return Entry.Empty() && Observes.Empty() && Mutates.Empty() &&
             Exit.Empty();
    }

    OwnerStateDelta &MergeFrom(const OwnerStateDelta &other) {
      Entry.MergeFrom(other.Entry);
      Observes.MergeFrom(other.Observes);
      Mutates.MergeFrom(other.Mutates);
      Exit.MergeFrom(other.Exit);
      return *this;
    }

    /// Merge theorem-facing component facts from another delta.
    ///
    /// This path keeps the Entry/Observes/Mutates/Exit bucket structure intact
    /// while preserving the precise per-component facts already attached to the
    /// source delta.
    OwnerStateDelta &MergeTheoremFactsFrom(const OwnerStateDelta &other) {
      Entry.MergeTheoremFactsFrom(other.Entry);
      Observes.MergeTheoremFactsFrom(other.Observes);
      Mutates.MergeTheoremFactsFrom(other.Mutates);
      Exit.MergeTheoremFactsFrom(other.Exit);
      return *this;
    }
  };

  /// Owner closures now carry OwnerStateDelta directly.  OwnerStateFacts remains
  /// as the builder bucket for producer-derived component facts; it is not a
  /// theorem-facing carrier on its own.

  /// Summary of suffix-visible observers that constrain state composition.
  ///
  /// These are the observable surfaces for suffix-stability proofs.  The model
  /// is intentionally selective: a state component only constrains composition
  /// when a later preserved owner actually observes it.
  struct OwnerObserverSummary {
    bool observesMacroExpansion = false;
    bool observesDefinedOperator = false;
    bool observesConditionalEvaluation = false;
    bool observesLineNumber = false;
    bool observesFileState = false;
    bool observesFileName = false;
    bool observesCounter = false;
    bool observesPragmaState = false;
    bool observesIncludeGuardState = false;
    bool observesIncludeState = false;

    bool Empty() const {
      return !observesMacroExpansion && !observesDefinedOperator &&
             !observesConditionalEvaluation && !observesLineNumber &&
             !observesFileState && !observesFileName && !observesCounter &&
             !observesPragmaState &&
             !observesIncludeGuardState && !observesIncludeState;
    }

    OwnerObserverSummary &MergeFrom(const OwnerObserverSummary &other) {
      observesMacroExpansion |= other.observesMacroExpansion;
      observesDefinedOperator |= other.observesDefinedOperator;
      observesConditionalEvaluation |= other.observesConditionalEvaluation;
      observesLineNumber |= other.observesLineNumber;
      observesFileState |= other.observesFileState;
      observesFileName |= other.observesFileName;
      observesCounter |= other.observesCounter;
      observesPragmaState |= other.observesPragmaState;
      observesIncludeGuardState |= other.observesIncludeGuardState;
      observesIncludeState |= other.observesIncludeState;
      return *this;
    }
  };

  /// Canonical theorem-facing closure record for one owner-local refolding
  /// candidate.
  ///
  /// This is the normalization point for Step 2 of the closed-domain roadmap:
  /// TU edits, include/header realizations, macro invocation rewrites,
  /// conditional-arm islands, sideband line/pragmas, and future mixed-owner
  /// tiling segments should all be expressible as an `OwnerClosure` before they
  /// are admitted by a proof gate.  The structure is intentionally passive in
  /// this patch; it gives the existing specialized machinery a common target
  /// without changing emission behavior.
  struct OwnerClosure {
    Owner owner;
    OwnerSourceRange source;
    OwnerTokenRange aTokens;
    OwnerTokenRange bTokens;
    OwnerStateDelta stateIn;
    OwnerStateDelta stateOut;
    OwnerObserverSummary observers;

    static OwnerClosure From(Owner owner, OwnerSourceRange source,
                             OwnerTokenRange aTokens,
                             OwnerTokenRange bTokens) {
      OwnerStateDelta stateIn;
      OwnerStateDelta stateOut;
      OwnerObserverSummary observers;
      return From(std::move(owner), std::move(source), aTokens, bTokens,
                  stateIn, stateOut, observers);
    }

    static OwnerClosure From(Owner owner, OwnerSourceRange source,
                             OwnerTokenRange aTokens,
                             OwnerTokenRange bTokens,
                             OwnerStateDelta stateIn,
                             OwnerStateDelta stateOut,
                             OwnerObserverSummary observers) {
      OwnerClosure closure;
      closure.owner = std::move(owner);
      closure.source = std::move(source);
      closure.aTokens = aTokens;
      closure.bTokens = bTokens;
      closure.stateIn = stateIn;
      closure.stateOut = stateOut;
      closure.observers = observers;
      return closure;
    }

    bool IsComplete() const {
      return owner.IsKnown() && source.IsComplete() && aTokens.IsValid() &&
             bTokens.IsValid();
    }

    bool IsStateNeutral() const {
      return stateIn.Empty() && stateOut.Empty() && observers.Empty();
    }

    bool HasSameOwner(const OwnerClosure &other) const {
      return owner.HasSameIdentity(other.owner);
    }
  };

  /// State component key used by the Phase-4 suffix-observer graph.
  ///
  /// These components are intentionally coarser than Clang's full preprocessor
  /// state.  They are the theorem-facing equivalence classes that an edit may
  /// disturb before a preserved suffix: later Phase-5 enforcement only needs to
  /// know whether a suffix owner can observe one of these components.
  enum class OwnerStateComponent : uint8_t {
    MacroState,
    DefinedOperator,
    ConditionalState,
    LineNumber,
    FileState,
    FileName,
    Counter,
    PragmaState,
    IncludeGuardState,
    IncludeState,
    UnmodeledState,
    Unknown
  };

  friend inline StringRef toString(OwnerStateComponent component) {
    switch (component) {
    case OwnerStateComponent::MacroState:
      return "MacroState";
    case OwnerStateComponent::DefinedOperator:
      return "DefinedOperator";
    case OwnerStateComponent::ConditionalState:
      return "ConditionalState";
    case OwnerStateComponent::LineNumber:
      return "LineNumber";
    case OwnerStateComponent::FileState:
      return "FileState";
    case OwnerStateComponent::FileName:
      return "FileName";
    case OwnerStateComponent::Counter:
      return "Counter";
    case OwnerStateComponent::PragmaState:
      return "PragmaState";
    case OwnerStateComponent::IncludeGuardState:
      return "IncludeGuardState";
    case OwnerStateComponent::IncludeState:
      return "IncludeState";
    case OwnerStateComponent::UnmodeledState:
      return "UnmodeledState";
    case OwnerStateComponent::Unknown:
      return "Unknown";
    }
    llvm_unreachable("Invalid owner state component");
  }

  /// Boundary before a preserved suffix whose state observers are being queried.
  ///
  /// A boundary may be source-based, token-based, or both.  Source boundaries
  /// are used for owner-local repair decisions (`#define`, `#line`, pragmas,
  /// etc.); token boundaries let future proof code ask about observers that are
  /// only comparable through the A-token stream.  Missing comparison facts are
  /// conservative: the query reports incomparable observers instead of assuming
  /// there is no suffix dependency.
  struct OwnerStateBoundary {
    OwnerSourceRange source;
    OwnerTokenRange aTokens;
    bool hasSourceBoundary = false;
    bool hasTokenBoundary = false;

    static OwnerStateBoundary FromSource(OwnerSourceRange source) {
      OwnerStateBoundary boundary;
      boundary.source = std::move(source);
      boundary.hasSourceBoundary = true;
      return boundary;
    }

    static OwnerStateBoundary FromATokens(OwnerTokenRange aTokens) {
      OwnerStateBoundary boundary;
      boundary.aTokens = aTokens;
      boundary.hasTokenBoundary = true;
      return boundary;
    }

    static OwnerStateBoundary FromSourceAndATokens(OwnerSourceRange source,
                                                   OwnerTokenRange aTokens) {
      OwnerStateBoundary boundary;
      boundary.source = std::move(source);
      boundary.aTokens = aTokens;
      boundary.hasSourceBoundary = true;
      boundary.hasTokenBoundary = true;
      return boundary;
    }
  };

  /// Canonical preprocessing-order node kinds for the Phase-4 state graph.
  ///
  /// These are theorem-facing semantic events, not construction algorithms.
  /// Ordinary token owners and macro invocations may have non-empty A-token
  /// ranges; directive/control events often have empty token ranges but still
  /// mutate or observe preprocessor state and therefore must remain ordered in
  /// the graph.
  enum class OwnerStateGraphNodeKind : uint8_t {
    OrdinaryTokenOwner,
    MacroInvocation,
    NestedMacroExpansion,
    IncludeEntry,
    IncludeExit,
    MacroDefineEvent,
    MacroUndefEvent,
    LineControlEvent,
    PragmaEvent,
    ConditionalEvent,
    CounterEvent,
    Unknown
  };

  friend inline StringRef toString(OwnerStateGraphNodeKind kind) {
    switch (kind) {
    case OwnerStateGraphNodeKind::OrdinaryTokenOwner:
      return "OrdinaryTokenOwner";
    case OwnerStateGraphNodeKind::MacroInvocation:
      return "MacroInvocation";
    case OwnerStateGraphNodeKind::NestedMacroExpansion:
      return "NestedMacroExpansion";
    case OwnerStateGraphNodeKind::IncludeEntry:
      return "IncludeEntry";
    case OwnerStateGraphNodeKind::IncludeExit:
      return "IncludeExit";
    case OwnerStateGraphNodeKind::MacroDefineEvent:
      return "MacroDefineEvent";
    case OwnerStateGraphNodeKind::MacroUndefEvent:
      return "MacroUndefEvent";
    case OwnerStateGraphNodeKind::LineControlEvent:
      return "LineControlEvent";
    case OwnerStateGraphNodeKind::PragmaEvent:
      return "PragmaEvent";
    case OwnerStateGraphNodeKind::ConditionalEvent:
      return "ConditionalEvent";
    case OwnerStateGraphNodeKind::CounterEvent:
      return "CounterEvent";
    case OwnerStateGraphNodeKind::Unknown:
      return "Unknown";
    }
    llvm_unreachable("Invalid owner state graph node kind");
  }


  /// Stable node in the persistent owner/state-event graph.
  ///
  /// Phase 4A--4C require every semantic event to be represented explicitly,
  /// including zero-token directives such as #define, #undef, #line, #pragma,
  /// conditional-control islands, include entry/exit markers, and __COUNTER__
  /// events.  `closure` names the owner and source/token evidence; `state` is
  /// the precise theorem-facing delta attached to that node.  `predecessor` and
  /// `successor` are the deterministic total-order neighbors in this graph,
  /// while later Phase-4/5 code may still reject nodes whose producer facts are
  /// insufficient to compare against a particular edit boundary.
  struct OwnerStateGraphNode {
    uint64_t id = 0;
    uint64_t order = 0;
    OwnerStateGraphNodeKind kind = OwnerStateGraphNodeKind::Unknown;
    OwnerClosure closure;
    OwnerStateDelta state;
    OwnerSourceRange source;
    OwnerTokenRange aTokens;
    std::optional<uint64_t> predecessor = std::nullopt;
    std::optional<uint64_t> successor = std::nullopt;
    std::optional<uint64_t> containingIncludeId = std::nullopt;
    std::optional<uint64_t> containingMacroInvocationId = std::nullopt;
    std::optional<uint64_t> containingConditionalArmId = std::nullopt;
    std::string detail;

    bool HasTokenAnchor() const {
      return aTokens.IsValid() && !aTokens.Empty();
    }

    bool IsZeroTokenEvent() const { return !HasTokenAnchor(); }
  };

  /// Observer kind returned by first-observer suffix queries.
  ///
  /// Phase 4D/4E keeps the state component coarse enough for the Phase-5
  /// gateway, but records the exact observation surface that caused the suffix
  /// dependency.  This lets repair/materialization code distinguish, for
  /// example, ordinary macro expansion from defined(NAME), or __FILE__ from
  /// __FILE_NAME__, without rescanning the owner.
  enum class SuffixObservationKind : uint8_t {
    MacroExpansionObservation,
    DefinedOperatorObservation,
    ConditionalMacroObservation,
    LineStateObservation,
    FileStateObservation,
    FileNameStateObservation,
    CounterObservation,
    PragmaStateObservation,
    IncludeGuardStateObservation,
    IncludeStateObservation,
    UnmodeledStateObservation,
    Unknown
  };

  friend inline StringRef toString(SuffixObservationKind kind) {
    switch (kind) {
    case SuffixObservationKind::MacroExpansionObservation:
      return "MacroExpansionObservation";
    case SuffixObservationKind::DefinedOperatorObservation:
      return "DefinedOperatorObservation";
    case SuffixObservationKind::ConditionalMacroObservation:
      return "ConditionalMacroObservation";
    case SuffixObservationKind::LineStateObservation:
      return "LineStateObservation";
    case SuffixObservationKind::FileStateObservation:
      return "FileStateObservation";
    case SuffixObservationKind::FileNameStateObservation:
      return "FileNameStateObservation";
    case SuffixObservationKind::CounterObservation:
      return "CounterObservation";
    case SuffixObservationKind::PragmaStateObservation:
      return "PragmaStateObservation";
    case SuffixObservationKind::IncludeGuardStateObservation:
      return "IncludeGuardStateObservation";
    case SuffixObservationKind::IncludeStateObservation:
      return "IncludeStateObservation";
    case SuffixObservationKind::UnmodeledStateObservation:
      return "UnmodeledStateObservation";
    case SuffixObservationKind::Unknown:
      return "Unknown";
    }
    llvm_unreachable("Invalid suffix observation kind");
  }

  /// Proof that a suffix observer was ordered relative to an edit boundary.
  ///
  /// Incomparable observer order is a named missing-producer-facts condition,
  /// never absence of observation.  Phase 5 consumes this value to decide
  /// whether state repair may be inserted before the first observer.
  enum class SuffixOrderingProofKind : uint8_t {
    SourceOrder,
    TokenOrder,
    SourceAndTokenOrder,
    IncomparableMissingProducerFacts,
    Unknown
  };

  friend inline StringRef toString(SuffixOrderingProofKind kind) {
    switch (kind) {
    case SuffixOrderingProofKind::SourceOrder:
      return "SourceOrder";
    case SuffixOrderingProofKind::TokenOrder:
      return "TokenOrder";
    case SuffixOrderingProofKind::SourceAndTokenOrder:
      return "SourceAndTokenOrder";
    case SuffixOrderingProofKind::IncomparableMissingProducerFacts:
      return "IncomparableMissingProducerFacts";
    case SuffixOrderingProofKind::Unknown:
      return "Unknown";
    }
    llvm_unreachable("Invalid suffix ordering proof kind");
  }

  /// One suffix observer discovered by the Phase-4 observer graph.
  ///
  /// `owner` identifies the later owner, `source`/`aTokens` give the comparable
  /// position evidence, and `component` names the state component observed by
  /// that owner.  The `observations` field preserves the full projected observer
  /// summary so Phase 5 can make component-specific repair/materialization
  /// decisions without rebuilding the graph.
  struct SuffixStateObserverSite {
    uint64_t nodeId = 0;
    OwnerStateGraphNodeKind nodeKind = OwnerStateGraphNodeKind::Unknown;
    Owner owner;
    OwnerSourceRange source;
    OwnerTokenRange aTokens;
    OwnerStateComponent component = OwnerStateComponent::Unknown;
    SuffixObservationKind observationKind = SuffixObservationKind::Unknown;
    OwnerObserverSummary observations;
    uint64_t order = 0;
    std::string componentKey;
    std::string detail;
  };

  /// Indexed observer result returned by Phase-4 first-observer queries.
  struct SuffixObserverResult {
    uint64_t observerSiteIndex = 0;
    uint64_t nodeId = 0;
    Owner firstObserver;
    OwnerStateComponent component = OwnerStateComponent::Unknown;
    SuffixObservationKind observationKind = SuffixObservationKind::Unknown;
    SuffixOrderingProofKind orderingProof =
        SuffixOrderingProofKind::Unknown;
    bool materializable = false;
    bool repairCanPrecede = false;
    SuffixStateObserverSite site;
  };

  /// Component-specific observer indexes for the persistent state graph.
  ///
  /// The unkeyed vectors are the Phase-5 gateway surface; keyed maps preserve
  /// the Phase-3 component identities so later proofs can ask more precise
  /// questions such as "who observes macro FOO?" without scanning every owner.
  struct OwnerStateGraphObserverIndex {
    std::vector<uint64_t> macroStateObservers;
    std::vector<uint64_t> definedOperatorObservers;
    std::vector<uint64_t> conditionalStateObservers;
    std::vector<uint64_t> lineStateObservers;
    std::vector<uint64_t> fileStateObservers;
    std::vector<uint64_t> fileNameStateObservers;
    std::vector<uint64_t> counterObservers;
    std::vector<uint64_t> pragmaStateObservers;
    std::vector<uint64_t> includeGuardStateObservers;
    std::vector<uint64_t> includeStateObservers;
    std::vector<uint64_t> unmodeledStateObservers;

    llvm::StringMap<std::vector<uint64_t>> observersByMacroName;
    llvm::StringMap<std::vector<uint64_t>> observersByCounterEvent;
    llvm::StringMap<std::vector<uint64_t>> observersByPragmaState;
    llvm::StringMap<std::vector<uint64_t>> observersByIncludeGuard;
    llvm::StringMap<std::vector<uint64_t>> observersByIncludeState;
    llvm::StringMap<std::vector<uint64_t>> observersByConditionalState;
  };

  /// Phase-4 graph census emitted to theorem/debug logs.
  struct OwnerStateGraphAuditStats {
    uint64_t ownerNodes = 0;
    uint64_t zeroTokenStateNodes = 0;
    uint64_t observedStateComponents = 0;
    uint64_t mutatedStateComponents = 0;
    uint64_t incomparableNodes = 0;
    uint64_t missingProducerFacts = 0;
  };


  /// Persistent Phase-4 owner/state-event graph.
  ///
  /// The graph is built once per RefoldEngine instance from producer facts.  It
  /// is the canonical census used by suffix queries: callers should not perform
  /// owner-specific scans for macro, include, line-control, pragma, conditional,
  /// or counter events once this graph is available.
  struct OwnerStateGraph {
    std::vector<OwnerStateGraphNode> nodes;
    std::vector<SuffixStateObserverSite> observerSites;
    OwnerStateGraphObserverIndex observerIndex;
    OwnerStateGraphAuditStats audit;

    bool Empty() const { return nodes.empty(); }
  };

  /// Cached Phase-4 owner/state-event graph.
  ///
  /// The refold model is immutable after construction, so the graph can be
  /// built lazily once and reused by every suffix-observer query.  This is the
  /// first step toward making suffix stability a graph query rather than a set
  /// of owner-specific rescans.
  mutable std::optional<OwnerStateGraph> ownerStateGraphCache_;

  /// Result of querying the preserved suffix after an edit boundary.
  ///
  /// `sites` contains observers that are proven to occur after the boundary.
  /// `hasIncomparableObserver` is a conservative signal: some owner observes the
  /// requested component, but the available producer facts were insufficient to
  /// order it relative to the boundary.  Phase 5 must treat that as an
  /// undischarged suffix-stability obligation, not as absence of observation.
  struct SuffixObserverQueryResult {
    std::vector<SuffixStateObserverSite> sites;
    std::vector<SuffixObserverResult> orderedObservers;
    std::vector<SuffixObserverResult> incomparableResults;
    std::optional<SuffixObserverResult> firstObserver = std::nullopt;
    bool hasIncomparableObserver = false;
    bool hasNoCanonicalSuffixOrder = false;
    OwnerObserverSummary incomparableObservers;

    bool Empty() const {
      return sites.empty() && !hasIncomparableObserver;
    }
  };

  /// Build the conservative Phase-3 state summary for a concrete owner.
  ///
  /// The result is monotone: missing producer facts or unknown owner identity set
  /// a MissingStateFact instead of clearing obligations.  This helper is a
  /// theorem-facing census primitive only; later phases decide how to consume or
  /// enforce the returned obligations.
  OwnerStateDelta BuildOwnerStateDelta(const Owner &owner) const;

  /// Project producer-derived builder facts into the theorem-facing delta.
  ///
  /// This helper is the R3 replacement for the old OwnerStateSummary bridge: it
  /// copies only precise component facts and explicit MissingStateFact markers
  /// into Entry/Observes/Mutates/Exit, then merges already-typed delta facts.
  static OwnerStateDelta BuildTheoremStateDelta(
      const OwnerStateFacts &facts, const OwnerStateDelta &directDelta);

  /// Query helpers for OwnerStateDelta.  Keeping these centralized prevents
  /// call sites from reintroducing flat OwnerStateSummary-style predicates.
  static bool OwnerStateDeltaHasUnmodeledState(
      const OwnerStateDelta &delta);
  static bool OwnerStateDeltaHasUnknownPragmaState(
      const OwnerStateDelta &delta);
  static bool OwnerStateDeltaMutatesAnyState(const OwnerStateDelta &delta);
  static OwnerObserverSummary
  OwnerStateDeltaToObserverSummary(const OwnerStateDelta &delta);

  /// Return `closure` with its canonical owner-state facts attached.
  ///
  /// Existing callers may still construct passive closures without summaries.
  /// This helper gives new proof code one normalization point that annotates a
  /// closure from producer metadata before composing it with neighboring owners.
  OwnerClosure AttachCanonicalStateSummary(OwnerClosure closure) const;

  /// Build the deterministic persistent Phase-4 owner/state-event graph.
  ///
  /// The graph contains ordinary token owners, include entry/exit events, nested
  /// macro expansion owners, and all zero-token state directives recorded by
  /// the producer.  It is a census only: suffix-stability enforcement remains a
  /// Phase-5 responsibility.
  OwnerStateGraph BuildOwnerStateGraph() const;

  /// Return the cached Phase-4 owner/state-event graph, building it once.
  const OwnerStateGraph &GetOwnerStateGraph() const;

  /// Return later suffix observers of `component` after `boundary`.
  ///
  /// This is the canonical suffix-observer API.  Callers that need raw graph
  /// nodes should consume `GetOwnerStateGraph()` directly rather than rebuild a
  /// compatibility projection; this keeps ordering and observer indexing in one
  /// proof surface.
  ///
  /// The query is conservative.  If an observer exists but cannot be ordered
  /// against the boundary using source or token facts, the result marks it as
  /// incomparable so the caller cannot accidentally discharge the obligation.
  SuffixObserverQueryResult
  FindSuffixObservers(const OwnerStateBoundary &boundary,
                      OwnerStateComponent component) const;

  /// How a source edit changes one state component at an edit boundary.
  ///
  /// Phase 5B separates the *kind* of state transition from the proof that
  /// makes that transition safe.  A consumed #define, a replayed #line, and a
  /// literalized __COUNTER__ occurrence may all affect suffix stability, but
  /// they have different proof obligations and diagnostics.  The gateway below
  /// receives one of these mutation kinds before it asks the suffix-observer
  /// graph whether any preserved owner can observe the changed component.
  enum class StateMutationKind : uint8_t {
    Consumed,
    Deleted,
    MovedEarlier,
    MovedLater,
    Replayed,
    PreservedAcrossReplacement,
    Materialized,
    Literalized,
    WidenedIntoClosure,
    Unknown
  };

  friend inline StringRef toString(StateMutationKind kind) {
    switch (kind) {
    case StateMutationKind::Consumed:
      return "Consumed";
    case StateMutationKind::Deleted:
      return "Deleted";
    case StateMutationKind::MovedEarlier:
      return "MovedEarlier";
    case StateMutationKind::MovedLater:
      return "MovedLater";
    case StateMutationKind::Replayed:
      return "Replayed";
    case StateMutationKind::PreservedAcrossReplacement:
      return "PreservedAcrossReplacement";
    case StateMutationKind::Materialized:
      return "Materialized";
    case StateMutationKind::Literalized:
      return "Literalized";
    case StateMutationKind::WidenedIntoClosure:
      return "WidenedIntoClosure";
    case StateMutationKind::Unknown:
      return "Unknown";
    }
    llvm_unreachable("Invalid state mutation kind");
  }


  /// Direct state-sensitive surface currently audited by Phase 5A.
  ///
  /// This is an inventory vocabulary, not a selector.  Each entry names one
  /// class of local source/preprocessor-state handling that must eventually be
  /// represented by OwnerStateDelta, OwnerStateGraph, the state-transition
  /// gateway, or a terminal proof failure before it can justify emitted bytes.
#define REFOLD_DIRECT_STATE_CHECK_KIND_LIST(REFOLD_X)                         \
  REFOLD_X(Unknown)                                                           \
  REFOLD_X(MacroDefinitionDirective)                                          \
  REFOLD_X(MacroUndefDirective)                                               \
  REFOLD_X(LineControlDirective)                                              \
  REFOLD_X(BuiltinLineObserver)                                               \
  REFOLD_X(BuiltinFileObserver)                                               \
  REFOLD_X(BuiltinFileNameObserver)                                           \
  REFOLD_X(CounterEvent)                                                      \
  REFOLD_X(PragmaDirective)                                                   \
  REFOLD_X(IncludeDirectiveState)                                             \
  REFOLD_X(IncludeGuardState)                                                 \
  REFOLD_X(ConditionalDirectiveState)                                         \
  REFOLD_X(MacroExpansionState)                                               \
  REFOLD_X(UnmodeledStateFact)

  enum class DirectStateCheckKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_DIRECT_STATE_CHECK_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(DirectStateCheckKind kind) {
    switch (kind) {
#define REFOLD_X(name) case DirectStateCheckKind::name: return #name;
      REFOLD_DIRECT_STATE_CHECK_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }

#undef REFOLD_DIRECT_STATE_CHECK_KIND_LIST

  /// How a direct state-sensitive local check is already closed.
  ///
  /// Phase 5A uses this enum to distinguish legitimate local producer-fact
  /// collection from legacy emission authority.  Only Unknown and
  /// ExplicitlyUnclosedLocalCheck are audit findings; the other values document
  /// the proof surface that now owns the state fact.
  enum class DirectStateCheckClosureKind : uint8_t {
    Unknown,
    OwnerStateDeltaFact,
    OwnerStateGraphEdge,
    StateTransitionGatewayWitness,
    TerminalFallbackProofFailure,
    ExplicitlyUnclosedLocalCheck
  };

  friend inline StringRef toString(DirectStateCheckClosureKind kind) {
    switch (kind) {
    case DirectStateCheckClosureKind::Unknown:
      return "Unknown";
    case DirectStateCheckClosureKind::OwnerStateDeltaFact:
      return "OwnerStateDeltaFact";
    case DirectStateCheckClosureKind::OwnerStateGraphEdge:
      return "OwnerStateGraphEdge";
    case DirectStateCheckClosureKind::StateTransitionGatewayWitness:
      return "StateTransitionGatewayWitness";
    case DirectStateCheckClosureKind::TerminalFallbackProofFailure:
      return "TerminalFallbackProofFailure";
    case DirectStateCheckClosureKind::ExplicitlyUnclosedLocalCheck:
      return "ExplicitlyUnclosedLocalCheck";
    }
    llvm_unreachable("Invalid direct state check closure kind");
  }

  /// Record one Phase-5A direct-state-check inventory item for no-legacy audit.
  void AuditDirectStateCheckClosure(DirectStateCheckKind checkKind,
                                    OwnerStateComponent component,
                                    DirectStateCheckClosureKind closure,
                                    StateMutationKind mutation,
                                    llvm::StringRef phase,
                                    llvm::StringRef detail) const;

  /// Map a state component to the closest direct-state-check inventory key.
  static DirectStateCheckKind
  DirectStateCheckKindForComponent(OwnerStateComponent component);


  /// Map owner-state graph nodes back to the Phase-5A direct-check inventory.
  static DirectStateCheckKind
  DirectStateCheckKindForGraphNode(OwnerStateGraphNodeKind kind);
  static OwnerStateComponent
  DirectStateComponentForGraphNode(OwnerStateGraphNodeKind kind);

  /// Producer-closure proof for edits that would otherwise reverse-solve an
  /// upstream directive from downstream B-side tokens.
  ///
  /// Phase 5K makes this a gateway-level invariant: changing macro definitions,
  /// #line operands, include paths, or conditional truth from the resulting
  /// expansion is forbidden unless the directive owner itself belongs to the
  /// accepted owner closure.  Unknown closure proof is also fail-closed because
  /// absence of a producer fact is not evidence that reverse-solving is safe.
  enum class DirectiveClosureStatus : uint8_t {
    NotADirectiveStateRewrite,
    DirectiveOwnerInsideAcceptedClosure,
    DirectiveOwnerOutsideAcceptedClosure,
    Unknown
  };

  friend inline StringRef toString(DirectiveClosureStatus status) {
    switch (status) {
    case DirectiveClosureStatus::NotADirectiveStateRewrite:
      return "NotADirectiveStateRewrite";
    case DirectiveClosureStatus::DirectiveOwnerInsideAcceptedClosure:
      return "DirectiveOwnerInsideAcceptedClosure";
    case DirectiveClosureStatus::DirectiveOwnerOutsideAcceptedClosure:
      return "DirectiveOwnerOutsideAcceptedClosure";
    case DirectiveClosureStatus::Unknown:
      return "Unknown";
    }
    llvm_unreachable("Invalid directive closure status");
  }

  /// Typed witness that the preserved suffix does not observe the changed state.
  struct SuffixUnobservedWitness {
    OwnerStateComponent component = OwnerStateComponent::Unknown;
    OwnerStateBoundary boundary;
    std::string detail;
  };

  /// Typed witness that equivalent state is repaired before the first observer.
  struct StateRepairWitness {
    OwnerStateComponent component = OwnerStateComponent::Unknown;
    OwnerStateBoundary boundary;
    std::optional<SuffixObserverResult> firstObserver = std::nullopt;
    std::string detail;
  };

  /// Typed witness that a suffix observer is materialized and no longer observes
  /// the changed preprocessor state through preserved source spelling.
  struct OwnerMaterializationWitness {
    OwnerStateComponent component = OwnerStateComponent::Unknown;
    OwnerStateBoundary boundary;
    std::optional<SuffixObserverResult> materializedObserver = std::nullopt;
    std::string detail;
  };

  /// Typed witness that the edit closure was widened to include every relevant
  /// suffix observer before it could observe the changed state.
  struct ClosureWideningWitness {
    OwnerStateComponent component = OwnerStateComponent::Unknown;
    OwnerStateBoundary boundary;
    std::optional<SuffixObserverResult> widenedThroughObserver = std::nullopt;
    std::string detail;
  };

  /// Typed witness that a stateful builtin event was replaced by a literal whose
  /// value was derived from the assigned B-side envelope.
  struct LiteralizationWitness {
    OwnerStateComponent component = OwnerStateComponent::Unknown;
    OwnerStateBoundary boundary;
    std::optional<SuffixObserverResult> literalizedObserver = std::nullopt;
    std::string detail;
  };

  /// Typed witness for the fail-closed terminal state-stability case.
  struct TerminalStateFailureWitness {
    OwnerStateComponent component = OwnerStateComponent::Unknown;
    OwnerStateBoundary boundary;
    TerminalFallbackProofFailure failure;
    bool hasFailure = false;
    std::string detail;
  };

  /// Compact tagged wrapper for Phase-5 suffix-stability witnesses.
  ///
  /// The individual witness structs above are the theorem-facing payloads.  This
  /// wrapper exists only to pass one typed witness through the existing C++17
  /// code without introducing a second hierarchy or duplicated switch logic.
  enum class SuffixStabilityWitnessKind : uint8_t {
    None,
    SuffixUnobserved,
    StateRepair,
    OwnerMaterialization,
    ClosureWidening,
    Literalization,
    TerminalStateFailure
  };

  friend inline StringRef toString(SuffixStabilityWitnessKind kind) {
    switch (kind) {
    case SuffixStabilityWitnessKind::None:
      return "None";
    case SuffixStabilityWitnessKind::SuffixUnobserved:
      return "SuffixUnobserved";
    case SuffixStabilityWitnessKind::StateRepair:
      return "StateRepair";
    case SuffixStabilityWitnessKind::OwnerMaterialization:
      return "OwnerMaterialization";
    case SuffixStabilityWitnessKind::ClosureWidening:
      return "ClosureWidening";
    case SuffixStabilityWitnessKind::Literalization:
      return "Literalization";
    case SuffixStabilityWitnessKind::TerminalStateFailure:
      return "TerminalStateFailure";
    }
    llvm_unreachable("Invalid suffix-stability witness kind");
  }

  struct SuffixStabilityWitness {
    SuffixStabilityWitnessKind kind = SuffixStabilityWitnessKind::None;
    std::optional<SuffixUnobservedWitness> suffixUnobserved = std::nullopt;
    std::optional<StateRepairWitness> stateRepair = std::nullopt;
    std::optional<OwnerMaterializationWitness> ownerMaterialization =
        std::nullopt;
    std::optional<ClosureWideningWitness> closureWidening = std::nullopt;
    std::optional<LiteralizationWitness> literalization = std::nullopt;
    std::optional<TerminalStateFailureWitness> terminalFailure = std::nullopt;

    static SuffixStabilityWitness None() { return SuffixStabilityWitness(); }

    static SuffixStabilityWitness From(SuffixUnobservedWitness witness) {
      SuffixStabilityWitness out;
      out.kind = SuffixStabilityWitnessKind::SuffixUnobserved;
      out.suffixUnobserved = std::move(witness);
      return out;
    }

    static SuffixStabilityWitness From(StateRepairWitness witness) {
      SuffixStabilityWitness out;
      out.kind = SuffixStabilityWitnessKind::StateRepair;
      out.stateRepair = std::move(witness);
      return out;
    }

    static SuffixStabilityWitness From(OwnerMaterializationWitness witness) {
      SuffixStabilityWitness out;
      out.kind = SuffixStabilityWitnessKind::OwnerMaterialization;
      out.ownerMaterialization = std::move(witness);
      return out;
    }

    static SuffixStabilityWitness From(ClosureWideningWitness witness) {
      SuffixStabilityWitness out;
      out.kind = SuffixStabilityWitnessKind::ClosureWidening;
      out.closureWidening = std::move(witness);
      return out;
    }

    static SuffixStabilityWitness From(LiteralizationWitness witness) {
      SuffixStabilityWitness out;
      out.kind = SuffixStabilityWitnessKind::Literalization;
      out.literalization = std::move(witness);
      return out;
    }

    static SuffixStabilityWitness From(TerminalStateFailureWitness witness) {
      SuffixStabilityWitness out;
      out.kind = SuffixStabilityWitnessKind::TerminalStateFailure;
      out.terminalFailure = std::move(witness);
      return out;
    }
  };

  /// Canonical theorem-facing result produced by the Phase-5 state gateway.
  ///
  /// The gateway now returns this proof directly.  Phase 5C removes the old
  /// diagnostic scalar result so callers cannot approve state transitions by
  /// reading component-local booleans beside the typed theorem witness.  The
  /// carrier records the before/after
  /// state deltas supplied by the caller, the typed suffix witnesses accepted by
  /// the gateway, the closure-widening subset needed by theorem consumers, and
  /// the classified terminal failure when the transition is outside the strict
  /// domain.  Phase 5C can therefore delete component-local approvals by moving
  /// them onto this proof instead of introducing another parallel state flag.
  struct StateTransitionProof {
    OwnerStateDelta before;
    OwnerStateDelta after;
    std::vector<SuffixStabilityWitness> suffixWitnesses;
    std::vector<ClosureWideningWitness> wideningWitnesses;
    std::optional<TerminalFallbackProofFailure> failure = std::nullopt;
  };

  /// Request passed through the single Phase-5 state-transition gateway.
  struct StateTransitionGatewayRequest {
    OwnerStateBoundary boundary;
    OwnerStateComponent component = OwnerStateComponent::Unknown;
    StateMutationKind mutation = StateMutationKind::Unknown;
    SuffixStabilityWitness witness;
    OwnerStateDelta before;
    OwnerStateDelta after;
    std::string phase;
    std::string detail;
    bool requireKnownObserver = false;

    /// Phase 5K reverse-solving gate.  When true, the request represents a state
    /// rewrite whose source directive would have to be inferred from downstream
    /// B-side expansion unless the directive owner is already inside the accepted
    /// edit closure.  The gateway rejects Outside/Unknown before consulting suffix
    /// observers, because suffix repair cannot justify changing unedited upstream
    /// source state.
    bool requiresDirectiveClosureProof = false;
    DirectiveClosureStatus directiveClosureStatus =
        DirectiveClosureStatus::NotADirectiveStateRewrite;
    std::string directiveKind;
  };

  /// Return the theorem-facing state components mutated by `delta`.
  ///
  /// R3 removes the OwnerStateSummary carrier from theorem paths.  Callers that
  /// consume or move an entire owner now enumerate mutated components directly
  /// from the canonical OwnerStateDelta.
  static std::vector<OwnerStateComponent>
  StateComponentsMutatedByDelta(const OwnerStateDelta &delta);

  /// Return the theorem state component made uncertain by one component-specific
  /// missing producer fact.  Phase 3L uses this to convert missing-fact markers
  /// into precise suffix-stability obligations instead of one global unmodeled
  /// state surface.
  static OwnerStateComponent
  StateComponentForMissingStateFact(MissingStateFactKind kind);

  /// Return the terminal fallback proof failure for a component-specific missing
  /// producer fact.  The caller supplies the detail recorded when the owner
  /// summary was built so diagnostics identify the missing fact, not merely the
  /// fallback algorithm that noticed it.
  static TerminalFallbackProofFailure
  MissingStateFactTerminalFailure(MissingStateFactKind kind, StringRef detail);

  /// Return the terminal fallback proof failure for an undischargeable suffix
  /// observer of `component`.
  static TerminalFallbackProofFailure
  SuffixStabilityTerminalFailureForComponent(OwnerStateComponent component);

  /// Return the Phase-5K terminal fallback proof failure for a forbidden
  /// reverse-solved directive rewrite.
  static TerminalFallbackProofFailure ReverseSolvedDirectiveTerminalFailure(
      OwnerStateComponent component, const OwnerStateBoundary &boundary,
      llvm::StringRef directiveKind, llvm::StringRef detail);

  /// Build a typed terminal witness for a forbidden reverse-solved directive
  /// rewrite.  Component-specific helpers may wrap this, but the proof failure
  /// kind is intentionally shared by macro, line-control, include, and
  /// conditional state.
  static SuffixStabilityWitness BuildReverseSolvedDirectiveTerminalWitness(
      const OwnerStateBoundary &boundary, OwnerStateComponent component,
      llvm::StringRef directiveKind, llvm::StringRef detail);

  /// Route an explicit reverse-solving decision through the same Phase-5 state
  /// gateway used for suffix stability.  The request is rejected unless the
  /// caller proves that the directive owner itself is inside the accepted edit
  /// closure.
  StateTransitionProof CheckReverseSolvedDirectiveAcrossEditBoundary(
      const OwnerStateBoundary &boundary, OwnerStateComponent component,
      StateMutationKind mutation, DirectiveClosureStatus directiveClosureStatus,
      llvm::StringRef directiveKind, llvm::StringRef phase,
      llvm::StringRef detail) const;

  /// Return the state component explicitly named by a typed suffix-stability
  /// witness, or Unknown when the witness is absent/malformed.
  static OwnerStateComponent
  ComponentNamedBySuffixStabilityWitness(const SuffixStabilityWitness &witness);

  /// Validate that a typed suffix-stability witness names the exact state
  /// component that the gateway request is checking.  This is the Phase-5L
  /// guard against treating a generic enum label as a proof.
  static bool SuffixStabilityWitnessNamesComponent(
      const SuffixStabilityWitness &witness, OwnerStateComponent component);

  /// Check one state transition through the uniform Phase-5 gateway.
  ///
  /// This is the only decision tree that is allowed to decide whether a changed
  /// state component can cross into a preserved suffix.  The scalar overload is
  /// the canonical call-site form for Phase 5C: callers name the component and
  /// typed witness directly instead of routing through component-specific
  /// "macro was okay" / "counter was okay" approval wrappers.
  StateTransitionProof CheckStateTransitionAcrossEditBoundary(
      const StateTransitionGatewayRequest &request) const;
  StateTransitionProof CheckStateTransitionAcrossEditBoundary(
      const OwnerStateBoundary &boundary, OwnerStateComponent component,
      StateMutationKind mutation, SuffixStabilityWitness witness,
      llvm::StringRef phase, llvm::StringRef detail,
      bool requireKnownObserver) const;

  /// Build one typed suffix-stability witness for the uniform state gateway.
  ///
  /// Phase 5C deliberately removes the old component-specific witness factories:
  /// the theorem fact is the component named in the witness, not which helper
  /// happened to create it.  Terminal witnesses use
  /// SuffixStabilityTerminalFailureForComponent(component), keeping the
  /// component-specific failed obligation but eliminating side-approval APIs.
  static SuffixStabilityWitness BuildStateTransitionWitness(
      SuffixStabilityWitnessKind kind, OwnerStateComponent component,
      const OwnerStateBoundary &boundary, llvm::StringRef detail);

  /// Return the source/token boundary for one producer-backed counter event.
  static OwnerStateBoundary
  CounterStateBoundaryForEvent(const CounterEventIdentity &event);

  /// Format one counter event for typed witness diagnostics.
  static std::string
  FormatCounterEventForWitness(const CounterEventIdentity &event);


  /// Build the source/token boundary for one include directive.
  static OwnerStateBoundary
  IncludeStateBoundaryForIncludeSite(const RefoldModel::IncludeItem &include);


  /// Return the coarse observer kind for a state component.
  static SuffixObservationKind
  ObservationKindForComponent(OwnerStateComponent component);

  /// Return the component-specific observer-site index for `component`.
  static ArrayRef<uint64_t> ObserverSiteIndexesForComponent(
      const OwnerStateGraph &graph, OwnerStateComponent component);

  /// True iff an observer summary contains `component`.
  static bool OwnerObserverSummaryObservesComponent(
      const OwnerObserverSummary &summary, OwnerStateComponent component);

  /// True iff a source-site record belongs to `owner`.
  ///
  /// `ownerIncludeId` is the producer's current include-instance owner for the
  /// source bytes.  The optional conditional arm on `owner` is treated as an
  /// additional restriction: when present, the site must lie inside that arm.
  bool OwnerMatchesSourceSite(const Owner &owner, StringRef file,
                              std::optional<uint64_t> ownerIncludeId,
                              uint64_t begin, uint64_t end) const;

  /// True iff a source byte interval lies wholly inside a producer-recorded
  /// conditional arm.  Failure to find the arm is treated as not proven; summary
  /// construction then records unmodeled state instead of accepting the site.
  bool SourceRangeInsideConditionalArm(uint64_t condArmId, StringRef file,
                                       std::optional<uint64_t> ownerIncludeId,
                                       uint64_t begin, uint64_t end) const;

  /// \brief Final theorem-facing proof vocabulary for accepted results.
  ///
  /// Phase 1A freezes this enum as the public proof calculus vocabulary used by
  /// comments, theorem-audit logs, and strict-domain tests.  These values answer
  /// "what theorem proof discharges this accepted edit?"; they never describe
  /// which builder happened to construct the candidate.  Implementation-local
  /// classes and paths may continue to exist below, but every emitted candidate
  /// must normalize into exactly one of these theorem classes before it can be
  /// selected or attached to a byte edit.
  #define REFOLD_THEOREM_PROOF_CLASS_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(IdentityPreservingProof) \
  REFOLD_X(InvocationPreservingProof) \
  REFOLD_X(DirectivePreservingProof) \
  REFOLD_X(StateRepairProof) \
  REFOLD_X(OwnerRealizationProof) \
  REFOLD_X(MixedOwnerTilingProof) \
  REFOLD_X(SuffixStabilizationProof) \
  REFOLD_X(TerminalOutOfDomainProof)

  enum class TheoremProofClass : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_THEOREM_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(TheoremProofClass value) {
    switch (value) {
#define REFOLD_X(name) case TheoremProofClass::name: return #name;
      REFOLD_THEOREM_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_THEOREM_PROOF_CLASS_LIST

  /// \brief Implementation-local proof family recorded while candidates are built.
  ///
  /// These values are intentionally not the final theorem vocabulary.  They are
  /// retained as compact construction metadata so existing builders do not need
  /// to be renamed en masse, but Phase 1C requires selection and emission to use
  /// the summary-owned theorem class before treating a candidate as discharged.
  #define REFOLD_ACCEPTED_PROOF_CLASS_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(InvocationPreserving) \
  REFOLD_X(InvocationRealization) \
  REFOLD_X(IncludePreserving) \
  REFOLD_X(IncludeRealization) \
  REFOLD_X(TUAnchor) \
  REFOLD_X(TUTextualEdit) \
  REFOLD_X(TerminalOutOfDomain)

  enum class AcceptedProofClass : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_ACCEPTED_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(AcceptedProofClass value) {
    switch (value) {
#define REFOLD_X(name) case AcceptedProofClass::name: return #name;
      REFOLD_ACCEPTED_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_ACCEPTED_PROOF_CLASS_LIST

  /// \brief Whether an accepted result preserves original structure or emits
  /// a realized edited surface.
  #define REFOLD_REALIZATION_MODE_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(PreserveOriginalStructure) \
  REFOLD_X(RealizeEditedSurface)

  enum class RealizationMode : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_REALIZATION_MODE_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(RealizationMode value) {
    switch (value) {
#define REFOLD_X(name) case RealizationMode::name: return #name;
      REFOLD_REALIZATION_MODE_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_REALIZATION_MODE_LIST

  /// \brief Ranking bucket for choosing among multiple valid candidates.
  ///
  /// Preference is tracked separately from proof validity so ordering policy
  /// remains explicit rather than being hidden in construction order.
  #define REFOLD_SELECTION_PREFERENCE_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(PreferStructurePreservation) \
  REFOLD_X(PreferSurfaceRealization) \
  REFOLD_X(PreferExactAnchoring)

  enum class SelectionPreference : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_SELECTION_PREFERENCE_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(SelectionPreference value) {
    switch (value) {
#define REFOLD_X(name) case SelectionPreference::name: return #name;
      REFOLD_SELECTION_PREFERENCE_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_SELECTION_PREFERENCE_LIST

  /// \brief Surface realization choices kept distinct from proof metadata.
  #define REFOLD_SURFACE_DISPOSITION_LIST(REFOLD_X) \
  REFOLD_X(None) \
  REFOLD_X(RealizeWholeCoverMacros) \
  REFOLD_X(RealizeInlineTouchedIncludesFromB) \
  REFOLD_X(RealizeMaterializedIncludeExpansion) \
  REFOLD_X(RealizeTranslationUnitByteEdit) \
  REFOLD_X(EmitEditedPreprocessedStream)

  enum class SurfaceDisposition : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_SURFACE_DISPOSITION_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(SurfaceDisposition value) {
    switch (value) {
#define REFOLD_X(name) case SurfaceDisposition::name: return #name;
      REFOLD_SURFACE_DISPOSITION_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "None";
  }
#undef REFOLD_SURFACE_DISPOSITION_LIST

  /// \brief Named theorem-lattice tie-breakers for otherwise local choices.
  ///
  /// Phase 3C keeps accepted-result ordering out of path-specific code.  A
  /// builder may attach one of these names only when it has already proved the
  /// corresponding witness preconditions; the shared lattice selector then owns
  /// the actual preference decision.  Unknown means no special tie-breaker.
  #define REFOLD_THEOREM_SELECTION_TIE_BREAKER_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(ExactTUArgumentEditOverEquivalentMacroArgsOnly)

  enum class TheoremSelectionTieBreakerKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_THEOREM_SELECTION_TIE_BREAKER_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(TheoremSelectionTieBreakerKind value) {
    switch (value) {
#define REFOLD_X(name) case TheoremSelectionTieBreakerKind::name: return #name;
      REFOLD_THEOREM_SELECTION_TIE_BREAKER_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_THEOREM_SELECTION_TIE_BREAKER_LIST

  /// \brief Implementation path that produced an accepted theorem carrier.
  ///
  /// This inventory is intentionally subordinate to TheoremProofClass.  It may
  /// say where a result came from, but the emitted result is justified only by
  /// its normalized theorem proof class and discharged obligations.  New paths should be
  /// added here only when they also map onto the strict-domain theorem
  /// vocabulary.
  #define REFOLD_ACCEPTED_PATH_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(MacroArgsOnlyStandard) \
  REFOLD_X(MacroArgsOnlyPasteSingle) \
  REFOLD_X(MacroArgsOnlyPasteMulti) \
  REFOLD_X(MacroArgsOnlyPurePasteOnly) \
  REFOLD_X(MacroArgsOnlyPairedPureInsertion) \
  REFOLD_X(MacroPasteDerivedCalleeSelector) \
  REFOLD_X(MacroDagSubtreeRoot) \
  REFOLD_X(MacroCallChainSuffix) \
  REFOLD_X(MacroCounterLiteral) \
  REFOLD_X(MacroWholeCoverRealization) \
  REFOLD_X(IncludePatchPendingMaterialization) \
  REFOLD_X(IncludeDeleteReplaceMappedHeaderTokens) \
  REFOLD_X(IncludeInsertSelectedConditionalBoundary) \
  REFOLD_X(IncludeInsertChildBoundary) \
  REFOLD_X(IncludeInsertRightNeighborPP) \
  REFOLD_X(IncludeInsertLeftNeighborPP) \
  REFOLD_X(IncludeInsertDeclBoundary) \
  REFOLD_X(IncludeRealizationInlineFromB) \
  REFOLD_X(IncludeMaterializedExpansion) \
  REFOLD_X(TUExactSlotBoundary) \
  REFOLD_X(TUProvableInsertionAnchor) \
  REFOLD_X(TUByteSpanMappedEdit) \
  REFOLD_X(TUByteSpanConservativeEdit) \
  REFOLD_X(TUIncludeClosureEdit) \
  REFOLD_X(TerminalEmitEditedPreprocessedStream)

  enum class AcceptedPathKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_ACCEPTED_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(AcceptedPathKind value) {
    switch (value) {
#define REFOLD_X(name) case AcceptedPathKind::name: return #name;
      REFOLD_ACCEPTED_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_ACCEPTED_PATH_KIND_LIST

  /// \brief Proof-lattice class assigned to an expansion-fallback branch.
  ///
  /// This enum is the Phase-4A inventory for the fallback translation unit.
  /// It classifies what an expansion-fallback branch is trying to emit before
  /// that branch is allowed to build an accepted-result carrier.  The names are
  /// intentionally theorem-facing, not implementation anecdotes: a fallback
  /// branch may survive only as an in-domain proof class or as the explicit
  /// terminal out-of-domain carrier.
#define REFOLD_EXPANSION_FALLBACK_BRANCH_PROOF_CLASS_LIST(REFOLD_X)           \
  REFOLD_X(Unknown)                                                           \
  REFOLD_X(OwnerRealizationProof)                                             \
  REFOLD_X(MixedOwnerTilingProof)                                             \
  REFOLD_X(DirectivePreservingProof)                                          \
  REFOLD_X(TUTextualEditProof)                                                \
  REFOLD_X(TerminalOutOfDomainProof)

  enum class ExpansionFallbackBranchProofClass : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_EXPANSION_FALLBACK_BRANCH_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(ExpansionFallbackBranchProofClass value) {
    switch (value) {
#define REFOLD_X(name) case ExpansionFallbackBranchProofClass::name: return #name;
      REFOLD_EXPANSION_FALLBACK_BRANCH_PROOF_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_EXPANSION_FALLBACK_BRANCH_PROOF_CLASS_LIST

  /// \brief Closed inventory of expansion-fallback branches that can emit.
  ///
  /// Phase 4A deliberately enumerates emitting fallback branches separately
  /// from their internal rejection checks.  Rejections do not emit; the emitted
  /// surfaces that remain in this file are the TU/include source-closure edit
  /// and the declared raw-B terminal carrier.  Adding another emitting branch
  /// requires adding it here and assigning exactly one proof class below.
#define REFOLD_EXPANSION_FALLBACK_BRANCH_KIND_LIST(REFOLD_X)                  \
  REFOLD_X(Unknown)                                                           \
  REFOLD_X(TUIncludeClosureEdit)                                              \
  REFOLD_X(PostStructuralTerminalOutOfDomain)

  enum class ExpansionFallbackBranchKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_EXPANSION_FALLBACK_BRANCH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(ExpansionFallbackBranchKind value) {
    switch (value) {
#define REFOLD_X(name) case ExpansionFallbackBranchKind::name: return #name;
      REFOLD_EXPANSION_FALLBACK_BRANCH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_EXPANSION_FALLBACK_BRANCH_KIND_LIST

  /// \brief Canonical Phase-4A classification for one fallback branch.
  ///
  /// `branchProofClass` records the fallback-specific proof inventory requested
  /// by Phase 4A.  `theoremClass` records the current normalized theorem class
  /// used by ProofSummary / EmittedProof.  They intentionally differ for
  /// TUTextualEditProof because the current theorem lattice represents TU text
  /// realizations with the generic OwnerRealizationProof carrier plus an owner
  /// realization witness; later phases may split that theorem class without
  /// changing the branch inventory.
  struct ExpansionFallbackBranchClassification {
    ExpansionFallbackBranchKind branch = ExpansionFallbackBranchKind::Unknown;
    ExpansionFallbackBranchProofClass branchProofClass =
        ExpansionFallbackBranchProofClass::Unknown;
    TheoremProofClass theoremClass = TheoremProofClass::Unknown;
    AcceptedPathKind acceptedPath = AcceptedPathKind::Unknown;

    bool IsClassified() const {
      return branch != ExpansionFallbackBranchKind::Unknown &&
             branchProofClass != ExpansionFallbackBranchProofClass::Unknown &&
             theoremClass != TheoremProofClass::Unknown &&
             acceptedPath != AcceptedPathKind::Unknown;
    }
  };

  static ExpansionFallbackBranchClassification
  ClassifyExpansionFallbackBranch(ExpansionFallbackBranchKind branch) {
    switch (branch) {
    case ExpansionFallbackBranchKind::TUIncludeClosureEdit:
      return {branch, ExpansionFallbackBranchProofClass::TUTextualEditProof,
              TheoremProofClass::OwnerRealizationProof,
              AcceptedPathKind::TUIncludeClosureEdit};
    case ExpansionFallbackBranchKind::PostStructuralTerminalOutOfDomain:
      return {branch,
              ExpansionFallbackBranchProofClass::TerminalOutOfDomainProof,
              TheoremProofClass::TerminalOutOfDomainProof,
              AcceptedPathKind::TerminalEmitEditedPreprocessedStream};
    case ExpansionFallbackBranchKind::Unknown:
      break;
    }
    return {};
  }

  /// \brief How mature the current acceptance path is in the migration plan.
  #define REFOLD_ACCEPTANCE_SUPPORT_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(ExplicitProofBacked) \
  REFOLD_X(DeterministicButNotFirstClass) \
  REFOLD_X(ExplicitOutOfDomainClass)

  enum class AcceptanceSupportKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_ACCEPTANCE_SUPPORT_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(AcceptanceSupportKind value) {
    switch (value) {
#define REFOLD_X(name) case AcceptanceSupportKind::name: return #name;
      REFOLD_ACCEPTANCE_SUPPORT_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_ACCEPTANCE_SUPPORT_KIND_LIST

  /// \brief Future proof-class placeholder targeted by a current path.
  #define REFOLD_FUTURE_PROOF_TARGET_LIST(REFOLD_X) \
  REFOLD_X(Unknown, "Unknown") \
  REFOLD_X(MacroStandardArgsOnly, "MacroStandardArgsOnly") \
  REFOLD_X(MacroPasteSingle, "MacroPasteSingle") \
  REFOLD_X(MacroPasteMultiFixedAnchor, "MacroPasteMultiFixedAnchor") \
  REFOLD_X(MacroPurePasteOnly, "MacroPurePasteOnly") \
  REFOLD_X(MacroPairedPureInsertion, "MacroPairedPureInsertion") \
  REFOLD_X(MacroPasteDerivedCalleeSelector, "MacroPasteDerivedCalleeSelector") \
  REFOLD_X(MacroDagLift, "MacroDagLift") \
  REFOLD_X(MacroCallChainSuffixPreservation, "MacroCallChainSuffixPreservation") \
  REFOLD_X(MacroCounterStabilizationRealization, "MacroCounterStabilizationRealization") \
  REFOLD_X(MacroRealizationWholeCover, "MacroRealizationWholeCover") \
  REFOLD_X(IncludePatchByMappedHeaderTokens, "IncludePatchByMappedHeaderTokens") \
  REFOLD_X(IncludeConditionalArmCertifiedInsertion, "IncludeConditionalArmCertifiedInsertion") \
  REFOLD_X(IncludeInsertionByChildBoundary, "IncludeInsertionByChildBoundary") \
  REFOLD_X(IncludeInsertionByRightNeighborPP, "IncludeInsertionByRightNeighborPP") \
  REFOLD_X(IncludeInsertionByLeftNeighborPP, "IncludeInsertionByLeftNeighborPP") \
  REFOLD_X(IncludeInsertionByDeclBoundary, "IncludeInsertionByDeclBoundary") \
  REFOLD_X(IncludeRealizationCover, "IncludeRealizationCover") \
  REFOLD_X(IncludeMaterializedExpansionRealization, "IncludeMaterializedExpansionRealization") \
  REFOLD_X(TUExactSlotAnchor, "TUExactSlotAnchor") \
  REFOLD_X(TUProvableInsertionAnchor, "TUProvableInsertionAnchor") \
  REFOLD_X(TUByteSpanTextualEdit, "TUByteSpanTextualEdit") \
  REFOLD_X(TUIncludeClosureEdit, "TUIncludeClosureEdit") \
  REFOLD_X(EditedPreprocessedStreamFallback, "ExplicitOutOfDomainTerminalResult")

  enum class FutureProofTarget : uint8_t {
#define REFOLD_X(name, text) name,
    REFOLD_FUTURE_PROOF_TARGET_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(FutureProofTarget value) {
    switch (value) {
#define REFOLD_X(name, text) case FutureProofTarget::name: return text;
      REFOLD_FUTURE_PROOF_TARGET_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_FUTURE_PROOF_TARGET_LIST

  /// \brief Inventory record that maps a current acceptance path onto the proof
  /// lattice.
  struct AcceptancePathInventory {
    AcceptedPathKind currentPath = AcceptedPathKind::Unknown;
    AcceptanceSupportKind support = AcceptanceSupportKind::Unknown;
    FutureProofTarget futureTarget = FutureProofTarget::Unknown;
  };

  /// \brief Conflict domain used by the global accepted-result lattice.
  ///
  /// This does not change how candidates are chosen. It names the owner
  /// domain in which two accepted artifacts may interact so the current global
  /// selection and overlap rules can be described explicitly and audited in one
  /// place.
  #define REFOLD_LATTICE_CONFLICT_DOMAIN_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(MacroInvocationRootSpan) \
  REFOLD_X(IncludeOwnerRegion) \
  REFOLD_X(TUAnchorPoint) \
  REFOLD_X(WholeTranslationUnit)

  enum class LatticeConflictDomain : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_LATTICE_CONFLICT_DOMAIN_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(LatticeConflictDomain value) {
    switch (value) {
#define REFOLD_X(name) case LatticeConflictDomain::name: return #name;
      REFOLD_LATTICE_CONFLICT_DOMAIN_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_LATTICE_CONFLICT_DOMAIN_LIST

  /// \brief Merge law used when two artifacts in the same lattice domain are
  /// compatible.
  #define REFOLD_LATTICE_MERGE_LAW_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(DisjointCompose) \
  REFOLD_X(NestedOuterShadowsInner) \
  REFOLD_X(SelectSingleWitness) \
  REFOLD_X(TerminalReplacesAll)

  enum class LatticeMergeLaw : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_LATTICE_MERGE_LAW_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(LatticeMergeLaw value) {
    switch (value) {
#define REFOLD_X(name) case LatticeMergeLaw::name: return #name;
      REFOLD_LATTICE_MERGE_LAW_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_LATTICE_MERGE_LAW_LIST

  /// \brief Conflict law used when two artifacts in the same lattice domain
  /// are not simultaneously admissible.
  #define REFOLD_LATTICE_CONFLICT_LAW_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(RejectPartialOverlap) \
  REFOLD_X(PreferStructurePreservation) \
  REFOLD_X(PreferExactAnchorWitness) \
  REFOLD_X(PreferOwnerPreservingBeforeRealization) \
  REFOLD_X(ExplicitOutOfDomainTerminalResult)

  enum class LatticeConflictLaw : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_LATTICE_CONFLICT_LAW_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(LatticeConflictLaw value) {
    switch (value) {
#define REFOLD_X(name) case LatticeConflictLaw::name: return #name;
      REFOLD_LATTICE_CONFLICT_LAW_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_LATTICE_CONFLICT_LAW_LIST

  /// \brief Normalized description of the current global lattice law.
  struct GlobalSelectionLattice {
    LatticeConflictDomain domain = LatticeConflictDomain::Unknown;
    LatticeMergeLaw mergeLaw = LatticeMergeLaw::Unknown;
    LatticeConflictLaw conflictLaw = LatticeConflictLaw::Unknown;
  };

  /// \brief Whether an accepted path currently participates in the declared
  /// completeness set.
  ///
  /// This does not claim the engine is globally complete yet. Instead it makes
  /// the scope of the completeness claim explicit: accepted paths either
  /// already correspond to a declared proof class, remain transitional while a
  /// class is still being closed, or sit outside the declared class set
  /// entirely (for example an explicit terminal out-of-domain result).
  #define REFOLD_COMPLETENESS_COVERAGE_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(DeclaredProofClass) \
  REFOLD_X(TransitionalGap) \
  REFOLD_X(ExplicitOutOfDomainClass)

  enum class CompletenessCoverageKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_COMPLETENESS_COVERAGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(CompletenessCoverageKind value) {
    switch (value) {
#define REFOLD_X(name) case CompletenessCoverageKind::name: return #name;
      REFOLD_COMPLETENESS_COVERAGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_COMPLETENESS_COVERAGE_KIND_LIST

  /// \brief What completeness promise the engine makes for a covered path.
  #define REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(MustDiscoverDeclaredOrStrongerCompatible) \
  REFOLD_X(NoClaimPendingClassClosure) \
  REFOLD_X(ExplicitlyOutsideDeclaredSet)

  enum class CompletenessExpectationKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(CompletenessExpectationKind value) {
    switch (value) {
#define REFOLD_X(name) case CompletenessExpectationKind::name: return #name;
      REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST

  /// \brief Normalized completeness contract for the declared domain above.
  ///
  /// This contract answers only one question: whether a theorem-facing carrier
  /// already lies in the declared proof-class set, remains an internal-only
  /// transitional gap that must not reach emission, or is explicitly outside
  /// the declared set as a named terminal boundary.
  struct CompletenessContract {
    CompletenessCoverageKind coverage = CompletenessCoverageKind::Unknown;
    CompletenessExpectationKind expectation =
        CompletenessExpectationKind::Unknown;
    FutureProofTarget declaredTarget = FutureProofTarget::Unknown;
    bool countsTowardDeclaredCoverage = false;
    bool hasExplicitExclusion = false;
    TheoremFallbackFailureKind explicitExclusion =
        TheoremFallbackFailureKind::Unknown;
  };

  /// \brief The summary's position relative to the declared theorem domain.
  ///
  /// This enum is the theorem-domain projection of the same declared-domain
  /// statement: theorem-facing results are either in-domain declared proof
  /// classes or explicit named out-of-domain classes. Transitional states may
  /// still exist internally, but they are not allowed to survive to emission.
  #define REFOLD_THEOREM_DOMAIN_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(DeclaredInDomainClass) \
  REFOLD_X(TransitionalGap) \
  REFOLD_X(ExplicitOutOfDomainClass)

  enum class TheoremDomainKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_THEOREM_DOMAIN_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(TheoremDomainKind value) {
    switch (value) {
#define REFOLD_X(name) case TheoremDomainKind::name: return #name;
      REFOLD_THEOREM_DOMAIN_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_THEOREM_DOMAIN_KIND_LIST

  /// \brief Explicit theorem-domain contract derived from the same statement.
  ///
  /// The theorem-domain view must say the same thing as the completeness view:
  /// theorem-facing carriers are either in-domain declared proof classes or
  /// explicit out-of-domain classes. Transitional states may still exist
  /// internally, but they must remain non-emitting staging objects.
  struct TheoremDomainContract {
    TheoremDomainKind kind = TheoremDomainKind::Unknown;
    bool inDeclaredDomain = false;
    bool countsTowardCompleteness = false;
    FutureProofTarget declaredTarget = FutureProofTarget::Unknown;
    bool hasExplicitExclusion = false;
    TheoremFallbackFailureKind explicitExclusion =
        TheoremFallbackFailureKind::Unknown;
  };

  /// \brief Evidence source used to justify an accepted TU anchor.
  ///
  /// Deterministic TU anchoring rules are represented as explicit proof
  /// witnesses so accepted TU-owned insertions can explain which anchor source
  /// was used and which non-crossing facts were relied upon.
  #define REFOLD_TUANCHOR_EVIDENCE_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(ExactSlotBoundary) \
  REFOLD_X(ArgLikeBegin) \
  REFOLD_X(ImmediateRightNeighbor) \
  REFOLD_X(ImmediateLeftNeighbor) \
  REFOLD_X(IncludeDirectiveBoundary) \
  REFOLD_X(ZeroTokenIncludeBoundary) \
  REFOLD_X(CorroboratedRightNeighbor) \
  REFOLD_X(CorroboratedLeftNeighbor)

  enum class TUAnchorEvidenceKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_TUANCHOR_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(TUAnchorEvidenceKind value) {
    switch (value) {
#define REFOLD_X(name) case TUAnchorEvidenceKind::name: return #name;
      REFOLD_TUANCHOR_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_TUANCHOR_EVIDENCE_KIND_LIST

  /// \brief Compact witness for an accepted TU anchor.
  struct TUAnchorWitness {
    TUAnchorEvidenceKind evidence = TUAnchorEvidenceKind::Unknown;
    bool hasPPGap = false;
    uint64_t ppGap = 0;
    bool hasTUByte = false;
    uint64_t tuByte = 0;

    // Exact structural slot anchor metadata.
    bool exactPPMatch = false;
    uint64_t slotId = 0;
    std::string slotKind;

    // Arg-like begin anchor metadata.
    uint64_t macroId = 0;

    // Neighbor-based anchor metadata.
    bool hasLeftNeighbor = false;
    uint64_t leftNeighborPP = 0;
    bool hasRightNeighbor = false;
    uint64_t rightNeighborPP = 0;

    // Ownership/non-crossing metadata for provable TU insertion anchors.
    bool outsideIncludeCoverage = false;
    bool ownerDepthStable = false;
  };

  /// \brief Evidence source used to justify an accepted include-preserving
  /// anchor or mapped include byte range.
  ///
  /// Include-preserving materialization paths use explicit local witnesses so
  /// each accepted include patch can explain which deterministic
  /// anchoring or mapping rule was used.
  #define REFOLD_INCLUDE_ANCHOR_EVIDENCE_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(MappedHeaderTokens) \
  REFOLD_X(SelectedConditionalBoundary) \
  REFOLD_X(ChildBoundary) \
  REFOLD_X(RightNeighborPP) \
  REFOLD_X(LeftNeighborPP) \
  REFOLD_X(DeclBoundary)

  enum class IncludeAnchorEvidenceKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_INCLUDE_ANCHOR_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(IncludeAnchorEvidenceKind value) {
    switch (value) {
#define REFOLD_X(name) case IncludeAnchorEvidenceKind::name: return #name;
      REFOLD_INCLUDE_ANCHOR_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_INCLUDE_ANCHOR_EVIDENCE_KIND_LIST

  /// \brief Compact witness for an accepted include-preserving path.
  struct IncludeAnchorWitness {
    IncludeAnchorEvidenceKind evidence = IncludeAnchorEvidenceKind::Unknown;

    // Concrete byte placement inside the edited header file. Inserts use a
    // single anchor byte; mapped delete/replace paths use an explicit byte
    // range.
    bool hasAnchorByte = false;
    uint64_t anchorByte = 0;
    bool hasByteRange = false;
    uint64_t startByte = 0;
    uint64_t endByte = 0;

    // PP-token provenance for mapped delete/replace paths and neighbor-based
    // insertion anchors.
    bool hasFirstPP = false;
    uint64_t firstPP = 0;
    bool hasLastPP = false;
    uint64_t lastPP = 0;
    bool hasNeighborPP = false;
    uint64_t neighborPP = 0;

    // Conditional, child-include, and declaration boundary metadata.
    bool hasCondArmId = false;
    uint64_t condArmId = 0;
    bool hasChildIncludeId = false;
    uint64_t childIncludeId = 0;
    bool hasDeclHeaderRange = false;
    uint64_t declHeaderB = 0;
    uint64_t declHeaderE = 0;
  };

  /// \brief Evidence source used to justify an accepted include realization.
  ///
  /// The include-realization domain boundary is explicit. An inline include
  /// realization is in-domain only when the include cover admits either the
  /// canonical A-cover -> B-envelope mapping or the Phase-8b
  /// BoundaryStableConsensusBCoverEnvelope proof.  The latter is not a legacy
  /// fallback branch: it is accepted only when all usable non-canonical boundary
  /// projections agree on the same non-empty B-token range.  Any include
  /// realization outside those declared witnesses remains an explicit terminal
  /// out-of-domain case instead of manufacturing a weaker proof class.
  #define REFOLD_INCLUDE_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(CanonicalBCoverEnvelope) \
  REFOLD_X(BoundaryStableConsensusBCoverEnvelope)

  enum class IncludeRealizationEvidenceKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_INCLUDE_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(IncludeRealizationEvidenceKind value) {
    switch (value) {
#define REFOLD_X(name) case IncludeRealizationEvidenceKind::name: return #name;
      REFOLD_INCLUDE_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_INCLUDE_REALIZATION_EVIDENCE_KIND_LIST

  using IncludeRealizationBTokenEnvelope = std::pair<size_t, size_t>;

  /// Owner-polymorphic evidence kind for realized output.
  ///
  /// Macro whole-cover realization, include realization, and direct TU byte
  /// realization still use owner-specific spelling mechanics.  Phase 6 gives
  /// those paths one shared theorem-facing carrier so the proof lattice can
  /// audit all realized output as an OwnerRealizationProof.
#define REFOLD_OWNER_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(MacroWholeCover) \
  REFOLD_X(IncludeBEnvelope) \
  REFOLD_X(IncludeMaterializedExpansion) \
  REFOLD_X(TUByteSpan)

  enum class OwnerRealizationEvidenceKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_OWNER_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(OwnerRealizationEvidenceKind value) {
    switch (value) {
#define REFOLD_X(name) case OwnerRealizationEvidenceKind::name: return #name;
      REFOLD_OWNER_REALIZATION_EVIDENCE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_OWNER_REALIZATION_EVIDENCE_KIND_LIST

  /// Canonical Phase-6 witness for owner realization.
  ///
  /// This does not replace owner-specific emission code.  It records the common
  /// proof facts every realized owner must discharge: owner identity, source
  /// interval, consumed A-token cover, emitted B-token envelope, and the
  /// canonical state summary attached to that owner.  Phase 6b centralizes those
  /// checks in \c TryBuildOwnerRealization(); macro/include/TU callers should
  /// only construct the owner-specific closure and spelling, then delegate the
  /// shared admissibility proof to that helper.
  struct OwnerRealizationWitness {
    OwnerRealizationEvidenceKind evidence =
        OwnerRealizationEvidenceKind::Unknown;
    OwnerClosure closure;

    // Typed Phase-5 state witnesses for the realized owner.  Each witness names
    // the exact state component it discharges, so owner realization no longer
    // stores a coarse legacy enum such as "closure widened" as theorem proof.
    std::vector<SuffixStabilityWitness> stateWitnesses;

    std::string detail;
  };

  /// Result returned by the shared owner-realization proof helper.
  ///
  /// Owner-specific code should keep constructing replacement text itself, but it
  /// should use this result to decide whether the common realization proof was
  /// discharged.  A rejected result names the failed strict-domain obligation
  /// without immediately changing emission control flow; callers can either stop
  /// attaching an OwnerRealizationProof or convert the failure into terminal
  /// fallback at their own proof boundary.
  struct OwnerRealizationResult {
    bool accepted = false;
    OwnerRealizationWitness witness;
    TerminalFallbackProofFailure failure;
    std::string detail;
  };


  /// Edge kind recorded in a persisted mixed-owner tiling witness.
  ///
  /// Token segments are the non-empty A/B envelopes that are emitted as ordinary
  /// normalized hunks.  State gaps are zero-token owners that sit between those
  /// emitted segments; they do not emit bytes by themselves, but they are part
  /// of the proof that the source gap was fully covered and state-composable.
#define REFOLD_MIXED_OWNER_TILING_EDGE_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(TokenSegment) \
  REFOLD_X(StateGap)

  enum class MixedOwnerTilingEdgeKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_MIXED_OWNER_TILING_EDGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(MixedOwnerTilingEdgeKind value) {
    switch (value) {
#define REFOLD_X(name) case MixedOwnerTilingEdgeKind::name: return #name;
      REFOLD_MIXED_OWNER_TILING_EDGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_MIXED_OWNER_TILING_EDGE_KIND_LIST

  /// Durable per-segment proof record for one mixed-owner tiling edge.
  ///
  /// Phase 8A makes the split itself theorem-facing instead of treating the
  /// partition as a transient normalizer detail.  Every emitted token segment
  /// and every zero-token state gap names the parent tiling proof, its stable
  /// segment index, the exact A/B token envelope, and the state-transition proof
  /// supplied by the owner closure for that segment.  State-gap entries have
  /// zero-width A/B envelopes at the adjoining boundary but still carry the
  /// source/state closure that made the gap composable.
  struct MixedOwnerTilingSegmentWitness {
    uint64_t parentTilingWitnessId = 0;
    uint32_t segmentIndex = 0;
    MixedOwnerTilingEdgeKind kind = MixedOwnerTilingEdgeKind::Unknown;
    uint64_t aStart = 0;
    uint64_t aEnd = 0;
    uint64_t bStart = 0;
    uint64_t bEnd = 0;
    bool zeroTokenStateGap = false;
    StateTransitionProof ownerTransitionProof;
  };

  /// Persisted proof for a deterministic mixed-owner tiling.
  ///
  /// Earlier Phase-7 patches used the mixed-owner partition only as a
  /// normalization step.  Phase 7f makes the tiling proof durable: every emitted
  /// token segment can point back to the full ordered proof path, including the
  /// zero-token state-gap edges that never become token hunks themselves.
  struct MixedOwnerTilingWitness {
    uint64_t witnessId = 0;
    uint64_t originalAStart = 0;
    uint64_t originalAEnd = 0;
    uint64_t originalBStart = 0;
    uint64_t originalBEnd = 0;
    uint32_t tokenSegmentCount = 0;
    uint32_t stateGapCount = 0;
    bool stateSummariesComposed = false;
    std::vector<MixedOwnerTilingSegmentWitness> segments;
  };

  /// Reverse index from an emitted token segment back to its mixed-owner tiling.
  ///
  /// The normalizer still emits ordinary token hunks for downstream classifiers.
  /// This binding lets those later accepted candidates recover the full tiling
  /// witness without changing the hunk type or duplicating state-gap edges in
  /// the emitted edit stream.
  struct MixedOwnerTilingSegmentBinding {
    uint64_t aStart = 0;
    uint64_t aEnd = 0;
    uint64_t bStart = 0;
    uint64_t bEnd = 0;
    size_t witnessIndex = 0;
    uint64_t parentTilingWitnessId = 0;
    uint32_t segmentIndex = 0;
  };

  /// \brief Status produced when the engine evaluates a local proof contract.
  ///
  /// The engine records explicit local obligations for every normalized proof
  /// summary. Converted selector sites already use the discharge result as the
  /// participation gate, while the remaining construction paths still mirror
  /// their local facts into the same record so the theorem boundary stays
  /// explicit during migration.
  #define REFOLD_PROOF_DISCHARGE_STATUS_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(PendingMaterialization) \
  REFOLD_X(Discharged) \
  REFOLD_X(Rejected)

  enum class ProofDischargeStatus : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_PROOF_DISCHARGE_STATUS_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(ProofDischargeStatus value) {
    switch (value) {
#define REFOLD_X(name) case ProofDischargeStatus::name: return #name;
      REFOLD_PROOF_DISCHARGE_STATUS_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_PROOF_DISCHARGE_STATUS_LIST

  /// \brief Named local obligations used by proof-discharge records.
#define REFOLD_PROOF_OBLIGATION_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(AcceptedPathClassified) \
  REFOLD_X(FutureTargetMapped) \
  REFOLD_X(ProofRootTracked) \
  REFOLD_X(MacroProofRootResolved) \
  REFOLD_X(MacroProofRootIsTopLevel) \
  REFOLD_X(MacroPasteWitnessPresent) \
  REFOLD_X(MacroPasteWitnessWellFormed) \
  REFOLD_X(MacroPasteFreeSurfaceTracked) \
  REFOLD_X(MacroSubtreeCertificateTracked) \
  REFOLD_X(MacroCallChainWitnessTracked) \
  REFOLD_X(CounterStateWitnessTracked) \
  REFOLD_X(SubtreeAdmissibilityTracked) \
  REFOLD_X(WholeCoverBoundsTracked) \
  REFOLD_X(WholeCoverContainmentTracked) \
  REFOLD_X(WholeCoverBoundaryAccountingTracked) \
  REFOLD_X(IncludePendingMaterializationClassified) \
  REFOLD_X(IncludePatchShapeTracked) \
  REFOLD_X(IncludeAnchorWitnessTracked) \
  REFOLD_X(IncludeAnchorByteTracked) \
  REFOLD_X(IncludeConditionalOwnershipTracked) \
  REFOLD_X(IncludeMappedHeaderRangeTracked) \
  REFOLD_X(IncludeMappedHeaderByteRangeTracked) \
  REFOLD_X(IncludeSelectedConditionalBoundaryWitnessTracked) \
  REFOLD_X(IncludeChildBoundaryWitnessTracked) \
  REFOLD_X(IncludeRightNeighborWitnessTracked) \
  REFOLD_X(IncludeLeftNeighborWitnessTracked) \
  REFOLD_X(IncludeDeclBoundaryWitnessTracked) \
  REFOLD_X(TUAnchorPathClassified) \
  REFOLD_X(TUAnchorWitnessTracked) \
  REFOLD_X(TUAnchorPPGapTracked) \
  REFOLD_X(TUAnchorByteTracked) \
  REFOLD_X(TUExactSlotWitnessTracked) \
  REFOLD_X(TUProvableEvidenceTracked) \
  REFOLD_X(TUOutsideIncludeCoverageTracked) \
  REFOLD_X(TUOwnerDepthStableTracked) \
  REFOLD_X(ExplicitOutOfDomainResultTracked) \
  REFOLD_X(PrimaryProofClassDeclared) \
  REFOLD_X(OwnerRealizationWitnessTracked)

  enum class ProofObligationKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_PROOF_OBLIGATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(ProofObligationKind obligation) {
    switch (obligation) {
#define REFOLD_X(name)                                                        \
  case ProofObligationKind::name:                                             \
    return #name;
      REFOLD_PROOF_OBLIGATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_PROOF_OBLIGATION_KIND_LIST

  /// \brief Why a local proof contract could not be discharged.
#define REFOLD_PROOF_FAILURE_REASON_LIST(REFOLD_X) \
  REFOLD_X(None) \
  REFOLD_X(PendingMaterialization) \
  REFOLD_X(MissingAcceptedPathClassification) \
  REFOLD_X(MissingFutureTargetMapping) \
  REFOLD_X(MissingProofRoot) \
  REFOLD_X(MissingMacroProofRootResolution) \
  REFOLD_X(NonTopLevelMacroProofRoot) \
  REFOLD_X(MissingPasteWitness) \
  REFOLD_X(MalformedPasteWitness) \
  REFOLD_X(UnexpectedPasteSurface) \
  REFOLD_X(MissingSubtreeCertificate) \
  REFOLD_X(MissingCallChainWitness) \
  REFOLD_X(MissingCounterStateWitness) \
  REFOLD_X(MissingSubtreeAdmissibility) \
  REFOLD_X(MissingWholeCoverBounds) \
  REFOLD_X(MissingWholeCoverContainment) \
  REFOLD_X(MissingWholeCoverBoundaryAccounting) \
  REFOLD_X(MissingIncludePatchShape) \
  REFOLD_X(MissingIncludeAnchorWitness) \
  REFOLD_X(MissingIncludeAnchorByte) \
  REFOLD_X(MissingConditionalOwnership) \
  REFOLD_X(MissingMappedHeaderRange) \
  REFOLD_X(MissingMappedHeaderByteRange) \
  REFOLD_X(MissingIncludeSelectedConditionalBoundaryWitness) \
  REFOLD_X(MissingIncludeChildBoundaryWitness) \
  REFOLD_X(MissingIncludeRightNeighborWitness) \
  REFOLD_X(MissingIncludeLeftNeighborWitness) \
  REFOLD_X(MissingIncludeDeclBoundaryWitness) \
  REFOLD_X(MissingTUAnchorClassification) \
  REFOLD_X(MissingTUAnchorWitness) \
  REFOLD_X(MissingTUAnchorGap) \
  REFOLD_X(MissingTUAnchorByte) \
  REFOLD_X(MissingTUExactSlotWitness) \
  REFOLD_X(MissingTUProvableAnchorWitness) \
  REFOLD_X(MissingTUOutsideIncludeCoverageProof) \
  REFOLD_X(MissingTUOwnerDepthStability) \
  REFOLD_X(ExplicitOutOfDomainResult) \
  REFOLD_X(MissingPrimaryProofClass) \
  REFOLD_X(MissingOwnerRealizationWitness)

  enum class ProofFailureReason : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_PROOF_FAILURE_REASON_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(ProofFailureReason reason) {
    switch (reason) {
#define REFOLD_X(name)                                                        \
  case ProofFailureReason::name:                                              \
    return #name;
      REFOLD_PROOF_FAILURE_REASON_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "None";
  }
#undef REFOLD_PROOF_FAILURE_REASON_LIST

  /// \brief Compact record of class-local obligation discharge.
  struct ProofDischargeRecord {
    ProofDischargeStatus status = ProofDischargeStatus::Unknown;
    ProofFailureReason failureReason = ProofFailureReason::None;
    ProofObligationKind failedObligation = ProofObligationKind::Unknown;
    uint16_t obligationsEvaluated = 0;
    uint16_t obligationsSatisfied = 0;
  };

  /// \brief Small helper that accumulates class-local obligations.
  ///
  /// This remains a private implementation detail because future proof work may
  /// replace the current local-discharge bookkeeping with even stronger proof
  /// objects. Keeping the accumulator nested here lets the out-of-line
  /// implementation reuse the normalized proof types without widening
  /// RefoldEngine's public API.
  struct ProofDischargeAccumulator;
  struct ProofSummary;
  struct IncludePatch;

  void RequireAcceptedPathBaseline(
      ProofDischargeAccumulator &discharge,
      const AcceptancePathInventory &inventory) const;
  ProofDischargeRecord BuildAcceptedPathBaselineDischarge(
      const AcceptancePathInventory &inventory,
      bool explicitOutOfDomain = false) const;
  void ConfigureProofSummary(ProofSummary &summary,
                             TheoremProofClass theoremClass,
                             AcceptedProofClass acceptedClass,
                             RealizationMode realizationMode,
                             SelectionPreference preference,
                             SurfaceDisposition surfaceDisposition,
                             bool structurePreserving) const;
  void FinalizeProofSummary(ProofSummary &summary) const;
  void RequireIncludeZeroWidthAnchor(
      ProofDischargeAccumulator &discharge, const IncludePatch &patch,
      const IncludeAnchorWitness *witness, IncludeAnchorEvidenceKind evidence,
      ProofObligationKind witnessObligation,
      ProofFailureReason witnessFailure) const;
  bool TUAnchorWitnessHasProvableEvidence(
      const TUAnchorWitness &witness) const;
  void AppendProofSummaryWitnessAudit(std::string &artifact,
                                      const ProofSummary &summary,
                                      bool firstOnly) const;

  /// \brief Return true when the accepted path is an owner-realization proof.
  ///
  /// Phase 6f makes the shared \c OwnerRealizationWitness mandatory at the
  /// theorem boundary for every path whose proof class is now represented by
  /// the generic owner-realization family.  This helper deliberately keys off
  /// the normalized accepted path, not just the broad accepted class: e.g.
  /// counter-literal macro stabilization is an invocation realization, but it
  /// is not a Phase-6 owner-realization witness.
  bool ProofSummaryRequiresOwnerRealizationWitness(
      const ProofSummary &summary) const;

  /// \brief Canonical theorem-facing proof carried by an emitted result.
  ///
  /// This is the single object that answers why a selected artifact is
  /// theorem-admissible.  Construction provenance such as AcceptedPathKind and
  /// AcceptedProofClass may still exist below this layer, but they are not
  /// theorem authority: they must first normalize into exactly one
  /// `theoremClass` plus the typed witnesses copied here.  The carrier is
  /// intentionally value-only and side-effect-free so ProofSummary can own it
  /// without changing selector or emission semantics.
  struct EmittedProof {
    TheoremProofClass theoremClass = TheoremProofClass::Unknown;
    ProofDischargeRecord discharge;

    std::optional<OwnerRealizationWitness> ownerRealization;
    std::optional<MixedOwnerTilingWitness> mixedOwnerTiling;
    std::optional<TUAnchorWitness> tuAnchor;
    std::optional<IncludeAnchorWitness> includeAnchor;
    std::optional<SuffixStabilityWitness> suffixStability;
    std::optional<TerminalFallbackWitness> terminalFallback;

    bool HasFinalTheoremClass() const {
      return theoremClass != TheoremProofClass::Unknown;
    }
  };

  /// \brief Common proof-summary carrier used during the proof/lattice model.
  ///
  /// The summary packages the construction inventory, local discharge result,
  /// lattice law, completeness contract, explicit theorem-domain position, and
  /// canonical emitted proof for one accepted artifact.  The older construction
  /// fields remain temporarily so Phase 1C and Phase 2 can migrate their call
  /// sites deliberately, but theorem-facing code must consume `emittedProof`
  /// rather than re-deriving proof authority from AcceptedProofClass or local
  /// side bits.
  struct ProofSummary {
    /// Final theorem class declared by the builder after construction
    /// provenance has been normalized.  Phase 1C makes this field, not
    /// AcceptedProofClass, the summary-local answer to "why is this valid?".
    TheoremProofClass theoremClass = TheoremProofClass::Unknown;

    AcceptedProofClass acceptedClass = AcceptedProofClass::Unknown;
    RealizationMode realizationMode = RealizationMode::Unknown;
    SelectionPreference preference = SelectionPreference::Unknown;
    SurfaceDisposition surfaceDisposition =
        SurfaceDisposition::None;
    TheoremSelectionTieBreakerKind selectionTieBreaker =
        TheoremSelectionTieBreakerKind::Unknown;
    AcceptancePathInventory inventory;
    GlobalSelectionLattice lattice;
    CompletenessContract completeness;
    TheoremDomainContract theoremDomain;
    ProofDischargeRecord discharge;
    bool structurePreserving = false;
    uint64_t proofRootMacroId = 0;
    bool hasTUAnchorWitness = false;
    TUAnchorWitness tuAnchorWitness;
    bool hasIncludeAnchorWitness = false;
    IncludeAnchorWitness includeAnchorWitness;
    // Phase-6 shared owner-realization proof carrier.  Realized macro,
    // include, and TU paths now surface through this single theorem-facing
    // witness; owner-specific input metadata remains local to the spelling
    // machinery instead of appearing as separate proof/audit families.
    bool hasOwnerRealizationWitness = false;
    OwnerRealizationWitness ownerRealizationWitness;

    // Phase-7f durable mixed-owner tiling proof.  A candidate that came from a
    // normalized mixed-owner split can carry the whole ordered path here, not
    // merely the individual emitted token segment.
    bool hasMixedOwnerTilingWitness = false;
    MixedOwnerTilingWitness mixedOwnerTilingWitness;

    // State-stabilization witness carried by proof summaries that discharge
    // a suffix/state theorem directly rather than through owner realization.
    // Phase 1A exposes the same witness slot on EmittedProof; Phase 2 will move
    // the remaining MacroPatch mirror fields into one macro-proof carrier.
    bool hasSuffixStabilityWitness = false;
    SuffixStabilityWitness suffixStabilityWitness;

    std::optional<TerminalFallbackWitness> terminalFallbackWitness;

    /// Canonical theorem-facing proof produced by FinalizeProofSummary().
    /// A disengaged optional means the current construction inventory is still
    /// transitional or failed to discharge the theorem obligations.  Keeping
    /// this cached in the summary prevents later selector/emission code from
    /// treating the construction inventory as an independent proof authority.
    std::optional<EmittedProof> emittedProof;

    bool HasCanonicalEmittedProof() const {
      return emittedProof && emittedProof->HasFinalTheoremClass();
    }

    /// True when the summary's primary proof class came from a concrete
    /// accepted-path or patch-proof enum rather than from legacy side bits such
    /// as legacy side-bit inference. Phase-1 proof closure
    /// requires theorem-facing emitted results to carry exactly one explicit
    /// primary proof class; implicit legacy classification is therefore allowed
    /// only for internal diagnostics and is rejected before emission.
    bool primaryProofClassExplicit = false;
  };


  /// \brief Normalized accepted-result artifact kind used by Patch A/B.
  ///
  /// Patch A introduced the common carrier so already-accepted
  /// macro/include/TU/terminal results could be wrapped in one uniform shape.
  /// Patch B starts using that shape at selected competition sites, while the
  /// rest of the engine still accepts results through path-specific control
  /// flow until later selector-closure steps land.
  #define REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(MacroPatch) \
  REFOLD_X(IncludePatch) \
  REFOLD_X(TUAnchor) \
  REFOLD_X(TUTextEdit) \
  REFOLD_X(TerminalOutOfDomain)

  enum class AcceptedResultCandidateKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(AcceptedResultCandidateKind value) {
    switch (value) {
#define REFOLD_X(name) case AcceptedResultCandidateKind::name: return #name;
      REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST

  /// \brief Normalized wrapper for a concrete accepted result.
  ///
  /// The carrier stays intentionally small and explicit. It holds the
  /// normalized proof summary plus enough artifact-local provenance for the
  /// converted Patch-B selection sites to compare accepted outcomes through the
  /// lattice without rebuilding path-specific ordering logic.
  struct AcceptedResultCandidate {
    AcceptedResultCandidateKind kind = AcceptedResultCandidateKind::Unknown;
    ProofSummary proofSummary = {};

    // Phase-3A enumeration of the concrete emission surface(s) represented by
    // this candidate. This is inventory only: proof validity still comes from
    // ProofSummary::emittedProof / TheoremProofClass.
    EmissionPathInventory emissionPaths = {};

    // Artifact-local span / owner metadata.
    uint64_t begin = 0;
    uint64_t end = 0;
    bool hasOwnerIncludeId = false;
    uint64_t ownerIncludeId = 0;
    bool hasRootMacroId = false;
    uint64_t rootMacroId = 0;
    bool hasAnchorByte = false;
    uint64_t anchorByte = 0;

    // Human-readable preview of the selected surface. This is tracing-only and
    // never participates in admissibility or ordering.
    bool hasPayloadPreview = false;
    std::string payloadPreview;
  };

  /// \brief Result returned by the accepted-result selector.
  ///
  /// This carrier is theorem-facing: the selected candidate must already
  /// normalize to one final emitted proof class. Selector-only staging objects
  /// are deliberately kept out of this type so no internal macro-ranking proof
  /// can be mistaken for an emitted accepted artifact.
  struct SelectedAcceptedResultCandidate {
    AcceptedResultCandidate candidate;
    size_t index = 0;
  };

  /// \brief Macro-local selector carrier for internal macro competition.
  ///
  /// Macro candidate ranking sometimes needs a staging proof that is valid only
  /// for choosing among macro spellings, such as a nested subtree/call-chain
  /// candidate whose remaining obligation is the top-level proof-root rule.
  /// That selector proof is not an emitted accepted artifact. When the same
  /// concrete macro patch also has an emission-normalized proof, the emitted
  /// carrier is stored separately and is the only object allowed to be stamped
  /// onto MacroPatch::selectedAcceptedCandidate.
  struct MacroSelectionCandidate {
    AcceptedResultCandidate selectorCandidate;
    std::optional<AcceptedResultCandidate> emittedCandidate;
    bool selectorOnly = false;
  };

  /// \brief Result returned by the macro-local selector.
  ///
  /// The index points back to the caller-owned MacroPatch entry. The selected
  /// macro carrier may have ranked by a selector-only proof, but callers must
  /// stamp only emittedCandidate, never selectorCandidate, onto an emitted
  /// MacroPatch.
  struct SelectedMacroSelectionCandidate {
    MacroSelectionCandidate candidate;
    size_t index = 0;
  };


  #define REFOLD_MACRO_PATCH_PROOF_KIND_LIST(REFOLD_X) \
  REFOLD_X(Unknown) \
  REFOLD_X(CounterLiteral) \
  REFOLD_X(ArgsOnlyPasteMulti) \
  REFOLD_X(ArgsOnlyPasteSingle) \
  REFOLD_X(ArgsOnlyPurePasteOnly) \
  REFOLD_X(ArgsOnlyStandard) \
  REFOLD_X(ArgsOnlyPairedPureInsertion) \
  REFOLD_X(PasteDerivedCalleeSelector) \
  REFOLD_X(DagSubtreeRoot) \
  REFOLD_X(CallChainSuffix) \
  REFOLD_X(WholeCoverRealization)

  enum class MacroPatchProofKind : uint8_t {
#define REFOLD_X(name) name,
    REFOLD_MACRO_PATCH_PROOF_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  };

  friend inline StringRef toString(MacroPatchProofKind value) {
    switch (value) {
#define REFOLD_X(name) case MacroPatchProofKind::name: return #name;
      REFOLD_MACRO_PATCH_PROOF_KIND_LIST(REFOLD_X)
#undef REFOLD_X
    }
    return "Unknown";
  }
#undef REFOLD_MACRO_PATCH_PROOF_KIND_LIST

  /// \brief Producer/replay evidence for paste-preserving macro proofs.
  ///
  /// Phase 2D keeps paste-specific proof evidence inside the canonical
  /// MacroPatchProof carrier. This witness is the durable home for the
  /// paste-specific part of the proof:
  /// either the producer supplied well-formed paste-token spans, or the engine
  /// replayed the pasted surface and proved that the edited output is exactly
  /// reconstructed.
  struct PasteWitness {
    uint64_t rootMacroId = 0;
    bool requiresProducerPasteSpans = false;
    bool replayValidated = false;
  };

  /// \brief Durable certificate for DAG/subtree macro preservation proofs.
  ///
  /// These fields intentionally summarize the existing subtree audit metadata.
  /// Centralizing them here lets ClassifyMacroPatchProof() consume one proof
  /// object instead of a scattered collection of path-local side bits.
  struct SubtreeCertificate {
    bool backed = false;
    uint64_t leafMacroId = 0;
    uint32_t witnessCount = 0;
    uint32_t invocationCertCount = 0;
    uint32_t formalCertCount = 0;
    uint32_t argCertCount = 0;
    uint32_t liftChainCount = 0;
    uint32_t liftStepCount = 0;
    uint32_t rootMergeCount = 0;
    bool usesLexicalBridge = false;
    bool touchesPaste = false;
    bool hasWrapperSemantics = false;
    bool hasStringifySemantics = false;
    bool hasWideStringifySemantics = false;
    bool hasPreferredChildSyntax = false;
    bool hasRawInvocationPreservation = false;
    bool hasPassthroughFlatten = false;
    bool hasBridgeSensitiveStructuredSemantics = false;
    bool deferredPasteDischarged = false;
    bool admissible = false;
    uint32_t expectedRootFormalCount = 0;
    uint32_t deferredRootArgCount = 0;
    uint32_t bridgeSensitiveFormalCount = 0;
    std::string expectedRootFormalSummary;
    std::string deferredRootArgSummary;
    std::string bridgeSensitiveFormalSummary;
  };

  /// \brief Root/callsite evidence for call-chain suffix preservation.
  ///
  /// A call-chain suffix proof is valid only when the emitted patch is tied to
  /// the same root macro invocation that owns the suffix slice. Keeping the
  /// relationship in a witness object avoids future proof code having to infer
  /// that relationship from unrelated MacroPatch scalar fields.
  struct CallChainWitness {
    uint64_t rootMacroId = 0;
    uint64_t callsiteMacroId = 0;
  };

  /// \brief Canonical MacroPatch-local proof carrier.
  ///
  /// Phase 2D makes this object the only MacroPatch-local proof authority.
  /// SetMacroPatchProof() installs it, SyncMacroPatchProofSummary() refreshes
  /// derived paste/subtree/call-chain witnesses, and ClassifyMacroPatchProof()
  /// copies the resulting theorem facts into ProofSummary / EmittedProof.
  struct MacroPatchProof {
    MacroPatchProofKind kind = MacroPatchProofKind::Unknown;
    uint64_t proofRootMacroId = 0;
    bool preservesInvocationStructure = false;

    std::optional<OwnerRealizationWitness> ownerRealization;
    std::optional<SuffixStabilityWitness> suffixStability;
    std::optional<PasteWitness> paste;
    std::optional<SubtreeCertificate> subtree;
    std::optional<CallChainWitness> callChain;
  };

  struct MacroPatch {
    uint64_t invStart = 0, invEnd = 0;
    std::string replacement;

    MacroPatch() = default;
    MacroPatch(uint64_t invStart, uint64_t invEnd, std::string replacement,
               uint64_t macroId = 0)
        : invStart(invStart), invEnd(invEnd),
          replacement(std::move(replacement)), macroId(macroId) {}

    // Canonical macro invocation id for this physical callsite patch. This is
    // used only for statistics attribution; the patch itself is still keyed by
    // byte span and owner.
    uint64_t macroId = 0;

    // Proof-lattice migration summary.  The summary is rebuilt from the
    // canonical MacroPatchProof carrier, not from path-local mirror fields.
    // Default-initialize it so aggregate construction of MacroPatch remains
    // warning-free.
    ProofSummary proofSummary = {};

    // Canonical MacroPatch-local proof carrier.  Phase 2D deletes the former
    // scalar/witness mirrors from MacroPatch; all macro-local theorem facts now
    // live here and are copied into ProofSummary only through
    // ClassifyMacroPatchProof().
    MacroPatchProof proof = {};

    // Phase-3B selected-result bridge.  Macro candidate discovery still returns
    // a concrete MacroPatch for legacy ownership of the replacement bytes, but
    // the final macro selector records the exact AcceptedResultCandidate that
    // won lattice selection here before the patch can be forwarded to emission.
    // Emission may restamp the carrier onto the emitted-source discharge rule,
    // but it must not rediscover which path-specific result was selected.
    std::optional<AcceptedResultCandidate> selectedAcceptedCandidate;

    // First-class macro whole-cover realization certificate. These
    // fields record the exact owner cover, containment witness, and B-side
    // token-envelope accounting used to justify realized whole-cover output.
    bool wholeCoverUsedBodyRange = false;
    bool wholeCoverSelfContained = false;
    bool wholeCoverAdjustedLeft = false;
    bool wholeCoverAdjustedRight = false;
    bool wholeCoverClaimsClipped = false;
    uint64_t wholeCoverALo = 0;
    uint64_t wholeCoverAHi = 0;
    uint64_t wholeCoverBRawLo = 0;
    uint64_t wholeCoverBRawHi = 0;
    uint64_t wholeCoverBAdjLo = 0;
    uint64_t wholeCoverBAdjHi = 0;

    // Layer-5 subtree-composition audit metadata. This is instrumentation only
    // and does not participate in admissibility yet.
    bool subtreeCertBacked = false;
    uint64_t subtreeLeafMacroId = 0;
    uint32_t subtreeWitnessCount = 0;
    uint32_t subtreeInvocationCertCount = 0;
    uint32_t subtreeFormalCertCount = 0;
    uint32_t subtreeArgCertCount = 0;
    uint32_t subtreeLiftChainCount = 0;
    uint32_t subtreeLiftStepCount = 0;
    uint32_t subtreeRootMergeCount = 0;
    bool subtreeUsesLexicalBridge = false;
    bool subtreeTouchesPaste = false;
    bool subtreeHasWrapperSemantics = false;
    bool subtreeHasStringifySemantics = false;
    bool subtreeHasWideStringifySemantics = false;
    bool subtreeHasPreferredChildSyntax = false;
    bool subtreeHasRawInvocationPreservation = false;
    bool subtreeHasPassthroughFlatten = false;
    bool subtreeHasBridgeSensitiveStructuredSemantics = false;
    bool subtreeDeferredPasteDischarged = false;
    bool subtreeAdmissible = false;
    uint32_t subtreeExpectedRootFormalCount = 0;
    uint32_t subtreeDeferredRootArgCount = 0;
    uint32_t subtreeBridgeSensitiveFormalCount = 0;
    std::string subtreeExpectedRootFormalSummary;
    std::string subtreeDeferredRootArgSummary;
    std::string subtreeBridgeSensitiveFormalSummary;

    // Direct args-only paste replay proof metadata.
    //
    // Some accepted paste-preserving args-only patches are justified directly
    // from the producer-side root `paste_tokens` witness stream. Others are
    // justified by the engine's own replay check, which re-applies the
    // rewritten invocation arguments and proves that every pasted token
    // occurrence reconstructs the edited B surface exactly. Record the latter
    // case on the patch so Patch C can treat that deterministic replay as an
    // authoritative proof source at the converted selector sites.
    bool pasteReplayValidated = false;

    // B-token envelope that corresponds to the materialized B-side surface for
    // this physical callsite patch. Whole-cover patches stamp the exact replay
    // envelope. Structure-preserving macro patches may also stamp the whole
    // expansion envelope when a compact invocation-argument rewrite represents
    // that expansion in the refolded source. Narrow pure-insertion patches leave
    // this unset so final emission can map only the inserted payload bytes.
    bool hasMaterializedBTokenRange = false;
    uint64_t materializedBTokStart = 0;
    uint64_t materializedBTokEnd = 0;

    // Optional byte range inside `replacement` that is the output-side surface
    // corresponding to the materialized B witness. This is deliberately
    // separate from invStart/invEnd: a structure-preserving macro patch may
    // physically rewrite the whole invocation while the B edit maps only to
    // the rewritten argument envelope inside that invocation.
    bool hasMaterializedOutputByteRange = false;
    uint64_t materializedOutputByteStart = 0;
    uint64_t materializedOutputByteEnd = 0;

    // Layer-6 mixed-owner decomposition certificate metadata.
    bool ownerCertPresent = false;
    bool ownerMixedWitness = false;
    uint8_t ownerKindCode = 0; // 0=unknown, 1=TU, 2=Include
    uint64_t ownerIncludeIdCert = 0;
    bool ownerHasCondArmCert = false;
    uint64_t ownerCondArmIdCert = 0;
    uint32_t ownerWitnessCount = 0;
  };

  /// \brief Deterministic whole-cover realization plan for one invocation.
  ///
  /// ComputeWholeCoverPlan() proves the exact A/B token envelope that a
  /// whole-cover realization may use. The accepted whole-cover result becomes
  /// a first-class macro realization proof by copying this plan
  /// onto the accepted MacroPatch.
  struct WholeCoverPlan {
    uint64_t covLoA = 0;
    uint64_t covHiA = 0;
    bool usedBodyRange = false;
    bool selfContained = false;
    size_t rawBTokStart = 0;
    size_t rawBTokEnd = 0;
    size_t bTokStart = 0;
    size_t bTokEnd = 0;
    bool adjustedLeft = false;
    bool adjustedRight = false;
    bool claimsClipped = false;
    std::string clippedText;
  };

  struct IncludePatch {
    const RefoldModel::IncludeItem *include;
    std::string insertBytes; // exact B bytes
    uint64_t aStart, aEnd;   // A-token interval inside include expansion
    uint64_t bStart, bEnd;   // B-token interval

    // Internal working summary for include-owned patch candidates.
    // Include patches are created before materialization chooses a concrete
    // preserving anchor or realization envelope, so pre-materialization
    // patches must not claim a normalized accepted path yet. The patch-level
    // summary therefore stays internal-only until materialization restamps the
    // emitted accepted result onto an explicit witness-backed preserving or
    // realization class.
    ProofSummary proofSummary = {};

    /// When present, this insertion was classified as belonging to a specific
    /// selected conditional arm inside the owning include. Include application
    /// must preserve that ownership and never anchor the insertion outside the
    /// certified arm body.
    bool ownerHasCondArmCert = false;
    uint64_t ownerCondArmIdCert = 0;

    /// Some include-local layout repairs are already proved as source-byte edits
    /// before the generic include patch applicator runs.  Keep the PP-token A/B
    /// range as the proof envelope, but do not ask the generic mapper to recover
    /// a different byte range from that envelope: the layout theorem has already
    /// chosen the exact header bytes that must be replaced.
    bool hasDirectHeaderByteRange = false;
    uint64_t directHeaderByteBegin = 0;
    uint64_t directHeaderByteEnd = 0;

    std::string ToString() const {
      // 1. Determine the path (Using StringRef to avoid extra copies)
      StringRef path;
      if (include->resolvedPath && !include->resolvedPath->empty()) {
        path = *include->resolvedPath;
      } else {
        path = stringutils::stripHeaderToken(include->target);
      }

      // 2. Handle the preview truncation
      StringRef preview = insertBytes;
      bool truncated = false;
      if (preview.size() > 80) {
        preview = preview.take_front(80);
        truncated = true;
      }

      // 3. Escape whitespace
      std::string escapedPreview = stringutils::escape(preview);

      return formatv(
                 "IncludePatch{{incId={0}, path={1}, A=[{2},{3}), B=[{4},{5}), "
                 "condArm={6}, directBytes={7}, insert='{8}{9}'}",
                 include->id, path, aStart, aEnd, bStart, bEnd,
                 ownerHasCondArmCert
                     ? std::to_string(ownerCondArmIdCert)
                     : std::string("(none)"),
                 hasDirectHeaderByteRange
                     ? formatv("[{0},{1})", directHeaderByteBegin,
                               directHeaderByteEnd)
                           .str()
                     : std::string("(none)"),
                 escapedPreview, (truncated ? "..." : ""))
          .str();
    }
  };

  struct IncludeEdits {
    const RefoldModel::IncludeItem *include;
    std::vector<IncludePatch> patches;

    explicit IncludeEdits(const RefoldModel::IncludeItem *item)
        : include(item) {}

    void Add(IncludePatch &&P) { patches.push_back(std::move(P)); }
  };

  /// Append include-local realization patches for preserved header-owned
  /// `__LINE__` observers whose B-side preprocessed layout merged the
  /// observer's physical source line with the previous PP line.  This is the
  /// include-owner analogue of AppendLineObserverLayoutRealizationEdits(): no
  /// legal `#line` directive can repair a source-spelled observer that must
  /// remain on the merged physical line, so the observer line must be
  /// materialized as B bytes inside the owning header include.
  bool AppendIncludeLineObserverLayoutRealizationEdits(
      DenseMap<uint64_t, IncludeEdits> &perInclude);

  // ------------------------------- Core Helpers ------------------------------

  /// \brief Maps preprocessor tokens into a stable lexeme sequence suitable for
  /// diff/LCS alignment.
  ///
  /// This method produces the token sequence consumed by the LCS/Myers
  /// pipeline. The emitted element is simply PPTok::spelling.
  ///
  /// The whitespace sentinel prevents the aligner from treating arbitrary
  /// whitespace runs as interchangeable anchors. By tying whitespace lexemes to
  /// their A-side byte position, we ensure whitespace cannot "float" to other
  /// whitespace elsewhere in the file and distort the alignment, which is
  /// especially important around include/conditional boundaries and for
  /// pure insertions.
  ///
  /// \param toks The token list (typically PP tokens) whose spellings form the
  ///             baseline sequence.
  /// \param offs The start-byte offset (in the corresponding source text) for
  ///             each token in \p Toks; must be the same size as \p Toks and
  ///             index-aligned.
  /// \returns A list of lexeme strings of size Toks.size(), containing either
  ///          the original token spelling or a position-tied whitespace
  ///          sentinel.
  static std::vector<StringRef> MapLexemes(ArrayRef<PPTok> toks,
                                           ArrayRef<size_t> offs);

  /// \brief Add boundary padding spaces only when needed to preserve lexical
  /// tokenization.
  ///
  /// Adds at most one space on the left and/or right edge of `text` so that,
  /// when `text` replaces `base[start,end)` (half-open), tokens do not glue
  /// across the replacement boundary. Existing whitespace at either edge of
  /// `text`, or immediate boundary whitespace already present in `base`, counts
  /// as already-separated and suppresses padding on that side. Callers can also
  /// suppress padding explicitly via `allowLeft` / `allowRight` (for example,
  /// when preserving an existing gap).
  ///
  /// #### Behavior
  /// * Find the first and last non-whitespace token in `text`.
  /// * On each allowed side, first check whether the immediate boundary is
  ///   already separated by whitespace in `text` or `base`.
  /// * If not already separated, compare Clang raw-lexing with and without an
  ///   inserted boundary space; add a single space only when omitting it would
  ///   change tokenization across that boundary.
  /// * Never inserts more than one space per side and never modifies `base`.
  ///
  /// Deterministic and local — decisions are based on the actual lexical
  /// boundary, not on broader formatting preferences.
  ///
  /// \param base       The original target string being patched.
  /// \param start      Start index (inclusive) of the slice in `base` to
  ///                   replace.
  /// \param end        End index (exclusive) of the slice in `base` to replace.
  /// \param text       The replacement snippet to be inserted.
  /// \param allowLeft  Whether a left-side pad is permitted.
  /// \param allowRight Whether a right-side pad is permitted.
  /// \returns `text`, possibly prefixed and/or suffixed with a single space to
  ///          preserve lexical separation across the replacement boundary.
  std::string PadAtBoundaries(StringRef base, size_t start, size_t end,
                              std::string text, bool allowLeft,
                              bool allowRight) const;

  bool MaybeConsumeOrdinarySeparatorGapForPunctuation(
      StringRef tuPath, StringRef tuBytes, std::pair<uint64_t, uint64_t> &span,
      StringRef replacement, StringRef tracePrefix) const;

  bool TUReplacementExtensionIsBTokenClosed(uint64_t aTokStart,
                                            uint64_t oldEnd,
                                            uint64_t extEnd,
                                            uint64_t bStart,
                                            uint64_t bEnd,
                                            StringRef tuPath) const;

  void MaybeExtendTUSpanOverClosedTrailingCallSuffix(
      const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
      StringRef replacement, std::pair<uint64_t, uint64_t> &span) const;

  bool MaybeAdvanceTUInsertionPastSourceLineControlPrefix(
      const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
      std::pair<uint64_t, uint64_t> &span, StringRef tracePrefix) const;

  bool TUInsertionCanDeferResyncToConditionalJoin(
      bool advancedOverSourceLineControlPrefix, StringRef tuPath,
      uint64_t anchor, StringRef tracePrefix) const;

  bool TUInsertionBeforeMaterializedInclude(
      const diffutils::Hunk &h, StringRef tuPath,
      const std::pair<uint64_t, uint64_t> &span,
      bool requireVisibleReplayText) const;

  TextEdit BuildDirectTUHunkTextEdit(
      const diffutils::Hunk &h, uint64_t hunkIndex,
      const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
      StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
      std::optional<uint64_t> materializedBByteBegin,
      std::optional<uint64_t> materializedBByteEnd,
      AcceptedPathKind acceptedPath) const;

  /// \brief Computes an "owner depth gap" array used to bias the weighted LCS
  /// anchoring for insertions.
  ///
  /// The refolder frequently chooses insertion positions by aligning PP token
  /// sequences. For insertions (gaps between tokens), we want to prefer
  /// positions that remain inside the correct *owner* region (nested includes
  /// and nested #if/#else arms), rather than being attracted to shallower
  /// boundaries. This method precomputes a depth-like metric per PP gap that
  /// can be used as a weighting term in the LCS cost function.
  ///
  /// **Definition:** for each PP gap `k` (between PP tokens `k-1` and `k`), we
  /// compute:
  ///
  ///     ownerDepthGap[k] = includeDepthLCA(leftInc, rightInc) + condDepthGap
  ///
  /// where:
  /// * **leftInc/rightInc** are the innermost include instance ids owning the
  ///   left and right side of the gap (TU is treated as depth 0).
  /// * **includeDepthLCA** is the include-nesting depth of the least common
  ///   ancestor include of the two sides (e.g. 0 for TU, 1 for direct TU
  ///   children, etc.).
  /// * **condDepthGap** is derived from the conditional-arm ownership of the
  ///   two sides. It is computed as the minimum of the two arm depths so that
  ///   a gap straddling a boundary does not receive an artificially deep score.
  ///
  /// **Indexing:** `numOfAOffs` is (#tokens + 1) because it represents PP
  /// offsets including the sentinel gap after the last token. This method
  /// returns an array sized `N + 1` where `N = numOfAOffs - 1` is the number of
  /// tokens, and indices `k in [0..N]` correspond to the `N+1` possible gaps.
  ///
  /// **Behavioral intent:** larger values indicate "deeper" nesting (more
  /// specific ownership). When used as a bias, insertions are encouraged to
  /// remain within the correct nested include/conditional region instead of
  /// drifting outward to more global boundaries.
  ///
  /// \param numOfAOffs number of PP offsets for the A-stream (#tokens + 1
  ///        including sentinel)
  /// \return an array `ownerDepthGap` indexed by PP gap `k` in `[0..N]`
  std::vector<uint32_t> ComputeOwnerDepthGapsForPP();

  /// \brief Compute structured A-side gap provenance for certified LCS mapping.
  ///
  /// This is the identity-preserving counterpart to `ComputeOwnerDepthGapsForPP`.
  /// It records the include, conditional-arm, and macro-expansion context on both
  /// sides of each PP-token gap while preserving the same scalar `ownerDepth`
  /// used by the core LCS objective. DiffAlgorithms uses these profiles to keep
  /// forced anchors and to restore ambiguous edge anchors only when a unique
  /// owner-preserving frontier is certified.
  std::vector<diffutils::LcsAGapProvenance> ComputeLcsAGapProvenanceForPP();

  /// \brief Compute edited-side source-surface profiles for B-side token gaps.
  ///
  /// These profiles describe line affinity and whitespace/newline shape around
  /// each B-token gap. The certified LCS map uses them as a structural
  /// discriminator for otherwise equivalent pure-insertion frontiers; absolute
  /// byte offsets are retained for trace diagnostics only.
  std::vector<diffutils::LcsBGapProvenance> ComputeLcsBGapProvenanceForPP();

  /// \brief Classifies the logical "owner" of a diff hunk using the precomputed
  /// segment map for the translation unit.
  ///
  /// A hunk is given in A-side token coordinates [\p a0, \p a1) and describes
  /// how a region of the preprocessed stream was edited. This method projects
  /// the hunk into TU byte space, finds the smallest matching Segment,
  /// and returns an Owner describing whether the hunk belongs to:
  ///   - the TU file itself (kind = OwnerKind::TU),
  ///   - a particular include (kind = OwnerKind::Include),
  ///   - a specific conditional arm (kind = OwnerKind::ConditionalArm or a
  ///     TU/include owner with `condArmId`),
  ///   - a macro invocation (kind = OwnerKind::MacroInvocation), or
  ///   - an unknown/ambiguous region.
  ///
  /// \headername Algorithm overview
  /// 1. Use tuByteSpan() to map the hunk's A-side token interval [\p a0, \p a1)
  ///    into a TU byte span [b, e) for the given \p tuPath. Even when the hunk
  ///    ultimately belongs to a header, segments for includes and conditional
  ///    arms are defined in TU byte space via include slots, so this step
  ///    is always performed.
  /// 2. Choose a probe byte offset inside [b, e):
  ///      - for non-empty hunks (\p a0 != \p a1), use the midpoint (b + e) / 2,
  ///      - for pure INSERT hunks (\p a0 == \p a1), use the left edge b so that
  ///        insertions at boundaries can be attached to the appropriate
  ///        "parent" segment.
  /// 3. Collect all Segment objects for the TU whose byte ranges intersect
  ///    [b, e). Sort the hits by segment length and apply additional rules for
  ///    pure INSERT hunks to handle boundary cases (for example, treating an
  ///    insertion at the end of a segment as belonging to that segment).
  /// 4. Map the selected segment to an Owner:
  ///      - if ownerIncludeId is empty, the hunk is classified as a TU edit,
  ///      - otherwise it is classified as an include or conditional-arm edit
  ///        based on ownerIncludeId and ownerCondArmId.
  /// 5. If no segment intersects the TU span, or the hunk falls into a
  ///    macro-expansion / slot boundary that cannot be cleanly attributed to a
  ///    single segment, the owner is returned as OwnerKind::Unknown. Callers
  ///    must then either (a) expand to a larger structural edit (e.g. realize
  ///    an include/conditional arm) or (b) reject the hunk as not safely
  ///    attributable.
  ///
  /// \param tuPath  absolute path of the TU file currently being refolded.
  /// \param h       the hunk to classify (in A-side token coordinates).
  /// \param aTokOff optional mapping from A-side token indices to TU byte
  ///                offsets; currently unused but reserved for future
  ///                refinements of segment selection.
  /// \returns An Owner describing which entity (TU, include, conditional arm,
  ///          macro, unknown) the hunk logically belongs to.
  Owner ClassifyOwnerWithSegments(StringRef tuPath,
                                  const diffutils::Hunk &h) const;

  /// \brief Returns \c true if the given macro invocation is lexically
  /// contained within a \c #define directive in the same source file.
  ///
  /// This is a safety/eligibility guard used by args-only macro patching. If an
  /// invocation occurs inside the spelling of a macro definition (i.e., within
  /// the byte range of a \c #define directive), treating it as a normal
  /// call-site can produce incorrect or destabilizing rewrites (e.g.,
  /// attempting to patch an invocation that is part of the macro's replacement
  /// list, creating self-referential edits, or breaking idempotence under
  /// re-preprocessing).
  ///
  /// The implementation relies on the producer-emitted directive list in the
  /// \c RefoldModel. It scans \c MacroDirective entries with
  /// \c subkind == "#define" in the same file as the invocation and checks
  /// whether the invocation's byte range \c [m.invB, m.invE) lies fully within
  /// the directive's site byte range \c [d.siteB, d.siteE).
  ///
  /// Note that this is a purely lexical containment test. It does not interpret
  /// macro semantics or nested directive structure; it assumes directive site
  /// ranges are accurate and non-overlapping for a given file.
  ///
  /// \param m Macro invocation metadata (file path and invocation byte range).
  /// \returns \c true if the invocation is fully contained within a \c #define
  ///          directive's site range in the same file; otherwise \c false.
  bool IsInvocationInsideDefineDirective(
      const RefoldModel::MacroInvocation &m) const;

  /// \brief Returns the innermost (smallest-width) patchable macro invocation
  /// that fully covers a given A-span.
  ///
  /// This is identical in spirit to smallestCoveringMacro (choose the smallest
  /// invocation whose preprocessed-token cover interval [coverBegin, coverEnd)
  /// contains [aStart, aEnd)), but with an additional constraint: the selected
  /// macro must have a valid invocation-site span in the original source (i.e.,
  /// inv_b/inv_e and inv_text are present).
  ///
  /// Why this matters: many object-like expansions that appear inside other
  /// macro bodies—especially predefined/builtin macros such as __FILE__ and
  /// __LINE__— may not carry their own patchable invocation-site metadata in
  /// the refold map. When an edit targets tokens produced by such an inner
  /// expansion, the refolder must attribute the edit to the nearest enclosing
  /// macro call that can be rewritten at its call site; this method performs
  /// that "bubble up to a patchable call site" selection deterministically by
  /// choosing the smallest enclosing patchable invocation.
  ///
  /// This is used during hunk classification to decide whether an edit should
  /// be realized as a macro call-site patch (whole-cover or argument patch)
  /// rather than as a TU/include byte patch.
  ///
  /// \param aStart  Inclusive start PP-token index in A.
  /// \param aEnd    Exclusive end PP-token index in A.
  /// \return        The smallest covering patchable macro invocation, or
  ///                nullptr if none cover the span.
  const RefoldModel::MacroInvocation *SmallestCoveringPatchableMacro(
      uint64_t aStart, uint64_t aEnd,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// \brief Return a patchable macro whose expansion ends at a pure insertion
  ///        gap and whose generated descendant chain contains `__VA_OPT__`.
  ///
  /// Ordinary macro ownership deliberately excludes half-open range boundaries:
  /// a pure insertion at `cover.end` normally belongs to the surrounding TU or
  /// include, not to the macro.  The only exception currently proved here is an
  /// activation of an inactive variadic tail, where the inserted B tokens are
  /// part of the macro's generated replacement-list grammar even though the
  /// old expansion had no A tokens at that position.
  ///
  /// This selector is intentionally narrower than
  /// SmallestCoveringPatchableMacro(): it does not make all right-boundary
  /// insertions macro-owned.  It only nominates a real callsite whose expansion
  /// ends at `aGap` and that has a generated descendant at the same boundary
  /// whose definition contains `__VA_OPT__`; the macro proof must still replay
  /// and validate the inserted tail before any patch is accepted.
  const RefoldModel::MacroInvocation *RightBoundaryVaOptActivationMacro(
      uint64_t aGap,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// \brief Return a patchable macro whose boundary insertion may be part of
  ///        replacing a generated callee selector.
  ///
  /// Selector replacement can change only tokens immediately around the old
  /// generated callee expansion, for example `STR(x)` -> `WRAP(x)` changes
  /// `"x"` into `"[" "x" "]"`.  Those prefix/suffix tokens are pure
  /// insertions at the old owner cover boundaries, but they are not ordinary
  /// boundary ownership: they are macro-owned only if a generated descendant
  /// callee came from a caller parameter.  The whole-cover macro proof must
  /// still replay the candidate selector and validate the full B envelope.
  const RefoldModel::MacroInvocation *BoundaryGeneratedSelectorMacro(
      uint64_t aGap,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// \brief Determine whether an A-token interval is owned by the translation
  ///        unit (TU).
  ///
  /// Non-empty ranges are TU-owned only when every mapped PP entry in
  /// ` [a0, a1) ` resolves to `tuPath`. Pure insertions (`a0 == a1`) are
  /// TU-owned only when the engine can derive a truthful TU insertion anchor at
  /// that exact PP gap via FindProvableTUInsertionAnchor().
  ///
  /// This function is intentionally fail-closed: any concrete mapping to a
  /// non-TU file, or any empty gap without a provable TU anchor, is treated as
  /// non-TU. This prevents header/include edits from being realized as TU byte
  /// edits.
  ///
  /// \param a0      Inclusive start preprocessed-token index in A.
  /// \param a1      Exclusive end preprocessed-token index in A.
  /// \param tuPath  Absolute canonical path of the TU’s source file.
  /// \returns       `true` if the interval is TU-owned; `false` otherwise.
  bool HunkMapsToTU(uint64_t a0, uint64_t a1, StringRef tuPath) const;

  /// \brief Return a conservative TU byte anchor for a pure insertion at PP
  ///        gap \p pp.
  ///
  /// Determines whether the empty A-side hunk at preprocessing-output gap
  /// \p pp has a \em provable insertion point in the translation unit
  /// identified by \p tuPath. This proof is used for two purposes: deciding
  /// whether the pure insertion is truthfully TU-owned, and materializing the
  /// corresponding zero-width TU span in byte space.
  ///
  /// The check is intentionally fail-closed. It accepts only:
  ///   - exact structural slot anchors recorded by the producer,
  ///   - exact TU-side macro "arg-like begin" anchors for wrapper/deferred
  ///     expansion shapes,
  ///   - immediate mapped TU neighbors when the gap is outside include
  ///     coverage,
  ///   - a zero-token top-level include boundary bracketed by the same PP gap,
  ///     or
  ///   - in non-strict mode, a bounded whitespace probe whose nearest mapped
  ///     neighbors on both sides agree on TU ownership without crossing an
  ///     owner-depth boundary.
  ///
  /// \param pp The PP-gap index in A-space for the pure insertion being
  ///        classified or materialized.
  /// \param tuPath The canonical path of the translation unit whose byte space
  ///        is being queried for a truthful insertion anchor.
  ///
  /// \returns The TU byte offset of the zero-width insertion anchor when a
  ///          truthful TU proof succeeds; otherwise \c std::nullopt.
  std::optional<uint64_t>
  FindProvableTUInsertionAnchor(
      uint64_t pp, StringRef tuPath, TUAnchorWitness *witness = nullptr,
      AcceptedResultCandidate *acceptedCandidate = nullptr) const;

  /// Anchors a *pure insertion* (a PP-gap insertion) to a deterministic,
  /// canonical TU byte boundary representing the *same* preprocessed
  /// coordinate, when possible.
  ///
  /// A PP-gap `ppGap` is a boundary between two adjacent PP tokens (i.e. a
  /// "gap" index). For an insertion that conceptually occurs at that PP
  /// boundary, this method returns the TU byte offset of an **exact**
  /// structural boundary corresponding to that same PP coordinate.
  ///
  /// **Key property:** this method performs *no* "nearest" snapping. If `ppGap`
  /// does not exactly match a known boundary PP coordinate, it returns
  /// `std::nullopt` so callers can fall back to neighbor-based span anchoring.
  /// This avoids regressions where an insertion belonging inside a nested owner
  /// (include/arm) is incorrectly pulled out to a shallower boundary.
  ///
  /// **Boundary sources considered** (each producing a candidate `(pp,b)`
  /// pair):
  /// * **Explicit TU slots** with an emitted `pp` coordinate and a conservative
  ///   "boundary-like" `kind` (file/arm/include boundaries).
  ///
  /// **Directive-line newline adjustment:** some recorded boundary slots may
  /// point at the newline that terminates a preprocessor directive line (e.g.
  /// after `#include`, `#else`, `#endif`). For *insertions* at those
  /// boundaries, anchoring at the newline byte can cause directive
  /// concatenation (e.g. `...;#else`). To preserve directive line integrity,
  /// candidates for selected `kind`s are adjusted to anchor *after* the newline
  /// (handling `\n` and `\r\n`).
  ///
  /// **Exact-match requirement:** candidates are filtered to those whose PP
  /// coordinate equals `ppGap` exactly. If none match, returns `std::nullopt`.
  ///
  /// **Deterministic tie-breaking:** if multiple candidates share the same PP
  /// coordinate, the chosen candidate is the one with the highest priority by
  /// `kind`, then the smallest TU byte offset, then the smallest slot id. This
  /// ensures stable output across runs.
  ///
  /// \param tuPath the TU path whose slots/owners are being consulted (must
  ///               match model file keys)
  /// \param ppGap  the PP gap index (between PP tokens) representing the
  ///               desired insertion coordinate
  /// \returns the TU byte offset of an exact canonical boundary matching
  ///          `ppGap`, or `std::nullopt` if `ppGap` is not exactly on a known
  ///          boundary (caller should fall back)
  std::optional<uint64_t>
  AnchorToExactSlotBoundaryFromPPGap(
      StringRef tuPath, uint64_t ppGap, TUAnchorWitness *witness = nullptr,
      AcceptedResultCandidate *acceptedCandidate = nullptr) const;

  /// Advance a proved zero-width insertion anchor past a contiguous prefix of
  /// active, source-authored line-control directives in the same owner.
  ///
  /// In PP token space, a source `#line` directive contributes no token, so the
  /// gap immediately before the directive and the gap immediately before the
  /// first token governed by that directive collapse to the same PP coordinate.
  /// When the insertion is anchored at such a zero-token prefix, prefer the
  /// post-directive byte position so preserved source line-control continues to
  /// dominate the inserted material rather than forcing a synthetic physical
  /// resync before the original directive.
  std::optional<uint64_t> AdvanceInsertionAnchorPastSourceLineControlPrefix(
      StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
      StringRef ownerBytes, uint64_t anchor) const;

  /// Return true iff a pure-insertion PP gap sits immediately after the
  /// selected arm's last PP token and before the rejoined suffix.  Such a gap
  /// belongs before suffix-local source line-control, not after it.
  bool IsPPGapAtSelectedConditionalArmExit(uint64_t ppGap) const;

  /// Return true iff a line-resync created at \p resumeOffset should be
  /// discharged by the conditional-join repair pass rather than emitted inside
  /// the selected arm.  This is the dominance counterpart of include-return
  /// deferral: when the first preserved line-state observer is after the
  /// conditional group rejoins, an arm-local #line before an intervening
  /// directive such as #include is valid but over-eager and can suppress the
  /// canonical post-group repair.
  bool LineResyncShouldDeferToConditionalJoin(
      StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
      uint64_t resumeOffset) const;

  /// \brief Finds the ID of the narrowest include range that covers a given PP
  /// index.
  ///
  /// Iterates through all include items in the provided model to find the one
  /// whose range [begin, end) contains the specified preprocessor index \p pp.
  /// If multiple includes cover the index, the one with the smallest span
  /// (shortest length) is selected to ensure the most specific match.
  ///
  /// \param pp  The preprocessor index to search for within the covers.
  ///
  /// \returns The ID of the most specific include item if a match is found;
  ///          otherwise, std::nullopt.
  std::optional<uint64_t> IncludeIdCoveringPPIndex(uint64_t pp) const {
    std::optional<uint64_t> bestId;
    uint64_t bestLen = std::numeric_limits<uint64_t>::max();

    for (const RefoldModel::IncludeItem &inc : model_.GetIncludes()) {
      if (pp >= inc.cover.begin && pp < inc.cover.end) {
        uint64_t len = inc.cover.end - inc.cover.begin;
        if (len < bestLen) {
          bestLen = len;
          bestId = inc.id;
        }
      }
    }

    return bestId;
  }

  /// \brief Compute the TU (translation unit) byte span \c [b,e) that
  /// corresponds to an A-side PP-token interval \c [a0,a1).
  ///
  /// This routine converts a diff hunk expressed in A-token indices into a
  /// concrete byte range in the TU source file. The contract is intentionally
  /// conservative: if the interval cannot be proven to touch the TU (or cannot
  /// be safely anchored into the TU for a pure insertion), the method returns
  /// \c std::nullopt rather than "snapping" across ownership boundaries.
  ///
  /// \par Behavior
  /// 1. **Normalize indices:** if \p a0 > \p a1 the bounds are swapped.
  /// 2. **Pure insertion fast-path:** if \p a0 == \p a1, first try to anchor on
  /// an
  ///    *exact* canonical TU slot boundary recorded at the same PP gap via
  ///    AnchorToExactSlotBoundaryFromPPGap(). If present, returns \c {b,b}.
  /// 3. **Direct TU coverage:** for non-empty intervals, scan PP indices in
  ///    \c [a0,a1) and consider only tokmap entries whose \c TokMapEntry::file
  ///    equals
  ///    \p tuPath. If any exist, returns the minimal enclosing TU byte span
  ///    \c [min(ent.b), max(ent.e)).
  /// 4. **No TU-mapped tokens:**
  ///    - If \p a0 != \p a1 (non-empty interval) and no TU tokens were found,
  ///      returns \c std::nullopt.
  ///    - If \p a0 == \p a1 (pure insertion at PP gap \c pp = a0), defer to
  ///      FindProvableTUInsertionAnchor(). If it succeeds, return \c {b,b};
  ///      otherwise return \c std::nullopt.
  ///
  /// A returned span \c {b,b} denotes a concrete insertion anchor point in the
  /// TU.
  ///
  /// \param a0 Inclusive start A-side PP-token index.
  /// \param a1 Exclusive end A-side PP-token index.
  /// \param tuPath Absolute/canonical TU path (must match \c
  /// TokMapEntry::file).
  /// \returns A TU byte span \c [b,e) (or \c {b,b} for a pure insertion
  /// anchor),
  ///          or \c std::nullopt if no TU span/anchor can be derived safely.
  std::optional<std::pair<uint64_t, uint64_t>>
  TUByteSpan(uint64_t a0, uint64_t a1, StringRef tuPath) const;

  /// \brief Determines whether an insertion hunk lands exactly on an include PP
  /// boundary and, if so, returns the include that should own the boundary
  /// insertion.
  ///
  /// This helper is used to conservatively assign ownership for edits that
  /// occur at an insertion point in the A-side PP token stream. The intent is
  /// to detect insertions that are *exactly* between sibling include regions
  /// and to attribute the insertion to their parent include, rather than
  /// incorrectly placing it inside one of the adjacent children.
  ///
  /// \par Applicability
  /// This policy is only applicable when the hunk has an empty A-span (\c
  /// h.aStart == h.aEnd). (Some diff producers may also guarantee \c h.bStart <
  /// h.bEnd for insertions; this function intentionally does not rely on that
  /// invariant.)
  ///
  /// \par Deterministic boundary-only policy
  /// To avoid heuristic ownership mistakes, this routine does \b not probe for
  /// “nearest” tokmap entries and does \b not snap to nearby PP tokens.
  /// Instead, it only considers includes whose A-domain PP cover interval \c
  /// [coverBegin, coverEnd) touches the insertion position exactly:
  ///
  /// - An include is considered immediately to the left if \c coverEnd == aPos.
  /// - An include is considered immediately to the right if \c coverBegin ==
  ///   aPos.
  /// - If multiple includes touch the boundary, the smallest-width include
  ///   (most-nested) is chosen independently on each side.
  ///
  /// The selected left/right include ids (or TU if absent) are then used to
  /// compute the least common ancestor via \c
  /// RefoldModel::LeastCommonAncestorInclude(leftId, rightId).
  ///
  /// \par Outcome
  /// - If both sides are TU (no exact boundary touch), return nullptr (policy
  ///   does not apply).
  /// - If the least common ancestor is absent, return nullptr (boundary meets
  ///   at TU).
  /// - Otherwise, return the ancestor \c IncludeItem, causing the caller to
  ///   treat the insertion as owned by that include level (i.e., between
  ///   sibling includes at the parent scope).
  ///
  /// \param h The diff hunk; only hunks with \c h.aStart == h.aEnd are
  ///          considered.
  /// \return The include that should own the boundary insertion, or nullptr
  ///         if the boundary policy does not apply.
  const RefoldModel::IncludeItem *
  BoundaryParentIncludeForPureInsertion(const diffutils::Hunk &h) const;

  /// \brief Build a fully specified IncludePatch for a pure INSERT hunk,
  ///        carrying the exact bytes from B.
  ///
  /// Constructs an `IncludePatch` that records the A-token interval
  /// (`h.aStart..h.aEnd`), the B-token interval (`h.bStart..h.bEnd`),
  /// and the literal bytes from **B** computed via `bTokOff_`. The slice is
  /// half-open in token space and byte-accurate, making it suitable for
  /// deterministic, line-local application.
  ///
  /// \param inc       The include item that owns this hunk.
  /// \param h         The diff hunk describing the A/B token intervals.
  /// \returns         A new `IncludePatch` containing the exact insertion
  ///                  payload from **B**.
  IncludePatch BuildIncludeInsertionPatch(const RefoldModel::IncludeItem &inc,
                                          const diffutils::Hunk &h) const;

  /// \brief Checks whether an args-only rewrite of a single macro parameter is
  /// consistent with all **observable** occurrences of that parameter in the
  /// edited preprocessed stream (B), for the given `MacroInvocation`.
  ///
  /// This is the primary safety gate used by args-only macro refolding. It
  /// validates the proposed replacement against:
  /// * STANDARD argument occurrences (non-paste expansions) in B
  /// * STRINGIFY occurrences in B (only when `strict` mode is enabled)
  /// * PASTE occurrences in B when they can be mapped soundly near the edit
  ///   site
  ///
  /// Paste-span validation can be sensitive to local A→B mapping quality, so it
  /// is intentionally conservative: if paste verification cannot be performed
  /// safely, the method prefers to return `true` (do not block args-only)
  /// unless an actual contradiction is detected.
  ///
  /// This wrapper enables paste-span validation (when applicable). For callers
  /// that validate paste-token correctness as a group (e.g., multi-span paste
  /// edits), see `MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste`.
  ///
  /// \param m macro invocation being patched
  /// \param argIdx zero-based macro parameter index being rewritten
  /// \param baseArg original argument spelling in the invocation (used to
  ///                classify paste behavior)
  /// \param newArg proposed new argument spelling for `argIdx`
  /// \param tokenHunks token-level edit hunks on the current file (used to
  ///                   widen B-envelopes at insertion boundaries)
  /// \returns `true` if all verifiable occurrences of `argIdx` in B are
  ///          consistent with the rewrite; `false` if any required occurrence
  ///          contradicts the rewrite.
  enum class OccurrenceSupportMode {
    /// Only metadata attached directly to the current invocation may justify
    /// occurrence-consistency success. Use this for direct args-only patch
    /// construction so descendant/sibling support cannot silently substitute
    /// for missing current-invocation evidence.
    CurrentInvocationOnly,

    /// Descendant/sibling dependency paths may justify occurrence-consistency
    /// success. Use this only inside the semantic/DAG certificate pipeline,
    /// where the caller is already proving carried rewrites through the
    /// invocation graph.
    AllowGraphSupport,
  };

  bool MacroArgReplacementMatchesAllOccurrencesInB(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
      StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans*/ true,
        OccurrenceSupportMode::CurrentInvocationOnly);
  }

  /// \brief Variant of `MacroArgReplacementMatchesAllOccurrencesInB` that
  /// **intentionally skips** per-span paste validation.
  ///
  /// This is used when the caller performs pasted-token validation at a higher
  /// level (e.g., validating that a set of arg replacements reconstructs the
  /// entire pasted token exactly via
  /// `PasteArgReplacementsMatchAllPasteTokensInB`). In that situation,
  /// re-validating each paste span in isolation can cause false negatives,
  /// because:
  /// * Multiple arguments may contribute to a single pasted token.
  /// * One edit hunk may modify multiple pasted segments simultaneously.
  /// * Local A->B mapping can be partial away from the edit site.
  ///
  /// STANDARD occurrences and (in `strict` mode) STRINGIFY occurrences are
  /// still enforced.
  ///
  /// \param m macro invocation being patched
  /// \param argIdx zero-based macro parameter index being rewritten
  /// \param baseArg original argument spelling in the invocation (used to
  ///                classify paste behavior)
  /// \param newArg proposed new argument spelling for `argIdx`
  /// \param tokenHunks token-level edit hunks on the current file (used to
  ///                   widen B-envelopes at insertion boundaries)
  /// \returns `true` if all verifiable non-paste occurrences of `argIdx` in B
  ///          are consistent with the rewrite; `false` if any required
  ///          occurrence contradicts it.
  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
      StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans*/ false,
        OccurrenceSupportMode::CurrentInvocationOnly);
  }

  /// \brief Semantic-proof variant of
  /// `MacroArgReplacementMatchesAllOccurrencesInB`.
  ///
  /// This preserves the existing DAG/subtree/root-proof behavior: when the
  /// current invocation has no direct occurrence metadata for a formal, the
  /// consumer may still accept the rewrite if a descendant/sibling dependency
  /// path proves that the formal participates in occurrence-bearing structure
  /// elsewhere in the invocation graph.
  bool MacroArgReplacementMatchesAllOccurrencesInBSemanticProof(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
      StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans*/ true,
        OccurrenceSupportMode::AllowGraphSupport);
  }

  /// \brief Semantic-proof variant of
  /// `MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste`.
  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
      StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans*/ false,
        OccurrenceSupportMode::AllowGraphSupport);
  }

  /// \brief Core implementation for validating whether a proposed args-only
  /// rewrite of a macro parameter is consistent with the edited preprocessed
  /// stream (B).
  ///
  /// **Goal:** ensure that rewriting parameter `argIdx` from `baseArg` to
  /// `newArg` at the invocation site will not contradict what is observed in B
  /// for the same `MacroInvocation`.
  ///
  /// ### Evidence sources
  /// * **STANDARD spans** (`m.argSpans` with kind `STANDARD`): represent direct
  ///   (non-paste) argument expansions visible in the A stream and alignable
  ///   into B.
  /// * **STRINGIFY spans** (`m.stringifySpans`): represent occurrences produced
  ///   by `#` stringification; checked only when `strict` is enabled.
  /// * **PASTE spans** (`m.pasteSpans`): represent subranges of a pasted token
  ///   (via `##`) attributable to `argIdx`. These are optionally verified.
  ///
  /// ### Conservatism and missing metadata
  /// When `supportMode` is `CurrentInvocationOnly`, missing STANDARD spans only
  /// succeed when the current invocation still carries direct STRINGIFY or
  /// PASTE support for `argIdx`; descendant/sibling support does not count.
  /// When `supportMode` is `AllowGraphSupport`, the semantic proof pipeline may
  /// also use descendant/sibling dependency paths as support.
  ///
  /// ### Stringify rule (strict mode)
  /// When `strict` is enabled and `argIdx` is stringified at least once, every
  /// stringify occurrence must exactly equal `QuoteCString(trimEdgeWs(newArg))`.
  /// If any stringify occurrence disagrees, the method returns `false`.
  ///
  /// ### Paste consumption model for STANDARD spans
  /// When an argument participates in token pasting, STANDARD occurrences may
  /// only expose a prefix/suffix of the argument spelling (the remainder is
  /// consumed into the pasted token). This method infers a coarse consumption
  /// direction using `baseArg` and the A-side pasted subrange, then validates
  /// STANDARD occurrences using equality or prefix/suffix checks as appropriate.
  ///
  /// ### Optional paste-span validation
  /// If `checkPasteSpans` is disabled, paste spans are not validated here
  /// (intended for callers that validate pasted-token correctness holistically).
  /// If enabled, paste spans are only validated when the provided `hintHunk`
  /// appears to touch a paste token, to avoid false negatives caused by
  /// incomplete A->B alignment away from the edit site.
  ///
  /// Paste-span checks validate the *segment* of the pasted token attributable
  /// to `argIdx`, but they explicitly do **not** require the rest of the token
  /// to match between A and B, because multiple arguments may contribute to the
  /// same pasted token and can be edited together.
  ///
  /// \param m macro invocation being patched
  /// \param argIdx zero-based macro parameter index being rewritten
  /// \param baseArg original argument spelling in the invocation (optional)
  /// \param newArg proposed new argument spelling for `argIdx`
  /// \param tokenHunks edit hunks on the B token stream (used to widen
  ///                   B-envelopes at insertion boundaries)
  /// \param checkPasteSpans whether to attempt per-span paste verification
  /// \param supportMode whether no-direct-support cases may consult only the
  ///        current invocation or may also use the invocation graph
  /// \returns `true` if all verifiable occurrences of `argIdx` in B are
  ///          consistent with applying the args-only rewrite; `false` on any
  ///          proven contradiction.
  bool MacroArgReplacementMatchesAllOccurrencesInBImpl(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
      StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks,
      bool checkPasteSpans, OccurrenceSupportMode supportMode) const;

  /// \brief Return the exact A-side occurrence that owns the pure-insertion gap
  ///        at \p aPos.
  ///
  /// Classifies an empty A-side hunk using only exact occurrence structure in
  /// the invocation's PP-token space. A gap may be owned by an occurrence when
  /// it lies strictly inside that occurrence, when it sits on the separator
  /// token immediately before a right-hand occurrence, or when it matches an
  /// occurrence end and no right-hand separator owner takes precedence.
  ///
  /// \param aPos The A-side PP-token gap index of the pure insertion.
  /// \param argSpans The invocation occurrences to test, expressed as half-open
  ///        A-side PP-token spans.
  ///
  /// \returns The index of the uniquely owning occurrence in \p argSpans when
  ///          an exact owner exists; otherwise \c std::nullopt.
  std::optional<size_t> FindExactOwningArgSpanForPureInsertion(
      uint64_t aPos, ArrayRef<RefoldModel::PPArgSpan> argSpans) const;

  /// \brief Return the B-side token range owned by \p span for the pure
  ///        insertion hunk \p h.
  ///
  /// Converts a pure insertion from the raw hunk-local B token range into the
  /// effective occurrence-owned B range used for argument derivation and
  /// cross-occurrence verification. For ordinary interior/begin/end ownership,
  /// the owned range is the raw inserted B range. For the exact
  /// separator-before-right-occurrence case, the owned range is shifted so that
  /// it excludes the shared leading separator and includes the separator that
  /// now precedes the original occurrence in B.
  ///
  /// \param span The occurrence that is being tested as the owner of the pure
  ///        insertion.
  /// \param argSpans All occurrence spans for the same argument, used to
  ///        determine exact separator-before-right-occurrence ownership.
  /// \param mappedEnv The existing mapped B-side token envelope for \p span
  ///        before any pure-insertion extension.
  /// \param h The pure insertion hunk whose owned B range is being computed.
  ///
  /// \returns The half-open B token range owned by \p span when the pure
  ///          insertion is exactly attributable to that occurrence; otherwise
  ///          \c std::nullopt.
  std::optional<std::pair<size_t, size_t>> GetOwnedPureInsertionBRangeForArgSpan(
      const RefoldModel::PPArgSpan &span,
      ArrayRef<RefoldModel::PPArgSpan> argSpans,
      std::pair<size_t, size_t> mappedEnv,
      const diffutils::Hunk &h) const;

  /// \brief Returns `true` if the given hunk touches any token-paste occurrence
  /// recorded for the macro invocation.
  ///
  /// This is used to decide whether a macro edit must be handled by
  /// paste-aware logic (e.g., mapping sub-token edits back into a contributing
  /// argument) rather than treating the edit as a normal argument-splice or
  /// forcing macro realization.
  ///
  /// ### Span conventions
  /// * Replacement/deletion hunks use the half-open interval `[aStart, aEnd)`
  ///   in A-token space.
  /// * Paste spans are treated as half-open intervals `[begin, end)` in
  ///   A-token space.
  ///
  /// Insertion hunks have `aStart == aEnd`. For insertions, we conservatively
  /// treat the hunk as touching a paste span if the insertion point lies on or
  /// within the paste span boundary (`begin <= aStart <= end`). This ensures
  /// that edits inserted exactly at a paste boundary are routed through
  /// paste-aware handling.
  ///
  /// \param m macro invocation metadata (optional)
  /// \param h edit hunk in A-token coordinates (optional)
  /// \returns `true` if the hunk intersects any paste span; otherwise `false`.
  static bool HunkTouchesAnyPasteToken(const RefoldModel::MacroInvocation &m,
                                       const diffutils::Hunk &h);

  /// \brief Attempts to derive a single-argument "paste edit" for a
  /// token-pasting (`##`) macro invocation.
  ///
  /// This is the fast-path used when a hunk touches a `pasteSpans` occurrence.
  /// In this case, the edited region in the preprocessed output may lie
  /// *inside* a single pasted token (e.g., changing `a_b_c` to `a_d_c`), which
  /// cannot be recovered by standard token-to-token argument span matching.
  /// Instead, the refolder tries to attribute the change to exactly one
  /// contributing argument by using the producer-provided `[byteBegin,
  /// byteEnd)` subrange describing each argument's slice within the pasted
  /// token.
  ///
  /// ### High-level algorithm
  /// 1. Collect all `pasteSpans` that intersect the hunk in A-token space.
  /// 2. Map the pasted token envelope from A to B and require that it
  ///    corresponds to exactly one B token (the pasted token).
  /// 3. Compute the minimal differing range between the A pasted token text and
  ///    the B pasted token text. If the token texts are identical, the hunk
  ///    cannot be attributed to paste behavior.
  /// 4. Select exactly one candidate paste span whose `[byteBegin, byteEnd)`
  ///    overlaps the differing region. If multiple argument slices overlap, the
  ///    edit is not representable as a single-argument args-only patch.
  /// 5. Verify that all changes are confined to that chosen byte slice (prefix/
  ///    suffix of the pasted token must match exactly), then return the derived
  ///    `PasteArgEdit`.
  ///
  /// Failure is conservative: this method returns `std::nullopt` unless it can
  /// prove that a single argument slice accounts for *all* changes to the
  /// pasted token. Callers should then fall back to normal occurrence-based arg
  /// patching or macro realization.
  ///
  /// Note: this method compares token *text* slices and strips trailing
  /// newlines from the raw token strings before diffing. It assumes that
  /// `byteBegin`/`byteEnd` are offsets into the pasted token spelling (not the
  /// overall PP stream).
  ///
  /// \param m macro invocation metadata containing `pasteSpans`
  /// \param h edit hunk in A-token coordinates
  /// \returns a `PasteArgEdit` describing a single-argument replacement for the
  ///          pasted token, or `std::nullopt` if no safe single-arg derivation
  ///          exists.
  std::optional<PasteArgEdit>
  DerivePasteArgEdit(const RefoldModel::MacroInvocation &m,
                     const diffutils::Hunk &h) const;

  /// \brief Derives one or more *token-paste* argument edits for a single macro
  /// invocation hunk.
  ///
  /// This helper is used by the args-only macro patching path when the edit
  /// touches a pasted token (i.e., a token produced via `##`). In that
  /// situation, the edited surface text in the preprocessed stream may be a
  /// single identifier that is composed of multiple argument contributions
  /// (e.g., `a_b_c`), and the refolder must attribute the change back to one or
  /// more specific macro arguments.
  ///
  /// The method operates on the **A-stream** (unmodified preprocessed output
  /// produced by Clang) and the edited **B-stream** (user-modified preprocessed
  /// output) by mapping the pasted-token A occurrence to a B token envelope via
  /// `MapAToBTokenEnvelopeByPPArgSpan`. It is intentionally conservative: it
  /// returns `std::nullopt` when it cannot produce a deterministic, unambiguous
  /// decomposition.
  ///
  /// ### High-level algorithm
  /// 1. Collect all `PPArgSpan` entries in `m.pasteSpans` that intersect the
  ///    hunk's A-token interval `[h.aStart, h.aEnd)`.
  /// 2. Use the pasted token's A-token interval `[begin, end)` (from the first
  ///    candidate span) and map it to a B token envelope using
  ///    `MapAToBTokenEnvelopeByPPArgSpan`.
  /// 3. Require that the pasted A-span maps to **exactly one** B token. If the
  ///    edit spans multiple B tokens, args-only refolding is not representable
  ///    and the method returns `std::nullopt`.
  /// 4. Slice the raw pasted token text from A and B and normalize by stripping
  ///    trailing newlines.
  /// 5. Deterministically segment the B token into per-argument regions.
  ///
  /// Segmentation is performed by anchoring on the *fixed* (non-span)
  /// substrings between paste spans in the A token. For a contiguous run of
  /// adjacent spans (no fixed internal anchor), the inversion now computes an
  /// explicit certificate: the run is invertible when unchanged neighboring
  /// segments pin the boundary, and conservatively ambiguous once more than one
  /// touching span in the same run would need to change. This supports edits
  /// such as `a##b##c -> foo##b##c` while still rejecting underdetermined
  /// rewrites like `a##b##c -> foo##bar##c`.
  ///
  /// ### Return value
  /// On success, returns a list of `PasteArgEdit` objects, one per affected
  /// argument, where each entry captures:
  /// * `argIdx`: the macro argument index that contributed to the pasted token.
  /// * `oldSeg`: the original A-token segment contributed by that argument.
  /// * `newSeg`: the corresponding replacement segment inferred from the edited
  ///   B token.
  ///
  /// If no argument contributions actually change (i.e., the pasted token text
  /// is identical), or if multiple spans imply edits for the same `argIdx`
  /// within one pasted token (ambiguous args-only representation), this method
  /// returns `std::nullopt`.
  ///
  /// \param m Macro invocation metadata containing paste span information.
  /// \param h Diff hunk in A/B token space being considered for args-only
  ///          refolding.
  /// \returns A list of per-argument paste edits, or `std::nullopt` if the edit
  ///          cannot be represented deterministically as args-only.
  std::optional<std::vector<PasteArgEdit>>
  DerivePasteArgEdits(const RefoldModel::MacroInvocation &m,
                      const diffutils::Hunk &h) const;

  /// \brief Segments the edited pasted-token spelling (`bTok`) into
  /// per-argument substrings by using the original pasted-token spelling
  /// (`aTok`) as an anchor template.
  ///
  /// The input `spansAsc` identifies the argument-contributed regions inside
  /// `aTok`: each `PPArgSpan` provides `[byteBegin, byteEnd)` offsets that
  /// delimit the portion of `aTok` produced by one argument during token
  /// pasting. The remaining characters in `aTok` (the gaps between spans, and
  /// the tail after the final span) are treated as *fixed* slices that must
  /// appear unchanged and in order in `bTok`.
  ///
  /// This method returns a list of segments where `segs[i]` is the substring of
  /// `bTok` corresponding to `spansAsc[i]`. It succeeds only when the
  /// segmentation is lexically deterministic:
  /// * All fixed slices from `aTok` match `bTok` in-order.
  /// * Each span's boundary in `bTok` can be determined by locating the next
  ///   fixed slice that follows it (or by consuming the remaining suffix for
  ///   the final span).
  ///
  /// Length-changing edits are supported (e.g. `a_b_c -> foo_bar_baz`) as long
  /// as fixed slices, or unchanged neighboring span spellings, uniquely pin the
  /// segment boundaries. For a touching run with no fixed internal delimiter,
  /// the inversion accepts at most one edited span in that run; once two
  /// neighboring span contributions both need to move, the boundary becomes
  /// underdetermined and this method returns `std::nullopt`.
  ///
  /// ### Implementation notes
  /// * `spansAsc` must be non-overlapping, sorted by `byteBegin` ascending, and
  ///   within `[0, aTok.length()]`; otherwise `std::nullopt` is returned.
  /// * The search for fixed anchors in `bTok` is memoized by `(idx, posB)` so
  ///   repeated suffix states are solved once while preserving the existing
  ///   left-to-right acceptance order.
  ///
  /// \param aTok The original pasted-token spelling from the unedited
  ///             preprocessed stream (A).
  /// \param bTok The edited pasted-token spelling from the edited preprocessed
  ///             stream (B).
  /// \param spansAsc Argument-contributed spans inside `aTok`, sorted by
  ///                 `byteBegin`.
  /// \returns A list of per-arg segment strings from `bTok` aligned to
  ///          `spansAsc`, or `std::nullopt` if segmentation is not provably
  ///          deterministic.
  static std::optional<std::vector<std::string>>
  SegmentPastedTokenArgsByFixedSlices(
      StringRef aTok, StringRef bTok,
      ArrayRef<const RefoldModel::PPArgSpan *> spansAsc);

  /// \brief Derives the replacement text for a pasted-token sub-segment
  /// (`oldSeg`) implied by an args-only rewrite from `baseArg` to `newArg`.
  ///
  /// In token-paste macros (`##`), an argument may contribute only a prefix or
  /// suffix of its spelling to the final pasted token. A paste-span records the
  /// exact subrange of the pasted token that originated from the argument under
  /// the *original* invocation spelling. When the invocation argument is
  /// rewritten, we need to compute the corresponding sub-segment that should
  /// appear in the pasted token after the rewrite.
  ///
  /// This helper implements a simple, deterministic model:
  /// * If `oldSeg` equals the full trimmed `baseArg`, the entire argument was
  ///   pasted, so the new segment is the full trimmed `newArg`.
  /// * If `baseArg` starts with `oldSeg`, then `oldSeg` is a pasted prefix.
  ///   The suffix remainder of `baseArg` must still appear as a suffix of
  ///   `newArg`, and the returned segment is `newArg` with that suffix removed.
  /// * If `baseArg` ends with `oldSeg`, then `oldSeg` is a pasted suffix.
  ///   The prefix remainder of `baseArg` must still appear as a prefix of
  ///   `newArg`, and the returned segment is `newArg` with that prefix removed.
  ///
  /// If `oldSeg` cannot be classified as the full argument, a prefix, or a
  /// suffix of `baseArg`, or if `newArg` is not structurally compatible with
  /// the inferred classification, this returns an empty `StringRef()`.
  ///
  /// \param baseArg original argument spelling (as written at the invocation
  ///                site)
  /// \param newArg proposed new argument spelling
  /// \param oldSeg sub-segment from the pasted token that originated from
  ///               `baseArg`
  /// \returns the pasted-token replacement segment implied by
  ///          `baseArg -> newArg`, or an empty `StringRef()` if the segment
  ///          cannot be derived soundly.
  static StringRef DeriveNewPasteSegmentFromSpellingReplacement(
      StringRef baseArg, StringRef newArg, StringRef oldSeg);

  /// \brief Validates that a set of candidate argument replacements
  /// (`replByArgIdx`) reproduces the edited spelling of every token produced
  /// via `##` token-pasting for a macro invocation.
  ///
  /// This is a strict safety gate used by args-only macro patching when the
  /// macro body contains token-paste operations. In token-pasting macros, the
  /// preprocessed output often contains *composite* identifiers (or other
  /// tokens) built from multiple argument contributions. When we attempt to
  /// refold by modifying only the invocation spelling (instead of realizing the
  /// macro body), we must ensure that the resulting pasted-token text in the
  /// edited preprocessed stream is exactly reproduced.
  ///
  /// The producer emits `PPArgSpan` entries for paste contributions. Each
  /// `PPArgSpan` in `m.pasteSpans` describes:
  /// * Which invocation argument contributed (`argIdx`).
  /// * The pasted-token occurrence in the A-stream via `[begin, end)` token
  ///   indices.
  /// * The sub-token character slice inside the pasted token via
  ///   `[byteBegin, byteEnd)`.
  ///
  /// This method groups paste spans by pasted-token occurrence, maps each
  /// A occurrence to a corresponding single-token envelope in B using
  /// `MapATokRangeAToBTokenEnvelope`, then simulates rewriting the A pasted
  /// token by applying per-span segment updates derived from invocation
  /// argument replacements. The simulation must match the edited B pasted
  /// token exactly.
  ///
  /// ### Key invariants / constraints
  /// * Each pasted-token occurrence must map to exactly one token in A and B.
  /// * Segment rewrites are applied right-to-left by `byteBegin` to keep
  ///   offsets stable.
  /// * Segment updates are derived deterministically from:
  ///   `baseInvText` + `invArgRanges` (original arg spelling), the candidate
  ///   replacement spelling, and the original pasted-token slice (`oldSeg`).
  /// * If any rewritten pasted token does not equal the B token spelling, this
  ///   returns `false` and the caller must fall back to macro realization
  ///   (expansion).
  ///
  /// \param m Macro invocation metadata (must contain `pasteSpans`).
  /// \param baseInvText Invocation spelling text (e.g., "CONCAT(a,b,c)").
  /// \param invArgRanges Argument content ranges within `baseInvText`.
  /// \param replByArgIdx Candidate replacements per argument index.
  /// \returns `true` if the replacements reproduce every pasted token
  ///          occurrence in B.
  bool PasteArgReplacementsMatchAllPasteTokensInB(
      const RefoldModel::MacroInvocation &m, StringRef baseInvText,
      ArrayRef<std::pair<size_t, size_t>> invArgRanges,
      const DenseMap<uint32_t, std::string> &replByArgIdx) const;

  /// \brief Splices a derived paste-segment edit into a macro argument's
  /// spelling text.
  ///
  /// This helper is used after `derivePasteArgEdit` determines that an edit
  /// inside a token-pasted (`##`) output token can be attributed to a single
  /// argument slice. The goal is to rewrite the corresponding invocation
  /// argument so that re-preprocessing reproduces the edited pasted token in
  /// B_PP, without realizing (expanding) the macro.
  ///
  /// The splice is intentionally conservative: it only applies when `oldSeg`
  /// matches an unambiguous boundary region of the argument spelling (after
  /// trimming edge whitespace):
  /// * Whole-argument replacement: `baseTrim == oldTrim`
  /// * Prefix splice: `baseTrim` starts with `oldTrim`
  /// * Suffix splice: `baseTrim` ends with `oldTrim`
  ///
  /// If `oldTrim` matches both the prefix and suffix (e.g., repeated text) or
  /// matches neither, the splice is rejected and an empty string is returned.
  /// This avoids ambiguous rewrites that could change the meaning of the macro
  /// invocation or fail idempotence under re-preprocessing.
  ///
  /// Whitespace handling: all comparisons are performed on edge-trimmed forms,
  /// and the returned value is constructed from the trimmed base text. Callers
  /// are responsible for re-integrating any desired formatting or argument
  /// quoting policy around the returned string.
  ///
  /// \param baseArg the original invocation argument spelling
  /// \param oldSeg the segment text contributed by the argument in the pasted
  ///               token (A stream)
  /// \param newSeg the replacement segment text observed in the edited pasted
  ///               token (B stream)
  /// \returns the rewritten argument spelling if a safe boundary splice is
  ///          possible; otherwise an empty string.
  static std::string SplicePasteSegmentIntoSpellingArg(StringRef baseArg,
                                                       StringRef oldSeg,
                                                       StringRef newSeg);

  /// \brief Splice a replay-derived paste edit through an exact producer slice.
  ///
  /// This is the non-inferential counterpart to
  /// `SplicePasteSegmentIntoSpellingArg`. The producer has already recorded the
  /// byte range inside the original invocation argument that supplied the paste
  /// part, so the consumer only verifies that the range still spells `oldSeg`
  /// and then replaces exactly that range with `newSeg`.
  static std::string SplicePasteSegmentIntoSpellingArgExact(
      StringRef baseArg, uint32_t argByteBegin, uint32_t argByteEnd,
      StringRef oldSeg, StringRef newSeg);

  /// \brief Return per-formal argument content ranges for a function-like macro
  /// invocation.
  ///
  /// The returned vector is indexed by **formal parameter index** (not by the
  /// number of "actual" comma-separated arguments in the invocation text). Each
  /// entry is a half-open byte range **`[begin, end)`** into `invText` that
  /// selects the *content* of the corresponding argument (excluding surrounding
  /// whitespace and outer delimiters as produced by the parser).
  ///
  /// ## Preferred source: producer-provided ranges
  ///
  /// If the refold-map contains `m.invArgRanges`, those byte ranges are treated
  /// as authoritative. They are recorded in the **B-domain** (absolute file
  /// offsets), so they are first validated and rebased relative to `m.invB` to
  /// produce indices into `invText`.
  ///
  /// This path is **required for variadic macros**, because multiple "actual"
  /// arguments may correspond to a single formal (e.g. `__VA_ARGS__` or a GNU
  /// named variadic like `args...`). In that case the *last formal* range spans
  /// the entire variadic tail, including separating commas.
  ///
  /// ## Recovery: conservative textual parse
  ///
  /// If some producer-provided ranges are missing/invalid, this routine
  /// attempts to fill them using a conservative parse of the invocation
  /// spelling. When the parsed arity differs from the formal arity:
  ///
  /// * **`actualN == formalN`**: fill missing entries 1:1.
  /// * **`actualN > formalN`**: treat as variadic and merge the remaining tail
  ///   into the last formal range.
  /// * **`actualN < formalN`**: treat trailing formals as empty (e.g. empty
  ///   `__VA_ARGS__`) by producing an empty range at the close-paren.
  ///
  /// Returns `std::nullopt` if the invocation cannot be parsed or if required
  /// anchoring information (e.g. `m.invB`) is unavailable.
  static std::optional<std::vector<std::pair<size_t, size_t>>>
  GetMacroInvocationFormalArgContentRanges(
      const RefoldModel::MacroInvocation &m, StringRef invText);

  /// \brief Attempts to build an *args-only* macro invocation patch for a hunk
  /// attributed to a macro invocation.
  ///
  /// An args-only patch rewrites only the invocation argument text (e.g.,
  /// `MACRO(x,y)`), leaving the macro definition unchanged, such that
  /// re-preprocessing the refolded TU reproduces the edited preprocessed
  /// stream `B_PP`. This is preferred over forcing macro realization because
  /// it preserves the original source structure and avoids pulling expanded
  /// tokens into the TU.
  ///
  /// This method enforces a strict safety contract:
  /// * The invocation must have a concrete TU byte range (`m.invB`/`m.invE`).
  /// * The hunk must be fully contained within one or more producer-recorded
  ///   argument-occurrence spans for this invocation (standard args and
  ///   stringify args).
  /// * For each touched argument, the derived replacement must be consistent
  ///   with *all* of its occurrences in `B_PP` (standard, paste, and stringify,
  ///   subject to strict/non-strict policy).
  /// * If any touched occurrence implies conflicting replacements for the same
  ///   `argIdx`, the patch is rejected.
  ///
  /// ### Paste-aware fast path
  /// If the hunk touches any `pasteSpans` occurrence, the edit may be a
  /// sub-token change inside a single pasted token (e.g., `a_b_c -> a_d_c`).
  /// In that case, the method attempts to derive per-arg segment edits and
  /// splice them into the corresponding invocation arguments via
  /// `SplicePasteSegmentIntoSpellingArg`.
  ///
  /// For multi-span paste edits, per-arg paste-span consistency cannot be
  /// validated in isolation when token lengths may change. Instead, the method
  /// first validates standard + stringify occurrences for each touched argument
  /// (ignoring paste spans), then applies a combined safety gate:
  /// `PasteArgReplacementsMatchAllPasteTokensInB(...)` must reconstruct every
  /// pasted token occurrence exactly as observed in B.
  ///
  /// For single-segment paste edits, the method may validate all occurrences
  /// (including paste spans) directly via
  /// `MacroArgReplacementMatchesAllOccurrencesInB(...)`.
  ///
  /// If paste derivation fails, the method falls through to the standard
  /// (non-paste) policy below.
  ///
  /// ### Standard (non-paste) path
  /// The method collects standard arg spans and stringify spans, requires the
  /// hunk to be covered by those spans, then derives candidate argument
  /// replacements by mapping each touched occurrence to a B token envelope via
  /// `MapAToBTokenEnvelopeByPPArgSpan`. It slices the corresponding edited B
  /// text and interprets it as either:
  /// * Raw token text for standard occurrences, or
  /// * Unstringified argument text for stringify occurrences.
  ///
  /// When an argument participates in token pasting, the method may attempt a
  /// "lift/paste" rewrite: it searches for the A occurrence slice inside the
  /// base argument spelling and replaces only that slice with the edited B
  /// slice.
  ///
  /// On success, returns a `MacroPatch` replacing `[m.invB, m.invE)` in the TU
  /// byte stream with a newly constructed invocation string. On failure,
  /// returns `std::nullopt`, signaling that the caller should fall back to
  /// macro realization/expansion.
  ///
  /// \param m Macro invocation metadata (argument spans, stringify spans,
  ///          paste spans, and TU byte range).
  /// \param h Edit hunk attributed to this macro invocation (A/B token
  ///          coordinates).
  /// \param baseInvocationText The original invocation source text to be
  ///                           rewritten (e.g., `MACRO(...)`).
  /// \returns A `MacroPatch` for args-only invocation rewriting, or
  ///          `std::nullopt` if not provably safe.
  std::optional<MacroPatch> BuildMacroInvocationPatchArgsOnly(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      StringRef baseInvocationText) const;

  /// \brief Slices a source text by token indices using a token-to-byte offset
  /// table.
  ///
  /// This helper converts a half-open token interval `[startTok, endTok)` into
  /// a byte-offset range using `tokOff` and returns the corresponding
  /// `StringRef` of `source`.
  ///
  /// `tokOff` is the token-to-byte offset table:
  /// * `tokOff[i]` is the starting byte offset of token `i`.
  /// * The table is expected to have length `(numTokens + 1)`, where the final
  ///   entry `tokOff[numTokens]` equals `source.size()` (end sentinel).
  ///
  /// For robustness, this method clamps token indices into the valid table
  /// range and clamps derived byte offsets into `[0, source.size()]`.
  ///
  /// \param tokOff   Token-to-byte offset table.
  /// \param source   The source text to slice.
  /// \param startTok Inclusive start token index.
  /// \param endTok   Exclusive end token index.
  /// \returns A `StringRef` of the source covered by tokens `[startTok,
  /// endTok)`.
  static StringRef SliceSource(ArrayRef<size_t> tokOff, StringRef source,
                               uint64_t startTok, uint64_t endTok);

  /// Slices the A-side source text by token indices.
  /// \see SliceSource
  StringRef SliceASource(uint64_t aStartTok, uint64_t aEndTok) const {
    return SliceSource(aTokOff_, aSource_, aStartTok, aEndTok);
  }

  /// Slices the B-side source text by token indices.
  /// \see SliceSource
  StringRef SliceBSource(uint64_t bStartTok, uint64_t bEndTok) const {
    return SliceSource(bTokOff_, bSource_, bStartTok, bEndTok);
  }

  /// \brief Attempts to invert a string literal token that was produced by
  /// macro stringification (`#param`) back into a single invocation-site
  /// argument spelling.
  ///
  /// In the preprocessor, `#X` produces a string literal token (e.g.,
  /// `"hello-world"`) even though the invocation argument text was
  /// `hello-world`. During refolding, edits to that produced literal may be
  /// projected back onto the invocation site by "unstringifying" the literal
  /// and using the result as the replacement argument text.
  ///
  /// This routine is intentionally conservative and only supports ordinary and
  /// prefixed string literals that look like one token:
  /// * ` "..." `
  /// * ` L"..." ` (wide)
  /// * ` u"..." ` / ` U"..." ` / ` u8"..." ` (Unicode)
  ///
  /// The body is extracted from the first double-quote through the final
  /// double-quote and is minimally unescaped: `\\` and `\"` are reduced to
  /// `\` and `"`, respectively. Other escape sequences are preserved verbatim
  /// (the backslash and following character are kept) to avoid changing
  /// semantics.
  ///
  /// ### Safety checks
  /// The resulting argument text must be representable as a single macro
  /// argument. If the unescaped body contains a comma, or contains a newline,
  /// this method returns `std::nullopt` to force callers to take a conservative
  /// path (e.g., expand the macro instance) rather than producing an ambiguous
  /// or syntactically invalid invocation such as `FOO(a, b)` when the macro has
  /// only one parameter.
  ///
  /// \param literalTok A token string that should represent a single C/C++
  ///                   string literal.
  /// \returns The corresponding invocation-site argument spelling, or
  ///          `std::nullopt` if the literal is not recognized or cannot be
  ///          safely mapped to a single argument.
  std::optional<std::string>
  UnstringifyLiteralToArgText(StringRef literalTok,
                              bool allowTopLevelComma = false) const;

  /// \brief Determines if a given edit hunk is entirely contained within a set
  /// of macro argument spans.
  ///
  /// This method serves as a safety gate for "surgical" macro patching. It
  /// ensures that an edit (the `Hunk`) only touches the tokens corresponding to
  /// macro parameters and does not overlap with the macro's "boilerplate" body
  /// text (e.g., operators, semicolons, or literal constants defined in the
  /// macro body).
  ///
  /// The containment logic differs based on the type of edit:
  ///
  /// - **Insertions (aStart == aEnd):** Attributed to an argument if the
  ///   insertion point `a0` falls within the *inclusive* range `[begin, end]`
  ///   of an argument span. This allows for prepending or appending to an
  ///   argument's content.
  /// - **Replacements/Deletions (aStart < aEnd):** Every token index in the
  ///   interval `[aStart, aEnd)` must be contained within at least one argument
  ///   span. This ensures no part of the macro's structural body is modified.
  ///
  /// As a side effect, the `touched` array is updated to track which specific
  /// span occurrences from the provided `argSpans` list are affected by the
  /// hunk. Callers that need per-formal state must compress these occurrence
  /// touches using each span's `argIdx`.
  ///
  /// \param h The hunk representing the edit in the original preprocessed (A)
  ///          token stream.
  /// \param argSpans A list of token spans representing the regions where macro
  ///                 parameters appear in the expanded preprocessed output.
  /// \param touched An output array of the same length as `argSpans`, where
  ///                indices will be set to `true` if the hunk overlaps that
  ///                specific argument.
  /// \return `true` if the hunk is fully contained within the provided argument
  ///         spans; `false` if any part of the hunk touches non-argument tokens
  ///         or falls outside the known argument regions.
  bool HunkFullyWithinArgSpans(const diffutils::Hunk &h,
                               ArrayRef<RefoldModel::PPArgSpan> argSpans,
                               MutableArrayRef<char> touched) const;

  /// \brief Converts a list of token-based edit hunks into byte-based hunks.
  ///
  /// This routine projects the token indices in \p tokenHunks back onto the raw
  /// source byte streams (A and B) using the pre-computed token offset arrays.
  ///
  /// The transformation accounts for the sentinel offsets at the end of each
  /// token stream, allowing hunks that point to the end of the file to be
  /// resolved correctly. Input indices are clamped to valid token ranges to
  /// ensure safety against alignment anomalies.
  ///
  /// \returns A vector of \c Hunk objects containing the corresponding
  ///          [begin, end) byte offsets in the A and B source buffers.
  std::vector<diffutils::Hunk> BuildByteHunksFromRawText() const;

  /// \brief Build the prefix-summed A->B byte delta cache for
  /// \c abByteHunks_.
  void BuildByteHunkPrefixDeltaCache();

  /// \brief Projects a byte offset from source A to source B using
  /// lower-bound semantics.
  ///
  /// This mapping resolves where a specific coordinate in the original
  /// preprocessed stream (A) lands in the edited stream (B). If the
  /// coordinate falls within a range that was deleted or replaced, it
  /// snaps to the beginning of the replacement in B.
  ///
  /// \param aByte The byte offset in the original preprocessed stream A.
  /// \returns The corresponding byte offset in the edited stream B.
  size_t MapAByteToBByteLowerBound(size_t aByte) const;

  /// \brief Projects a byte offset from source A to source B using
  /// upper-bound semantics.
  ///
  /// Similar to the lower-bound mapping, this projects coordinates from A
  /// to B, but treats boundaries differently to ensure inclusive ranges.
  /// If aByte falls exactly at the start of an insertion, the mapping
  /// includes that insertion in the resulting B offset. If it falls inside
  /// a replaced range, it snaps to the end of the replacement in B.
  ///
  /// \param aByte The byte offset in the original preprocessed stream A.
  /// \returns The corresponding byte offset in the edited stream B.
  size_t MapAByteToBByteUpperBound(size_t aByte) const;

  /// \brief Finds the largest token index `i` such that `bTokOff_[i] <= bByte`.
  ///
  /// This performs a floor-based binary search on the B-stream token offsets.
  /// It identifies the token that contains the given byte offset or the most
  /// recent token starting before it.
  ///
  /// \param bByte The byte offset in the edited preprocessed stream B.
  /// \returns The index of the token corresponding to the floor of \p bByte.
  size_t BTokIndexFloor(size_t bByte) const;

  /// \brief Finds the smallest token index `i` such that `bTokOff_[i] >= bByte`.
  ///
  /// This performs a ceiling-based binary search on the B-stream token offsets.
  /// It is typically used to find the exclusive end-boundary of a token range
  /// corresponding to a byte-level span.
  ///
  /// \param bByte The byte offset in the edited preprocessed stream B.
  /// \returns The index of the token corresponding to the ceiling of \p bByte.
  size_t BTokIndexCeil(size_t bByte) const;

  /// \brief Maps a byte range in source A to a token envelope in source B.
  ///
  /// This is the core mapping routine that projects a character-level span
  /// from the original preprocessed stream onto a discrete range of tokens
  /// in the edited stream. It accounts for edit hunks to find the correct
  /// B-space byte offsets before performing a binary search to find the
  /// containing tokens.
  ///
  /// \param aByteBegin The starting byte offset in source A.
  /// \param aByteEnd The ending byte offset (exclusive) in source A.
  /// \returns A pair representing the [begin, end) token indices in source B.
  std::pair<size_t, size_t>
  MapAByteRangeToBTokenEnvelope(size_t aByteBegin, size_t aByteEnd) const;

  /// \brief Maps an A byte range to a B token envelope while preserving
  /// boundary insertions.
  ///
  /// This variant is used when the mapped range must retain B-side insertions
  /// that occur exactly at the projected A-range boundaries. Unlike the plain
  /// envelope mapper, it treats boundary-adjacent inserted tokens as part of
  /// the resulting B envelope when they are associated with the mapped range
  /// rather than with surrounding untouched text.
  ///
  /// \param aByteBegin The starting byte offset in source A.
  /// \param aByteEnd The ending byte offset (exclusive) in source A.
  /// \returns A pair representing the [begin, end) token indices in source B.
  std::pair<size_t, size_t>
  MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(
      size_t aByteBegin, size_t aByteEnd) const;

  /// \brief Maps a macro argument span to its corresponding B-token envelope.
  ///
  /// This function prioritizes high-precision preprocessor byte offsets stored
  /// in the \p PPArgSpan. If those offsets are missing or invalid, it falls
  /// back to using the token-index-based offsets from the consumer stream.
  ///
  /// \param sp The macro argument span metadata from the RefoldModel.
  /// \returns The B-token range if mapping is successful, std::nullopt
  /// otherwise.
  std::optional<std::pair<size_t, size_t>>
  MapAToBTokenEnvelopeByPPArgSpan(const RefoldModel::PPArgSpan &sp) const;

  /// \brief Maps a token range in source A to a token envelope in source B.
  ///
  /// Converts a token-index-based range from the original stream into a
  /// byte-offset range, then projects that range into the edited stream's
  /// token space. This is used when identifying the B-side impact of edits
  /// defined by A-side token boundaries.
  ///
  /// \param beginTok The starting token index in source A.
  /// \param endTok The ending token index (exclusive) in source A.
  /// \returns The B-token range if the input is valid, std::nullopt otherwise.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelope(uint64_t beginTok, uint64_t endTok) const;

  /// \brief Maps an A token range to a B token envelope while preserving
  /// boundary insertions.
  ///
  /// Converts an A-side token range into its corresponding A byte range, then
  /// projects that range into B token space using the boundary-preserving
  /// envelope mapper. This keeps B-side insertions that occur at the projected
  /// range boundaries attached to the mapped token envelope when they belong to
  /// the range being materialized.
  ///
  /// \param beginTok The starting token index in source A.
  /// \param endTok The ending token index (exclusive) in source A.
  /// \returns The B-token range if the input is valid, std::nullopt otherwise.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
      uint64_t beginTok, uint64_t endTok) const;

  /// \brief Map an A-token range to its B-token envelope for whole-cover
  /// replacement.
  ///
  /// Unlike \c MapATokRangeAToBTokenEnvelope, this preserves pure-insertion
  /// payloads anchored exactly at the A-range boundaries. Whole-cover macro
  /// replacement wants the full B-side image of the cover, and later claim
  /// clipping is responsible for preventing duplicate emission of standalone
  /// boundary insertions.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopeWholeCover(uint64_t beginTok,
                                          uint64_t endTok) const;

  /// \brief Variant of `MapATokRangeAToBTokenEnvelope` that trims *edge* pure
  /// insertion hunks from the resulting B-token envelope.
  ///
  /// Rationale:
  ///   Byte-level envelope mapping can legally absorb a pure-insertion hunk
  ///   anchored exactly at `beginTok` or `endTok` into the mapped B envelope
  ///   for the interior A span. For whole-cover macro replacement, those edge
  ///   insertions are outside the macro cover and are applied separately; if
  ///   they are also included in the whole-cover slice, the insertion material
  ///   is duplicated. This method enforces a deterministic "no absorption of
  ///   edge insertions" rule by trimming any token-level pure-insertion hunks
  ///   anchored at the requested A-span boundaries.
  ///
  /// This method is intended for *whole-cover* mapping where the A-span is a
  /// coverage interval (e.g. a macro cover). It should not be used for mapping
  /// argument-content spans where an insertion at the span boundary may be
  /// semantically part of the argument.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(uint64_t beginTok,
                                                  uint64_t endTok) const;

  /// \brief Resolve an include-realization B-token envelope from an A-cover.
  ///
  /// The canonical include-realization path uses
  /// \c MapATokRangeAToBTokenEnvelope to recover the exact B-side token
  /// envelope for the include cover.  When that mapper cannot prove an
  /// envelope, Phase 8b permits exactly one additional declared proof:
  /// BoundaryStableConsensusBCoverEnvelope.
  ///
  /// That proof is deliberately narrow.  The available non-canonical boundary
  /// projections must agree on one non-empty B-token range; otherwise B-envelope
  /// recovery remains an explicit out-of-domain terminal case rather than a
  /// guessed realization.
  std::optional<std::pair<size_t, size_t>>
  ResolveIncludeRealizationBTokenEnvelope(
      uint64_t beginTok, uint64_t endTok,
      IncludeRealizationEvidenceKind *evidenceKind = nullptr) const;

  /// \brief Parses the raw text of a function-like macro invocation to identify
  /// the byte ranges of its individual arguments.
  ///
  /// This method performs a shallow scan of the invocation text starting from
  /// the opening parenthesis. It follows macro-argument collection rules rather
  /// than C expression/list splitting rules: only nested parentheses protect a
  /// comma from separating arguments. Brackets and braces are ordinary
  /// preprocessing tokens here, so `M(arr[1, 2], 3)` is parsed as three macro
  /// arguments while `M((1, 2), 3)` is parsed as two.
  ///
  /// The parser is also comment-, string-, and character-literal aware; it
  /// skips over escaped characters and delimiters inside opaque tokens to avoid
  /// misinterpreting their text as macro argument separators.
  ///
  /// \param invText The full source text of the macro invocation
  ///                (e.g., "MY_MACRO(a, f(b, c))").
  /// \return A list of `[start, end]` byte ranges for each argument, with
  ///         leading and trailing whitespace trimmed; returns `std::nullopt` if
  ///         the text is malformed or the closing parenthesis is missing.
  static std::optional<std::vector<std::pair<size_t, size_t>>>
  ParseMacroInvocationArgContentRanges(StringRef invText);

  // ------------------------- __COUNTER__ stabilization -----------------------

  /// \brief Determine which macro invocations must be forced to remain expanded
  /// in order to stabilize edited __COUNTER__ semantics.
  ///
  /// Policy:
  ///   If any expanded __COUNTER__ occurrence is edited in B (relative to its
  ///   A-side expansion token(s)), then that occurrence and every subsequent
  ///   __COUNTER__ occurrence in PP-token order must be emitted as a literal
  ///   expansion (whole-cover), even if unchanged. This preserves the edited
  ///   preprocessed semantics because replacing a __COUNTER__ site with a
  ///   literal stops the counter from incrementing, which would otherwise shift
  ///   all later __COUNTER__ values.
  ///
  /// The producer emits a MacroInvocation item for each __COUNTER__ expansion
  /// with A-token coverage.  Precise body/spans are preferred, but a fallback
  /// cover is also a valid producer anchor.  Zero-length anchors are normalized
  /// to the concrete following A-token before edit detection, so pasted or
  /// stringified counter literals still participate in stabilization instead of
  /// escaping through a separate fallback path.  We use the A→B alignment map
  /// \p a2b to detect the first edited occurrence.
  ///
  /// Important: a __COUNTER__ occurrence may be spelled inside a macro
  /// definition's replacement list (e.g. "#define PRINT(...) __COUNTER__"). In
  /// that case, patching the __COUNTER__ invocation span would illegally mutate
  /// the macro definition. Instead, we must force expansion of the *smallest
  /// patchable macro callsite* that covers the __COUNTER__ expansion token(s)
  /// (e.g. the PRINT(...) call), and then force all later occurrences
  /// similarly.
  ///
  /// This function therefore returns the set of covering, patchable macro
  /// invocations that should be whole-cover expanded.
  struct ForcedMacroPatchRequest {
    const RefoldModel::MacroInvocation *macro = nullptr;
    uint64_t aStart = 0;
    uint64_t aEnd = 0;
    CounterEventIdentity event;
  };

  struct CounterOccurrence {
    const RefoldModel::MacroInvocation *macro;
    uint64_t aStart;
    uint64_t aEnd;
    std::optional<uint64_t> ownerIncludeId;
    CounterEventIdentity event;
  };

  /// Return the producer-backed A-token ranges for one `__COUNTER__`
  /// invocation.  Body spans are preferred over explicit spans, which are
  /// preferred over the conservative cover.  Zero-width anchors are preserved
  /// here and widened by BuildCounterEventIdentity() so every caller uses one
  /// deterministic event normalizer.
  std::vector<std::pair<uint64_t, uint64_t>>
  CounterOutputRangesForInvocation(
      const RefoldModel::MacroInvocation &macro) const;

  /// Format the A-side counter value represented by a normalized token range.
  std::string CounterValueForTokenRange(uint64_t begin, uint64_t end) const;

  /// Build the Phase-3G identity for one concrete counter event.
  CounterEventIdentity BuildCounterEventIdentity(
      const RefoldModel::MacroInvocation &macro, uint64_t occurrenceOrdinal,
      uint64_t begin, uint64_t end,
      std::optional<uint64_t> ownerIncludeOverride = std::nullopt) const;

  SmallVector<CounterOccurrence, 32>
  CollectCounterOccurrences(StringRef tuPath) const;

  void SortCounterOccurrences(SmallVectorImpl<CounterOccurrence> &occs) const;

  /// \brief Compute forced expansion patches needed to stabilize __COUNTER__
  /// semantics.
  ///
  /// If any expanded __COUNTER__ occurrence is edited in B, then that
  /// occurrence and every subsequent __COUNTER__ occurrence (in PP-token order)
  /// must be emitted as a literal expansion, even if unchanged. This preserves
  /// the edited preprocessed semantics because replacing a __COUNTER__ site
  /// with a literal stops the counter from incrementing, which would otherwise
  /// shift all later
  /// __COUNTER__ values.
  ///
  /// Important: a __COUNTER__ occurrence may be spelled inside a macro
  /// definition's replacement list (e.g. "#define PRINT(...) __COUNTER__"). In
  /// that case, patching the __COUNTER__ invocation span would illegally mutate
  /// the macro definition. Instead, we force expansion of the smallest
  /// patchable macro callsite that covers the __COUNTER__ expansion token(s)
  /// (e.g. the PRINT(...) callsite), and then force all later occurrences
  /// similarly.
  ///
  /// The producer may record multiple body occurrences for a single invocation
  /// spelling (e.g. when a header is included multiple times). We therefore
  /// treat each body-span occurrence separately and filter by the true PP-owner
  /// include.
  SmallVector<ForcedMacroPatchRequest, 32>
  ComputeForcedCounterPatches(StringRef tuPath, ArrayRef<int64_t> a2b) const;

  /// \brief Compute additional forced __COUNTER__ stabilization patches caused
  ///        by macro callsites that already remain expanded after normal hunk
  ///        attribution.
  ///
  /// If a normal macro patch expands a callsite whose expansion contains a
  /// __COUNTER__ occurrence, recompiling the refolded source would stop that
  /// callsite from incrementing the counter. Every later occurrence in PP-token
  /// order must then be forced to remain expanded as well.
  SmallVector<ForcedMacroPatchRequest, 32>
  ComputeForcedCounterPatchesFromExpandedMacros(
      StringRef tuPath,
      const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
          &macroPatchByOwnerByMacroId) const;

  /// \brief Inject forced __COUNTER__ stabilization patches after normal hunk
  /// attribution.
  ///
  /// __COUNTER__ expansions are time-dependent and can shift when unrelated
  /// edits add/remove counter uses earlier in the stream. After the main hunk
  /// attribution pass, this routine injects additional whole-cover MacroPatches
  /// for each forced occurrence so that all __COUNTER__ sites match the edited
  /// preprocessed stream, even if no diff hunk directly touched the invocation.
  void AddForcedCounterPatches(
      ArrayRef<ForcedMacroPatchRequest> forced,
      DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
          &macroPatchByOwnerByMacroId) const;

  /// \brief Compute the A-token interval used for whole-cover replacement of
  ///        a macro invocation.
  ///
  /// This usually matches \c m.cover, but some zero-parameter function-like
  /// macros are conservatively widened by the producer when nested in parent
  /// arguments. In those cases, the precise replacement slice is derived from
  /// the bounding box of \c bodySpans.
  std::optional<std::pair<uint64_t, uint64_t>>
  GetWholeCoverATokRange(const RefoldModel::MacroInvocation &m) const;

  /// \brief Return true when the invocation's whole-cover replacement surface
  ///        is self-contained at the callsite.
  ///
  /// Whole-cover replacement is only valid when every A token in the chosen
  /// cover interval is claimed by this invocation through its own body, arg,
  /// stringify, or paste spans. Nested child invocations whose emitted tokens
  /// are interleaved with parent-owned syntax are not self-contained and must
  /// be lifted through an ancestor rather than replaced at the child callsite.
  bool MacroWholeCoverIsSelfContained(
      const RefoldModel::MacroInvocation &m) const;

  /// \brief Compute whole-cover replacement text for a macro invocation.
  ///
  /// Maps the invocation's A-domain PP-token cover interval to the
  /// corresponding B-domain token envelope and returns the B bytes that should
  /// replace the callsite. The mapping is structural and claim-aware (B-only
  /// insertion segments that are committed to standalone emission are clipped
  /// out so they are not double-emitted).
  std::optional<WholeCoverPlan>
  ComputeWholeCoverPlan(const RefoldModel::MacroInvocation &m) const;

  /// \brief Return whether an existing whole-cover patch still matches the
  ///        current whole-cover plan.
  ///
  /// Reusable expanded patches are accepted only when their recorded proof
  /// root, replacement span, clipped replacement text, containment flags, and
  /// B-side envelope accounting agree with the plan that would be constructed
  /// for the current root macro invocation.
  ///
  /// \param patch The previously built macro patch being considered for reuse.
  /// \param plan The current whole-cover plan for the root macro invocation.
  /// \param rootMacroId The proof-root macro id that the patch must match.
  /// \returns True if \p patch is the same whole-cover realization described by
  ///          \p plan.
  bool WholeCoverPatchMatchesPlan(const MacroPatch &patch,
                                  const WholeCoverPlan &plan,
                                  uint64_t rootMacroId) const;

  /// \brief Build the owner certificate used for macro patch continuity.
  ///
  /// Re-classifies the hunk owner in the current TU context and normalizes it
  /// into the non-mixed owner shape stored on macro patches. This gives later
  /// same-root reuse and merge checks a stable owner witness instead of relying
  /// only on byte spans or macro ids.
  ///
  /// \param tuPath The translation-unit source path used for owner lookup.
  /// \param h The token hunk whose owner is being normalized.
  /// \returns The normalized owner certificate for \p h.
  Owner NormalizeHunkOwnerForPatch(StringRef tuPath,
                                   const diffutils::Hunk &h) const;

  /// \brief Return whether a macro patch belongs to the same normalized owner.
  ///
  /// Compares the owner certificate carried by \p patch with the owner inferred
  /// for the current hunk. Mixed or missing owner evidence does not match; this
  /// predicate is used to prevent patch reuse or merge across distinct include
  /// or TU ownership domains.
  ///
  /// \param patch The macro patch whose stored owner witness is being checked.
  /// \param owner The normalized owner certificate for the current hunk.
  /// \returns True if \p patch and \p owner describe the same non-mixed owner.
  bool MacroPatchOwnerMatches(const MacroPatch &patch,
                              const Owner &owner) const;

  /// \brief Copy an existing macro patch owner certificate onto another patch.
  ///
  /// Used when a candidate patch is derived from or merged with an already
  /// accepted patch and must preserve that patch's owner witness for later
  /// continuity checks and theorem audit.
  ///
  /// \param dst The patch receiving the owner certificate.
  /// \param src The patch whose owner certificate should be preserved.
  void CarryMacroPatchOwnerCertificate(MacroPatch &dst,
                                       const MacroPatch &src) const;

  /// \brief Stamp a normalized owner witness onto a macro patch.
  ///
  /// Records the owner certificate for the patch at construction time so later
  /// reuse, merge, and expanded-patch stabilization checks can verify that the
  /// patch still belongs to the same ownership domain.
  ///
  /// \param patch The macro patch to annotate.
  /// \param owner The normalized owner certificate to store on \p patch.
  void StampMacroPatchOwnerWitness(MacroPatch &patch, const Owner &owner) const;

  /// \brief Build the normalized proof summary for a macro patch.
  ///
  /// This bridges the macro-specific proof fields onto the normalized proof
  /// summary. Converted selector sites already use the resulting lattice and
  /// discharge metadata operationally; non-converted sites still mirror their
  /// chosen patch through this same summary for auditability.
  ProofSummary ClassifyMacroPatchProof(const MacroPatch &patch) const;

  /// \brief Refresh proof witnesses derived from macro-patch metadata.
  ///
  /// Phase 2D removes the old proof mirror fields from MacroPatch, but some
  /// durable witnesses are still backed by richer patch metadata such as paste
  /// replay status, DAG subtree certificates, and call-chain identity.  This
  /// helper copies that metadata into MacroPatchProof without changing the
  /// primary proof kind, root, or invocation-preservation bit.
  void RefreshMacroPatchDerivedProofWitnesses(MacroPatch &patch) const;

  /// \brief Rebuild the normalized proof summary from MacroPatchProof.
  ///
  /// This is now a carrier-only bridge: the canonical MacroPatchProof is first
  /// refreshed with derived paste/subtree/call-chain witnesses, then classified
  /// into ProofSummary and its embedded EmittedProof.
  void SyncMacroPatchProofSummary(MacroPatch &patch) const;

  /// \brief Strict/theorem guard for accepted proof summaries.
  ///
  /// These helpers do not restamp, reorder, or manufacture replacement proof
  /// authority.  In strict/theorem validation they report places where an
  /// accepted artifact is still explainable by transitional side data instead
  /// of by the final theorem carrier.
  bool AuditProofSummaryForLegacyAuthority(const ProofSummary &summary,
                                           llvm::StringRef role) const;
  bool AuditAcceptedResultCandidateForLegacyAuthority(
      const AcceptedResultCandidate &candidate, llvm::StringRef role) const;
  bool AuditMacroPatchProofForLegacyAuthority(const MacroPatch &patch,
                                              llvm::StringRef role) const;
  bool AuditTerminalFallbackForLegacyAuthority(
      const TerminalFallbackProofFailure &failure, llvm::StringRef role) const;
  bool AuditFinalLineControlAuthorityContract(
      const FinalLineControlAuthorityContract &authority,
      llvm::StringRef role) const;
  bool AuditFinalLineControlRemovalProofPopulation(
      llvm::ArrayRef<FinalLineControlPruneCandidate> candidates,
      llvm::StringRef role) const;

  /// \brief Report-only Phase-4A guard for expansion-fallback branches.
  ///
  /// The fallback fragment now has to name the proof class of every emitted
  /// branch before it builds an accepted-result carrier.  These helpers do not
  /// change behavior; they only make the no-legacy audit report an emitting
  /// fallback branch that has not been classified or whose accepted carrier no
  /// longer matches the branch's declared proof class.
  bool AuditExpansionFallbackBranchClassification(
      const ExpansionFallbackBranchClassification &classification,
      llvm::StringRef role) const;
  bool AuditExpansionFallbackAcceptedCandidate(
      const ExpansionFallbackBranchClassification &classification,
      const AcceptedResultCandidate &candidate, llvm::StringRef role) const;

  /// \brief Build the canonical proof carrier for a macro patch.
  ///
  /// This small factory keeps call sites from open-coding the three primary
  /// proof facts while Phase 2C flips stamping to be carrier-first.  More
  /// specialized witnesses are attached to the returned MacroPatchProof before
  /// SetMacroPatchProof() when the construction site already owns them.
  MacroPatchProof MakeMacroPatchProof(MacroPatchProofKind kind,
                                      bool preservesInvocationStructure,
                                      uint64_t proofRootMacroId) const;

  /// \brief Install the canonical macro proof carrier and refresh summaries.
  ///
  /// Phase 2D makes this the only primary stamping API for macro-patch proof
  /// identity.  The normalized ProofSummary and its canonical EmittedProof are
  /// rebuilt from MacroPatchProof immediately.
  ///
  /// \param patch The macro patch to annotate.
  /// \param proof The complete canonical macro-local proof carrier.
  void SetMacroPatchProof(MacroPatch &patch, MacroPatchProof proof) const;

  /// \brief Record the accepted candidate selected for a macro patch.
  ///
  /// Macro selection still returns the concrete MacroPatch so existing
  /// ownership maps remain unchanged, but the selected theorem carrier is
  /// stored on the patch at the selection boundary.  Later emission code may
  /// only consume this stamped carrier; the fact that this path-local result
  /// competed and won is no longer implicit in raw MacroPatch control flow.
  void StampSelectedMacroPatchCandidate(
      MacroPatch &patch, const AcceptedResultCandidate &candidate,
      llvm::StringRef role) const;

  /// \brief Materialize the explicit whole-cover realization proof.
  ///
  /// Whole-cover output is no longer tracked as an anonymous fallback result.
  /// This helper stamps the accepted patch as a first-class invocation
  /// realization proof and copies the exact realization envelope derived by
  /// ComputeWholeCoverPlan() into the patch-local certificate fields.
  void StampMacroWholeCoverRealizationPatch(
      MacroPatch &patch, const WholeCoverPlan &plan,
      const RefoldModel::MacroInvocation &macro) const;

  /// \brief Build the accepted-path inventory for a macro proof carrier.
  ///
  /// MacroPatchProof is the only proof-facing input to macro classification.
  /// The patch-level overload remains for call-site convenience, but it
  /// delegates here instead of inspecting unrelated MacroPatch metadata.
  ///
  /// \param proof The canonical macro proof carrier to classify.
  /// \returns The accepted-path inventory for \p proof.
  AcceptancePathInventory
  InventoryMacroPatchProofAcceptancePath(const MacroPatchProof &proof) const;

  /// \brief Compatibility wrapper for macro-patch inventory construction.
  ///
  /// This overload intentionally reads only MacroPatch::proof.  It exists so
  /// callers that already own a MacroPatch do not duplicate the proof-kind to
  /// accepted-path mapping.
  AcceptancePathInventory
  InventoryMacroPatchAcceptancePath(const MacroPatch &patch) const;

  /// \brief Map an accepted path to its proof-discharge inventory.
  ///
  /// Centralizes the mapping from the engine's current accepted-result path to
  /// the future theorem-facing proof target that is expected to discharge it.
  /// Validators use this inventory to check that each accepted result is both
  /// classified and assigned to a declared proof class.
  ///
  /// \param currentPath The accepted path currently used by the engine.
  /// \returns The inventory record for \p currentPath, including its future
  ///          proof target.
  AcceptancePathInventory
  BuildAcceptancePathInventory(AcceptedPathKind currentPath) const;

  /// \brief Map construction provenance onto the final theorem proof class.
  ///
  /// Phase 1C keeps AcceptedPathKind / AcceptedProofClass as construction
  /// provenance only.  This helper is the single path-to-theorem bridge used by
  /// builders before the summary is finalized; selectors and emitters then read
  /// ProofSummary::theoremClass / ProofSummary::emittedProof instead of
  /// re-deriving validity from AcceptedProofClass.
  TheoremProofClass
  BuildTheoremProofClassForAcceptedPath(AcceptedPathKind currentPath) const;

  /// \brief Build a normalized proof summary for a concrete accepted path.
  ///
  /// This helper attaches class-local obligation/discharge metadata to
  /// non-macro accepted paths such as include anchors and TU anchors. Callers
  /// may supply an explicit TU/include witness so the
  /// accepted-path audit can report the exact deterministic anchor or mapped
  /// byte range that was used. When \p patch is null, only obligations that
  /// can be discharged from the path classification itself are evaluated.
  ProofSummary BuildAcceptedPathProofSummary(
      AcceptedPathKind currentPath, const IncludePatch *patch = nullptr,
      const TUAnchorWitness *tuAnchorWitness = nullptr,
      const IncludeAnchorWitness *includeAnchorWitness = nullptr,
      const TerminalFallbackWitness *terminalFallbackWitness = nullptr) const;

  /// \brief Build the internal working proof summary for an include patch.
  ///
  /// Include patches are created before materialization chooses a concrete
  /// preserving anchor or realization envelope. The resulting summary therefore
  /// is only a staging object, so the default summary intentionally avoids
  /// claiming any normalized accepted path. Callers must restamp the emitted
  /// accepted result onto a concrete witness-backed include class once
  /// materialization chooses the final path.
  ProofSummary BuildIncludePatchProofSummary(
      bool realizedSurface,
      AcceptedPathKind currentPath = AcceptedPathKind::Unknown,
      const IncludePatch *patch = nullptr) const;

  /// \brief Build the canonical emitted proof for an accepted candidate.
  ///
  /// This is the Phase 1A theorem gate.  It rejects candidates whose proof class
  /// is missing, transitional, out of sync with its construction path, or
  /// locally undischarged.  Success returns the final theorem class together
  /// with every typed witness already present on the proof summary.
  std::optional<EmittedProof>
  BuildEmittedProof(const AcceptedResultCandidate &candidate) const;

  /// \brief Normalize an accepted candidate to the final theorem vocabulary.
  ///
  /// Compatibility wrapper for pre-Phase-1C call sites.  The implementation
  /// delegates to BuildEmittedProof(), so selector/emission code that still asks
  /// only for a theorem enum is not a second theorem authority.
  std::optional<TheoremProofClass>
  NormalizeAcceptedProof(const AcceptedResultCandidate &candidate) const;

  static EmittedProof
  BuildEmittedProofFromSummary(TheoremProofClass theoremClass,
                               const ProofSummary &summary);

  /// \brief Normalize a proof summary into its canonical emitted proof.
  ///
  /// This is the Phase 1C summary-owned theorem gate. It intentionally reads
  /// only the summary's final theorem class, construction inventory, discharge
  /// record, domain contract, and typed witnesses; AcceptedResultCandidate
  /// remains construction provenance and is checked only as a compatibility
  /// wrapper above this layer.
  std::optional<EmittedProof>
  BuildCanonicalEmittedProofFromSummary(const ProofSummary &summary) const;

  /// \brief Compute the current global lattice law for an accepted summary.
  ///
  /// Centralize the merge/conflict policy that already exists across
  /// macro, include, TU-anchor, and terminal-fallback paths. Patch B starts
  /// using that lattice at converted competition sites; later work will still
  /// be needed before every selector in the engine is routed through it.
  GlobalSelectionLattice
  BuildGlobalSelectionLattice(const ProofSummary &summary) const;

  /// \brief Compute the completeness contract for an accepted summary.
  ///
  /// This is the machine-readable form of the declared-domain statement above:
  /// a theorem-facing summary either belongs to a declared proof class, is an
  /// internal-only transitional state that must not survive to emission, or is
  /// explicitly outside the declared set as a named terminal boundary.
  CompletenessContract
  BuildCompletenessContract(const ProofSummary &summary) const;

  /// \brief Compute the explicit theorem-domain contract for an accepted
  /// summary.
  ///
  /// This must stay definitionally aligned with the completeness contract and
  /// theorem audit: theorem-facing summaries are in-domain only when they are
  /// declared, explicit-proof-backed, locally discharged, lattice-resolved, and
  /// free of unresolved owner / envelope exclusions other than named terminal
  /// out-of-domain results.
  TheoremDomainContract
  BuildTheoremDomainContract(const ProofSummary &summary) const;

  /// \brief Return whether the normalized lattice prefers \p lhs over \p rhs.
  ///
  /// Patch B uses this comparator directly at the converted selection sites.
  /// The ordering is deterministic and stable-on-ties so equal summaries can
  /// preserve the existing caller-supplied precedence order.
  bool LatticePrefers(const ProofSummary &lhs, const ProofSummary &rhs) const;

  /// \brief Return the primary emitted surface for a normalized candidate kind.
  ///
  /// Phase 3A keeps this mapping centralized so later selector-closure work can
  /// audit the full emission-path set without duplicating candidate-kind switch
  /// logic at each builder.  Mixed-owner and owner-realization paths are added
  /// separately from ProofSummary witnesses because they are proof overlays, not
  /// primary byte-edit surfaces.
  static EmissionPathKind PrimaryEmissionPathForCandidateKind(
      AcceptedResultCandidateKind kind);

  /// \brief Rebuild the Phase-3A emission-path inventory for one candidate.
  ///
  /// The function is intentionally deterministic and side-effect free except for
  /// the candidate's inventory field. It is called after each builder finishes
  /// mutating ProofSummary so overlay paths cannot go stale when a witness is
  /// attached late, such as mixed-owner tiling after owner realization.
  void RefreshAcceptedCandidateEmissionPathInventory(
      AcceptedResultCandidate &candidate) const;

  /// \brief Build an accepted-result candidate for a macro patch.
  ///
  /// Packages an already constructed macro patch with its normalized accepted
  /// path, proof-discharge inventory, proof summary, and macro-patch audit
  /// metadata. This is the macro-specific candidate wrapper used by final
  /// selection, theorem audit, and proof discharge.
  ///
  /// \param patch The macro patch whose stamped proof metadata should be
  /// wrapped
  ///        as an accepted-result candidate.
  /// \returns The normalized accepted-result candidate for \p patch.
  AcceptedResultCandidate
  BuildAcceptedMacroCandidate(const MacroPatch &patch) const;

  /// \brief Restamp a macro selector carrier for byte-edit emission.
  ///
  /// The input candidate must come from BuildAcceptedMacroCandidate().  This
  /// helper owns only the selector-to-emission proof transition, so macro
  /// selection can derive both carriers without rebuilding or reauditing the
  /// same MacroPatch proof twice.
  AcceptedResultCandidate RestampAcceptedMacroCandidateForEmission(
      const MacroPatch &patch, AcceptedResultCandidate candidate) const;

  /// \brief Build the accepted-result carrier used by macro emission.
  ///
  /// Final macro selection uses BuildAcceptedMacroCandidate(), because selector
  /// competition must still enforce selector-only obligations such as the
  /// top-level proof-root rule.  At the byte-edit emission boundary, however,
  /// an already accepted nested preserving macro patch is validated with the
  /// emitted-artifact contract instead of being reclassified as a selector-only
  /// candidate.  This helper is the single normalization point for that
  /// emitted macro carrier so selection finalization and TextEdit attachment do
  /// not duplicate the restamping logic.
  AcceptedResultCandidate
  BuildAcceptedMacroEmissionCandidate(const MacroPatch &patch) const;

  /// \brief Ensure a macro patch queued for emission has a selected carrier.
  ///
  /// All MacroPatch objects that reach macroPatchesByOwner must cross this
  /// helper before they can later materialize as TextEdit objects.  It refreshes
  /// the canonical MacroPatchProof summary, builds the emitted-boundary
  /// AcceptedResultCandidate through the shared theorem gate, and stamps the
  /// selected carrier on the patch.  If the patch cannot normalize to one final
  /// theorem proof, the shared missing-carrier invariant requests terminal
  /// fallback instead of allowing an unstamped raw MacroPatch to survive to
  /// emission.
  bool FinalizeSelectedMacroPatchForEmission(MacroPatch &patch,
                                             llvm::StringRef role) const;

  /// \brief Return the selected carrier used at the actual macro byte-edit
  /// emission boundary.
  ///
  /// Step 3 makes this helper a pure boundary accessor: proof authority must
  /// already have been selected and stamped before the MacroPatch entered the
  /// final emission bucket.  A missing selectedAcceptedCandidate is therefore
  /// an invariant violation and is never repaired here by rebuilding from the
  /// raw MacroPatch.
  AcceptedResultCandidate
  BuildAcceptedEmittedMacroCandidate(const MacroPatch &patch) const;

  /// \brief Build an accepted-result candidate for an include-preserving path.
  ///
  /// Packages an already constructed include patch with its normalized accepted
  /// path, proof-discharge inventory, and optional include-anchor witness.
  /// Include realization no longer carries a parallel include-specific closure
  /// witness; realized include output is routed through OwnerRealizationWitness
  /// by BuildAcceptedIncludeRealizationCandidate().
  ///
  /// \param currentPath The accepted include path represented by \p patch.
  /// \param patch The include patch being wrapped for selection/audit.
  /// \param includeAnchorWitness Optional anchor witness for include-preserving
  ///        paths.
  /// \returns The normalized accepted-result candidate for the include result.
  AcceptedResultCandidate BuildAcceptedIncludeCandidate(
      AcceptedPathKind currentPath, const IncludePatch &patch,
      const IncludeAnchorWitness *includeAnchorWitness = nullptr) const;

  /// \brief Build the generic proof summary for realized include/TU output.
  ///
  /// Phase 7B separates proof from spelling: include and TU emitters still own
  /// their materialized surface metadata, while this helper owns only the
  /// theorem-facing OwnerRealizationWitness installation.  Keeping the helper
  /// proof-only prevents include/TU spelling fields from becoming a second
  /// owner-realization proof family.
  ProofSummary BuildOwnerRealizationProofSummary(
      AcceptedPathKind currentPath,
      const OwnerRealizationResult &ownerRealization) const;

  /// \brief Build an accepted-result candidate for an emitted include
  ///        realization.
  ///
  /// Used when the engine materializes an include expansion directly rather
  /// than carrying an `IncludePatch` from the ordinary include-patch path.  The
  /// optional B-token envelope is owner-realization input, not a separate
  /// include proof family: TryBuildOwnerRealization() validates the common
  /// owner/source/A-cover/B-envelope/state obligations.
  ///
  /// \param currentPath The include realization path being emitted.
  /// \param include The include item whose expansion was realized.
  /// \param evidenceKind The include-envelope producer proof, when one exists.
  /// \param bTokenEnvelope Optional B-token envelope for inline-from-B
  ///        realization.
  /// \returns The normalized accepted-result candidate for the emitted include
  ///          realization.
  AcceptedResultCandidate BuildAcceptedIncludeRealizationCandidate(
      AcceptedPathKind currentPath, const RefoldModel::IncludeItem &include,
      IncludeRealizationEvidenceKind evidenceKind =
          IncludeRealizationEvidenceKind::Unknown,
      std::optional<IncludeRealizationBTokenEnvelope> bTokenEnvelope =
          std::nullopt) const;

  /// \brief Build an accepted-result candidate for a TU anchor edit.
  ///
  /// Packages a TU exact-slot or provable-insertion anchor witness into the
  /// normalized candidate form used by final selection, theorem audit, and
  /// proof discharge.
  ///
  /// \param currentPath The TU anchor accepted path.
  /// \param witness The local TU anchor witness that justifies the insertion
  ///        point.
  /// \returns The normalized accepted-result candidate for the TU anchor.
  AcceptedResultCandidate
  BuildAcceptedTUAnchorCandidate(AcceptedPathKind currentPath,
                                 const TUAnchorWitness &witness) const;

  /// \brief Build an accepted-result candidate for a direct TU byte-span edit.
  ///
  /// Represents a theorem-facing TU text edit whose source byte range is
  /// already known. The optional payload preview is diagnostic/audit metadata
  /// only; the candidate identity is the accepted path plus byte span.
  ///
  /// \param currentPath The TU byte-span accepted path.
  /// \param begin The starting TU byte offset.
  /// \param end The ending TU byte offset, exclusive.
  /// \param payloadPreview Optional shortened replacement payload for audit
  ///        diagnostics.
  /// \returns The normalized accepted-result candidate for the TU byte edit.
  AcceptedResultCandidate BuildAcceptedTUTextEditCandidate(
      AcceptedPathKind currentPath, uint64_t begin, uint64_t end,
      StringRef payloadPreview = StringRef()) const;

  /// \brief Build an accepted-result candidate for terminal fallback.
  ///
  /// Wraps the explicit out-of-domain terminal result in the same normalized
  /// candidate structure as ordinary accepted results. This keeps terminal
  /// fallback visible to selection, theorem audit, and proof-discharge
  /// reporting instead of treating it as an implicit escape path.
  ///
  /// \param witness The terminal fallback witness describing the fallback kind
  ///        and primary reason.
  /// \returns The normalized accepted-result candidate for terminal fallback.
  AcceptedResultCandidate
  BuildAcceptedTerminalCandidate(const TerminalFallbackWitness &witness) const;

  /// \brief Return whether a normalized accepted result is admissible for
  /// lattice-based selection at the converted selector sites.
  ///
  /// Patch C makes proof discharge the participation gate for the converted
  /// selector sites. A non-terminal candidate may participate only when its
  /// declared proof class discharged successfully; the explicit terminal
  /// out-of-domain result remains selectable only as the named terminal
  /// rejection class.
  bool IsSelectableAcceptedResultCandidate(
      const AcceptedResultCandidate &candidate) const;

  /// \brief Return whether \p lhs outranks \p rhs under the normalized
  /// candidate ordering used by Patch B.
  ///
  /// This lifts the proof-summary lattice comparison to the accepted-result
  /// carrier and then applies deterministic artifact-local tie-breakers. When
  /// two candidates are still indistinguishable after those tie-breakers, the
  /// caller's original enumeration order is preserved.
  bool AcceptedResultCandidatePrefers(
      const AcceptedResultCandidate &lhs,
      const AcceptedResultCandidate &rhs) const;

  /// \brief Return whether \p candidate failed only the nested-macro
  /// top-level selector rule.
  ///
  /// Remove the last direct macro-selector bypass by letting nested
  /// DAG/callsite preservation artifacts flow through the explicit candidate
  /// selector instead of returning them directly. Those intermediate nested
  /// artifacts are still rejected by the theorem-facing discharge rule that
  /// requires a top-level proof root, so this helper isolates the one
  /// narrowly-scoped selector-only failure that the non-top-level macro
  /// construction site may admit locally while preserving the existing final
  /// top-level gating everywhere else.
  bool AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
      const AcceptedResultCandidate &candidate) const;

  /// \brief Shared deterministic selector loop.
  ///
  /// Accepted-result selection and macro-local selection use different
  /// admissibility predicates, but they must share one stable ranking loop so
  /// selector accounting and tie behavior cannot drift apart.
  std::optional<size_t> SelectPreferredCandidateIndex(
      size_t candidateCount, llvm::function_ref<bool(size_t)> isSelectable,
      llvm::function_ref<bool(size_t, size_t)> prefers) const;

  /// \brief Build the macro-local selector carrier for one macro patch.
  ///
  /// The selector candidate retains macro-ranking obligations. The emitted
  /// candidate is built through the emission-specific macro proof gate and is
  /// present only when it normalizes to one final theorem class. This keeps
  /// selector-only macro proofs useful for deterministic ranking without making
  /// them acceptable TextEdit carriers.
  MacroSelectionCandidate BuildMacroSelectionCandidate(
      const MacroPatch &patch, bool allowNonTopLevelMacroSelectorFailure) const;

  /// \brief Return whether a macro selector carrier may participate in ranking.
  ///
  /// A macro candidate may rank either because its selector proof discharged as
  /// a normal accepted result or because it is the narrow, caller-enabled
  /// selector-only nested macro case. The latter is still not an emitted
  /// accepted artifact.
  bool IsSelectableMacroSelectionCandidate(
      const MacroSelectionCandidate &candidate) const;

  /// \brief Return whether one macro selector carrier outranks another.
  ///
  /// Macro-local ranking is intentionally based on selectorCandidate only. The
  /// emittedCandidate exists solely for the later emission stamp and must not
  /// change selector ordering.
  bool MacroSelectionCandidatePrefers(
      const MacroSelectionCandidate &lhs,
      const MacroSelectionCandidate &rhs) const;

  /// \brief Return the strongest selectable macro-local candidate.
  ///
  /// This selector is the only place where selector-only macro proofs may
  /// participate. It never returns an AcceptedResultCandidate directly, which
  /// prevents non-final selector proofs from reaching the emitted artifact
  /// boundary.
  std::optional<SelectedMacroSelectionCandidate>
  SelectPreferredMacroSelectionCandidate(
      ArrayRef<MacroSelectionCandidate> candidates) const;

  /// \brief Return the strongest selectable accepted candidate.
  ///
  /// Accepted-result selection is reserved for emission-admissible artifacts.
  /// Every candidate considered here must normalize to one final theorem proof;
  /// selector-only macro staging objects must use
  /// SelectPreferredMacroSelectionCandidate().
  std::optional<SelectedAcceptedResultCandidate>
  SelectPreferredAcceptedResultCandidate(
      ArrayRef<AcceptedResultCandidate> candidates) const;

  /// \brief Compatibility wrapper returning only the selected index.
  ///
  /// Existing callers that do not own a concrete artifact can still ask for the
  /// index, but the implementation delegates to the carrier-returning selector
  /// above so there is only one selection authority.
  std::optional<size_t> SelectPreferredAcceptedResultCandidateIndex(
      ArrayRef<AcceptedResultCandidate> candidates) const;

  /// \brief Return whether \p m carries usable producer-side paste witnesses.
  ///
  /// The producer's exact `##` decomposition is serialized into the model.
  /// Patch C may discharge a paste-preserving args-only class from either of
  /// two deterministic proof sources: a usable producer-side root witness
  /// stream, checked here, or an explicit direct replay check recorded on the
  /// accepted patch. This helper validates only the producer-side witness
  /// source.
  ///
  /// The producer records only the argument-derived fragments of each pasted
  /// token; literal glue bytes from the macro body (for example the `_` in
  /// `X##_##Y`) may appear as gaps between recorded parts. The helper is
  /// therefore strict about ordered, non-overlapping half-open ranges that
  /// stay within the final spelling, but it does not require the recorded
  /// parts to form a contiguous partition of the pasted token. It also accepts
  /// the parent-level case where `pasteSpans` are only propagated child-paste
  /// contributors inside a standard occurrence of the same formal, because
  /// those spans are validated by the args-only replay proof rather than by a
  /// direct parent-level `paste_tokens` decomposition.
  bool MacroInvocationHasWellFormedPasteWitnesses(
      const RefoldModel::MacroInvocation &m) const;

  /// \brief Validate the current local proof contract for an accepted class.
  ///
  /// These validators lift the existing deterministic checks into named local
  /// obligations. Converted selector sites already use their discharge result
  /// operationally; remaining sites still mirror their facts into the same
  /// proof contract until they are moved onto the normalized selector path.
  ProofDischargeRecord
  ValidateInvocationPreservingProof(const MacroPatch &patch) const;

  ProofDischargeRecord ValidateInvocationPreservingProofImpl(
      const MacroPatch &patch, bool requireTopLevelRoot) const;

  /// \brief Validate the emitted semantic proof contract for a preserving
  /// macro patch.
  ///
  /// Remove the byte-edit boundary's selector-only exception by
  /// rebuilding emitted nested preserving macro carriers under this validator.
  /// It discharges the same semantic obligations as
  /// ValidateInvocationPreservingProof(), but it does not re-impose the
  /// top-level proof-root selector rule that is only relevant while competing
  /// for final selection.
  ProofDischargeRecord
  ValidateEmittedInvocationPreservingProof(const MacroPatch &patch) const;

  /// \brief Discharge the proof obligations for a macro invocation-realization
  ///        patch.
  ///
  /// Validates that the macro patch is classified as a realization path rather
  /// than an invocation-preserving path, carries a tracked proof root, and has
  /// the proof metadata required by its realization class. Whole-cover
  /// realization patches must additionally record their A/B envelopes,
  /// containment status, and boundary-accounting metadata.
  ///
  /// \param patch The macro patch whose realization proof metadata should be
  ///        validated.
  /// \returns The proof-discharge record containing all satisfied and failed
  ///          obligations.
  ProofDischargeRecord
  ValidateInvocationRealizationProof(const MacroPatch &patch) const;

  /// \brief Discharge the proof obligations for an include-preserving path.
  ///
  /// Validates that the accepted include path is classified, mapped to a future
  /// proof target, backed by an include patch shape, and justified by the
  /// anchor witness required for that specific include-preserving path. This
  /// covers mapped-header replacement/deletion and the supported include
  /// insertion-anchor classes.
  ///
  /// \param currentPath The include-preserving accepted path being validated.
  /// \param patch The include patch shape being discharged.
  /// \param witness Optional anchor witness for the include path.
  /// \returns The proof-discharge record containing all satisfied and failed
  ///          obligations.
  ProofDischargeRecord ValidateIncludePreservingProof(
      AcceptedPathKind currentPath, const IncludePatch *patch,
      const IncludeAnchorWitness *witness = nullptr) const;

  /// \brief Discharge the proof obligations for a TU anchor path.
  ///
  /// Validates that the accepted path is a TU exact-slot or provable-insertion
  /// anchor and that it carries the local witness fields required by that
  /// anchor class. Exact-slot anchors must identify the matched slot, while
  /// provable insertion anchors must record the supporting neighbor,
  /// include-boundary, macro, or corroborated-owner evidence.
  ///
  /// \param currentPath The TU anchor accepted path being validated.
  /// \param witness Optional TU anchor witness describing the insertion point.
  /// \returns The proof-discharge record containing all satisfied and failed
  ///          obligations.
  ProofDischargeRecord
  ValidateTUAnchorProof(AcceptedPathKind currentPath,
                        const TUAnchorWitness *witness = nullptr) const;

  /// Include-realization input witnesses are intentionally not formatted as a
  /// separate theorem-facing proof family anymore.  Phase 6d routes realized
  /// include, macro, and TU output through \c OwnerRealizationWitness so audit
  /// logs expose one owner-polymorphic realization proof instead of parallel
  /// include-specific and owner-specific proof records.

  void AttachMixedOwnerTilingWitnessForTokenEnvelope(
      ProofSummary &summary, uint64_t aStart, uint64_t aEnd, uint64_t bStart,
      uint64_t bEnd) const;

  /// Discharge the common owner-realization obligations for a constructed
  /// closure.  This is the Phase-6b collapse point: macro/include/TU realization
  /// paths may still have distinct spelling mechanics, but they all prove the
  /// same owner/source/A-cover/B-envelope/state obligations here.
  OwnerRealizationResult TryBuildOwnerRealization(
      OwnerRealizationEvidenceKind evidence, OwnerClosure closure,
      StringRef detail) const;

  /// Build owner-realization results for the existing realized-output families.
  /// These helpers do not manufacture replacement text; they only construct the
  /// owner-specific closure and delegate the shared proof obligations to
  /// \c TryBuildOwnerRealization().
  OwnerRealizationResult BuildMacroWholeCoverOwnerRealization(
      const RefoldModel::MacroInvocation &macro,
      const WholeCoverPlan &plan) const;
  OwnerRealizationResult BuildIncludeOwnerRealization(
      const RefoldModel::IncludeItem &include, AcceptedPathKind currentPath,
      IncludeRealizationEvidenceKind evidenceKind =
          IncludeRealizationEvidenceKind::Unknown,
      std::optional<IncludeRealizationBTokenEnvelope> bTokenEnvelope =
          std::nullopt) const;
  OwnerRealizationResult BuildTUOwnerRealization(
      AcceptedPathKind currentPath, uint64_t begin, uint64_t end) const;

  /// Attach the shared owner-realization result to an accepted proof summary.
  ///
  /// Phase 6c makes the shared realization gate authoritative: once a macro,
  /// include, or TU realization path delegates to \c TryBuildOwnerRealization(),
  /// the caller may not independently declare the realized edit admissible after
  /// that gate rejects it.  This helper therefore records accepted witnesses and
  /// converts rejected results into a local proof-discharge failure on the
  /// candidate that tried to realize the owner.
  void ApplyOwnerRealizationResultToProofSummary(
      ProofSummary &summary, const OwnerRealizationResult &result) const;

  /// \brief Build the terminal-fallback witness for the current refold pass.
  ///
  /// Captures the terminal fallback kind, the number of fallback requests, and
  /// the primary recorded reason for falling out of the declared refolding
  /// domain. The resulting witness is used to make terminal fallback explicit
  /// in accepted-result selection, theorem audit, and proof-discharge
  /// reporting.
  ///
  /// \returns The terminal fallback witness for the current pass.
  TerminalFallbackWitness BuildTerminalFallbackWitness() const;

  /// \brief Return whether the hunk lies on the explicit unresolved-owner
  /// / no-TU-anchor theorem boundary.
  ///
  /// Use the conservative domain-wall interpretation for the last
  /// ownership gap. This predicate does not search for any new witness. It only
  /// re-states the evidence that has already been exhausted:
  ///
  /// * no resolved macro/TU/include owner remained,
  /// * no exact include-boundary owner exists for a pure insertion,
  /// * no truthful TU-owned mapped span exists for the A interval, and
  /// * for pure insertions, no exact/provable TU insertion anchor exists.
  ///
  /// When all of those facts hold, the edit is explicitly outside the declared
  /// structural refolding domain and must terminate via
  /// `OwnerUnresolvedNoTUAnchor`.
  bool IsOwnerUnresolvedNoTUAnchorOutOfDomain(
      const diffutils::Hunk &h, StringRef tuPath, const Owner &owner,
      bool mapsToTU) const;

  /// \brief Build a detailed terminal-fallback reason for the unresolved-owner
  /// / no-TU-anchor domain wall.
  ///
  /// This helper records the deterministic owner and TU-anchor searches that
  /// were already exhausted before the engine concluded that no declared
  /// macro/include/TU proof class could own the edit. It does not guess a new
  /// owner or widen admissibility.
  std::string BuildOwnerUnresolvedNoTUAnchorDetail(
      size_t hunkIndex, const diffutils::Hunk &h, StringRef tuPath,
      const Owner &owner, bool mapsToTU) const;

  /// \brief Build the replacement text for a macro whole-cover realization.
  ///
  /// Computes the same whole-cover plan used by macro patch construction and
  /// returns its clipped replacement text. This helper is for callers that need
  /// the materialized whole-cover text without constructing a full
  /// `MacroPatch`.
  ///
  /// \param m The macro invocation whose whole expansion should be realized.
  /// \returns The planned replacement text if a valid whole-cover plan exists;
  ///          std::nullopt otherwise.
  std::optional<std::string>
  BuildWholeCoverReplacementText(const RefoldModel::MacroInvocation &m) const;

  /// \brief Build a macro callsite patch for an invocation using only
  /// span-driven evidence.
  ///
  /// This routine produces the replacement text for a macro invocation at its
  /// callsite byte span \c [m.invB, m.invE) in the owning file. The strategy
  /// is intentionally deterministic and avoids heuristic “best-looking slice"
  /// selection.
  ///
  /// The patching strategy is:
  ///
  /// 1. **Do-not-downgrade guard:** If an existing patch for this invocation
  ///    already exists in \p patchMap and its replacement no longer resembles a
  ///    callsite invocation (i.e. the macro has effectively been realized/
  ///    expanded), preserve and return that patch unchanged.
  ///
  /// 2. **Args-only rewrite (preferred when safe):** If the diff hunk \p h is
  ///    fully contained within the invocation's argument-like spans (normal
  ///    arguments, stringify spans, and paste spans), attempt a surgical patch
  ///    using \c BuildMacroInvocationPatchArgsOnly. This preserves the original
  ///    callsite formatting and whitespace. Args-only rewriting is only
  ///    attempted when the provided invocation text still matches the expected
  ///    callsite prefix for \p m.
  ///
  /// 3. **Whole-cover realization:** If structure-preserving patching is not
  ///    applicable, replace the entire invocation only after the selected
  ///    A-domain cover passes MacroWholeCoverIsSelfContained() and the result is
  ///    stamped with OwnerRealizationProof. The B-envelope is then tightened
  ///    only by exact boundary-token accounting, not by nested/coarse-span
  ///    fallback.
  ///
  /// \param m Metadata for the macro invocation, including callsite byte span
  ///          and A-domain cover.
  /// \param h The diff hunk associated with this macro's region.
  /// \param baseInvText The original callsite invocation text from the source
  ///                    file.
  /// \param patchMap Previously computed macro patches indexed by owner id then
  ///                 macro id; used only to preserve existing non-callsite
  ///                 replacements for this invocation.
  /// \returns A \c MacroPatch for the callsite span if a deterministic
  ///          replacement can be built; otherwise \c std::nullopt (e.g. invalid
  ///          invocation span, invalid cover, or failure to compute a valid
  ///          B-token envelope).
  std::optional<MacroPatch> BuildMacroInvocationPatchWholeCover(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      StringRef baseInvText,
      const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
          &patchMap) const;

  /// \brief Phase-8e note on nested whole-cover realization.
  ///
  /// Nested macro whole-cover expansion is no longer admitted by aggregating
  /// descendant or coarse `spans` provenance.  A whole-cover patch must first
  /// pass MacroWholeCoverIsSelfContained(), and then it is stamped as an
  /// OwnerRealizationProof.  Nested edits that cannot satisfy that direct owner
  /// proof must be handled by DAG/call-chain structure-preserving proofs, by a
  /// wider owner realization, or by terminal fallback.

  /// \brief Validates that a raw source span contains a valid macro callsite
  /// prefix matching the invocation metadata.
  ///
  /// This check ensures that the text at the projected invocation coordinate
  /// starts with the correct macro name and, for function-like macros, is
  /// immediately followed by an opening parenthesis. This provides a safety
  /// proof that the refold mapping correctly aligned with the actual source
  /// code callsite before attempting an args-only rewrite.
  ///
  /// \param invSpanText The raw text extracted from the source file at the
  ///                    projected invocation span.
  /// \param m The macro invocation metadata containing the expected name
  ///          and subkind (object-like vs function-like).
  /// \returns True if the text contains a valid prefix for the macro call.
  bool InvocationSpanMatchesCallsitePrefix(
      StringRef invSpanText, const RefoldModel::MacroInvocation &m) const;

  /// \brief Look up a macro invocation record by its stable refold-model id.
  ///
  /// The refold metadata stores each macro invocation with a unique id and may
  /// reference that id from nested invocations, patches, and statistics logic.
  /// This helper performs a linear scan over the recorded invocation list and
  /// returns the matching metadata node when present.
  ///
  /// \param macroId Model-assigned macro invocation id to resolve.
  /// \returns The matching \c MacroInvocation, or \c nullptr if the id is not
  ///          present in the loaded model.
  const RefoldModel::MacroInvocation *
  FindMacroInvocationById(uint64_t macroId) const {
    for (const auto &mi : model_.GetMacroInvocations()) {
      if (mi.id == macroId)
        return &mi;
    }
    return nullptr;
  }

  /// \brief Return the top-level macro invocation that owns \p macroId.
  ///
  /// Macro invocations in the refold model may form a caller chain via
  /// \c callerMacroId when one macro expansion is nested within another. This
  /// helper follows that chain to the unique root invocation representing the
  /// user-visible callsite in source.
  ///
  /// \param macroId Model-assigned macro invocation id whose root should be
  ///        computed.
  /// \returns The id of the outermost invocation in the caller chain. If
  ///          \p macroId is unknown, returns \p macroId unchanged.
  uint64_t GetRootMacroId(uint64_t macroId) const;

  /// Return the source spelling site that observes a line-state builtin.
  ///
  /// For direct uses this is \p macro itself. For builtins expanded from a
  /// user macro replacement list, this follows callerMacroId links to the
  /// outermost callsite that remains visible in refolded source. The builtin
  /// record still supplies the produced-token cover; the returned site supplies
  /// owner, byte offset, physical line, and conditional-depth placement.
  const RefoldModel::MacroInvocation *LineStateObservableMacroSite(
      const RefoldModel::MacroInvocation &macro) const;

  /// \brief Return whether a macro patch leaves its owning root callsite
  ///        expanded in the final refolded source.
  ///
  /// A patch is considered to remain expanded when its replacement text does
  /// not reconstruct the original macro call spelling at the root callsite.
  /// This predicate is used only for statistics attribution: nested/internal
  /// macro work is charged back to the root user-visible invocation exactly
  /// once.
  ///
  /// \param patch Macro patch candidate to classify.
  /// \returns True if the patch should count as an expanded macro in the final
  ///          statistics, false if it preserves/reconstructs the macro callsite.
  bool MacroPatchRemainsExpanded(const MacroPatch &patch) const {
    const RefoldModel::MacroInvocation *mi =
        FindMacroInvocationById(patch.macroId);
    if (!mi)
      return true;
    return !InvocationSpanMatchesCallsitePrefix(patch.replacement, *mi);
  }

  /// \brief Applies a deterministic, stable ordering to include-scoped
  /// insertion patches.
  ///
  /// During refolding, multiple insertions can be attributed to the same
  /// include instance. The refold pipeline may discover or enqueue these
  /// patches in an order that depends on iteration order (e.g., map traversal)
  /// or on incidental properties of the diff/LCS reconstruction. This helper
  /// normalizes that by sorting each include's patch list into a canonical
  /// order.
  ///
  /// \par Ordering rule
  /// For each IncludeEdits entry in \p perInclude, this method sorts
  /// IncludeEdits::patches by:
  /// 1. IncludePatch::aStart (primary key; A-side insertion point in token
  /// space)
  /// 2. IncludePatch::bStart (secondary key; B-side position used to derive
  ///    the payload)
  ///
  /// \par Non-goals
  /// - No widening, merging, coalescing, or re-chunking is performed.
  /// - No payload rewriting occurs; the patch text/slices remain exactly as
  ///   computed earlier.
  /// - No ownership decisions are made here; this function only reorders
  ///   already-attributed patches.
  ///
  /// The \c bSource_ and \c bTokOff_ member variables are available for
  /// potential future diagnostics, but are not used by the current ordering
  /// logic.
  ///
  /// \param perInclude Mapping from include id to accumulated edits/patches for
  ///                   that include; each non-empty patch list is sorted in
  ///                   place.
  void
  OrderIncludeInsertions(DenseMap<uint64_t, IncludeEdits> &perInclude) const {
    for (auto &it : perInclude) {
      IncludeEdits &ie = it.second;
      if (ie.patches.empty())
        continue;

      // Use llvm::sort for a highly optimized stable sort.
      // We sort by aStart primarily and bStart secondarily.
      llvm::sort(ie.patches,
                 [](const IncludePatch &lhs, const IncludePatch &rhs) {
                   if (lhs.aStart != rhs.aStart)
                     return lhs.aStart < rhs.aStart;
                   return lhs.bStart < rhs.bStart;
                 });
    }
  }

  /// \brief Fully materialize the bytes for a single `#include` instance and
  ///        cache the result.
  ///
  /// Materialization is **depth-first** and **deterministic**. Starting from
  /// the include’s raw header bytes (preseeded in `includeExpansion` or read
  /// from disk), this method:
  ///
  /// 1. **Applies macro patches owned by this include** — each `MacroPatch`
  ///    replaces the invocation byte range `[invStart, invEnd)` in the
  ///    include’s own byte space.
  /// 2. **Applies A/B include patches** — insert/delete/replace edits derived
  ///    from the B text are applied within the header using
  ///    `applyIncludeInsertions()`.
  /// 3. **Recursively expands child includes that have work** — for each direct
  ///    child whose subtree contains edits or macro patches (as determined by
  ///    `hasDescendantWork()`), first materialize the child, then replace the
  ///    child’s directive site `[siteB, siteE)` (recorded in the parent’s byte
  ///    space) with the child’s fully materialized bytes.
  ///
  /// Edits collected for the current header are applied
  /// **highest-offset-first** to keep indices stable. The final materialized
  /// bytes are memoized into `includeExpansion` under `includeId`. If an entry
  /// already exists, the method returns immediately (idempotent).
  ///
  /// #### Coordinate Spaces
  /// * **siteB / siteE:** byte offsets in the includer’s file where the child
  ///   directive appears.
  /// * **Macro patches:** byte offsets in the owning include’s file.
  /// * **Coverage (coverBegin / coverEnd):** A-token indices; used to decide
  ///   ownership, not for byte replacement.
  ///
  /// \param includeId           Unique ID of the include to materialize.
  /// \param perInclude          Map of include ID → include-level A/B edits
  ///                            (insert/delete/replace).
  /// \param macroPatchesByOwner Map of owner ID → macro patches whose
  ///                            invocation ranges are expressed in that owner’s
  ///                            byte space. Include-owned patches are applied
  ///                            directly here; TU-owned patches are handled by
  ///                            the TU materialization path.
  /// \param children            Map of parent include ID → direct child include
  ///                            items (with `siteB` / `siteE` in the parent).
  /// \param includeExpansion    Cache/output: include ID → fully materialized
  ///                            header bytes; may be preseeded with raw header
  ///                            text.
  /// \param includeExpansionStartLineNos
  ///                            Cache/output: include ID → logical header line
  ///                            of the first emitted materialized line.  This
  ///                            can be greater than one when leading sideband
  ///                            source lines are deleted before wrapping the
  ///                            include with #line directives.
  /// \param appliedExpandedMacroRootIds Optional output set that, when non-null,
  ///                            records root macro invocation ids for macro
  ///                            patches that remain expanded after being applied
  ///                            while materializing this include subtree.
  void MaterializeIncludeExpansion(
      uint64_t includeId, const DenseMap<uint64_t, IncludeEdits> &perInclude,
      const DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
          &macroPatchesByOwner,
      const DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>>
          &children,
      DenseMap<uint64_t, std::string> &includeExpansion,
      DenseMap<uint64_t, std::vector<FinalLineControlPruneCandidate>>
          &includeExpansionLineControlPruneCandidates,
      DenseMap<uint64_t, std::vector<FinalLineControlSourceMapping>>
          &includeExpansionLineControlSourceMappings,
      DenseMap<uint64_t, size_t> &includeExpansionStartLineNos,
      DenseMap<uint64_t, AcceptedResultCandidate>
          &includeExpansionAcceptedResults,
      DenseSet<uint64_t> *appliedExpandedMacroRootIds = nullptr) const;

  /// \brief Realize an include directly from the edited preprocessed stream B.
  ///
  /// Builds the single-pass include-realization replacement used when local
  /// include preservation cannot discharge a deterministic anchored edit plan.
  /// The realization is taken from the include's B-side token envelope and,
  /// when requested, records the accepted-result candidate/witness that
  /// justifies the emitted materialization.
  ///
  /// \param inc The include item whose expansion should be realized.
  /// \param reason Diagnostic context explaining why inline realization was
  ///        selected.
  /// \param acceptedCandidate Optional output slot for the normalized accepted
  ///        result candidate associated with this realization.
  /// \returns The realized include replacement text if construction succeeds;
  ///          std::nullopt otherwise.
  std::optional<std::string> BuildInlineIncludeRealizationFromB(
      const RefoldModel::IncludeItem &inc, llvm::StringRef reason,
      AcceptedResultCandidate *acceptedCandidate = nullptr) const;

  /// \brief Selects the most appropriate HeaderDecl within an IncludeItem to
  /// serve as the declaration-level anchor for an include-scoped patch.
  ///
  /// Include edits are often more robust when anchored to a concrete
  /// declaration region rather than to the entire header. The refold model may
  /// provide a set of HeaderDecl entries for each include, each with a PP-space
  /// span (HeaderDecl::ppSpan) describing the declaration's extent in the
  /// preprocessed token/index space. This method chooses a single "best"
  /// declaration to which a patch should attach.
  ///
  /// \par Selection strategy
  ///
  /// The method considers the patch's A-side PP-token interval [\p p.aStart, \p
  /// p.aEnd) and applies the following rules:
  /// 1. **Prefer full coverage:** choose a declaration whose span fully
  /// contains the
  ///    patch interval.
  /// 2. **Otherwise allow overlap:** if no declaration fully contains the
  /// interval,
  ///    choose a declaration whose span merely overlaps the interval.
  /// 3. **Most specific wins:** if multiple candidates satisfy a rule, pick the
  /// one
  ///    with the smallest span length (ppSpan.end - ppSpan.begin) to obtain the
  ///    most specific (tightest) anchor.
  ///
  /// \par Pure insertions
  ///
  /// If the patch is a pure insertion (\p p.aStart == \p p.aEnd), the insertion
  /// is treated as a single attachment point at \c aLo = \p p.aStart. A
  /// declaration is considered a candidate if \c aLo lies within the
  /// declaration's span. Insertions anchored exactly at a declaration end are
  /// treated as belonging to that declaration (i.e., end-inclusive for this
  /// special case).
  ///
  /// \par Return value
  ///
  /// If a full-cover candidate exists, it is returned. Otherwise, the best
  /// overlap candidate is returned. If no declaration is relevant (e.g., the
  /// patch falls in a header preamble region not covered by any recorded
  /// HeaderDecl, or the edit is in macro-only glue that has no associated decl
  /// span), this method returns \c nullptr.
  ///
  /// \param inc the include instance containing declaration metadata
  ///        (IncludeItem::decls)
  /// \param p the include-scoped patch whose A-side interval is used for
  ///        anchor selection
  /// \return the best-matching HeaderDecl to anchor this patch, or \c nullptr
  ///         if none is applicable
  static const RefoldModel::HeaderDecl *
  FindHeaderDeclForPatch(const RefoldModel::IncludeItem &inc,
                         const IncludePatch &p);

  /// \brief Computes (but does not apply) the header-local `TextEdit`s implied
  /// by `ie`.
  ///
  /// `IncludePatch` coordinates are expressed in A-side preprocessed (PP) token
  /// indices. This routine projects those token ranges onto `headerText` using
  /// the refold map token→(file, byte-span) mapping in
  /// `RefoldModel::tokmapByPP`.
  ///
  /// The returned edits use byte offsets into `headerText` and are sorted in
  /// descending `start` order so a caller can apply them without re-mapping
  /// subsequent offsets.
  ///
  /// ### Patch classification
  ///
  /// - **INSERT**: `aStart == aEnd` and `bStart < bEnd`. Inserts
  ///   `IncludePatch::insertBytes` at a deterministic byte anchor inside this
  ///   header.
  /// - **DELETE**: `aStart < aEnd` and `bStart == bEnd`. Deletes the mapped
  ///   A-range (replacement is `""`).
  /// - **REPLACE**: `aStart < aEnd` and `bStart < bEnd`. Replaces the mapped
  ///   A-range with `IncludePatch::insertBytes`.
  ///
  /// ### Scope and clamping
  ///
  /// - The include instance contributes a PP “cover” window `[coverBegin,
  ///   coverEnd)` describing which PP indices belong to this header instance.
  /// - A patch may be associated with an owning `HeaderDecl` via
  ///   `findHeaderDeclForPatch(...)`.
  /// - **DELETE/REPLACE** are restricted to the owning declaration when
  /// present:
  ///   the patch’s effective PP window is intersected with `decl.ppSpan` and
  ///   the resulting byte range is clamped to `[decl.headerB, decl.headerE)`.
  /// - **INSERT** intentionally operates at include scope (not decl scope) so
  ///   that an insertion on a declaration boundary can anchor to the first
  ///   token of the following declaration instead of being forced “back inside”
  ///   the previous one.
  ///
  /// ### Deterministic anchoring for INSERT
  ///
  /// For an INSERT at A-position `pos = aStart`, the insertion byte offset is
  /// chosen in the following priority order within the effective PP window:
  /// 1. **Right neighbor**: smallest `pp >= pos` mapping to this header’s file;
  ///    insert immediately *before* that token (use its byte-start).
  /// 2. **Left neighbor**: greatest `pp < pos` mapping to this file; insert
  ///    immediately *after* that token (use its byte-end).  If `pos` is exactly
  ///    the include cover end, this same proof may anchor at physical EOF, but
  ///    only after the mapped left neighbor proves the insertion belongs to this
  ///    owner suffix.  Phase 8g classifies that case as an explicit
  ///    include-anchor proof, not as PP source-mapping fallback-to-EOF.
  /// 3. **Owning decl end**: if an owning decl exists, anchor at
  ///    `decl.headerE`.
  /// 4. **Child-include boundary proof**: prove a stable insertion anchor from
  ///    a direct child `#include` site (via
  ///    `ComputeChildBoundaryInsertByte(...)`). Phase 8d classifies this as a
  ///    declared include-preserving anchor proof, not as an unclassified fallback branch: the
  ///    witness must name the child include and the anchor must be exactly that
  ///    child directive's begin or end byte. If no such unique boundary exists,
  ///    the patch is skipped.
  ///
  /// ### Mapping for DELETE/REPLACE
  ///
  /// For non-empty A-ranges, the method locates all PP indices in the patch’s
  /// (possibly intersected) effective window that map into this header file.
  /// The resulting byte interval is `[byteStart(firstPP), byteEnd(lastPP))`. If
  /// no PP tokens map into this header, the patch is skipped.
  ///
  /// ### Formatting policy
  ///
  /// Replacement bytes are applied *literally*. This routine does not perform
  /// whitespace normalization, token reformatting, or “prefix surgery”;
  /// refold-map slices are treated as ground truth.
  ///
  /// \param M Refold model providing PP token→(file, byte range) mappings and
  ///          include/decl metadata.
  /// \param ie Per-include edits: the include instance plus a set of
  ///           `IncludePatch`es.
  /// \param headerText Current text of the header corresponding to
  ///                   `ie.include`.
  /// \returns Header-local `TextEdit`s (byte offsets into `headerText`), sorted
  ///          by descending `start`.
  IncludeTextEditPlan ComputeIncludeTextEdits(const IncludeEdits &ie,
                                              std::string headerText) const;

  /// \brief Proves a deterministic child-include boundary insertion anchor.
  ///
  /// This Phase-8d proof class applies to header-scoped pure INSERT patches
  /// (`p.aStart == p.aEnd`) whose PP gap coincides exactly with the begin or end
  /// of a direct child include's expansion cover.  Token-neighbor and
  /// declaration-boundary anchors remain preferred elsewhere, but this path is
  /// not a legacy fallback: it is an include-preserving proof backed by an
  /// `IncludeAnchorWitness` that names the child include and records the exact
  /// directive byte used as the anchor.
  ///
  /// The proof is needed because a parent header may contain no ordinary tokens
  /// at the insertion gap: the adjacent PP material can belong entirely to a
  /// child include.  In that case, materializing the child merely to host a
  /// boundary-inherent insertion is less structural than anchoring at the
  /// preserved child `#include` directive itself.
  ///
  /// ### Approach
  ///
  /// Let `owner` be the include instance whose header text is being edited
  /// (`p.include`). We scan the model’s includes to find *direct children* of
  /// `owner` whose `IncludeItem::sitePath` equals `file`. Each such child
  /// represents an include directive that is physically written in this header
  /// and has a PP cover window `[kid.coverBegin, kid.coverEnd)` and a site byte
  /// range `[kid.siteB, kid.siteE)` in `file`.
  ///
  /// For the insertion PP position `pos = p.aStart`, we consider only
  /// "between-children" positions:
  /// - If `pos` lies *strictly inside* a child’s cover window
  ///   (`kid.coverBegin < pos && pos < kid.coverEnd`), this proof does not
  ///   apply and returns `std::nullopt` (the insertion should have been owned
  ///   by that child include).
  /// - Boundary positions are allowed (`pos == kid.coverBegin` or
  ///   `pos == kid.coverEnd`) and are treated as being "between" children.
  ///
  /// ### Anchor selection
  ///
  /// Among eligible children in `file`:
  /// - `left` is the child with the greatest `coverEnd` such that
  ///   `coverEnd <= pos`
  /// - `right` is the child with the smallest `coverBegin` such that
  ///   `coverBegin >= pos`
  ///
  /// The insertion anchor is then chosen deterministically:
  /// 1. If `right` exists, insert *before* the right child’s `#include` site
  ///    by returning `right.siteB`.
  /// 2. Else if `left` exists, insert *after* the left child’s `#include` site
  ///    by returning `left.siteE`.
  /// 3. Otherwise return `std::nullopt` to indicate that no child-boundary
  ///    proof can be derived.
  ///
  /// This method deliberately fails closed on ambiguous boundaries.  If more
  /// than one direct child include has the same begin or end cover boundary, the
  /// caller cannot prove a unique source anchor and the edit must be classified
  /// by another proof path.  The method does not validate that the returned site
  /// offsets are within the current header text bounds; callers should ensure
  /// the returned byte offset is usable in the current editing context.
  ///
  /// \param p The include-scoped patch whose PP insertion position is
  ///          `p.aStart`.
  /// \param file The header file path whose text is being edited; only child
  ///             includes whose `sitePath` equals `file` are considered as
  ///             anchors.
  /// \param witness Optional witness sink that records which child include and
  ///               directive boundary discharged the proof.
  /// \return A byte offset within `file` at which the insertion should be
  ///         applied, or `std::nullopt` if this proof class does not apply or no
  ///         stable anchor can be found.
  std::optional<uint64_t> ComputeChildBoundaryInsertByte(
      const IncludePatch &p, StringRef file,
      IncludeAnchorWitness *witness = nullptr) const;

  /// \brief Creates a TextEdit for [start,end) in original that preserves
  /// __LINE__ transparency.
  ///
  /// If the replacement changes the newline count relative to the removed span,
  /// this method will attempt to keep line accounting correct by either:
  /// * injecting a local #line directive inside the replacement (when safe), or
  /// * deferring the correction by attaching a PendingResync to the returned
  ///   edit, to be flushed later by the file-emission layer.
  ///
  /// The returned edit always stores the original span boundaries unchanged,
  /// and stores either:
  /// * text = injectedReplacement, pending = nullopt on successful local
  ///   injection, or
  /// * text = replacement, pending = PendingResync(fileSpelling) when
  ///   deferring.
  ///
  /// \param original The pre-edit file contents the edit coordinates refer to.
  /// \param start Start offset (inclusive) in original.
  /// \param end End offset (exclusive) in original.
  /// \param replacement Replacement text to emit for [start,end).
  /// \param fileSpelling The producer-provided spelled path to use in any
  ///        emitted #line directive.
  /// \return A TextEdit representing the change and (optionally) a pending
  ///         resync to flush later.
  TextEdit MakeTextEditWithResyncOrPending(
      StringRef original, uint64_t start, uint64_t end, StringRef replacement,
      StringRef fileSpelling,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const {
    ResyncOutcome o = ApplyResyncOrPend(original, start, end, replacement,
                                        fileSpelling, ownerIncludeId);
    TextEdit edit{start, end, std::move(o.text), std::move(o.pending),
                  std::nullopt, {}, {}};
    edit.lineControlPruneCandidates = std::move(o.lineControlPruneCandidates);
    return edit;
  }

  /// \brief Computes how to preserve __LINE__ after applying replacement to
  /// [start,end) in originalFileText.
  ///
  /// This method detects "line drift" by comparing the newline count in the
  /// original span versus the replacement text. If there is no drift, it
  /// returns (replacement, nullopt).
  ///
  /// If drift is detected, the method:
  /// 1. Computes the logical resume line for the first character at `end` in
  ///    the original file,
  /// 2. Attempts a local resync by calling
  ///    LineDirectiveInserter::MaybeAppendResyncAfterReplacement,
  /// 3. If local injection succeeds, returns (injectedReplacement, nullopt),
  /// 4. Otherwise, returns (replacement, PendingResync(fileSpelling)) so the
  ///    emission layer can flush a #line directive at the next safe BOL.
  ///
  /// Safety note: local injection may fail when inserting a directive would
  /// change token adjacency (e.g., when the replacement ends mid-line, or when
  /// no safe BOL exists in/around the replacement). In that case, pending
  /// resync state is carried only because a model-recorded suffix __LINE__
  /// observer exists; otherwise no synthetic directive is produced.
  ///
  /// \param originalFileText Pre-edit file contents the offsets refer to.
  /// \param start Start offset (inclusive) in originalFileText.
  /// \param end End offset (exclusive) in originalFileText.
  /// \param replacement Replacement text.
  /// \param fileSpellingForDirective Path used in any injected #line.
  /// \param ownerIncludeId Include-instance owner for \p originalFileText, or
  ///        std::nullopt for the TU.  This is passed through to the refold-map
  ///        conditional-activity oracle.
  /// \return A ResyncOutcome containing the emitted text and optional pending
  ///         state.
  ResyncOutcome ApplyResyncOrPend(
      StringRef originalFileText, uint64_t start, uint64_t end,
      StringRef replacement, StringRef fileSpellingForDirective,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// \brief Return true iff the materialized include subtree contains a
  /// line-state-sensitive builtin invocation.
  bool IncludeSubtreeHasLineStateSensitiveBuiltin(uint64_t includeId) const;

  /// \brief Return true iff a recorded location-sensitive builtin still has a
  /// token-identical A/B surface and therefore remains a preserved observer.
  ///
  /// A source builtin that will be materialized by a later accepted edit is not
  /// a demand witness for synthetic #line insertion.
  bool LineStateBuiltinInvocationIsPreservedObserver(
      const RefoldModel::MacroInvocation &macro) const;

  /// Return true iff a preserved observer represented by \p macro cannot be
  /// justified solely by direct lexical final-source spelling.
  ///
  /// Direct source-spelled predefined builtins are visible as ordinary final
  /// tokens when preserved.  Builtins reached through a caller_macro_id chain,
  /// through incomplete macro metadata, or through missing token-map proof need
  /// model-backed line-state demand.  Phase 6F no longer lowers this into a
  /// separate final observer scanner; it is construction metadata for deciding
  /// whether a synthetic #line obligation should be emitted.
  bool LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(
      const RefoldModel::MacroInvocation &macro) const;

  /// Describes which components of the logical location are observed by
  /// preserved location-sensitive builtins in an owner suffix.
  struct LineStateObserverDemand {
    bool needsLine = false;
    bool needsFile = false;
    bool needsFileName = false;

    // True when at least one demand witness needs model metadata rather than
    // direct lexical final-source spelling.  This is construction metadata for
    // producing a repair obligation, not a late pruning veto.
    bool hasModelBackedLineStateDemand = false;

    bool Any() const { return needsLine || needsFile || needsFileName; }
    bool PrunableByCompactFinalLineControl() const { return Any(); }
  };

  /// \brief Return true iff a source-authored line-control directive in the
  /// same owner is producer-proven active before \p offset.
  ///
  /// Include-entry line-control minimization needs to know whether the emitted
  /// child body will overwrite the synthetic entry state before any preserved
  /// location-sensitive observer can consume it.  This query is intentionally
  /// source/model-backed: it recognizes line-control spelling in the owner file
  /// and uses the producer-recorded conditional arm selection rather than
  /// re-evaluating #if expressions.
  bool SourcePrefixHasProducerActiveLineControl(
      StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
      uint64_t offset) const;

  /// \brief Return the logical-location components observed by preserved
  /// location-sensitive builtins in an include subtree.
  ///
  /// This is the child-entry analogue of OwnerSuffixLineStateObserverDemand():
  /// the query is owner-polymorphic across the include tree and counts only
  /// builtins whose A-side token surface is still preserved in B. Materialized
  /// former builtins are ordinary replacement text and therefore do not demand
  /// a synthetic #line transition.
  LineStateObserverDemand IncludeSubtreeLineStateObserverDemand(
      uint64_t includeId) const;

  /// \brief Return true iff an include-entry #line discharges a concrete
  /// zero-token layout obligation in the parent owner.
  ///
  /// If a materialized include is the first token-producing text after
  /// preserved zero-token parent material, omitting the entry directive can make
  /// `clang -E -P` reproduce the parent's physical blank line rather than the
  /// B-side layout. The decision is derived from the producer map and the
  /// parent source, not from formatting preference.
  bool IncludeEntryLineDirectiveDischargesLayoutBarrier(
      const RefoldModel::IncludeItem &child,
      StringRef parentOwnerFileForDemand) const;

  /// \brief Return the logical-location components observed by the untouched
  /// suffix of an owner file.
  ///
  /// The result is owner-polymorphic and projects nested predefined builtins
  /// back to their outermost source callsite before comparing byte offsets.
  /// `__LINE__` observes the logical line component; `__FILE__` observes the
  /// logical file component; and `__FILE_NAME__` observes the logical basename
  /// component. `__BASE_FILE__` is
  /// intentionally excluded because synthetic #line directives do not affect
  /// its value.
  LineStateObserverDemand OwnerSuffixLineStateObserverDemand(
      std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
      uint64_t offset) const;

  /// Return true iff two line-control physical-file spellings denote the same
  /// owner surface for producer-backed line-control recovery. Pseudo files are
  /// intentionally never canonicalized against real files.
  bool SameLineControlPhysicalFile(StringRef lhs, StringRef rhs) const;

  /// Return the latest active, producer-proven source line-control directive end
  /// at or before \p offset in the given owner.
  std::optional<uint64_t> LatestProducerLineControlEndBefore(
      std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
      uint64_t offset) const;

  /// Recover the producer-backed logical location at \p offset in an owner, when
  /// the producer recorded an active line-control event dominating that offset.
  std::optional<LineDirectiveLocation> ProducerBackedLineControlLocationAt(
      StringRef ownerBytes, StringRef ownerFile,
      std::optional<uint64_t> ownerIncludeId, uint64_t eventEndLimit,
      uint64_t locationOffset) const;

  /// Return the logical location at \p offset in an owner, preferring direct
  /// source recovery and falling back to producer-backed line-control events.
  LineDirectiveLocation LogicalLocationAtOwnerOffset(
      StringRef ownerBytes, StringRef ownerFile,
      std::optional<uint64_t> ownerIncludeId, uint64_t offset) const;

  /// Earliest preserved line-state observer in an owner suffix.
  ///
  /// This is the ordered counterpart of OwnerSuffixLineStateObserverDemand().
  /// It follows caller_macro_id chains so that a predefined builtin spelled in
  /// a macro definition is attributed to the observable callsite in the owner
  /// file.  The returned byte offset is therefore a source byte where a
  /// dominating #line can be emitted to repair all preserved configurations
  /// reaching that observer.
  struct LineStateObserverSite {
    uint64_t offset = 0;
    LineStateObserverDemand demand;
  };

  std::optional<LineStateObserverSite> FirstOwnerSuffixLineStateObserverSite(
      std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
      uint64_t offset) const;

  /// \brief Return true iff the untouched suffix of an owner file contains a
  /// location-sensitive builtin invocation that can observe synthetic #line
  /// repair.
  bool OwnerSuffixHasLineStateSensitiveBuiltin(
      std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
      uint64_t offset) const;

  /// Validate the compact owner-local proof attached to a sideband pragma edit.
  ///
  /// This is the first consolidation gate for the sideband proof system: every
  /// accepted sideband edit must explicitly discharge identity, owner, ordered
  /// anchor, closure, realization, state, location/provenance, and composition
  /// obligations before it is lowered into TU or include materialization edits.
  bool ValidateSidebandPragmaEditProof(const SidebandPragmaEdit &edit,
                                       StringRef phase) const;

  /// \brief Wrap materialized include text with the correct #line policy.
  ///
  /// The child-enter line is the logical source line of the first emitted
  /// materialized header line, not blindly line one: deleting a leading sideband
  /// directive must advance the enter line to the first surviving header line.
  /// Entry and return directives are emitted only when they discharge a
  /// preserved line-state observer or a concrete zero-token layout obligation.
  /// The final boolean enables the narrower sideband-pragma-only path; it uses
  /// the same demand proof but may return an empty wrapper immediately when no
  /// observable bytes remain.
  LineControlWrappedText WrapIncludeExpansionForMaterialization(
      const RefoldModel::IncludeItem &child, StringRef parentFileSpelling,
      StringRef parentOwnerFileForDemand,
      std::optional<uint64_t> parentOwnerIncludeId, uint64_t parentResumeOffset,
      size_t childEntryLineNo, size_t parentResumeLineNo, StringRef childBody,
      ArrayRef<FinalLineControlPruneCandidate> childBodyLineControlCandidates,
      ArrayRef<FinalLineControlSourceMapping> childBodyLineControlSourceMappings,
      bool allowUnobservableLineDirectiveSuppression) const;

  /// \brief Applies a set of TextEdits to originalFileText, producing the final
  /// refolded text for a single file (TU or header), while preserving __LINE__
  /// transparency via "pending resync".
  ///
  /// ### Core responsibilities
  /// * Normalizes edits by de-duplicating exact-span edits (same [start,end)):
  ///   the last one wins.
  /// * Orders edits by increasing start (then end) and enforces non-overlap.
  /// * Streams output in order: original slices + replacement text.
  /// * When an edit carries a PendingResync, defers the #line emission until
  /// the
  ///   next safe BOL in the subsequent unchanged original text, using
  ///   AppendOriginalSliceWithPending.
  ///
  /// **Pending semantics:** if multiple edits produce pending drift and earlier
  /// pending could not be flushed yet, the "last drift wins" policy applies
  /// (the most recent PendingResync overwrites the previous one).
  ///
  /// **EOF behavior:** if a pending resync remains at end-of-file, it is
  /// dropped as harmless because there is no subsequent original code whose
  /// __LINE__ needs correction.
  ///
  /// \param originalFileText The pre-edit file contents the edits are expressed
  ///        against.
  /// \param edits Non-overlapping edits expressed in offsets of
  ///        originalFileText.
  /// \param appliedExpandedMacroRootIds Optional output set that, when non-null,
  ///        records root macro invocation ids for edits whose final applied
  ///        replacement leaves the corresponding macro expanded in the emitted
  ///        text.
  /// \param ownerIncludeId Include-instance owner for \p originalFileText, or
  ///        std::nullopt when streaming the TU.  Pending #line recovery uses this
  ///        to query the correct producer-selected conditional arms.
  /// \return The edited file text with any necessary #line directives emitted
  ///         (locally or deferred).
  std::string ApplyTextEditsWithPendingResync(
      StringRef originalFileText, ArrayRef<TextEdit> edits,
      DenseSet<uint64_t> *appliedExpandedMacroRootIds = nullptr,
      StringRef emissionOwner = StringRef(),
      std::optional<uint64_t> ownerIncludeId = std::nullopt,
      std::vector<MaterializedEditMapping> *materializedEditMappings = nullptr,
      std::vector<FinalLineControlPruneCandidate>
          *lineControlPruneCandidates = nullptr,
      std::vector<FinalLineControlSourceMapping>
          *lineControlSourceMappings = nullptr) const;

  /// Convert a B-token range into a half-open B-byte range.
  std::optional<std::pair<uint64_t, uint64_t>>
  BTokenRangeToByteRange(uint64_t bTokBegin, uint64_t bTokEnd) const;

  /// Remove visible sideband replay bytes from an ordinary replay payload when
  /// those bytes are owned by a separate, non-insertion sideband source edit.
  ///
  /// This enforces the replay-partition invariant: B-only sideband insertions
  /// may be carried by an ordinary insertion island, but sideband replacements
  /// and deletions have their own source edit and must not be duplicated by the
  /// ordinary token-envelope replay.
  std::string StripSeparatelyOwnedSidebandReplay(
      StringRef replayText, std::optional<uint64_t> replayBByteBegin,
      std::optional<uint64_t> replayBByteEnd) const;

  /// Stamp a TextEdit with the B-byte range of the materialized surface.
  void StampTextEditMaterializedBByteRange(TextEdit &edit, uint64_t begin,
                                           uint64_t end) const;

  /// Stamp a TextEdit with the B-byte range described by a B-token envelope.
  void StampTextEditMaterializedBTokenRange(TextEdit &edit,
                                            uint64_t bTokBegin,
                                            uint64_t bTokEnd) const;

  /// Stamp a TextEdit with the materialized ranges witnessed by a sideband
  /// edit proof.  The complete sideband proof binds the emitted replacement
  /// payload to its raw-B byte provenance, so TU sideband emission should stamp
  /// those two edit-map facts through this single gate rather than as
  /// independent fields.
  void StampTextEditMaterializedBReplayProof(
      TextEdit &edit, const SidebandPragmaEdit &sideband) const;

  /// Return the B-byte envelope contributed by sideband pragma edits owned by
  /// one materialized include, if that sideband stream supplies such a witness.
  std::optional<std::pair<uint64_t, uint64_t>>
  SidebandPragmaMaterializedBByteRangeForInclude(uint64_t includeId) const;

  /// Stamp a TextEdit with the replacement-text subrange to report on the
  /// refolded-output side of the optional materialized edit map.
  void StampTextEditMaterializedOutputTextRange(TextEdit &edit,
                                                uint64_t begin,
                                                uint64_t end) const;

  /// Return the replacement-text subrange to report for a final TextEdit.
  std::optional<std::pair<uint64_t, uint64_t>>
  TextEditMaterializedOutputTextRange(const TextEdit &edit) const;

  /// Return the replacement-text subrange to report for a macro patch.
  std::optional<std::pair<uint64_t, uint64_t>>
  MacroPatchMaterializedOutputTextRange(const MacroPatch &patch) const;

  /// Return the B-byte range carried by a final TextEdit, recovering direct TU
  /// hunk ranges from token provenance when the byte range was not pre-stamped.
  std::optional<std::pair<uint64_t, uint64_t>>
  TextEditMaterializedBByteRange(const TextEdit &edit) const;

  /// Return the B-byte range for a macro patch, using its stamped envelope when
  /// present and falling back to the macro's mapped expansion cover otherwise.
  std::optional<std::pair<uint64_t, uint64_t>>
  MacroPatchMaterializedBByteRange(const MacroPatch &patch) const;

  /// \brief Audit the complete accepted-proof surface before bytes are emitted.
  ///
  /// Phase 1F makes this the last accepted-proof gate before the applicator
  /// splices replacement text into a source file.  The audit checks every
  /// normalized edit, every carrier attached to each edit, and the composition
  /// law for multi-carrier edits.
  bool AuditAcceptedEditProofs(ArrayRef<TextEdit> edits,
                               StringRef emissionPhase,
                               StringRef emissionOwner = StringRef()) const;

  /// \brief Return whether an emitted non-terminal byte edit is backed only by
  /// emission-discharged normalized accepted-result carriers.
  ///
  /// Proof discharge is the universal gate at the actual emission boundary.
  /// Every non-terminal artifact that survives to a TextEdit must
  /// carry at least one normalized accepted-result candidate, and every such
  /// carrier must already be fully discharged under the proof contract that is
  /// appropriate for emitted source text. Terminal out-of-domain results are
  /// never valid carriers for non-terminal emitted edits.
  bool EmittedTextEditHasDischargedAcceptedResults(
      const TextEdit &edit, StringRef emissionPhase,
      StringRef emissionOwner = StringRef()) const;

  /// \brief Verify that multiple carriers on one edit compose in source order.
  ///
  /// Individual carrier normalization is not enough for a composite `TextEdit`:
  /// the carriers must either be equivalent witnesses for the same source
  /// surface or form a deterministic, gap-free ordered segment sequence in one
  /// comparable coordinate space.  Until later state-gap phases provide typed
  /// state-closed gap witnesses, non-empty inter-segment gaps are rejected here
  /// rather than guessed.
  bool EmittedTextEditHasOrderedAcceptedProofComposition(
      const TextEdit &edit, StringRef emissionPhase,
      StringRef emissionOwner = StringRef()) const;

  /// \brief Attach accepted-result metadata to an emitted text edit.
  ///
  /// Copies the normalized accepted-result carrier selected by the proof
  /// lattice onto the concrete `TextEdit` that will be emitted. This preserves
  /// the accepted path, proof-discharge inventory, witnesses, and audit
  /// metadata at the byte-edit boundary so later validation/reporting can
  /// reason about the emitted edit without re-running candidate selection.
  ///
  /// \param edit The emitted text edit to annotate.
  /// \param candidate The accepted-result candidate whose carrier metadata
  ///        should be copied onto \p edit.
  void
  AttachAcceptedResultCarrier(TextEdit &edit,
                              const AcceptedResultCandidate &candidate) const;

  /// \brief Appends an unchanged slice of the original file original[from:to)
  /// into out, while attempting to flush a previously-deferred PendingResync at
  /// the earliest safe point.
  ///
  /// A pending resync represents a required logical #line correction that could
  /// not be emitted inside a prior replacement without risking token adjacency
  /// changes. This method flushes that directive when it becomes safe to do so
  /// while streaming unchanged original content.
  ///
  /// ### Flush rules
  /// 1. **Immediate flush at slice begin:** if the output is currently at BOL
  ///    and `from` is a BOL in `original`, emit the directive before appending
  ///    the slice.
  /// 2. **Otherwise:** scan forward in `original[from:to)` for the first
  /// newline
  ///    boundary that is safe (i.e., not a preprocessor line-splice such as
  ///    `\\\n`). After copying through that newline, emit the directive at the
  ///    following BOL.
  /// 3. **No-op suppression:** even when a flush location is found, the
  ///    directive is only appended if
  ///    LineDirectiveInserter::shouldEmitLineDirective indicates it would
  ///    change logical state (i.e., it is not already in the same file/line
  ///    context).
  ///
  /// If no safe flush point exists within [from,to), the pending resync is
  /// returned unchanged so it can be attempted again on the next original
  /// slice.
  ///
  /// \param out Destination output buffer for the final file emission.
  /// \param original The original file contents we are streaming from.
  /// \param from Start offset (inclusive) of the unchanged slice.
  /// \param to End offset (exclusive) of the unchanged slice.
  /// \param pending A pending resync to flush.  Its ownerIncludeId is preserved
  ///        when querying the refold-map conditional activity for the flush site.
  /// \return std::nullopt if the pending resync was flushed; otherwise the
  ///         still-pending resync.
  std::optional<PendingResync>
  AppendOriginalSliceWithPending(SmallVectorImpl<char> &out, StringRef original,
                                 uint64_t from, uint64_t to,
                                 std::optional<PendingResync> pending,
                                 StringRef emissionOwner = StringRef(),
                                 std::optional<uint64_t> ownerIncludeId = std::nullopt,
                                 std::vector<FinalLineControlPruneCandidate>
                                     *lineControlPruneCandidates = nullptr,
                                 std::vector<FinalLineControlSourceMapping>
                                     *lineControlSourceMappings = nullptr) const;

  // ------------------------ Low-level Mapping & Utils ------------------------

  /// \brief Resolves the TU/file byte start offset corresponding to a PP
  /// coordinate for a specific file.
  ///
  /// The refold model maintains a PP->(file, byte-range) mapping (e.g.
  /// \c tokmapByPP) that allows code working in PP space to locate the
  /// corresponding region in an owning file (TU or header).
  ///
  /// Phase 8g deliberately makes this helper exact-only.  A PP coordinate that
  /// does not map into \p file is not silently projected to physical EOF.  EOF
  /// insertions are admissible only through an explicit include-anchor proof
  /// (for example a mapped left-neighbor insertion whose zero-width patch is at
  /// the include cover end), or else the caller must realize the include/fall
  /// closed to a wider declared proof class.
  ///
  /// \param file the file path whose mapping is being queried (TU or included
  ///        header)
  /// \param pp the PP token index in the A-side preprocessed token stream to
  ///        resolve
  /// \return the mapped start byte offset in \p file, or `std::nullopt` if the
  ///         PP coordinate has no exact source mapping into \p file
  std::optional<uint64_t> ByteStartForPPInFile(StringRef file,
                                               uint64_t pp) const {
    auto it = model_.GetTokmapByPP().find(pp);
    if (it != model_.GetTokmapByPP().end() && PathsEqual(it->second.file, file))
      return it->second.b;
    return std::nullopt;
  }

  /// \brief Resolves the TU/file byte end offset corresponding to a PP
  /// coordinate for a specific file.
  ///
  /// Analogous to ByteStartForPPInFile() but returns the mapped end byte offset
  /// (\c e).  This helper is also exact-only: an unmapped PP coordinate must not
  /// manufacture an EOF byte anchor.
  ///
  /// \param file the file path whose mapping is being queried (TU or included
  ///        header)
  /// \param pp the PP token index in the A-side preprocessed token stream to
  ///        resolve
  /// \return the mapped end byte offset in \p file, or `std::nullopt` if the PP
  ///         coordinate has no exact source mapping into \p file
  std::optional<uint64_t> ByteEndForPPInFile(StringRef file, uint64_t pp) const {
    auto it = model_.GetTokmapByPP().find(pp);
    if (it != model_.GetTokmapByPP().end() && PathsEqual(it->second.file, file))
      return it->second.e;
    return std::nullopt;
  }

  /// \brief Ensure that \p path has a cached weakly-canonical spelling.
  ///
  /// Canonicalization failures are fatal, matching the historical behavior of
  /// \c PathsEqual().
  void CacheCanonicalPath(StringRef path) const;

  /// \brief Compare two paths for equality after weak canonicalization.
  ///
  /// If either path is empty, returns string equality directly. Otherwise both
  /// paths are weakly canonicalized (`std::filesystem::weakly_canonical`) and
  /// compared. Canonicalization failures are fatal.
  ///
  /// \param a First path.
  /// \param b Second path.
  /// \returns `true` if canonical paths are equal; `false` otherwise.
  bool PathsEqual(StringRef a, StringRef b) const;

};

inline RefoldEngine::OwnerStateDelta
RefoldEngine::BuildTheoremStateDelta(const OwnerStateFacts &facts,
                                     const OwnerStateDelta &directDelta) {
  OwnerStateDelta projected;

  auto projectComponentFacts = [&](const OwnerStateFacts &bucket) {
    // Entry/Observes receive state requirements and observations.
    for (const MacroStateIdentity &identity : bucket.macroRequirements) {
      projected.Entry.AddMacroRequirement(identity);
      projected.Observes.AddMacroRequirement(identity);
    }
    for (const MacroStateObservation &observation :
         bucket.macroExpansionObservations) {
      projected.Entry.AddMacroObservation(observation);
      projected.Observes.AddMacroObservation(observation);
    }
    for (const MacroStateObservation &observation :
         bucket.definedOperatorObservations) {
      projected.Entry.AddMacroObservation(observation);
      projected.Observes.AddMacroObservation(observation);
    }
    for (const MacroStateObservation &observation :
         bucket.conditionalMacroObservations) {
      projected.Entry.AddMacroObservation(observation);
      projected.Observes.AddMacroObservation(observation);
    }
    for (const BuiltinLocationObservation &observation :
         bucket.builtinLocationObservations) {
      projected.Entry.AddBuiltinLocationObservation(observation);
      projected.Observes.AddBuiltinLocationObservation(observation);
    }
    for (const CounterEventIdentity &identity : bucket.counterEvents) {
      projected.Entry.AddCounterObservation(identity);
      projected.Observes.AddCounterObservation(identity);
      projected.Mutates.AddCounterMutation(identity);
      projected.Exit.AddCounterMutation(identity);
    }
    for (const IncludeStateIdentity &identity : bucket.includeStateEvents) {
      projected.Entry.AddIncludeStateEvent(identity);
      projected.Observes.AddIncludeStateEvent(identity);
      projected.Mutates.AddIncludeStateEvent(identity);
      projected.Exit.AddIncludeStateEvent(identity);
    }
    for (const IncludeGuardStateIdentity &identity :
         bucket.includeGuardStateEvents) {
      projected.Entry.AddIncludeGuardStateEvent(identity);
      projected.Observes.AddIncludeGuardStateEvent(identity);
      projected.Mutates.AddIncludeGuardStateEvent(identity);
      projected.Exit.AddIncludeGuardStateEvent(identity);
    }
    for (const PragmaStateIdentity &identity : bucket.pragmaStateEvents) {
      projected.Entry.AddPragmaStateEvent(identity);
      projected.Observes.AddPragmaStateEvent(identity);
      projected.Mutates.AddPragmaStateEvent(identity);
      projected.Exit.AddPragmaStateEvent(identity);
    }
    for (const ConditionalStateIdentity &identity :
         bucket.conditionalStateEvents) {
      projected.Entry.AddConditionalStateEvent(identity);
      projected.Observes.AddConditionalStateEvent(identity);
      projected.Mutates.AddConditionalStateEvent(identity);
      projected.Exit.AddConditionalStateEvent(identity);
    }

    // Mutates/Exit receive state transitions.
    for (const MacroStateIdentity &identity : bucket.macroDefinitions) {
      projected.Mutates.AddMacroDefinition(identity);
      projected.Exit.AddMacroDefinition(identity);
    }
    for (const MacroStateIdentity &identity : bucket.macroUndefinitions) {
      projected.Mutates.AddMacroUndefinition(identity);
      projected.Exit.AddMacroUndefinition(identity);
    }
    for (const LineControlStateIdentity &identity :
         bucket.lineControlEvents) {
      projected.Mutates.AddLineControlEvent(identity);
      projected.Exit.AddLineControlEvent(identity);
    }

    // Missing facts are theorem obligations, so they remain visible in every
    // bucket.  MissingOwnerOrderingFacts remains the precise unmodeled-ordering
    // marker; no flat poison bit is synthesized.
    for (const MissingStateFact &fact : bucket.missingStateFacts) {
      projected.Entry.AddMissingStateFact(fact.kind, fact.detail);
      projected.Observes.AddMissingStateFact(fact.kind, fact.detail);
      projected.Mutates.AddMissingStateFact(fact.kind, fact.detail);
      projected.Exit.AddMissingStateFact(fact.kind, fact.detail);
    }
  };

  // Project precise facts attached to the builder surface, then merge precise
  // facts that later phases may have attached directly to delta buckets.
  projectComponentFacts(facts);
  OwnerStateDelta preciseDelta;
  preciseDelta.MergeTheoremFactsFrom(directDelta);
  projected.MergeFrom(preciseDelta);
  return projected;
}

inline bool RefoldEngine::OwnerStateDeltaHasUnmodeledState(
    const OwnerStateDelta &delta) {
  return delta.Entry.HasMissingFactKind(
             MissingStateFactKind::MissingOwnerOrderingFacts) ||
         delta.Observes.HasMissingFactKind(
             MissingStateFactKind::MissingOwnerOrderingFacts) ||
         delta.Mutates.HasMissingFactKind(
             MissingStateFactKind::MissingOwnerOrderingFacts) ||
         delta.Exit.HasMissingFactKind(
             MissingStateFactKind::MissingOwnerOrderingFacts);
}

inline bool RefoldEngine::OwnerStateDeltaHasUnknownPragmaState(
    const OwnerStateDelta &delta) {
  return delta.Entry.HasTheoremUnknownPragmaState() ||
         delta.Observes.HasTheoremUnknownPragmaState() ||
         delta.Mutates.HasTheoremUnknownPragmaState() ||
         delta.Exit.HasTheoremUnknownPragmaState();
}

inline bool RefoldEngine::OwnerStateDeltaMutatesAnyState(
    const OwnerStateDelta &delta) {
  return delta.Mutates.MutatesAnyState() || delta.Exit.MutatesAnyState();
}

inline RefoldEngine::OwnerObserverSummary
RefoldEngine::OwnerStateDeltaToObserverSummary(const OwnerStateDelta &delta) {
  const StateObservations &observations = delta.Observes;
  OwnerObserverSummary observers;
  observers.observesMacroExpansion =
      observations.HasMacroRequirements() ||
      observations.HasMacroExpansionObservations();
  observers.observesDefinedOperator =
      observations.HasDefinedOperatorObservations();
  observers.observesConditionalEvaluation =
      observations.HasConditionalMacroObservations() ||
      observations.HasConditionalStateEvents();
  observers.observesLineNumber = llvm::any_of(
      observations.builtinLocationObservations,
      [](const BuiltinLocationObservation &obs) {
        return obs.kind == BuiltinLocationObservationKind::LineState;
      });
  observers.observesFileState = llvm::any_of(
      observations.builtinLocationObservations,
      [](const BuiltinLocationObservation &obs) {
        return obs.kind == BuiltinLocationObservationKind::FileState;
      });
  observers.observesFileName = llvm::any_of(
      observations.builtinLocationObservations,
      [](const BuiltinLocationObservation &obs) {
        return obs.kind == BuiltinLocationObservationKind::FileNameState;
      });
  observers.observesCounter = observations.HasCounterEvents();
  observers.observesPragmaState = observations.HasPragmaStateEvents() ||
                                  observations.HasTheoremUnknownPragmaState();
  observers.observesIncludeGuardState =
      observations.HasIncludeGuardStateEvents();
  observers.observesIncludeState = observations.HasIncludeStateEvents();
  for (const MissingStateFact &fact : observations.missingStateFacts) {
    switch (fact.kind) {
    case MissingStateFactKind::MissingMacroFacts:
      observers.observesMacroExpansion = true;
      observers.observesDefinedOperator = true;
      observers.observesConditionalEvaluation = true;
      break;
    case MissingStateFactKind::MissingLineControlFacts:
      observers.observesLineNumber = true;
      observers.observesFileState = true;
      observers.observesFileName = true;
      break;
    case MissingStateFactKind::MissingCounterFacts:
      observers.observesCounter = true;
      break;
    case MissingStateFactKind::MissingPragmaFacts:
      observers.observesPragmaState = true;
      break;
    case MissingStateFactKind::MissingIncludeGuardFacts:
      observers.observesIncludeGuardState = true;
      break;
    case MissingStateFactKind::MissingConditionalFacts:
      observers.observesConditionalEvaluation = true;
      break;
    case MissingStateFactKind::MissingOwnerOrderingFacts:
      // There is no precise component when the owner/event ordering fact is
      // absent.  The missing fact remains in the delta so later graph/gateway
      // checks can report the ordering obligation explicitly.
      break;
    }
  }
  return observers;
}

} // namespace refold
} // namespace clang

namespace llvm {
using namespace clang::refold;

template <> struct format_provider<RefoldEngine::TerminalFallbackWitness> {
  static void format(const RefoldEngine::TerminalFallbackWitness &witness,
                     raw_ostream &os, StringRef style) {
    os << toString(witness);
  }
};

template <> struct DenseMapInfo<std::optional<uint64_t>> {
  static inline std::optional<uint64_t> getEmptyKey() {
    return std::optional<uint64_t>(~0ULL);
  }
  static inline std::optional<uint64_t> getTombstoneKey() {
    return std::optional<uint64_t>(~1ULL);
  }
  static unsigned getHashValue(const std::optional<uint64_t> &val) {
    // If val has a value, hash it and cast to unsigned.
    // If not (std::nullopt), return a constant (like 0).
    return val ? static_cast<unsigned>(llvm::hash_value(*val)) : 0u;
  }
  static bool isEqual(const std::optional<uint64_t> &lhs,
                      const std::optional<uint64_t> &rhs) {
    return lhs == rhs;
  }
};
} // namespace llvm

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H
