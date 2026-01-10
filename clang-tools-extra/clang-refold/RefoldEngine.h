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
#include "LineDirectiveInserter.h"
#include "RefoldModel.h"
#include "StringUtils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

struct PPTok {
  std::string spelling;
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
/// The engine intentionally does not attempt to model complex preprocessor
/// features that require semantic re-expansion (e.g., token-pasting, variadics,
/// __COUNTER__, or aggressive escape normalization). In such cases it prefers
/// deterministic expansion of the affected macro instance over speculative
/// rewriting.
///
/// \author jeikenberry
class RefoldEngine {
public:
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
  /// \param onlyCheck  If true, then we should only verify that \c aToks and
  ///                   \c bToks align.
  /// \param noLines    If true, then do not inject #line
  /// \returns          The refolded, partially expanded C source.
  static Expected<std::string>
  Refold(const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
         ArrayRef<std::size_t> aTokOff, StringRef bSource,
         ArrayRef<PPTok> bToks, ArrayRef<std::size_t> bTokOff, bool onlyCheck,
         bool noLines);

private:
  const RefoldModel model_;
  StringRef aSource_, bSource_;
  ArrayRef<PPTok> aToks_, bToks_;
  ArrayRef<std::size_t> aTokOff_, bTokOff_;
  LineDirectiveInserter lineDirs_;

  /// Construct an engine from concrete inputs. The instance method `Refold()`
  /// runs the full pipeline using these captured members.
  RefoldEngine(RefoldModel model, StringRef aSource, ArrayRef<PPTok> aToks,
               ArrayRef<std::size_t> aTokOff, StringRef bSource,
               ArrayRef<PPTok> bToks, ArrayRef<std::size_t> bTokOff,
               bool noLines)
      : model_(std::move(model)), aSource_(aSource), bSource_(bSource),
        aToks_(aToks), bToks_(bToks), aTokOff_(aTokOff), bTokOff_(bTokOff),
        lineDirs_(!noLines, model_.GetPPCwd()) {}

  std::string Refold();

  // ---------------------------- Small Data Records ---------------------------

  /// Represents a pending resync that must be flushed at the next safe BOL.
  struct PendingResync {
    std::string fileSpellingForDir;

    explicit PendingResync(llvm::StringRef file)
        : fileSpellingForDir(file.str()) {}
  };

  /// The final text and optional pending state for an edit.
  struct ResyncOutcome {
    std::string text;
    std::optional<PendingResync> pending;

    ResyncOutcome(std::string t, std::optional<PendingResync> p)
        : text(std::move(t)), pending(std::move(p)) {}
  };

  struct TextEdit {
    int start, end;
    std::string text;
    std::optional<PendingResync> pending;
  };

  struct MacroPatch {
    int invStart, invEnd;
    std::string replacement;
  };

  struct IncludePatch {
    const RefoldModel::IncludeItem *include;
    std::string insertBytes; // exact B bytes
    int aStart, aEnd;        // A-token interval inside include expansion
    int bStart, bEnd;        // B-token interval

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

      // 4. Format everything using formatv
      // {0} = id, {1} = path, {2} = aStart, etc.
      return formatv(
                 "IncludePatch{{incId={0}, path={1}, A=[{2},{3}), B=[{4},{5}), "
                 "insert='{6}{7}'}",
                 include->id, path, aStart, aEnd, bStart, bEnd, escapedPreview,
                 (truncated ? "..." : ""))
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

  struct MacroDefineInfo {
    int directiveId;
    std::string name;

    /// Formal parameter names in the order they appear in the #define.
    std::vector<std::string> params;

    /// The indices of parameters that are stringified (#), in the order
    /// they appear in the macro replacement list.
    std::vector<int> stringifyParamOrder;

    /// A set of parameter indices that undergo stringification,
    /// used for O(1) lookups.
    DenseSet<int> stringifyParamSet;
  };

  // ---------------------------- Ownership Helpers ----------------------------

  enum class OwnerKind { TU, Include, Unknown };

  static inline StringRef toString(OwnerKind kind) {
    switch (kind) {
    case OwnerKind::TU:
      return "TU";
    case OwnerKind::Include:
      return "Include";
    case OwnerKind::Unknown:
      return "Unknown";
    }
    llvm_unreachable("Invalid owner kind");
  }

  // Grant access to the specific formatter specialization
  template <typename T, typename Enable> friend struct llvm::format_provider;

  struct Owner {
    OwnerKind kind = OwnerKind::Unknown;
    std::optional<int> includeId; // non-nullopt only when kind == INCLUDE
    std::optional<int> condArmId; // nullable; non-nullopt when segment is in a
                                  // specific arm

    static Owner TU(std::optional<int> condArmId = std::nullopt) {
      Owner o;
      o.kind = OwnerKind::TU;
      o.condArmId = condArmId;
      return o;
    }
    static Owner Include(int includeId,
                         std::optional<int> condArmId = std::nullopt) {
      Owner o;
      o.kind = OwnerKind::Include;
      o.includeId = includeId;
      o.condArmId = condArmId;
      return o;
    }
    static Owner Unknown() { return Owner(); }
  };

  // ------------------------------- Core Helpers ------------------------------

  /// \brief Maps preprocessor tokens into a stable lexeme sequence suitable for
  /// diff/LCS alignment.
  ///
  /// This method produces the token sequence consumed by the LCS/Myers
  /// pipeline. For most tokens, the emitted element is simply PPTok::spelling.
  /// For ASCII-whitespace tokens, the emitted element is replaced with a
  /// position-tied sentinel of the form "WS@<offset>".
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
  ///          the original token spelling or a position-tied whitespace sentinel.
  static std::vector<std::string> MapLexemes(ArrayRef<PPTok> toks,
                                             ArrayRef<std::size_t> offs);

  /// \brief Finds the nearest mapped B-side token index at or after an A-side
  /// position.
  ///
  /// The \p a2b array is an A→B alignment map where `a2b[k] >= 0` indicates
  /// that A token `k` is aligned to B token `a2b[k]`, and `a2b[k] == -1`
  /// indicates deletion (no aligned B token). This helper scans forward from \p
  /// I to locate the first aligned token.
  ///
  /// This is commonly used when anchoring a boundary insertion or determining
  /// the "next" B-side anchor for an edit whose natural A-side anchor has been
  /// deleted.
  ///
  /// \param a2b An A→B alignment array; entries are B indices or -1 for
  ///            unmapped/deleted A tokens.
  /// \param i   The starting A index to search from; values outside the range
  ///            [0, a2b.size()) are clamped to the nearest valid search start.
  /// \returns The first `a2b[k]` with `k >= i` and `a2b[k] >= 0`, or -1 if no
  ///          such mapping exists.
  static int MapForwardToB(ArrayRef<int> a2b, int i) {
    const std::size_t start = (i < 0) ? 0u : static_cast<std::size_t>(i);
    for (std::size_t k = start; k < a2b.size(); ++k)
      if (a2b[k] >= 0)
        return a2b[k];
    return -1;
  }

  /// \brief Finds the nearest mapped B-side token index at or before an A-side
  /// position.
  ///
  /// The \p a2b array is an A→B alignment map where `a2b[k] >= 0` indicates
  /// that A token `k` is aligned to B token `a2b[k]`, and `a2b[k] == -1`
  /// indicates deletion (no aligned B token). This helper scans backward from
  /// \p I to locate the last aligned token.
  ///
  /// This is commonly used when anchoring a boundary insertion or determining
  /// the "previous" B-side anchor for an edit whose natural A-side anchor has
  /// been deleted.
  ///
  /// \param a2b An A→B alignment array; entries are B indices or -1 for
  ///            unmapped/deleted A tokens.
  /// \param i   The starting A index to search from; values outside the range
  ///            [0, a2b.size()) are clamped to the nearest valid search start.
  /// \returns The first `a2b[k]` with `k <= i` and `a2b[k] >= 0`, or -1 if no
  ///          such mapping exists.
  static int MapBackwardToB(ArrayRef<int> a2b, int i) {
    for (int k = std::min(i, static_cast<int>(a2b.size()) - 1); k >= 0; --k)
      if (a2b[k] >= 0)
        return a2b[k];
    return -1;
  }

