#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_LINEDIRECTIVEINSERTER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_LINEDIRECTIVEINSERTER_H

#include "StringUtils.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Path.h>
#include <optional>
#include <string>

using namespace llvm;

namespace clang {
namespace refold {

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

  LineDirectiveState(StringRef file, size_t line, size_t idx)
      : fileSpelling(file.str()), lineAfterDir(line), afterDirIdx(idx) {}
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
/// **Note on indices:** All offsets passed to this class are Java String
/// indices. In practice your inputs are ASCII/UTF-8 preprocessor output where
/// "byte offset" and "char index" coincide; if that assumption changes, call
/// sites must be audited.
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
  /// * If `spelledPath` is relative and `cwd` is absent, `toAbsolutePath()` is
  ///   used as a best-effort fallback.
  ///
  /// \param spelledPath the producer-provided spelling (may be relative)
  /// \return an absolute, normalized path suitable for filesystem access
  std::string ToAbsolutePath(StringRef spelledPath) const;

  /// \brief Formats a single #line directive with conventional clang-style
  /// quoting.
  ///
  /// The returned string always ends in `\n` to ensure the directive forms a
  /// complete preprocessing line when inserted at a BOL.
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
    result += " \"";
    result += EscapeForLineDirective(spelledFile);
    result += "\"\n";
    return result;
  }

  /// \brief Wraps a realized include expansion so the preprocessor logical
  /// file/line state matches the original header for the duration of the
  /// expanded body, then resumes the includer's state.
  ///
  /// The returned string has the form:
  ///
  ///     #line 1 "child"
  ///     (childBody...)
  ///     #line resumeLine "parent"
  ///
  /// **BOL guarantee for the exit directive:** If `childBody` does not end with
  /// a newline, this method inserts a single `\n` so that the "exit" #line
  /// begins at BOL. This is considered safe because:
  ///
  /// * it is whitespace-only, and
  /// * the subsequent #line immediately resets the logical line mapping back to
  ///   the parent, preventing drift for later slices.
  ///
  /// \param childFileSpelling producer spelling for the included header
  /// \param parentFileSpelling producer spelling for the includer (TU or parent
  ///        header)
  /// \param parentResumeLineNo logical line in the parent file to resume at
  ///        after the include body
  /// \param childBody realized include body (may be null)
  /// \return wrapped include body, or childBody unchanged if disabled
  std::string WrapIncludeExpansion(StringRef childFileSpelling,
                                   StringRef parentFileSpelling,
                                   size_t parentResumeLineNo,
                                   StringRef childBody) const;

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
  /// unchanged. In that case, callers should defer correction (e.g., via a
  /// pending-resync mechanism that flushes at the next safe BOL while emitting
  /// subsequent original slices).
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
  /// \param fileSpellingForDirective spelled file path to embed in the
  ///        directive
  /// \return either replacement unchanged, or replacement with a
  ///         locally-inserted directive
  std::string
  MaybeAppendResyncAfterReplacement(StringRef originalFileText, uint64_t s,
                                    uint64_t e, StringRef replacement,
                                    StringRef fileSpellingForDirective) const;

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

  /// Parses a single line that is expected to be a #line directive.
  ///
  /// Expected shape (whitespace tolerant):
  ///
  ///     #line <digits> "file"
  ///
  /// This parser is intentionally minimal and only supports the directive forms
  /// the refolder itself emits. It supports basic backslash escaping within the
  /// quoted file string by treating `\"` and `\\` as a single character.
  ///
  /// \param src source buffer
  /// \param from start index (inclusive) of the line
  /// \param to end index (exclusive) of the line (not including the newline)
  /// \return parsed directive state or null if the line does not match the
  ///         expected form
  static std::optional<LineDirectiveState> ParseLineDirective(StringRef src,
                                                              size_t from, size_t to);

  /// \brief Computes whether emitting `#line targetLine "fileSpellingForDir"` at
  /// the current output position would have no effect on the logical location.
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

    size_t delta = stringutils::countNonSplicedNewlines(
        src, st->afterDirIdx, src.size());
    return (st->lineAfterDir + delta) == targetLine;
  }
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_LINEDIRECTIVEINSERTER_H
