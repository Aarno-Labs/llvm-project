//===--- LineDirectiveInserter.h -------------------------------*- C++ -*-===//
//
// Source #line directive insertion and logical-location tracking.
//
// LineDirectiveInserter formats synthetic #line directives, suppresses no-op
// resyncs, wraps materialized include bodies with entry/return directives, and
// evaluates source-authored line-control state when a caller needs the logical
// location at a source byte offset.  It does not own refold orchestration state;
// callers provide source text, producer spellings, and candidate offsets.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_LINEDIRECTIVEINSERTER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_LINEDIRECTIVEINSERTER_H

#include "util/StringUtils.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Path.h>
#include <optional>
#include <string>

using namespace llvm;

namespace clang {
namespace refold {

class RefoldModel;

/// \brief Parsed state of an emitted #line directive sufficient to reason about
/// whether a future directive would be a no-op.
///
/// lineAfterDirective is the logical 1-based line number established by the
/// directive. afterDirectiveIndex points to the first character position in the
/// output after the directive line (i.e., after the newline if present), which
/// is where newline accumulation for subsequent output begins.
struct LineDirectiveState {
  std::string fileSpelling;
  size_t lineAfterDir;
  size_t afterDirIdx;
  bool hasFileSpelling;

  LineDirectiveState(StringRef file, size_t line, size_t idx,
                     bool hasFile = true)
      : fileSpelling(file.str()), lineAfterDir(line), afterDirIdx(idx),
        hasFileSpelling(hasFile) {}
};

/// \brief Logical source location at a byte offset in an original source file.
///
/// This is the location the preprocessor would assign to the next token emitted
/// from that offset after accounting for preceding `#line` directives.
struct LineDirectiveLocation {
  std::string fileSpelling;
  size_t lineNo;

  /// True only when the logical state was recovered without encountering an
  /// unmodeled source-authored line-control directive.  The refolder may use
  /// such a location to emit a synthetic resync directive.  When false, the
  /// safest source-preserving action is to avoid overriding the still-present
  /// source line-control stream, because doing so would replace producer-owned
  /// preprocessing semantics with an inferred physical fallback.
  bool producerProven = true;

  /// Byte offset of the last source line-control directive before this location
  /// whose effect could not be proven from the current model-backed owner-local
  /// scan.  This distinguishes an unmodeled preserved prefix, where suppressing
  /// an extra synthetic resync leaves the real source directive in force, from
  /// a consumed unmodeled directive, which must fail closed.
  std::optional<uint64_t> unprovenLineControlDirectiveOffset;

  LineDirectiveLocation(StringRef file, size_t line, bool proven = true,
                        std::optional<uint64_t> unprovenOffset = std::nullopt)
      : fileSpelling(file.str()), lineNo(line), producerProven(proven),
        unprovenLineControlDirectiveOffset(unprovenOffset) {}
};

/// \brief Inserts #line directives to preserve preprocessor "logical location"
/// transparency after refolding edits.
///
/// ### Problem this class solves
/// Refolding can:
/// * physically move header-originating tokens into the TU (via realized
///   include expansion), and
/// * change the number of emitted newlines (via insertions/replacements,
///   including macro edits).
///
/// Either effect can cause `__FILE__`, `__FILE_NAME__`, and `__LINE__` to
/// differ from what they were in the edited preprocessed stream unless the
/// logical file/line state is explicitly restored.
///
/// ### Design constraints
/// * **Spelling is producer-owned.** This class does not "choose" how to spell
///   file paths. It expects the producer (clang) to emit the correct spelled
///   paths (e.g., as written on the command line for the TU, and as
///   relative-to-`-I` for includes).
/// * **Safety first.** Local insertion of #line is only performed when the
///   directive can begin at a beginning-of-line (BOL) in the emitted output
///   without changing token adjacency. When not safely placeable, the caller
///   may defer insertion (via a "pending resync" mechanism in `RefoldEngine`).
/// * **Idempotence.** Where possible, this class suppresses duplicate or no-op
///   directives so refolding does not accumulate gratuitous #line directives.
///
/// ### Primary responsibilities
/// 1. **Include expansion wrapping:** emit an "enter child" `#line 1 "..."`
///    before the realized header body, and an "exit to parent"
///    `#line N "..."` after it.
/// 2. **Local resynchronization:** when a replacement changes newline count,
///    attempt to inject a resync #line inside the replacement at a safe BOL
///    position.
/// 3. **No-op suppression:** when the engine flushes pending resync
///    directives, determine whether emitting a #line would be a no-op given the
///    most recent emitted #line state and the naturally-emitted newlines since
///    then.
///
/// **Note on indices:** All offsets passed to this class are byte offsets into
/// the corresponding `StringRef`. The refolder treats source and preprocessor
/// buffers as byte streams; callers must not pass decoded character indices.
class LineDirectiveInserter {
public:
  LineDirectiveInserter(bool enabled, StringRef cwd);