  /// \brief Determine if two adjacent characters would “glue” tokens under C’s
  ///        maximal-munch rules.
  ///
  /// Returns `true` if concatenating `left` and `right` without a space would
  /// likely glue tokens under C’s maximal-munch rules, and therefore should be
  /// separated by a single space.
  ///
  /// #### Conservative rules (not a full lexer)
  /// * identifier + identifier → glues
  ///   (e.g., `return` + `injected` → `returninjected`)
  /// * identifier + `(` → glues
  ///   (looks like a call, e.g., `foo(...)`)
  /// * identifier + operator-like punctuation
  ///   (one of `: ? + - * / % & | ^ < > = !`) → prefer separating space
  ///   (e.g., `0:` becomes `0 :`)
  ///
  /// The check is intentionally conservative and line-local; it avoids changing
  /// tokenization without performing full lexical analysis.
  ///
  /// \param left  Character immediately to the left of a boundary (or `'\0'` if
  /// none).
  /// \param right Character immediately to the right of a boundary (or `'\0'`
  /// if none).
  /// \returns `true` if a space should be inserted to prevent token gluing;
  ///          otherwise `false`.
  static bool BoundaryGlues(char left, char right);

  /// \brief Add padding spaces around a replacement snippet to prevent token
  /// gluing.
  ///
  /// Adds at most one space on the left and/or right edge of `text` so that,
  /// when `text` replaces `base[start,end)` (half-open), tokens do not “glue”
  /// across the boundaries. If `text` already contains leading or trailing
  /// whitespace, no additional space is added on that side. Callers can
  /// suppress padding on either side via `allowLeft` / `allowRight` (useful
  /// when preserving an existing gap).
  ///
  /// #### Behavior
  /// * Find the first and last non-whitespace character inside `text`.
  /// * If allowed and needed (per `boundaryGlues(char, char)`), prepend or
  /// append
  ///   a single space.
  /// * Never inserts more than one space per side and never modifies `base`.
  ///
  /// Deterministic and local — decisions are made only from the immediate
  /// boundary characters and the first/last non-whitespace character in `text`.
  ///
  /// \param base       The original target string being patched.
  /// \param start      Start index (inclusive) of the slice in `base` to
  /// replace.
  /// \param end        End index (exclusive) of the slice in `base` to replace.
  /// \param text       The replacement snippet to be inserted.
  /// \param allowLeft  Whether a left-side pad is permitted.
  /// \param allowRight Whether a right-side pad is permitted.
  /// \returns `text`, possibly prefixed and/or suffixed with a single space to
  ///          avoid token gluing.
  static std::string PadAtBoundaries(StringRef base, size_t start, size_t end,
                                     std::string text, bool allowLeft,
                                     bool allowRight);

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
  std::vector<unsigned> ComputeOwnerDepthGapsForPP(size_t numOfAOffs);

  /// \brief Classifies the logical "owner" of a diff hunk using the precomputed
  /// segment map for the translation unit.
  ///
  /// A hunk is given in A-side token coordinates [\p a0, \p a1) and describes
  /// how a region of the preprocessed stream was edited. This method projects
  /// the hunk into TU byte space, finds the smallest matching Segment,
  /// and returns an Owner describing whether the hunk belongs to:
  ///   - the TU file itself (kind = Owner::Kind::TU),
  ///   - a particular include (kind = Owner::Kind::INCLUDE}),
  ///   - a specific conditional arm inside an include
  ///     (kind = Owner::Kind::COND_ARM), or
  ///   - a macro expansion or unknown/ambiguous region.
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
  ///    single segment, the owner is returned as Owner::Kind::UNKNOWN and
  ///    callers are expected to fall back to other heuristics (e.g.
  ///    hunkMapsToTU or smallestCoveringInclude).
  ///
  /// \param tuPath  absolute path of the TU file currently being refolded.
  /// \param h       the hunk to classify (in A-side token coordinates).
  /// \param aTokOff optional mapping from A-side token indices to TU byte
  ///                offsets; currently unused but reserved for future
  ///                refinements of segment selection.
  /// \returns An Owner describing which entity (TU, include, conditional arm,
  ///          macro, unknown) the hunk logically belongs to.
  Owner ClassifyOwnerWithSegments(StringRef tuPath, const diffutils::Hunk &h);

  /// Returns the innermost (smallest-width) patchable macro invocation that
  /// fully covers a given A-span.
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
  const RefoldModel::MacroInvocation *
  SmallestCoveringPatchableMacro(int aStart, int aEnd) const;

  /// \brief Returns the smallest include whose PP coverage contains a
  /// zero-width patch position, or (for non-empty ranges) fully covers the span.
  ///
  /// Policy: for pure insertions (\p ALo == \p AHi) we treat positions that
  /// land exactly on an include's PP start boundary (coverBegin) as inside that
  /// include, and positions at or beyond coverEnd as outside. In interval terms
  /// we treat the include's PP coverage as [coverBegin, coverEnd) and require
  /// coverBegin <= \p ALo < coverEnd.
  ///
  /// For non-empty ranges (\p ALo < \p AHi) we keep the original semantics: any
  /// include whose coverage interval [coverBegin, coverEnd) fully contains
  /// [\p ALo, \p AHi) is a candidate, and we pick the one with the smallest
  /// width.
  ///
  /// \param aLo  Inclusive start PP-token index in A.
  /// \param aHi  Exclusive end PP-token index in A.
  /// \returns The smallest covering include item, or nullptr if none cover the
  ///          span.
  const RefoldModel::IncludeItem *SmallestCoveringInclude(int aLo,
                                                          int aHi) const;

  /// \brief Return the innermost (smallest-width) macro invocation that fully
  ///        covers a given A-span.
  ///
  /// Determines which macro invocation in the refold model fully covers the
  /// specified token span `[aStart, aEnd)` in A-token units. The coverage test
  /// uses each macro’s preprocessed-token interval `[coverBegin, coverEnd)`.
  /// Among all invocations that contain the queried span, the one with the
  /// minimal width is selected. Ties are broken by macro ID (ascending) to
  /// ensure deterministic results. Macro items without valid cover information
  /// are ignored.
  ///
  /// Used during hunk classification to select the most local macro call site
  /// that owns an edit.
  ///
  /// \param aStart  Inclusive start preprocessed-token index in A.
  /// \param aEnd    Exclusive end preprocessed-token index in A.
  /// \returns       The smallest covering macro invocation, or `nullptr` if
  ///                none cover the span.
  const RefoldModel::MacroInvocation *SmallestCoveringMacro(int aStart,
                                                            int aEnd) const;

  /// \brief Determine whether an A-token interval is owned by the translation
  ///        unit (TU).
  ///
  /// Determines ownership purely from the refold map’s token→file mapping:
  ///
  /// * If the interval contains any mapped preprocessed (PP) tokens whose
  ///   mapped file is *not* `tuPath`, the hunk is **not** TU-owned (returns
  ///   `false`).
  /// * Whitespace-only regions (unmapped tokens) are ignored for ownership.
  /// * Pure insertions (`a0 == a1`) and whitespace-only ranges are treated as
  ///   TU-owned; their final byte span is derived later from neighboring edits.
  ///
  /// This function is intentionally strict: any concrete mapping to a non-TU
  /// file disqualifies the hunk. This keeps refolding deterministic and
  /// prevents edits from being applied to the wrong file.
  ///
  /// \param a0      Inclusive start preprocessed-token index in A.
  /// \param a1      Exclusive end preprocessed-token index in A.
  /// \param tuPath  Absolute canonical path of the TU’s source file.
  /// \returns       `true` if the interval is TU-owned (only TU mappings or
  ///                unmapped/empty);
  ///                `false` if any mapped token belongs to a non-TU file.
  bool HunkMapsToTU(int a0, int a1, StringRef tuPath) const;

