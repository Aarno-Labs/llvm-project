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
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

// If you don’t already have PPTok defined elsewhere, this minimal form matches
// your Java usage.
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
                                      StringRef aSource,
                                      const std::vector<PPTok> &aToks,
                                      const std::vector<std::size_t> &aTokOff,
                                      StringRef bSource,
                                      const std::vector<PPTok> &bToks,
                                      const std::vector<std::size_t> &bTokOff);

private:
  const RefoldModel model_;
  StringRef aSource_, bSource_;
  const std::vector<PPTok> &aToks_, &bToks_;
  std::vector<std::size_t> aTokOff_, bTokOff_; // Keep a copy

  /// Construct an engine from concrete inputs. The instance method `Refold()`
  /// runs the full pipeline using these captured members.
  RefoldEngine(RefoldModel model, StringRef aSource,
               const std::vector<PPTok> &aToks,
               const std::vector<std::size_t> &aTokOff, StringRef bSource,
               const std::vector<PPTok> &bToks,
               const std::vector<std::size_t> &bTokOff)
      : model_(std::move(model)), aSource_(aSource), bSource_(bSource),
        aToks_(aToks), bToks_(bToks), aTokOff_(aTokOff), bTokOff_(bTokOff) {}

  std::string Refold();

  // ----------------------- small data records -----------------------
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
    int aStart, aEnd;        // A-token interval inside include expansion
    int bStart, bEnd;        // B-token interval
    std::string insertBytes; // exact B bytes
  };

  struct IncludeEdits {
    const RefoldModel::IncludeItem *include;
    std::vector<IncludePatch> patches;
  };

  // ----------------------- core helpers -----------------------

  /// Build the LCS lexeme sequence from PP tokens, replacing any all-whitespace
  /// token with a position-tied marker `"WS@<byteOffset>"` to prevent
  /// cross-line anchoring during LCS.
  ///
  /// @param toks  Preprocessed tokens (with `.spelling`).
  /// @param offs  Byte start offsets parallel to `toks` (size ≥ toks.size()).
  /// @return A vector of lexeme strings used for A↔B alignment.
  static std::vector<std::string>
  MapLexemes(const std::vector<PPTok> &toks,
             const std::vector<std::size_t> &offs);

  /// Return the first B-index at or after A-index `i` in a one-sided LCS map,
  /// or -1 if no forward-mapped element exists.
  ///
  /// @param a2b  A→B map where `a2b[a] = b` or `-1` if unmatched.
  /// @param i    Starting A-index (negative values start from 0).
  /// @return B-index ≥ 0 on success, otherwise -1.
  static int MapForwardToB(const std::vector<int> &a2b, int i) {
    const std::size_t start = (i < 0) ? 0u : static_cast<std::size_t>(i);
    for (std::size_t k = start; k < a2b.size(); ++k)
      if (a2b[k] >= 0)
        return a2b[k];
    return -1;
  }

  /// Return the last B-index at or before A-index `i` in a one-sided LCS map,
  /// or -1 if no backward-mapped element exists.
  ///
  /// @param a2b  A→B map where `a2b[a] = b` or `-1` if unmatched.
  /// @param i    Starting A-index (clamped to `size()-1`).
  /// @return B-index ≥ 0 on success, otherwise -1.
  static int MapBackwardToB(const std::vector<int> &a2b, int i) {
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

  /// \brief Returns true if \p pos is at the line’s indentation column.
  ///
  /// Checks the characters immediately to the left of \p pos on the same line
  /// and verifies they are only spaces or tabs, stopping at the previous '\n'
  /// or '\r' (or beginning of the string).
  ///
  /// \param s   Full buffer to inspect (no ownership taken).
  /// \param pos Byte offset within \p s to test.
  /// \return True iff every character in the half-open range [BOL, pos) is
  ///         ' ' or '\t'. Runs in O(column) time.
  static bool IsAtLineIndent(StringRef s, int pos);

  /// \brief Compute line bounds and the start index of the identifier preceding
  ///        the first '(' on the same line.
  ///
  /// For a given `pos`, this function computes:
  /// 1. **bol** — the index of the first character of the line containing `pos`
  ///    (the first index ≥ 0 after the most recent `'\n'` or `'\r'`, or 0).
  /// 2. **eol** — the index one past the last character of that line
  ///    (the first index ≥ `bol` at `'\n'`/`'\r'`, or `text.length()`).
  /// 3. Scan `[bol, eol)` for the first `'('`.
  ///    * If none is found:
  ///      set `fnStart = bol` and return `{ bol, fnStart, eol }`.
  ///    * If a `'('` is found at index `i`, then:
  ///      * Set `j = i - 1`.
  ///      * Decrement `j` while `text[j]` is a space or tab.
  ///      * Decrement `j` while `text[j] == '*'` (pointer stars immediately
  ///      left
  ///        of the name).
  ///      * Decrement `j` while `text[j]` is an identifier character
  ///        (`[A-Za-z0-9_]`).
  ///      * Finally, set `fnStart = max(bol, j + 1)` and return
  ///        `{ bol, fnStart, eol }`.
  ///
  /// This is a **purely syntactic, line-local rule** that operates over the raw
  /// bytes of `text`. It does not interpret C/C++ grammar or semantics, does
  /// not cross line boundaries, and yields a deterministic result for any
  /// input.
  ///
  /// **Usage:**
  /// Callers compare an insertion anchor to `fnStart`. If equal, they may shift
  /// the anchor to the line indent so that a leading type, qualifier, or
  /// pointer prefix on the next line remains intact. Because this rule depends
  /// only on exact characters, its behavior is deterministic even in
  /// non-function contexts that match the same pattern.
  ///
  /// \param text Entire file contents.
  /// \param pos  Character offset within `text`.
  /// \returns `{ bol, fnStart, eol }` where `[bol, eol)` are the line bounds
  /// and
  ///          `fnStart` is defined as above.
  static std::array<int, 3> LineAndFuncNameStart(StringRef text, int pos);

  /// \brief Adjust an insertion anchor if it starts at a function name.
  ///
  /// If `pos` is exactly at the start of a function name on its line (the
  /// identifier immediately preceding the first `'('` on that line), the anchor
  /// is moved to the line’s first non-space character (the indentation before
  /// the type/qualifier/pointer prefix). Otherwise, `pos` is returned
  /// unchanged.
  ///
  /// **Why:**
  /// When an insertion is anchored at the function-name start, placing it at
  /// the line indent instead preserves the leading type/qualifier/pointer
  /// prefix on the next line (e.g., `unsigned long`) and prevents it from being
  /// “eaten” by the edit.
  ///
  /// **Implementation detail:**
  /// Uses `lineAndFuncNameStart(String, int)` to locate the line bounds and the
  /// function-name start on that line. The returned position is always on the
  /// same line as `pos`.
  ///
  /// \param text Entire file text.
  /// \param pos  Character offset to test, usually an insertion anchor.
  /// \returns The line’s first non-space offset if `pos` equals the
  ///          function-name start; otherwise returns `pos`.
  static int ShiftAnchorToLineIndentIfAtFuncName(StringRef text, int pos);

  /// \brief Return the innermost include item that fully covers a given A-span.
  ///
  /// Determines which include item in the refold model fully covers the
  /// specified token span `[aLo, aHi)` in A-token units. The coverage test uses
  /// each include’s preprocessed-token interval `[coverBegin, coverEnd)`. Among
  /// all includes that contain the queried span, the one with the smallest
  /// width is selected. Includes missing cover data are skipped. Ties are
  /// broken stably by iteration order or ID to maintain deterministic results.
  ///
  /// This function is used by hunk classification to prefer include-based
  /// ownership before macro ownership.
  ///
  /// \param aLo      Inclusive start preprocessed-token index in A.
  /// \param aHi      Exclusive end preprocessed-token index in A.
  /// \returns        The smallest covering include item, or `nullptr` if none
  ///                 cover the span.
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

  /// \brief Compute the TU byte span `[b, e)` corresponding to an A-token
  ///        interval `[a0, a1)`.
  ///
  /// Uses a deterministic, line-local two-phase strategy to map preprocessed
  /// tokens in **A** back to byte offsets in the translation unit.
  ///
  /// ### Phase 1: Direct Mapping
  /// * Scan mapped tokens inside `[a0, a1)` that resolve to `tuPath`.
  /// * If any exist, return the minimal enclosing byte range
  ///   `[min(t.b), max(t.e))` across those tokens.
  ///
  /// ### Phase 2: Neighbor Fallback
  /// If no TU-mapped tokens exist (e.g., for pure insertions or
  /// whitespace-only regions), derive the span from neighbors:
  /// * **Left boundary:** end byte of the closest preceding TU-mapped token;
  ///   if none, beginning of file (0).
  /// * **Right boundary:** start byte of the closest following TU-mapped token;
  ///   if none, end of file (EOF).
  /// * Neighbor search halts if a mapping from a non-TU file is encountered,
  ///   preserving file boundaries.
  ///
  /// ### Notes
  /// * All indices are half-open: token intervals `[a0, a1)`, byte intervals
  ///   `[b, e)`.
  /// * `tuPath` must be absolute and canonical to match entries in the refold
  /// map.
  /// * File length (for EOF) is computed once here; if reading fails, an empty
  ///   length is assumed.
  ///
  /// \param a0      Inclusive start preprocessed-token index in A.
  /// \param a1      Exclusive end preprocessed-token index in A.
  /// \param tuPath  Absolute canonical path of the TU’s source file.
  /// \returns       A two-element array `{ b, e }` giving the TU byte range
  ///                `[b, e)`.
  std::array<int, 2> TUByteSpan(int a0, int a1, StringRef tuPath) const;

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
                                          const diffutils::Hunk &h) const {
    IncludePatch p;
    p.include = &inc;
    p.aStart = h.aStart;
    p.aEnd = h.aEnd;
    p.bStart = h.bStart;
    p.bEnd = h.bEnd;
    p.insertBytes.assign(bSource_.data() + bTokOff_[h.bStart],
                         bSource_.data() + bTokOff_[h.bEnd]);
    return p;
  }

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
                                      const std::vector<int> &a2b) const;

  /// \brief Normalize and coalesce *pure insertion* hunks per include site on
  ///        the B side.
  ///
  /// Given the B-side tokenization and the set of include-scoped patches, this
  /// routine performs the following steps:
  ///
  /// 1. **Widen the B-span** to include any same-line, left-hand
  ///    **type/qualifier/pointer** prefix run (e.g., `unsigned long`,
  ///    `const volatile`, `**`) to prevent prefix tokens from being “eaten”
  ///    by a neighboring REPLACE.
  /// 2. **Merge adjacent insert patches** for the same include and physical
  /// line,
  ///    preserving the matched B “gaps” between them.
  /// 3. **Apply boundary hygiene** — insert a single space only when
  ///    concatenation would change tokenization — and trim after the first
  ///    newline to keep edits line-local.
  ///
  /// All operations are local to a single physical line and never cross
  /// newlines, ensuring deterministic placement and idempotent behavior.
  ///
  /// \param perInclude Map from include ID → include-scoped edits to normalize
  ///                   (mutated in place).
  void
  NormalizeIncludeInsertions(std::map<int, IncludeEdits> &perInclude) const;

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
      int includeId, const std::map<int, IncludeEdits> &perInclude,
      const std::map<std::optional<int>, std::vector<MacroPatch>>
          &macroPatchesByOwner,
      const std::map<int, std::vector<const RefoldModel::IncludeItem *>>
          &children,
      std::map<int, std::string> &includeExpansion) const;

  /// \brief Apply include-scoped insertion and replacement edits to a single
  ///        header’s text.
  ///
  /// For each include patch, this function computes a deterministic anchor
  /// within the target header and emits a `TextEdit` that is safe with respect
  /// to C lexical boundaries.
  ///
  /// The algorithm follows these preferences:
  ///
  /// * **Right-neighbor START** — used when the target token belongs to the
  ///   header; falls back to left-neighbor END or the first token in the header
  ///   if needed.
  /// * If an insert anchor lands at the *function-name start* on that line
  ///   (the identifier immediately before the first `'('`), the anchor is
  ///   *shifted to the line’s first non-space column* (indent) so the existing
  ///   type/qualifier/pointer prefix remains intact and is not consumed by the
  ///   insertion.
  /// * For **REPLACE** hunks at line indent whose replacement ends with a
  ///   newline, if the deleted range lies entirely within the line’s
  ///   type/qualifier/pointer prefix (before the function name), the operation
  ///   is converted to a zero-width **INSERT** to preserve that prefix.
  ///
  /// Edits emitted by this method are strictly **line-local** and
  /// **idempotent**; token gluing is avoided by inserting a single space only
  /// when maximal-munch would otherwise alter the token sequence.
  ///
  /// \param ie         Include-scoped edits to apply (already normalized and
  ///                   merged).
  /// \param headerText Entire text of the resolved include file to patch.
  /// \returns          The header text with all include-scoped edits applied.
  std::string ApplyIncludeInsertions(const IncludeEdits &ie,
                                     std::string headerText) const;

  // ----------------------- low-level mapping & utils -----------------------

  /// \brief Advance past a single optional newline at \p pos.
  ///
  /// If `text[pos]` begins a newline sequence, advances past it:
  /// * `\r\n` → returns `pos + 2`
  /// * `\r`   → returns `pos + 1`
  /// * `\n`   → returns `pos + 1`
  /// Otherwise returns `pos` unchanged. Bounds are respected via `len`.
  ///
  /// \param text Source buffer.
  /// \param pos  Current byte position (0-based).
  /// \param len  Total buffer length.
  /// \returns New position after skipping at most one newline sequence.
  static int HopPastOptionalNewline(StringRef text, int pos, int len);

  /// \brief Look up the byte start for a PP token index within a specific file.
  ///
  /// If the refold model maps preprocessed token \p pp to \p file, returns its
  /// starting byte offset. If no matching mapping exists, returns \p fileLen
  /// when \p fallbackToEOF is true, otherwise `-1`.
  ///
  /// \param file          Canonical path of the target file.
  /// \param pp            Preprocessed token index.
  /// \param fallbackToEOF Return \p fileLen when no mapping is found.
  /// \param fileLen       File length used when falling back.
  /// \returns Byte start offset, or `fileLen`/`-1` per fallback policy.
  int ByteStartForPPInFile(StringRef file, int pp, bool fallbackToEOF,
                           int fileLen) const {
    auto it = model_.GetTokmapByPP().find(pp);
    if (it != model_.GetTokmapByPP().end() && PathsEqual(it->second.file, file))
      return it->second.b;
    return fallbackToEOF ? fileLen : -1;
  }

  /// \brief Look up the byte end for a PP token index within a specific file.
  ///
  /// If the refold model maps preprocessed token \p pp to \p file, returns its
  /// ending byte offset. If no matching mapping exists, returns \p fileLen when
  /// \p fallbackToEOF is true, otherwise `-1`.
  ///
  /// \param file          Canonical path of the target file.
  /// \param pp            Preprocessed token index.
  /// \param fallbackToEOF Return \p fileLen when no mapping is found.
  /// \param fileLen       File length used when falling back.
  /// \returns Byte end offset, or `fileLen`/`-1` per fallback policy.
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

  /// \brief Strip angle or quote delimiters from an include token.
  ///
  /// Trims surrounding whitespace, then removes a single leading/trailing pair
  /// of `< >` or `"` `"`, if present. Returns the undecorated header name.
  ///
  /// \param token Raw include token (e.g., `<foo.h>`, `"bar/baz.h"`).
  /// \returns Header name without surrounding whitespace or delimiters.
  static std::string StripHeaderToken(StringRef token) {
    auto s = token.trim();
    if (s.size() >= 2 && s.front() == '<' && s.back() == '>')
      return s.substr(1, s.size() - 2).str();
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
      return s.substr(1, s.size() - 2).str();
    return s.str();
  }
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H