  /// Returns whether line-directive insertion is enabled.
  bool Enabled() const { return enabled_; }

  /// \brief Resolves a possibly-relative spelled path to an absolute normalized
  /// Path.
  ///
  /// This method is **not** used to decide how paths should be spelled in
  /// emitted #line directives; it is only a convenience for locating files on
  /// disk when the JSON contains relative spellings.
  ///
  /// * If `spelledPath` is already absolute, it is normalized and returned.
  /// * If `spelledPath` is relative and `cwd` is present, it is resolved
  ///   against `cwd` and normalized.
  /// * If `spelledPath` is relative and `cwd` is absent, `ToAbsolutePath()` is
  ///   used as a best-effort fallback.
  ///
  /// \param spelledPath the producer-provided spelling (may be relative)
  /// \return an absolute, normalized path suitable for filesystem access
  std::string ToAbsolutePath(StringRef spelledPath) const;

  /// \brief Formats a single #line directive with conventional clang-style
  /// quoting.
  ///
  /// For nonzero line numbers, the returned string always ends in `\n` so the
  /// directive forms a complete preprocessing line when inserted at a BOL. A
  /// zero line number produces the empty string.
  ///
  /// \param lineNo 1-based logical line number to set
  /// \param spelledFile file spelling to embed in the directive
  ///        (producer-owned)
  /// \return a complete #line ... line including trailing newline
  static std::string FormatLineDirective(size_t lineNo, StringRef spelledFile) {
    if (lineNo == 0)
      return "";

    std::string result = "#line ";
    result += std::to_string(lineNo);
    result += " ";
    result += stringutils::quoteLineDirectivePath(spelledFile);
    result += "\n";
    return result;
  }

  /// \brief Computes the logical parent location using producer-proven
  /// conditional activity from the refold map.
  ///
  /// Conditional groups are not re-evaluated from source text: a directive
  /// effect is visible only when the RefoldModel proves that the directive byte
  /// was executed for the requested owner file/include instance. Macro-state
  /// directives use producer-observed MacroDirective items; line-control
  /// directives use producer-proven selected conditional ownership.
  ///
  /// \param model producer refold map model containing conditional arm selection
  /// \param ownerFile file whose bytes are being scanned
  /// \param ownerIncludeId include instance that owns \p ownerFile, or
  ///        std::nullopt for the TU owner
  static LineDirectiveLocation LogicalLocationAtOffset(
      StringRef src, uint64_t offset, StringRef defaultFileSpelling,
      const RefoldModel &model, StringRef ownerFile,
      std::optional<uint64_t> ownerIncludeId = std::nullopt);


  /// \brief Attempts a *local* resynchronization by injecting a #line directive
  /// into the replacement text when (and only when) the replacement changes the
  /// newline count of the replaced region.
  ///
  /// ### When this is needed
  /// If a replacement changes the number of newlines in
  /// `originalFileText[start:end)`, then subsequent untouched slices of the
  /// original file would otherwise have shifted logical line numbers. A resync
  /// directive restores `__LINE__` (and can reaffirm `__FILE__`) for code that
  /// follows.
  ///
  /// ### Injection strategy (safe-BOL only)
  /// This method only injects where the directive can begin at BOL without
  /// re-tokenizing adjacent code:
  /// * If `replacement` is empty, it returns just the directive (caller emits
  ///   it at the edit site).
  /// * If `replacement` ends with `\n`, it appends the directive (unless
  ///   already present).
  /// * If `replacement` contains at least one newline and ends with an
  ///   indentation-only suffix (spaces/tabs/CR), it inserts the directive
  ///   immediately after the last newline and before that suffix.
  ///
  /// If none of the safe cases apply, this method returns `replacement`
  /// unchanged. This includes cases where the directive would be at BOL within
  /// the replacement itself, but the replacement would rejoin untouched
  /// original bytes that continue on the same physical line. In that case,
  /// callers should defer correction (e.g., via a pending-resync mechanism
  /// that flushes at the next safe BOL while emitting subsequent original
  /// slices).
  ///
  /// ### Idempotence
  /// The method suppresses duplicate insertion when the replacement already
  /// ends with (or already contains immediately-before-indent) the exact
  /// directive string it would emit.
  ///
  /// \param originalFileText the pre-edit file text that start/end refer to
  /// \param s start offset (inclusive) in originalFileText
  /// \param e end offset (exclusive) in originalFileText
  /// \param replacement replacement text to emit for [s,e)
  /// \param resumeLoc logical file/line location to restore before the
  ///        untouched suffix resumes
  /// \return either replacement unchanged, or replacement with a
  ///         locally-inserted directive
  std::string MaybeAppendResyncAfterReplacement(
      StringRef originalFileText, uint64_t s, uint64_t e,
      StringRef replacement, const LineDirectiveLocation &resumeLoc) const;