  /// \brief Attempts to anchor a *pure insertion* (a PP-gap insertion) to a
  /// deterministic, canonical TU byte boundary that represents the *same*
  /// preprocessed coordinate.
  ///
  /// A PP-gap `ppGap` is a boundary between two adjacent PP tokens (i.e. a
  /// "gap" index). For an insertion that conceptually occurs at that PP
  /// boundary, this method tries to return a TU byte offset that is an
  /// **exact** structural boundary in the TU corresponding to that same PP
  /// coordinate.
  ///
  /// **Key property:** this method performs *no* "nearest" snapping. If the
  /// insertion site does not correspond exactly to a known boundary PP
  /// coordinate, it returns `std::nullopt` so callers can fall back to
  /// neighbor-based span anchoring. This avoids regressions where an insertion
  /// that belongs inside a nested owner (include/arm) is incorrectly pulled out
  /// to a shallower boundary.
  ///
  /// **Boundary sources considered** (each producing a candidate `(pp,b)`
  /// pair):
  /// * **(A) Explicit TU slots** with an emitted `pp` coordinate and a
  ///   conservative "boundary-like" `kind` (file/arm/include boundaries).
  /// * **(B) Include directive boundaries** whose site is in `tuPath`:
  ///   begin PP = `min(span.begin)`, end PP = `max(span.end)`; mapped to
  ///   `before_include`/`after_include` slots.
  /// * **(C) Conditional arm boundaries** in `tuPath` when `ppSpan` exists:
  ///   begin PP = `arm.ppSpan.begin`, end PP = `arm.ppSpan.end`; mapped to
  ///   `arm_begin`/`arm_end` slots.
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
  ///        match model file keys)
  /// \param ppGap the PP gap index (between PP tokens) representing the desired
  ///        insertion coordinate
  /// \return the TU byte offset of an exact canonical boundary matching
  ///         `ppGap`, or `std::nullopt` if `ppGap` is not exactly on a known
  ///         boundary (caller should fall back)
  std::optional<int> AnchorToNearestSlotBoundaryFromPPGap(StringRef tuPath,
                                                          int ppGap) const;

  /// \brief Computes the minimum PPSpan::begin value across a collection of
  /// preprocessor spans.
  ///
  /// This helper is used to summarize a set of PP spans into a single "earliest
  /// begin" coordinate in PP space. If the input is empty, -1 is returned as
  /// a sentinel.
  ///
  /// \param Spans A list of PP spans.
  /// \returns The minimum begin value across all spans, or -1 if Spans is
  /// empty.
  static int MinPPBegin(ArrayRef<RefoldModel::PPSpan> spans) {
    if (spans.empty())
      return -1;

    int min = std::numeric_limits<int>::max();
    for (const auto &s : spans)
      min = std::min(min, s.begin);

    return (min == std::numeric_limits<int>::max()) ? -1 : min;
  }

  /// \brief Computes the maximum PPSpan::end value across a collection of
  /// preprocessor spans.
  ///
  /// This helper is used to summarize a set of PP spans into a single "latest
  /// end" coordinate in PP space. If the input is empty, -1 is returned as
  /// a sentinel.
  ///
  /// \param Spans A list of PP spans.
  /// \returns The maximum end value across all spans, or -1 if Spans is empty.
  static int MaxPPEnd(ArrayRef<RefoldModel::PPSpan> spans) {
    if (spans.empty())
      return -1;

    int max = std::numeric_limits<int>::min();
    for (const auto &s : spans)
      max = std::max(max, s.end);

    return (max == std::numeric_limits<int>::min()) ? -1 : max;
  }

  /// \brief Computes the TU (translation unit) byte span [b, e) corresponding
  /// to an A-side PP-token interval [a0, a1).
  ///
  /// This method is used to convert a diff hunk expressed in A-token indices
  /// into a concrete byte range in the TU source file. Its contract is
  /// intentionally conservative: if the A-token interval does not touch the TU,
  /// the method returns {-1, -1} rather than "snapping" to an arbitrary
  /// TU neighbor, because snapping causes header/arm insertions to be
  /// mis-owned as TU edits.
  ///
  /// \par Contract
  ///
  /// 1. **Normalize indices:** if \p a0 > \p a1, the bounds are swapped so the
  ///    interval is well-formed.
  /// 2. **Direct TU coverage:** scan PP tokens in [\p a0, \p a1) and consider
  /// only
  ///    tokens whose \c TokMapEntry::file equals \p tuPath. If at least one
  ///    such token exists, return the minimal enclosing TU byte range over
  ///    those TU-mapped tokens: [min(ent.b), max(ent.e)).
  /// 3. **No TU tokens in the interval:**
  ///    - If \p a0 != \p a1 (non-empty interval) and no TU-mapped tokens were
  ///      found, return {-1, -1}. This indicates "not TU-owned / no TU span".
  ///    - If \p a0 == \p a1 (pure insertion at a PP gap):
  ///      1. First, attempt to anchor on an *exact* canonical slot boundary
  ///         recorded for the TU at the same PP coordinate via
  ///         AnchorToNearestSlotBoundaryFromPPGap(). If present, return {b, b}
  ///         using that slot's TU byte offset.
  ///      2. If no slot boundary exists at that PP coordinate, inspect the
  ///         nearest mapped neighbors (left of \p a0 and right of \p a0):
  ///         - If the insertion is bracketed by the same non-TU file (both
  ///           neighbors map to the same header/include), return {-1, -1}.
  ///         - If either neighbor maps to a non-TU file, return {-1, -1}.
  ///         - Otherwise, anchor to the nearest TU neighbor:
  ///           - If the right neighbor is TU-mapped, return {right.b, right.b}.
  ///           - Else if the left neighbor is TU-mapped, return {left.e,
  ///           left.e}.
  ///           - Else return {-1, -1}.
  ///
  /// All intervals are half-open. The sentinel {-1, -1} means "no TU span /
  /// not TU-owned". A zero-length span {b, b} denotes a concrete insertion
  /// anchor point in the TU.
  ///
  /// \param a0 inclusive start A-side PP-token index.
  /// \param a1 exclusive end A-side PP-token index.
  /// \param tuPath absolute/canonical TU path that must match
  /// TokMapEntry::file.
  /// \return A pair representing the TU byte span [b, e), or{-1, -1}.
  std::pair<int, int> TUByteSpan(int a0, int a1, StringRef tuPath) const;

  /// \brief Detects whether a pure insertion hunk lies exactly on the boundary
  /// between two sibling include regions, and if so, assigns ownership of the
  /// insertion to their parent include.
  ///
  /// A "pure insertion" is a hunk where the A-side is empty and the B-side is
  /// non-empty (h.aStart == h.aEnd && h.bStart < h.bEnd). For such hunks, the
  /// usual "innermost owner" logic can incorrectly attribute the insertion to
  /// one of the adjacent child headers, even when the intended location is
  /// between #include directives at the parent level.
  ///
  /// \par Heuristic
  /// This method probes PP-space neighbors around the insertion point:
  /// - Find the nearest mapped PP token to the left of \p aPos and to the right
  ///   of \p aPos using FindNearestTokmapEntry().
  /// - Resolve each neighbor to its innermost include id via
  ///   M.InnermostIncludeAtPP(pp). TU-owned regions are represented as nullopt.
  /// - If the two innermost include ids differ, compute their least common
  ///   ancestor include id via M.LeastCommonAncestorInclude(leftIncId,
  ///   rightIncId).
  ///
  /// \par Outcome
  /// - If the least common ancestor is a valid include id, return that
  ///   IncludeItem. This causes the insertion to be emitted at the parent
  ///   include level (i.e., between sibling #include lines) rather than inside
  ///   either child header.
  /// - If the least common ancestor is nullopt, the boundary meets only at the
  ///   TU, so this method returns nullptr to indicate "TU-owned" and allow the
  ///   caller's normal TU-level handling to proceed.
  /// - If the hunk is not a pure insertion, if neighbors cannot be resolved, or
  ///   if both sides map to the same include id, return nullptr and let the
  ///   standard ownership logic handle it.
  ///
  /// Note: \p ownerDepthGap is used only to establish \p maxPP (the PP
  /// coordinate range) for bounded probing; it does not otherwise participate
  /// in the decision.
  ///
  /// \param h the diff hunk; only pure insertion hunks are considered.
  /// \param tuPath the TU path for the refold (currently unused by this
  ///        heuristic but kept for signature consistency).
  /// \return the parent IncludeItem that should own the insertion if it is
  ///         between sibling includes; otherwise nullptr.
  const RefoldModel::IncludeItem *
  BoundaryParentIncludeForPureInsertion(const diffutils::Hunk &h,
                                        StringRef tuPath) const;

