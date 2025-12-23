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
#include "RefoldModel.h"
#include "StringUtils.h"
#include "llvm/ADT/ArrayRef.h"
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

/// \brief Deterministic refolder for edits made to raw preprocessed C (B)
///        back onto the original, partially expanded C translation unit (TU).
///
/// ### Inputs
/// * **A** — Original raw preprocessed source (`.i`)
/// * **B** — Edited raw preprocessed source (`.i.mod`)
/// * **M** — Refold map JSON produced by a modified **clang** preprocessor
///   (`-E -P --refold-map=...`).
///   The map encodes byte/token provenance for includes, macros, and
///   conditionals, using absolute canonicalized paths.
/// * **tuPath** — Original TU path (from M)
///
/// ### Goal
/// Reconstruct a translation unit (`.mod`) that expands only those
/// preprocessor constructs whose expanded bytes were edited in **B**.
/// Edits are attributed to one of:
/// * **Include** instances (`#include` / `#include_next`), including nested
///   and conditional branches
/// * **Macro call sites** (`#define` / `#undef`), replacing invocation spans
///   with edited expansion bytes
/// * **TU-owned bytes** — direct text present in the TU after preprocessing
///
/// ### Pipeline (high level)
/// 1. Validate and load **M**; canonicalize paths.
/// 2. Lex **A** and **B** with Clang’s raw token dumper; map tokens to stable
///    identities.
/// 3. Compute LCS over token identities and build A→B token offset mapping.
/// 4. Derive edit hunks (A-intervals with corresponding B-intervals).
/// 5. **Classify** each hunk deterministically as Include / Macro call-site /
/// TU edit;
///    refuse if no covering provenance exists.
/// 6. For Includes: collect per-include insertions, then **realize** includes
///    bottom-up, applying owner macro patches and child realizations along
///    the actually taken conditional branches recorded in **M**.
/// 7. Apply TU-level edits (realized include text, TU macro patches, direct TU
/// edits)
///    in stable order to produce the output TU.
///
/// ### Determinism & Formatting Policy
/// * All iteration over work buckets is stable (sorted where needed);
///   byte edits apply high→low to avoid offset drift.
/// * Include insertion normalization coalesces adjacent insert hunks,
///   preserves identifier boundaries, and trims after the first newline
///   to avoid leaking the next line.
/// * Macro call-site replacements use *boundary normalization*
///   (trim leading/trailing spaces/tabs only) to prevent doubled spaces or
///   token gluing; internal whitespace is preserved verbatim.
/// * Paths are absolute/canonical everywhere except re-emitted include
///   targets, which remain in source form.
///
/// ### Supported Preprocessor Constructs
/// * `#include` / `#include_next` (arbitrary nesting)
/// * `#define` / `#undef` (replacements at invocation sites)
/// * `#if` / `#ifdef` / `#elif` / `#else` (branch recorded in M)
/// * `#pragma` (treated opaquely)
///
/// ### Non-goals
/// * Re-running the preprocessor or fully parsing C; re-lexing anchors
///   tokens and relies on **M**.
/// * Heuristic recovery when **M** is incomplete; the engine fails fast so
///   gaps can be fixed in the map producer.
///
/// ### Failure Mode
/// If an edited region in **B** cannot be mapped to a deterministic
/// Include/Macro/TU span in **M**, the engine terminates with a precise
/// diagnostic rather than guessing.
///
/// ### Public API
/// * `refold(...)` — orchestrates the entire pipeline and writes the
///   refolded translation unit.
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
  /// \param root     Parsed refold map JSON (immutable model root).
  /// \param aSource  Original preprocessed text A (e.g., `test.c.i`).
  /// \param aToks    Tokens of A.
  /// \param aTokOff  Byte offsets for A tokens (size = |A| + 1).
  /// \param bSource  Edited preprocessed text B (e.g., `test.c.i.mod`).
  /// \param bToks    Tokens of B.
  /// \param bTokOff  Byte offsets for B tokens (size = |B| + 1).
  /// \returns        The refolded, partially expanded C source.
  static Expected<std::string> Refold(const json::Object &rootJson,
                                      StringRef aSource, ArrayRef<PPTok> aToks,
                                      ArrayRef<std::size_t> aTokOff,
                                      StringRef bSource, ArrayRef<PPTok> bToks,
                                      ArrayRef<std::size_t> bTokOff);