  /// \brief Determines whether a #line directive should be emitted at the
  /// *current output position*.
  ///
  /// This is used primarily by the engine's "pending resync flush" path to
  /// suppress directives that would not change the current logical file/line
  /// state (e.g., the "gratuitous #line 7" case).
  ///
  /// ### Suppression rules
  /// * If `out` already ends with exactly `directive`, do not emit it again.
  /// * If not at BOL, be conservative and allow emission (we cannot reliably
  ///   reason about whether it is a no-op without risking token adjacency
  ///   changes).
  /// * If at BOL, attempt to reconstruct the current logical location from the
  ///   most recent emitted #line directive (within a bounded lookback) plus the
  ///   number of *non-line-spliced* newlines since then. If that reconstructed
  ///   location already equals `(fileSpellingForDirective, targetLine)`, treat
  ///   the directive as a no-op and suppress it.
  ///
  /// \param src source buffer
  /// \param fileSpellingForDirective target file spelling for the directive
  /// \param targetLine target 1-based line number for the directive
  /// \param directive the fully formatted directive string
  /// \return true if the directive should be emitted; false if it would be
  ///         redundant/no-op
  static bool ShouldEmitLineDirective(StringRef src,
                                      StringRef fileSpellingForDir,
                                      size_t targetLine, StringRef directive) {
    // If we literally just emitted the exact same directive, don't duplicate
    // it.
    if (src.ends_with(directive))
      return false;

    // Only attempt the "no-op" suppression at BOL; otherwise be conservative.
    if (!stringutils::outAtBOL(src))
      return true;

    return !LineDirectiveWouldBeNoOp(src, fileSpellingForDir, targetLine);
  }

  /// \brief Escapes a path string for safe inclusion inside a quoted `#line`
  /// directive.
  ///
  /// This performs conservative escaping:
  /// - `\` becomes `\\`
  /// - `"` becomes `\"`
  /// - named control bytes such as newline and tab are emitted as C escapes
  ///
  /// Other characters are left unchanged.
  ///
  /// \param path raw file spelling
  /// \return escaped representation for use inside "..." in a directive
  static std::string EscapeForLineDirective(StringRef path);

private:
  bool enabled_;
  StringRef cwd_;

  /// \brief Finds the most recent parseable #line directive in `out`.
  ///
  /// This scan is intentionally bounded (currently 16 KiB) to keep refolding
  /// fast even for large files. If the last relevant directive lies beyond the
  /// lookback window, this method may return null and callers will behave
  /// conservatively (i.e., they will emit directives rather than risk missing a
  /// needed resync).
  ///
  /// \param src source buffer
  /// \return parsed state for the last directive, or null if none is
  ///         found/parseable in the window
  static std::optional<LineDirectiveState>
  FindLastLineDirectiveState(StringRef src);

  /// \brief Computes whether emitting `#line targetLine "fileSpellingForDir"`
  /// at the current output position would have no effect on the logical
  /// location.
  ///
  /// Mechanism: find the most recent parseable #line directive in `out`
  /// (bounded lookback), then compute:
  ///
  ///     currentLine = lastDirective.line +
  ///                   countNonSplicedNewlines(out, lastDirective.afterIdx,
  ///                                           out.length())
  ///
  /// If `lastDirective.file` matches `fileSpellingForDir` and `currentLine ==
  /// targetLine`, then the new directive is a no-op.
  ///
  /// If parsing fails or no directive is found, this returns false (meaning
  /// "unknown" so callers should emit conservatively).
  static bool LineDirectiveWouldBeNoOp(StringRef src,
                                       StringRef fileSpellingForDir,
                                       size_t targetLine) {
    auto st = FindLastLineDirectiveState(src);
    if (!st || st->fileSpelling != fileSpellingForDir)
      return false;

    size_t delta =
        stringutils::countNonSplicedNewlines(src, st->afterDirIdx, src.size());
    return (st->lineAfterDir + delta) == targetLine;
  }
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_LINEDIRECTIVEINSERTER_H