  /// \brief Finds the nearest TokMapEntry in PP space by linear probing from a
  /// starting PP coordinate.
  ///
  /// This helper walks PP coordinates beginning at \p start and repeatedly adds
  /// \p step until it either finds a non-null TokMapEntry in the model's
  /// tokmapByPP or the probe index exits the inclusive bounds [0, \p maxPP].
  /// It performs an exact-key lookup at each PP coordinate; it does not
  /// interpolate or choose the "closest" by distance beyond the first hit
  /// encountered in the chosen direction.
  ///
  /// Typical use cases include locating the nearest mapped PP token to the
  /// left/right of a PP gap when anchoring insertions or deciding ownership
  /// (TU vs include/header) in boundary cases.
  ///
  /// \param start The PP coordinate at which to begin probing.
  /// \param step The probe direction and stride; use -1 to search left and +1
  /// to
  ///             search right.
  /// \param maxPP The maximum PP coordinate to consider (inclusive); probing
  ///              stops when p < 0 or p > maxPP.
  /// \return The first TokMapEntry encountered while probing in the requested
  ///         direction, or nullptr if none exists within [0, maxPP].
  const RefoldModel::TokMapEntry *FindNearestTokmapEntry(int start, int step,
                                                         int maxPP) const {
    const auto &tokmapByPP = model_.GetTokmapByPP();
    int p = start;

    while (p >= 0 && p <= maxPP) {
      auto it = tokmapByPP.find(p);
      if (it != tokmapByPP.end()) {
        return &it->second;
      }
      p += step;
    }

    return nullptr;
  }

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

  /// \brief Computes a B-side preprocessor token envelope `[begin, end)` for a
  /// macro invocation.
  ///
  /// The envelope is the minimal half-open token range on the B-side covering
  /// the union of the macro invocation's **BODY spans** and **ARGUMENT spans**.
  /// When `onlyInvFile` is `true`, tokens are counted only if their
  /// `TokMapEntry::File` equals `m.InvFile`; otherwise, tokens from *any* file
  /// are eligible (useful when macro body tokens originate from headers).
  ///
  /// Implementation details:
  ///   - Walks all BODY and ARG spans in `m`.
  ///   - For each preprocessor token index `pp` in a span, looks up
  ///     `M.tokmapByPP[pp]` and (optionally) filters by `InvFile`.
  ///   - Tracks the minimal `begin` and maximal `end` (exclusive) token index
  ///   seen.
  ///
  /// Returns `false` if no eligible tokens were found (e.g., spans are
  /// empty/invalid, or all tokens were filtered out by `onlyInvFile`).
  ///
  /// @param m            The macro invocation whose BODY/ARG spans contribute
  /// to the envelope.
  /// @param onlyInvFile  If `true`, restrict tokens to those mapped to
  /// `m.InvFile`;
  ///                     if `false`, accept tokens regardless of file origin.
  /// @param begin        (Output) Lowest qualifying B-token index.
  /// @param end          (Output) Highest qualifying B-token index (exclusive).
  /// @return `true` if an envelope was found, `false` otherwise.
  bool MacroExpansionEnvelopeB(const RefoldModel::MacroInvocation &m,
                               bool onlyInvFile, int &begin, int &end) const;

  /// \brief Validates that an "args-only" macro refolding is representable at
  /// the invocation site.
  ///
  /// This is a safety gate used by the args-only policy: we prefer to rewrite
  /// only the macro invocation argument text (e.g., `FOO(x)`) instead of
  /// forcing a full expansion, but only when doing so is consistent with what
  /// the edited preprocessed output (B) implies for every occurrence of that
  /// formal parameter in the macro expansion.
  ///
  /// Occurrence identification is driven by refold-map metadata: `m.bodySpans`
  /// and `m.argSpans` are combined into "argument holes" (PP-token spans)
  /// tagged with an `argIdx`. Each hole corresponds to one use-site occurrence
  /// of a formal parameter within the expansion.
  ///
  /// For each occurrence, the method derives the corresponding B-token envelope
  /// deterministically using the A→B alignment map `a2b` (LCS-derived):
  /// - **Preferred:** map the A tokens adjacent to the occurrence and take the
  ///   strict interior range in B.
  /// - **Fallback:** map any surviving A tokens within the occurrence span
  ///   itself and take the inclusive envelope.
  ///
  /// The extracted B slice is compared against `newArg` after trimming edge
  /// whitespace.
  ///
  /// **Stringification handling:** if the B slice looks like a string literal,
  /// the method attempts to invert stringification (unstringify) and compare
  /// the resulting text against `newArg`. Additionally, if `argIdx` is listed
  /// in `stringifyParams`, then a mismatching string-literal occurrence does
  /// *not* automatically reject args-only: the B stream may contain a stale
  /// diagnostic string (e.g., `assert`) that is intentionally not kept in sync
  /// with the rewritten invocation argument. In that case, agreement on the
  /// unstringified / non-literal occurrences is treated as sufficient.
  ///
  /// **Conservatism:** if required metadata is missing (e.g., `m.argSpans` not
  /// present), the method returns `true` (vacuously satisfied) rather than
  /// forcing expansion. If an occurrence cannot be located in B or a
  /// non-stringified occurrence disagrees with `newArg`, it returns `false`.
  ///
  /// \param m Macro invocation being patched (provides cover/body/arg-span
  ///          metadata).
  /// \param argIdx Zero-based formal parameter index to validate.
  /// \param newArg Candidate invocation-site argument text after refolding.
  /// \param a2b Token mapping from A token indices to B token indices; -1
  ///            indicates deletion in B.
  /// \param stringifyParams Set of formal parameter indices that are
  ///                        stringified somewhere in the macro body (may be
  ///                        null and treated as empty).
  /// \returns `true` iff every occurrence of `argIdx` in the macro's B
  ///          expansion matches `newArg` (directly or via unstringification),
  ///          allowing mismatching string-literal occurrences when `argIdx` is
  ///          stringified; `false` otherwise.
  bool MacroArgReplacementMatchesAllOccurrencesInB(
      const RefoldModel::MacroInvocation &m, int argIdx, StringRef newArg,
      ArrayRef<int> a2b, const DenseSet<int> &stringifyParams) const;