private:
  const RefoldModel model_;
  StringRef aSource_, bSource_;
  ArrayRef<PPTok> aToks_, bToks_;
  ArrayRef<std::size_t> aTokOff_, bTokOff_;

  /// Construct an engine from concrete inputs. The instance method `Refold()`
  /// runs the full pipeline using these captured members.
  RefoldEngine(RefoldModel model, StringRef aSource, ArrayRef<PPTok> aToks,
               ArrayRef<std::size_t> aTokOff, StringRef bSource,
               ArrayRef<PPTok> bToks, ArrayRef<std::size_t> bTokOff)
      : model_(std::move(model)), aSource_(aSource), bSource_(bSource),
        aToks_(aToks), bToks_(bToks), aTokOff_(aTokOff), bTokOff_(bTokOff) {}

  std::string Refold();

  // ---------------------------- Small Data Records ---------------------------

  struct TextEdit {
    int start, end;
    std::string text;
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
      llvm::StringRef path;
      if (include->resolvedPath && !include->resolvedPath->empty()) {
        path = *include->resolvedPath;
      } else {
        path = stringutils::stripHeaderToken(include->target);
      }

      // 2. Handle the preview truncation
      llvm::StringRef preview = insertBytes;
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

  // ---------------------------- Ownership Helpers ----------------------------

  enum class OwnerKind { TU, Include, Unknown };

  // Grant access to the specific formatter specialization
  template <typename T, typename Enable> friend struct llvm::format_provider;

  struct Owner {
    OwnerKind kind = OwnerKind::Unknown;
    std::optional<int> includeId;
    std::optional<int> condArmId;

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
  static std::string PadAtBoundaries(StringRef base, int start, int end,
                                     std::string text, bool allowLeft,
                                     bool allowRight);

  /// \brief Compute the "owner depth gap" array used to bias the weighted LCS.
  ///
  /// For each PP gap k (between PP tokens k-1 and k), we compute:
  ///
  ///   ownerDepthGap[k] = includeDepthLCA(leftInc, rightInc) + condDepthGap
  ///
  /// where:
  ///   - includeDepthLCA is the depth of the least-common-ancestor include
  ///     instance for the two sides of the gap (0 for TU, 1 for direct TU
  ///     children, etc.).
  ///   - condDepthGap is derived from the conditional arms that own the two
  ///     sides of the gap, taking nested conditionals into account.
  ///
  /// This makes deeper, nested regions (nested includes and nested #if groups)
  /// "deeper" in the weighting, so insertions prefer to sit inside their
  /// correct owner rather than being pulled out to shallower boundaries.
  ///
  /// \param numOfAOffs  The number of A-stream tokens (plus the sentinel).
  /// \returns A sequence of owner depth gaps.
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

  /// \brief For a pure insertion at PP-gap \p ppGap, return the TU byte offset
  /// of an EXACT canonical slot boundary (same PP coordinate).
  ///
  /// If \p ppGap is not on a boundary, returns std::nullopt so callers fall
  /// back to the neighbor-based tuByteSpan logic.
  ///
  /// This intentionally avoids "nearest" snapping, which causes regressions by
  /// re-anchoring insertions that are actually interior to an include/arm.
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

  /// Computes the TU (translation unit) byte span [b, e) corresponding to an
  /// A-side PP-token interval [a0, a1).
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

  /// Detects whether a pure insertion hunk lies exactly on the boundary between
  /// two sibling include regions, and if so, assigns ownership of the insertion
  /// to their parent include.
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

  /// Finds the nearest TokMapEntry in PP space by linear probing from a
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

  /// Computes a B-side preprocessor token envelope `[begin, end)` for a macro
  /// invocation.
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

  /// \brief Build a macro replacement patch by splicing the macro’s B-side
  ///        expansion into the call site.
  ///
  /// Constructs a `MacroPatch` that replaces a macro invocation with its fully
  /// expanded bytes from **B**.
  ///
  /// ### Primary Path
  /// * Map the macro’s A-cover `[coverBegin, coverEnd)` to a B-token interval
  ///   using the A→B LCS map (`a2b`).
  /// * Slice `bSource_` via `bTokOff_` to obtain the replacement bytes.
  ///
  /// ### Fallbacks (deterministic)
  /// * If the cover cannot be mapped (unmapped or inverted), and the hunk
  ///   contributes no tokens in **B**, produce an empty replacement (macro
  ///   vanished).
  /// * Otherwise, use the exact **B** slice from the hunk (`[h.bStart,
  /// h.bEnd)`).
  ///
  /// A final defensive clamp ensures byte indices are monotonic; if violated,
  /// an empty replacement is emitted instead of failing. Leading and trailing
  /// edge spaces are trimmed to prevent token gluing at boundaries.
  ///
  /// \param m         Macro invocation metadata (includes call site byte range
  ///                  and A-cover).
  /// \param h         The diff hunk associated with this macro region.
  /// \param a2b       Map from A-token index → matching B-token index (or -1)
  ///                  as produced by the LCS.
  /// \returns         A `MacroPatch` targeting the macro call site with the
  ///                  computed replacement bytes.
  MacroPatch
  BuildMacroInvocationPatchWholeCover(const RefoldModel::MacroInvocation &m,
                                      const diffutils::Hunk &h,
                                      ArrayRef<int> a2b) const;

  /// Applies a deterministic, stable ordering to include-scoped insertion
  /// patches.
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

  /// Selects the most appropriate HeaderDecl within an IncludeItem to serve as
  /// the declaration-level anchor for an include-scoped patch.
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

  /// Applies the IncludeEdits for a single include instance to that header's
  /// in-memory text.
  ///
  /// This method consumes a list of IncludePatches whose coordinates are
  /// expressed in the A-side token (PP) index space and projects them onto \p
  /// headerText using the refold map token->file/byte mappings in
  /// RefoldModel::tokmapByPP.
  ///
  /// \par Patch classification
  ///
  /// - **INSERT**: \c aStart == \c aEnd and \c bStart < \c bEnd. The patch
  ///   inserts IncludePatch::insertBytes at a deterministic byte anchor inside
  ///   this header.
  /// - **DELETE**: \c aStart < \c aEnd and \c bStart == \c bEnd. The patch
  ///   deletes the mapped A-range (replacement is "").
  /// - **REPLACE**: \c aStart < \c aEnd and \c bStart < \c bEnd. The patch
  ///   replaces the mapped A-range with IncludePatch::insertBytes.
  ///
  /// \par Scope and clamping rules
  ///
  /// - The include provides an overall PP "cover" window [coverBegin, coverEnd)
  ///   describing the PP indices that belong to this header instance.
  /// - Each patch is optionally associated with an owning HeaderDecl via
  ///   findHeaderDeclForPatch().
  /// - **DELETE/REPLACE** are restricted to the owning declaration when
  ///   available: the effective PP window is intersected with \c decl.ppSpan,
  ///   and the resulting byte range is clamped to [decl.headerB, decl.headerE)
  ///   so the edit cannot cross declaration boundaries.
  /// - **INSERT** intentionally operates at include scope (not decl scope) so
  ///   that insertions that land exactly on a declaration boundary can anchor
  ///   to the first token of the following declaration rather than being forced
  ///   "back inside" the previous one.
  ///
  /// \par Deterministic anchoring for INSERT
  ///
  /// For an INSERT at A-position \c pos = \c aStart, the insertion byte offset
  /// is chosen using the following priority order within the effective PP
  /// window:
  /// 1. **Right neighbor**: find the smallest \c pp >= \c pos mapping to this
  ///    header's file and insert immediately *before* that token (use its byte-
  ///    start).
  /// 2. **Left neighbor**: otherwise find the greatest \c pp < \c pos mapping
  ///    to this file and insert immediately *after* that token (use its byte-
  ///    end).
  /// 3. **Owning decl end**: otherwise, if an owning declaration exists, anchor
  ///    at \c decl.headerE.
  /// 4. **Child-include boundary fallback**: otherwise, attempt to synthesize a
  ///    stable anchor using child \c #include sites within this header (via
  ///    computeChildBoundaryInsertByte()), optionally padding with
  ///    padAtBoundaries(). If no anchor can be found, the patch is skipped.
  ///
  /// \par Mapping for DELETE/REPLACE
  ///
  /// For non-empty A-ranges, the method finds all PP indices in the patch's
  /// (possibly intersected) effective window that map into this header file.
  /// The resulting byte interval is computed as [byteStart(firstPP),
  /// byteEnd(lastPP)). If no PP tokens map into this header, the patch is
  /// skipped.
  ///
  /// \par Application order and formatting policy
  ///
  /// - All projected header TextEdits are collected first and then applied in
  ///   descending \c start order so earlier replacements do not invalidate
  ///   later coordinates.
  /// - Patch payload bytes are applied *literally*; this routine does not
  ///   attempt any whitespace normalization, token reformatting, or
  ///   "prefix surgery". The refold map's slices are treated as ground truth.
  ///
  /// \param ie Per-include edits: the include instance plus a set of
  ///           IncludePatches to apply.
  /// \param headerText The current text of the header file corresponding to
  ///                   \p ie.include.
  /// \return The updated header text after applying all applicable include-
  ///         scoped patches.
  std::string ApplyIncludeEdits(const IncludeEdits &ie,
                                std::string headerText) const;

  /// Computes a deterministic insertion byte offset for a header-scoped pure
  /// INSERT when the normal token-based anchoring mechanisms provide no usable
  /// neighbor.
  ///
  /// This is a fallback used only in degenerate header cases where:
  /// - The patch is a pure insertion (p.aStart == p.aEnd)
  /// - There are no mapped tokmap neighbors in file near p.aStart to anchor on
  /// - There is no suitable HeaderDecl span to provide a declaration-based
  ///   anchor
  ///
  /// In such cases, we attempt to anchor relative to the literal #include sites
  /// that appear inside the same header file.
  ///
  /// \par Approach
  ///
  /// Let \c owner be the include instance whose header text is being edited
  /// (p.include). We scan the model's includes to find *direct children* of \c
  /// owner whose IncludeItem::sitePath equals \p file. Each such child
  /// represents an include directive that is physically written in this header
  /// and has a PP cover window [kid.coverBegin, kid.coverEnd) and a site byte
  /// range [kid.siteB, kid.siteE) in \p file.
  ///
  /// For the insertion PP position \c pos = \p p.aStart, we consider only
  /// "between-children" positions:
  /// - If \c pos lies *strictly inside* a child's cover window
  ///   (kid.coverBegin < pos && pos < kid.coverEnd), this fallback does not
  ///   apply and returns -1 (the insertion should have been owned by that
  ///   child include).
  /// - Boundary positions are allowed (pos == kid.coverBegin or
  ///   pos == kid.coverEnd) and are treated as being "between" children.
  ///
  /// \par Anchor selection
  ///
  /// Among eligible children in \p file:
  /// - \c left is the child with the greatest \c coverEnd such that
  ///   \c coverEnd <= \c pos
  /// - \c right is the child with the smallest \c coverBegin such that
  ///   \c coverBegin >= \c pos
  ///
  /// The insertion anchor is then chosen deterministically:
  /// 1. If \c right exists, insert *before* the right child's #include site by
  ///    returning \c right.siteB.
  /// 2. Else if \c left exists, insert *after* the left child's #include site
  ///    by returning \c left.siteE.
  /// 3. Otherwise return -1 to indicate that no sane anchor could be derived.
  ///
  /// This method does not validate that the returned site offsets are within
  /// the current header text bounds; callers should ensure the returned byte
  /// offset is usable in the current editing context.
  ///
  /// \param p the include-scoped patch (expected to be a pure insertion) whose
  ///        PP insertion position is \p p.aStart
  /// \param file the header file path whose text is being edited; only child
  ///        includes whose sitePath equals \p file are considered as anchors
  /// \return a byte offset within \p file at which the insertion should be
  ///         applied, or -1 if this fallback does not apply or no stable
  ///         anchor can be found
  int ComputeChildBoundaryInsertByte(const IncludePatch &p,
                                     StringRef file) const;

  // ------------------------ Low-level Mapping & Utils ------------------------

  /// Resolves the TU/file byte start offset corresponding to a PP coordinate
  /// for a specific file.
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

  /// Resolves the TU/file byte end offset corresponding to a PP coordinate for
  /// a specific file.
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

  /// Emits a detailed TRACE log line describing how a given hunk maps into an
  /// include's token/byte space.
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