  /// \brief Attempts to build a `MacroPatch` by reconciling edits strictly
  /// within the arguments of a function-like macro invocation, rather than
  /// replacing the entire macro expansion/call-site text.
  ///
  /// This is a “surgical” alternative to whole-macro replacement. It is
  /// applicable only when the edit hunks are entirely attributable to
  /// substitutions of one or more formal macro parameters (i.e., within
  /// argument “holes” in the macro expansion). If the edit overlaps macro
  /// boilerplate (tokens not originating from a parameter), or if the edit
  /// cannot be represented by modifying invocation arguments without changing
  /// other occurrences, the method returns `nullopt` (or equivalent) so the
  /// caller can fall back to a whole-expansion patch.
  ///
  /// ### Strategy
  ///
  /// - **Eligibility:** Requires `m.subkind == "func"`, a non-empty
  ///   `baseInvocationText`, and sane invocation byte offsets `invB/invE`.
  ///
  /// - **Hole identification:** Uses `body_spans` to identify where parameters
  ///   appear in the expansion and maps those spans to call-site argument
  ///   indices via `arg_spans` (producing `PPArgSpan` holes annotated with
  ///   `arg_index`).
  ///
  /// - **Stringification fallback:** If argument holes are absent (common for
  ///   stringified occurrences, `#param`, whose expansion token may not be
  ///   tagged as a macro-arg expansion), attempts to synthesize an argument
  ///   hole based on the active macro definition and the edited string literal
  ///   in B.
  ///
  /// - **Containment check:** Verifies the triggering `Hunk` lies fully within
  /// one
  ///   or more argument holes. If any part overlaps non-parameter expansion
  ///   text, the method fails.
  ///
  /// - **Mapping A→B:** For each touched argument hole, projects the A-side
  /// token
  ///   span to a B-side token envelope using `a2b`, widens to include inserted
  ///   boundary tokens from the hunk when necessary, and extracts the
  ///   replacement argument text from `bSource` using `bTokOff` (with
  ///   edge-whitespace trimmed).
  ///
  /// - **Multi-occurrence consistency:** If a formal parameter appears multiple
  ///   times in the expansion, the method requires that all corresponding
  ///   occurrences in B agree with the same argument text. Otherwise, modifying
  ///   the call-site argument would implicitly change other occurrences and
  ///   diverge from the edited preprocessed output; in that case it returns
  ///   `nullopt` (or equivalent) to force a whole-expansion patch.
  ///
  /// - **Unstringify:** For edits originating from a `#param` occurrence, if
  /// the
  ///   extracted replacement is a string-literal token, attempts to invert it
  ///   back into argument text (failing safely if the literal cannot be
  ///   unstringified).
  ///
  /// - **Reconstruction:** Parses `baseInvocationText` to locate per-argument
  /// byte
  ///   ranges in the original invocation, then applies replacements
  ///   right-to-left. Multiple hunks (or multiple holes) that map to the same
  ///   `arg_index` must resolve to an identical replacement string; conflicts
  ///   cause failure.
  ///
  /// \param M The refold model (used for macro-definition/metadata lookups
  ///          during hole synthesis and validation).
  /// \param m The macro invocation being patched.
  /// \param h The hunk (edit) that triggered the patch attempt.
  /// \param a2b The token mapping array from original preprocessed (A) to
  ///            edited (B).
  /// \param bSource The full text of the edited preprocessed source.
  /// \param bTokOff List of byte offsets for tokens in `bSource`.
  /// \param baseInvocationText The original text of the macro call in the TU
  ///            source.
  /// \returns A `MacroPatch` containing updated call-site text, or `nullopt`
  ///          (or equivalent) if the edit is not strictly representable as
  ///          argument-only changes (or if mapping/consistency checks fail).
  std::optional<MacroPatch> BuildMacroInvocationPatchArgsOnly(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      const ArrayRef<int> a2b, StringRef baseInvocationText) const;

  /// \brief Finds the `\#define` that is considered "in effect" for a given
  /// macro invocation.
  ///
  /// This uses the refold-map event `id` as an ordering surrogate for source
  /// order: only `\#define` directives with `d.id < inv.id` are eligible (i.e.,
  /// they occur before the invocation in the preprocessor event stream).
  ///
  /// Each eligible directive is parsed via `parseMacroDefineInfo`. Directives
  /// that cannot be parsed are ignored. Among parsed directives whose macro
  /// name matches `inv.name`, the directive with the greatest `id` is selected
  /// (the most recent definition prior to the invocation), yielding
  /// deterministic behavior.
  ///
  /// Note: this routine only considers `\#define` directives. It does not model
  /// `\#undef` or conditional visibility; callers should treat a `nullopt`
  /// result (or a potentially stale result in the presence of `\#undef`) as a
  /// signal to fall back to a more conservative strategy (e.g., whole-cover
  /// expansion).
  ///
  /// \param inv Macro invocation whose active definition should be resolved.
  /// \returns The most recent matching `\#define` prior to `inv`, or
  /// `std::nullopt`
  ///          if none can be determined.
  std::optional<MacroDefineInfo> FindActiveMacroDefineForInvocation(
      const RefoldModel::MacroInvocation &inv) const;

  /// \brief Parses a `\#define` directive text into a compact definition summary
  /// used by refolding.
  ///
  /// The directive text is expected to match the textual form recorded in the
  /// refold map (e.g., `"#define FOO(X) ...\n"`). This parser is intentionally
  /// lightweight: it recognizes the directive keyword, extracts the macro
  /// identifier, and (if the macro is function-like) extracts the parameter
  /// list by splitting on commas up to the first closing `')'`.
  ///
  /// For function-like macros, this method also scans the replacement list
  /// (everything after the name/parameter list) for stringification operators
  /// of the form `\#param`. Token pasting `\#\#` is explicitly ignored. Each
  /// `\#param` occurrence contributes the corresponding parameter index to
  /// `stringifyOrder` in left-to-right textual order, which is later used to
  /// associate produced string literals with invocation arguments when
  /// refolding edits to stringified expansions.
  ///
  /// ### Limitations (by design):
  /// * This is not a full preprocessor parser. It does not model comments, line
  ///   continuations, nested parentheses, or variadics robustly, and it assumes
  ///   the parameter list is a simple comma-separated list ending at the first
  ///   `')'`.
  /// * The replacement scan is heuristic and may misclassify `\#` occurrences
  ///   inside literals or comments if those are present in the recorded text.
  ///
  /// \param d Macro directive record (must be a `\#define`) whose text is
  /// parsed.
  /// \returns A `MacroDefineInfo` containing the directive id, macro name,
  ///          parameter names, and stringification parameter indices in textual
  ///          occurrence order; or `std::nullopt` if the text does not parse as
  ///          a `\#define`.
  static std::optional<MacroDefineInfo>
  ParseMacroDefineInfo(const RefoldModel::MacroDirective &d);

  /// \brief Normalizes and records a single parameter slot from a function-like
  /// `\#define`.
  ///
  /// This helper takes the raw text accumulated for one parameter (as delimited
  /// by commas and the closing `')'` in `parseMacroDefineInfo`) and appends the
  /// parameter identifier to `params`, if one can be extracted.
  ///
  /// ### Normalization rules:
  /// * Whitespace-only and empty slots are ignored.
  /// * Variadic markers are ignored (`"..."`, and `"name..."` will naturally
  ///   record `"name"` while the ellipsis is dropped by the identifier-prefix
  ///   rule).
  /// * Only the leading identifier prefix of the slot is kept; any trailing
  ///   punctuation or annotations are discarded.
  ///
  /// This is used to build the parameter name table for stringify mapping;
  /// parameters that cannot be reduced to an identifier are skipped to keep
  /// downstream logic conservative and deterministic.
  ///
  /// \param params Output list to append the extracted parameter identifier to.
  /// \param raw Raw parameter slot text (may include whitespace and trailing
  ///            punctuation).
  static void AddDefineParam(std::vector<std::string> &params, StringRef raw);

  /// \brief Computes synthetic macro-argument "holes" for hunks that edit a
  /// stringified macro parameter.
  ///
  /// Clang's token-origin tracking typically does not attribute the produced
  /// string literal from `\#param` back to the corresponding invocation
  /// argument (i.e., `arg_spans` may be missing for stringify output). This
  /// method bridges that gap by mapping an edit hunk that targets a string
  /// literal token in the preprocessed B stream back onto the appropriate
  /// invocation argument index, returning a `PPArgSpan` that callers can treat
  /// like a normal edited argument span.
  ///
  /// ### Eligibility and mapping rules:
  /// * The macro definition must have at least one stringification occurrence
  ///   recorded in `def.stringifyParamOrder`.
  /// * The hunk must target a string literal token in B (checked at
  /// `h.bStart`).
  /// * If the macro body contains exactly one `\#param` occurrence, that
  ///   parameter index is used directly.
  /// * If the macro body contains multiple `\#param` occurrences, this method
  ///   determines the B-token envelope covered by the macro expansion for this
  ///   hunk, then counts string literal tokens within that envelope in
  ///   left-to-right order. The string literal at `h.bStart` is treated as the
  ///   `occ`-th stringification occurrence, which maps to the parameter index
  ///   `def.stringifyParamOrder[occ]`.
  ///
  /// The returned `PPArgSpan` uses the hunk's A-span (`[h.aStart, h.aEnd)`) and
  /// the resolved argument index. If any required information is missing or the
  /// mapping is ambiguous, this method returns an empty vector so callers can
  /// fall back to a conservative strategy (e.g., whole-cover expansion).
  ///
  /// \param def Parsed `\#define` summary for `m` (including stringify
  ///            occurrence order).
  /// \param m Macro invocation whose expansion contains the edited string
  /// literal.
  /// \param h Edit hunk being classified/mapped.
  /// \param a2b A-to-B token mapping for the LCS alignment.
  /// \param bSource Full B-side preprocessed source text.
  /// \param bTokOff B token start offsets (token index -> byte offset).
  /// \returns A singleton vector containing a synthetic `PPArgSpan` for the
  ///          stringified argument, or an empty vector if the hunk is not a
  ///          supported stringify edit.
  std::vector<RefoldModel::PPArgSpan>
  ComputeStringifyArgHolesForHunk(const std::optional<MacroDefineInfo> &def,
                                  const RefoldModel::MacroInvocation &m,
                                  const diffutils::Hunk &h,
                                  ArrayRef<int> a2b) const;

  /// \brief Slices the B-side source text by token indices.
  ///
  /// This helper converts a half-open B-token interval `[bStartTok, bEndTok)`
  /// into a byte-offset range using `bTokOff` and returns the corresponding
  /// substring of `bSource`.
  ///
  /// `bTokOff` is the B token-to-byte offset table and must be consistent with
  /// the token stream used elsewhere in refolding:
  /// * `bTokOff[i]` is the starting byte offset of B token `i`.
  /// * The table is expected to have length `(numBTokens + 1)`, where the final
  ///   entry `bTokOff[numBTokens]` equals `bSource.size()` (end sentinel).
  ///
  /// For robustness, this method clamps token indices into the valid table
  /// range and clamps derived byte offsets into `[0, bSource.size()]`. If
  /// inputs are empty, it returns the empty string.
  ///
  /// \param bTokOff Token start offsets for B (length `numBTokens + 1`).
  /// \param bSource Full B-side preprocessed source text.
  /// \param bStartTok Inclusive start token index.
  /// \param bEndTok Exclusive end token index.
  /// \returns The substring of `bSource` covered by tokens `[bStartTok,
  /// bEndTok)`.
  StringRef SliceBSource(int bStartTok, int bEndTok) const;

  /// \brief Computes a conservative B-token envelope for the portion of `m`'s
  /// expansion affected by a hunk.
  ///
  /// The refold map records macro coverage in A-token space via
  /// `[m.coverBegin, m.coverEnd)`. This routine projects that interval into
  /// B-token space using the A→B alignment (`a2b`), producing a half-open B
  /// interval `[lo, hi)` that can be used to scan the macro's expanded tokens
  /// in B (e.g., to locate string literals produced by `\#param`
  /// stringification).
  ///
  /// Because the A→B mapping may be incomplete near edits, the computed
  /// envelope is intentionally conservative:
  /// * `lo` is derived from mapping `m.coverBegin` forward into B; if unmapped,
  ///   it falls back to `h.bStart`.
  /// * `hi` is derived from mapping the last covered A token (`m.coverEnd - 1`)
  ///   backward into B and converting to exclusive-end form; if unmapped, it
  ///   falls back to `(h.bEnd - 1) + 1`.
  /// * The envelope is then widened to include the hunk's own B span
  ///   `[h.bStart, h.bEnd)`.
  ///
  /// The returned interval is clamped to the valid token table range, where
  /// `bTokOff.size()` is `numBTokens + 1`. If no valid envelope can be formed
  /// (e.g., both endpoints are unmapped and the hunk has no B span), this
  /// method returns `std::nullopt`.
  ///
  /// \param m Macro invocation providing A-token coverage (`coverBegin/End`).
  /// \param h Edit hunk whose B span should be included in the envelope.
  /// \param a2b A-to-B token mapping produced by the alignment.
  /// \param bTokOff B token start offsets (length `numBTokens + 1`).
  /// \returns A pair representing a half-open B-token interval `[lo, hi)`, or
  ///          `std::nullopt` if no valid envelope can be determined.
  std::optional<std::pair<int, int>>
  MacroCoverTokenEnvelopeInB(const RefoldModel::MacroInvocation &m,
                             const diffutils::Hunk &h, ArrayRef<int> a2b) const;

  /// \brief Attempts to invert a string literal token that was produced by macro
  /// stringification (`\#param`) back into a single invocation-site argument
  /// spelling.
  ///
  /// In the preprocessor, `\#X` produces a string literal token (e.g.,
  /// `"hello-world"`) even though the invocation argument text was
  /// `hello-world`. During refolding, edits to that produced literal may be
  /// projected back onto the invocation site by "unstringifying" the literal
  /// and using the result as the replacement argument text.
  ///
  /// This routine is intentionally conservative and only supports ordinary and
  /// prefixed string literals that look like one token:
  /// * `"..."`
  /// * `L"..."` (wide)
  /// * `u"..."` / `U"..."` / `u8"..."` (Unicode)
  ///
  /// The body is extracted from the first double-quote through the final
  /// double-quote and is minimally unescaped: `\\` and `\"` are reduced to `\`
  /// and `"`, respectively. Other escape sequences are preserved verbatim (the
  /// backslash and following character are kept) to avoid changing semantics.
  ///
  /// ### Safety checks:
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
  static std::optional<std::string>
  UnstringifyLiteralToArgText(StringRef literalTok);

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
  /// argument indices (based on the provided `argSpans` list) are affected by
  /// the hunk.
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
  static bool
  HunkFullyWithinArgSpans(const diffutils::Hunk &h,
                          const std::vector<RefoldModel::PPArgSpan> &argSpans,
                          MutableArrayRef<char> touched);

  /// \brief Reconstructs the token spans of macro arguments by identifying the
  /// "holes" between consecutive macro body segments.
  ///
  /// In function-like macros, the preprocessor provides `body_spans` which
  /// represent the literal parts of the macro definition. These spans alternate
  /// with the positions where macro parameters are expanded. By calculating the
  /// gap between `bodySpans[i].end` and `bodySpans[i+1].begin`, we identify an
  /// expansion site.
  ///
  /// Because macros may use parameters multiple times, out of order, or not at
  /// all, this method correlates each identified "hole" with the original
  /// invocation argument index by checking for overlaps against the metadata in
  /// `argSpans`.
  ///
  /// \param bodySpans The alternating list of literal spans in the macro
  ///                  expansion.
  /// \param argSpans Metadata used to map expansion regions back to specific
  ///                 parameter indices.
  /// \return A list of `PPArgSpan` objects representing the expansion sites and
  ///         their associated argument indices; returns an empty list if no
  ///         holes are found.
  static std::vector<RefoldModel::PPArgSpan>
  ComputeArgSpansFromBodySpansWithArgIdx(
      const std::vector<RefoldModel::PPSpan> &bodySpans,
      const std::vector<RefoldModel::PPArgSpan> &argSpans);

  /// \brief Determines if two preprocessor (PP) token spans overlap, with
  /// special handling for zero-width insertion points.
  ///
  /// This method is used during macro argument patching to correlate edit
  /// locations with parameter "holes" in the expanded body. Unlike standard
  /// half-open interval overlap checks, this logic is designed to be inclusive
  /// of boundary points when dealing with insertions.
  ///
  /// ### Overlap Rules:
  ///
  /// - **Point vs. Point:** Two empty spans (insertions) overlap only if they
  ///   occur at the exact same token index.
  /// - **Point vs. Range:** An empty span overlaps a non-empty range if the
  ///   insertion point falls anywhere within the range, *including* the start
  ///   and end boundaries (e.g., point 5 overlaps range [5, 10)).
  /// - **Range vs. Range:** Two non-empty ranges overlap if they share at least
  ///   one common token (standard half-open interval intersection).
  ///
  /// \param b0 The beginning index of the first span.
  /// \param e0 The end index (exclusive) of the first span.
  /// \param b1 The beginning index of the second span.
  /// \param e1 The end index (exclusive) of the second span.
  /// \return `true` if the spans overlap according to the rules above;
  ///         `false` otherwise or if indices are logically invalid (begin >
  ///         end).
  static bool SpansOverlapForArgIdx(int b0, int e0, int b1, int e1) {
    // Spans are PP token index ranges, generally half-open [begin, end).
    // Treat empty spans as a single insertion point that can overlap a
    // non-empty span at its edges.
    if (b0 > e0 || b1 > e1)
      return false;

    if (b0 == e0 && b1 == e1)
      return b0 == b1;

    if (b0 == e0)
      return b0 >= b1 && b0 <= e1;

    if (b1 == e1)
      return b1 >= b0 && b1 <= e0;

    return std::max(b0, b1) < std::min(e0, e1);
  }

  /// \brief Calculates the bounding "envelope" of tokens in the edited
  /// preprocessed stream (B) that correspond to a specific range of tokens from
  /// the original preprocessed stream (A).
  ///
  /// Because edits can reorder, delete, or duplicate tokens, a contiguous range
  /// in A might map to a fragmented or shifted set of indices in B. This method
  /// finds the minimum and maximum mapped indices to create a single half-open
  /// interval `[min, max + 1)` in the B-domain that covers all "surviving"
  /// tokens from the original A-range.
  ///
  /// This is primarily used to determine the expansion area of a macro or an
  /// include argument after edits have been applied to the preprocessed source.
  ///
  /// \param a2b The mapping array where `a2b[a]` provides the index of token
  /// `a`
  ///            in the B-stream (or -1 if the token was deleted).
  /// \param aBegin The starting token index in the A-domain (inclusive).
  /// \param aEnd The ending token index in the A-domain (exclusive).
  /// \return A pair representing the half-open interval `[bMin, bMaxExcl]` in
  /// the
  ///         B-domain, or `std::nullopt` if none of the tokens in the A-range
  ///         exist in B.
  static std::optional<std::pair<int, int>>
  MapAToBTokenEnvelope(const std::vector<int> &a2b, int aBegin, int aEnd) {

    int bMin = std::numeric_limits<int>::max();
    int bMax = std::numeric_limits<int>::min();

    for (int a = aBegin; a < aEnd; ++a) {
      if (a < 0 || static_cast<size_t>(a) >= a2b.size())
        continue;

      int b = a2b[a];
      if (b < 0)
        continue;

      bMin = std::min(bMin, b);
      bMax = std::max(bMax, b);
    }

    if (bMin == std::numeric_limits<int>::max())
      return std::nullopt;

    return std::make_pair(bMin, bMax + 1);
  }

  /// \brief Parses the raw text of a function-like macro invocation to identify
  /// the byte ranges of its individual arguments.
  ///
  /// This method performs a shallow, brace-aware scan of the invocation text
  /// starting from the opening parenthesis. It correctly handles nested
  /// parentheses, brackets, and braces, ensuring that commas within nested
  /// expressions (like function calls or initializer lists) do not prematurely
  /// terminate an argument.
  ///
  /// The parser is also string- and character-literal aware; it skips over
  /// escaped characters and delimiters within quotes to avoid misinterpreting
  /// structural C characters as macro argument separators.
  ///
  /// \param invText The full source text of the macro invocation
  ///                (e.g., "MY_MACRO(a, f(b, c))").
  /// \return A list of `[start, end]` byte ranges for each argument, with
  ///         leading and trailing whitespace trimmed; returns `std::nullopt` if
  ///         the text is malformed or the closing parenthesis is missing.
  static std::optional<std::vector<std::pair<int, int>>>
  ParseMacroInvocationArgContentRanges(StringRef invText);

  /// \brief Adjusts the boundaries of a character range to exclude leading and
  /// trailing whitespace.
  ///
  /// Given a string and a half-open interval `[b, e)`, this method increments
  /// the start index and decrements the end index until they point to
  /// non-whitespace characters or meet in the middle. This is useful for
  /// normalizing macro arguments or code segments before performing comparisons
  /// or replacements.
  ///
  /// \param s The source string containing the range to be trimmed.
  /// \param b The initial starting index (inclusive).
  /// \param e The initial ending index (exclusive).
  /// \return A pair `{newB, newE}` representing the trimmed half-open interval.
  ///         If the entire range consists of whitespace, `newB` will equal
  ///         `newE`.
  static std::pair<int, int> TrimWsRange(StringRef s, int b, int e) {
    while (b < e && stringutils::isWs(s[b]))
      b++;
    while (e > b && stringutils::isWs(s[e - 1]))
      e--;
    return {b, e};
  }

  /// \brief Builds a macro replacement patch by reconciling changes across
  /// nested macro expansions, argument-specific edits, and whole-expansion byte
  /// mapping.
  ///
  /// The reconciliation follows a multi-tier fallback strategy:
  ///
  /// 1. **Nested Patch Integration:** If the macro's arguments contain other
  ///    macros that were already patched (available in `patchMap`), those
  ///    patches are spliced into the base invocation text.
  /// 2. **Surgical Argument Patching:** Attempts to apply edits strictly to
  ///    macro parameters via `buildMacroInvocationPatchArgsOnly`. This is the
  ///    most precise method as it preserves the original call site's
  ///    formatting.
  /// 3. **LCS Mapping (Fallback):** Maps the macro's A-cover to the edited
  ///    B-token stream. It intelligently decides between a "strict" envelope
  ///    and an "all" envelope (including adjacent structural body tokens like
  ///    semicolons) based on whether the extra tokens originate from the macro
  ///    body or from edited arguments.
  ///
  /// \param m Metadata for the macro invocation, including call site ranges and
  ///          A-domain cover.
  /// \param h The diff hunk associated with this macro's region.
  /// \param a2b Token mapping array from original preprocessed (A) to edited
  /// (B). \param baseInvText The original invocation text from the source file.
  /// \param patchMap Registry of patches already computed (used to resolve
  ///                 nested macros).
  /// \return A `MacroPatch` targeting the call site with reconciled replacement
  ///         text.
  MacroPatch BuildMacroInvocationPatchWholeCover(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      ArrayRef<int> a2b, StringRef baseInvText,
      const DenseMap<int, DenseMap<int, MacroPatch>> &patchMap) const;

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
  void OrderIncludeInsertions(DenseMap<int, IncludeEdits> &perInclude) const {
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
  /// \param macroPatchesByOwner Map of include ID → macro patches whose
  ///                            invocation ranges are in that include’s byte
  ///                            space.
  /// \param children            Map of parent include ID → direct child include
  ///                            items (with `siteB` / `siteE` in the parent).
  /// \param includeExpansion    Cache/output: include ID → fully materialized
  ///                            header bytes; may be preseeded with raw header
  ///                            text.
  void MaterializeIncludeExpansion(
      int includeId, const DenseMap<int, IncludeEdits> &perInclude,
      const DenseMap<int, std::vector<MacroPatch>> &macroPatchesByOwner,
      const DenseMap<int, std::vector<const RefoldModel::IncludeItem *>>
          &children,
      DenseMap<int, std::string> &includeExpansion) const;

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
  ///    immediately *after* that token (use its byte-end).
  /// 3. **Owning decl end**: if an owning decl exists, anchor at
  ///    `decl.headerE`.
  /// 4. **Child-include boundary fallback**: attempt to synthesize a stable
  ///    anchor from child `#include` sites (via
  ///    `computeChildBoundaryInsertByte(...)`). When this path is taken, the
  ///    inserted text may be padded with `padAtBoundaries(...)`. If no anchor
  ///    can be found, the patch is skipped.
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
  std::vector<TextEdit> ComputeIncludeTextEdits(const IncludeEdits &ie,
                                                std::string headerText) const;

  /// \brief Computes a deterministic insertion byte offset for a header-scoped
  /// *pure INSERT* when the normal token-based anchoring mechanisms provide no
  /// usable neighbor.
  ///
  /// This is a fallback used only in degenerate header cases where:
  /// - The patch is a pure insertion (`p.aStart == p.aEnd`)
  /// - There are no mapped tokmap neighbors in `file` near `p.aStart` to anchor
  ///   on
  /// - There is no suitable `HeaderDecl` span to provide a declaration-based
  ///   anchor
  ///
  /// In such cases, we attempt to anchor relative to the literal `#include`
  /// sites that appear inside the same header file.
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
  ///   (`kid.coverBegin < pos && pos < kid.coverEnd`), this fallback does not
  ///   apply and returns `-1` (the insertion should have been owned by that
  ///   child include).
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
  /// 3. Otherwise return `-1` to indicate that no sane anchor could be derived.
  ///
  /// This method does not validate that the returned site offsets are within
  /// the current header text bounds; callers should ensure the returned byte
  /// offset is usable in the current editing context.
  ///
  /// \param M The refold model providing include hierarchy, PP cover windows,
  ///          and include-site byte ranges.
  /// \param p The include-scoped patch (expected to be a pure insertion) whose
  ///          PP insertion position is `p.aStart`.
  /// \param file The header file path whose text is being edited; only child
  ///             includes whose `sitePath` equals `file` are considered as
  ///             anchors.
  /// \return A byte offset within `file` at which the insertion should be
  /// applied,
  ///         or `-1` if this fallback does not apply or no stable anchor can be
  ///         found.
  int ComputeChildBoundaryInsertByte(const IncludePatch &p,
                                     StringRef file) const;

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
  TextEdit MakeTextEditWithResyncOrPending(StringRef original, int start,
                                           int end, StringRef replacement,
                                           StringRef fileSpelling) const {
    ResyncOutcome o =
        ApplyResyncOrPend(original, start, end, replacement, fileSpelling);
    return TextEdit{start, end, std::move(o.text), std::move(o.pending)};
  }

  /// \brief Computes how to preserve __LINE__ after applying replacement to
  /// [start,end) in originalFileText.
  ///
  /// This method detects "line drift" by comparing the newline count in the
  /// original span versus the replacement text. If there is no drift, it returns
  /// (replacement, nullopt).
  ///
  /// If drift is detected, the method:
  /// 1. Computes the logical resume line for the first character at `end` in the
  ///    original file,
  /// 2. Attempts a local resync by calling
  ///    LineDirectiveInserter::maybeAppendResyncAfterReplacement,
  /// 3. If local injection succeeds, returns (injectedReplacement, nullopt),
  /// 4. Otherwise, returns (replacement, PendingResync(fileSpelling)) so the
  ///    emission layer can flush a #line directive at the next safe BOL.
  ///
  /// Safety note: local injection may fail when inserting a directive would
  /// change token adjacency (e.g., when the replacement ends mid-line, or when
  /// no safe BOL exists in/around the replacement).
  ///
  /// \param originalFileText Pre-edit file contents the offsets refer to.
  /// \param start Start offset (inclusive) in originalFileText.
  /// \param end End offset (exclusive) in originalFileText.
  /// \param replacement Replacement text.
  /// \param fileSpellingForDirective Path used in any injected #line.
  /// \return A ResyncOutcome containing the emitted text and optional pending state.
  ResyncOutcome ApplyResyncOrPend(StringRef originalFileText, int start,
                                  int end, StringRef replacement,
                                  StringRef fileSpellingForDirective) const;

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
  /// \return The edited file text with any necessary #line directives emitted
  ///         (locally or deferred).
  std::string ApplyTextEditsWithPendingResync(StringRef originalFileText,
                                              ArrayRef<TextEdit> edits) const;

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
  /// \param pending A pending resync to flush.
  /// \return std::nullopt if the pending resync was flushed; otherwise the
  ///         still-pending resync.
  std::optional<PendingResync>
  AppendOriginalSliceWithPending(SmallVectorImpl<char> &out, StringRef original,
                                 int from, int to,
                                 std::optional<PendingResync> pending) const;

  // ------------------------ Low-level Mapping & Utils ------------------------

  /// \brief Resolves the TU/file byte start offset corresponding to a PP
  /// coordinate for a specific file.
  ///
  /// The refold model maintains a PP->(file, byte-range) mapping (e.g.
  /// \c tokmapByPP) that allows code working in PP space to locate the
  /// corresponding region in an owning file (TU or header).
  ///
  /// If \p pp maps to an entry whose \c file matches the requested \p file,
  /// this returns the mapped begin byte offset (\c b). Otherwise, this returns
  /// \p fileLen when \p fallbackToEOF is enabled, or -1 when fallback is
  /// disabled.
  ///
  /// The \p fallbackToEOF behavior is intended for boundary cases where we want
  /// a deterministic anchoring point even when a PP position does not directly
  /// map into the requested file (for example, appending at end-of-file in a
  /// file-owned segment).
  ///
  /// \param file the file path whose mapping is being queried (TU or included
  ///        header)
  /// \param pp the PP coordinate (typically a PP byte offset) to resolve
  /// \param fallbackToEOF if true, return \p fileLen when \p pp does not map
  ///        into \p file
  /// \param fileLen the length of \p file in bytes (used only when
  ///        \p fallbackToEOF is true)
  /// \return the mapped start byte offset in \p file, or \p fileLen when
  ///         falling back to EOF, or -1 if unmapped and fallback is disabled
  int ByteStartForPPInFile(StringRef file, int pp, bool fallbackToEOF,
                           int fileLen) const {
    auto it = model_.GetTokmapByPP().find(pp);
    if (it != model_.GetTokmapByPP().end() && PathsEqual(it->second.file, file))
      return it->second.b;
    return fallbackToEOF ? fileLen : -1;
  }

  /// \brief Resolves the TU/file byte end offset corresponding to a PP
  /// coordinatefor a specific file.
  ///
  /// Analogous to ByteStartForPPInFile() but returns the mapped end byte offset
  /// (\c e). If \p pp maps to an entry whose \c file matches the requested \p
  /// file, this returns \c e. Otherwise, this returns \p fileLen when \p
  /// fallbackToEOF is enabled, or -1 when fallback is disabled.
  ///
  /// \param file the file path whose mapping is being queried (TU or included
  ///        header)
  /// \param pp the PP coordinate (typically a PP byte offset) to resolve
  /// \param fallbackToEOF if true, return \p fileLen when \p pp does not map
  ///        into \p file
  /// \param fileLen the length of \p file in bytes (used only when
  ///        \p fallbackToEOF is true)
  /// \return the mapped end byte offset in \p file, or \p fileLen when falling
  ///         back to EOF, or -1 if unmapped and fallback is disabled
  int ByteEndForPPInFile(StringRef file, int pp, bool fallbackToEOF,
                         int fileLen) const {
    auto it = model_.GetTokmapByPP().find(pp);
    if (it != model_.GetTokmapByPP().end() && PathsEqual(it->second.file, file))
      return it->second.e;
    return fallbackToEOF ? fileLen : -1;
  }

  /// \brief Compare two paths for equality after weak canonicalization.
  ///
  /// If either path is empty, returns string equality directly. Otherwise both
  /// paths are weakly canonicalized (`std::filesystem::weakly_canonical`) and
  /// compared. Canonicalization failures are fatal.
  ///
  /// \param a First path.
  /// \param b Second path.
  /// \returns `true` if canonical paths are equal; `false` otherwise.
  static bool PathsEqual(StringRef a, StringRef b);

  // ---------------------- Diagnostics & Debug Utilities ----------------------

  /// \brief Emits a detailed TRACE log line describing how a given hunk maps
  /// into an include's token/byte space.
  ///
  /// This method is purely diagnostic. It computes:
  /// - A-side byte interval for the hunk using aTokOff (token start offsets in
  ///   aSource)
  /// - B-side byte interval for the hunk using bTokOff (token start offsets in
  ///   bSource)
  /// - Clipped source slices for both sides (escaped and whitespace-visualized)
  ///   and logs a single structured line under "include/patch".
  ///
  /// The log payload is designed to make it straightforward to validate that
  /// include-owned edits are using the correct token indices, byte ranges, and
  /// local context, especially for boundary insertions and for hunks that are
  /// fully inside header-owned regions.
  ///
  /// \param tag a short tag describing the call site / phase (e.g., "before",
  ///        "after", "emit")
  /// \param inc the include item that is expected to own the hunk
  /// \param h the diff hunk being examined (A token range and B token range)
  void DebugIncludePatch(StringRef tag, const RefoldModel::IncludeItem &inc,
                         const diffutils::Hunk &h) const;
};

} // namespace refold
} // namespace clang

namespace llvm {
using namespace clang::refold;

template <> struct format_provider<RefoldEngine::OwnerKind> {
  static void format(const RefoldEngine::OwnerKind &kind, raw_ostream &os,
                     StringRef style) {

    switch (kind) {
    case RefoldEngine::OwnerKind::TU:
      os << "TU";
      break;
    case RefoldEngine::OwnerKind::Include:
      os << "Include";
      break;
    case RefoldEngine::OwnerKind::Unknown:
      os << "Unknown";
      break;
    }
  }
};
} // namespace llvm

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H
