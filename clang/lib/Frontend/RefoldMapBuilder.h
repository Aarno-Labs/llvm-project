//===- RefoldMapBuilder.h - Build clang-refold mapping ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header declares RefoldMapBuilder, a thin recorder around Clang’s
// Preprocessor that constructs the “refold map” consumed by clang-refold.
//
// The refold map is a deterministic description of how the *original*
// preprocessed token stream (A) was produced from source: it includes
// per-item token spans (macros, include directives, files), byte anchors for
// source sites (e.g. the line of an #include), optional coverage intervals
// in A (“pp_cover”), and a pp-token-to-source byte mapping (tokmap). The map
// is emitted as JSON and enables clang-refold to re-integrate edits made to a
// retransformed preprocessed stream (B) back into the original C/C++ sources.
//
// RefoldMapBuilder listens to:
//   - File entry/exit (#include nesting) to build a logical include tree.
//   - Macro define/undef and expansions to capture items and spans.
//   - Raw #pragma lines (opaque items) with byte anchors only.
//   - Every printed preprocessor token to extend half-open token spans and
//     fill the primary token map.
//
// Invariants:
//   - Token spans per item are contiguous half-open intervals [Begin, End).
//   - Item “cover” is the minimal A interval covering all spans (may be
//     absent/empty and represented as [-1,-1) downstream).
//   - All file paths are canonicalized to absolute paths when available; the
//     TU path is always absolute if the main file entry is present.
//
// This builder is intentionally serialization-agnostic except for the final
// `writeJSON()` pass.
//
// This header is private to clang/lib/Frontend/ (not installed).
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_FRONTEND_REFOLDMAPBUILDER_H
#define LLVM_CLANG_FRONTEND_REFOLDMAPBUILDER_H

#include "clang/Basic/FileEntry.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringMap.h"

#include <string>
#include <utility>

namespace clang {
namespace refold {

/// Half-open token range over the printed stream: [Begin, End). `Open == true`
/// while we are still extending the contiguous run for the owning item.
struct TokenSpan {
  uint64_t Begin = 0, End = 0;
  bool Open = false;
};

struct ArgTokenSpan {
  uint64_t Begin = 0, End = 0;
  int ArgIndex = -1;
  bool Open = false;
};

struct HeaderDecl {
  std::string Kind;  // "function", "unknown", etc.
  std::string Name;  // e.g. "first"
  std::string File;  // header path for header_span.file
  unsigned HeaderB = 0;
  unsigned HeaderE = 0;
  unsigned PPBegin = 0; // A-token index
  unsigned PPEnd = 0;   // A-token index
};

/// Discriminates item category in the map: preprocessor directive, macro, or
/// file pseudo-item (for TU-level token spans).
enum ItemKind { IK_Directive, IK_Macro, IK_File };

/// A recorded unit in the map: include/macro/directive/file, with text,
/// anchors, and contiguous token spans over the printed stream.
struct Item {
  int ID = -1;
  ItemKind Kind = IK_File;
  std::string Subkind; // "#include", "#define", ...
  std::string Name;    // macro name
  std::string Text;    // directive text
  std::string InvText; // macro call text
  std::string InvFile; // file containing the macro invocation
  bool IsBuiltinMacro = false; // true for predefined/builtin macros (e.g. __FILE__)
  SourceLocation Loc;  // primary location
  std::vector<TokenSpan> Spans;
  std::vector<ArgTokenSpan> ArgSpans; // tokens from any actual arguments
  std::vector<TokenSpan> BodySpans;   // tokens from the macro body
  // Invocation-site byte ranges [begin,end) for each actual argument (index
  // matches formal parameter order).
  std::vector<std::pair<long long, long long>> InvArgRanges;
  std::vector<HeaderDecl> Decls;

  // main-file byte range of the macro invocation (if applicable)
  long long InvBegin = -1;
  long long InvEnd = -1;

  // --- New: include-site anchors and structure ---
  long long SiteBegin = -1;    // byte offset of '#' in the directive's file
  long long SiteEnd = -1;      // one-past-end of the directive line (incl. EOL)
  std::string SitePath;        // file path that contains the directive
  std::string TargetAsWritten; // as-written header token ("e.h" or <vector>)
  std::string ResolvedPath;    // filesystem path actually opened for include
  bool IsAngled = false;       // <...> vs "..."
  int Parent = -1;             // parent include item id, or -1 if top-level
  int OwnerIncludeId =
      -1; // include item id that opened the file containing this item
};

// Small utility to append/extend a half-open token span list.
inline void touchTokSpan(std::vector<TokenSpan> &V, uint32_t TokIdx) {
  if (V.empty() || V.back().End != TokIdx) V.push_back({TokIdx, TokIdx+1});
  else V.back().End++;
}

// Small utility to append/extend a half-open token span list (args only).
inline void touchArgTokSpan(std::vector<ArgTokenSpan> &V, uint32_t TokIdx, int ArgIndex) {
  if (!V.empty() && V.back().Open && V.back().End == TokIdx &&
      V.back().ArgIndex == ArgIndex) {
    V.back().End = TokIdx + 1;
    return;
  }

  if (!V.empty() && V.back().Open)
    V.back().Open = false;

  ArgTokenSpan S;
  S.Begin = TokIdx;
  S.End = TokIdx + 1;
  S.ArgIndex = ArgIndex;
  S.Open = true;
  V.push_back(S);
}

/// Mapping from a printed PP token to its source file byte range.
struct TokMapEntry {
  std::string File; // path of the source file containing [SrcBegin,SrcEnd)
  uint64_t PPIndex = 0;
  long long SrcBegin = -1;
  long long SrcEnd = -1;
};

/// One arm of a conditional group (#if/#elif/#else), with tag, condition text,
/// and body byte range (exclusive of directive lines).
struct CondArm {
  std::string Tag;  // "if","ifdef","ifndef","elif","else"
  std::string Cond; // optional (if/elif expr, or macro for ifdef/ifndef)
  uint64_t BodyB = 0, BodyE = 0;
};

/// A conditional group (#if..#endif) within a file, holding arms in order and
/// the byte range of the whole group.
struct CondGroup {
  std::string File;
  uint64_t GroupB = 0, GroupE = 0; // [#if .. #endif] as bytes
  std::vector<CondArm> Arms;
};

/// Builds a deterministic “refold map” by observing the Clang preprocessor.
///
/// This class hooks into `clang::Preprocessor` callbacks (include directives,
/// macro definitions and expansions, pragmas, file enter/exit, and token
/// emission) and records enough structure to later refold edits in a modified
/// preprocessed stream back into the original source files.
///
/// Responsibilities:
///  - Assign stable IDs to items (includes, macros, files).
///  - Record per-item token spans over the printed PP token stream as
///    contiguous half-open intervals [Begin, End), extending them incrementally
///    as tokens are seen.
///  - Capture byte-accurate source anchors for directive sites (e.g. the full
///    line containing an `#include`) and, for macro invocations, the exact
///    callsite range in the owning file.
///  - Maintain the include nesting (owner/parent) to disambiguate repeated
///    includes and enable deterministic placement of edits.
///  - Emit the assembled structure to JSON (`writeJSON()`).
///
/// Paths:
///  - If the main file can be resolved, the TU path and any file paths are
///    written as canonical absolute paths. Otherwise “pseudo” paths are
///    preserved as-is.
///
/// Lifetimes & usage:
///  - Construct once per TU with a `Preprocessor&`.
///  - Feed PP events as preprocessing proceeds.
///  - Call `writeJSON()` at the end to serialize the accumulated map.
///
/// All recording is deterministic; there is no heuristic guessing here—the
/// builder emits exactly what the preprocessor did.
class RefoldMapBuilder {
  Preprocessor &PP;
  SourceManager &SM;
  const LangOptions &Lang;
  bool IgnoreComments = true;
  uint64_t TokIndex = 0;

  std::vector<Item> Items;
  llvm::StringMap<int> MacroKey2Item;
  llvm::StringMap<int> IncludeKey2Item;
  std::vector<int> IncludeStack; // item indices (include items), -1 for none
  int CurrentFileItem = -1;

  std::vector<TokMapEntry> TokMap;

  std::string OutPath;
  std::string Cwd; // Captured working directory (for resolving relative spellings)

  std::string TUSourcePath;   // TU path spelling (for JSON 'source')
  bool EmitAbsPaths = false;  // If true, emit canonical absolute paths

  /// Map resolved absolute include directories -> original `-I` spellings.
  llvm::StringMap<std::string> IncludeDirAbs2Spelling;
  /// Map resolved absolute file paths -> chosen spelling (TU/header).
  llvm::StringMap<std::string> FileAbs2Spelling;

  /// Compute the byte interval for a directive’s *entire line*.
  ///
  /// Given the location of the `#` token, returns a half-open byte range
  /// [begin, end) covering from the `#` through the terminating newline (or
  /// end-of-file if the line is unterminated). Used to anchor directive sites
  /// in the source file.
  ///
  /// \returns (begin, end) in the directive’s source file.
  std::pair<long long, long long> computeDirectiveLine(SourceLocation HashLoc);

  /// Canonical absolute path for a file entry (when possible).
  ///
  /// Resolves the given `FileEntryRef` to a canonical absolute path suitable
  /// for emission into the JSON map. Pseudo paths (e.g. virtual buffers) are
  /// preserved as-is.
  ///
  /// \returns absolute/canonical path string or the original pseudo path.
  static std::string absolutePathFor(const clang::FileEntryRef &FER);

  /// Path for a source location; absolute if requested.
  ///
  /// Looks up the file corresponding to `L` and returns either a canonical
  /// absolute path (when `WantAbs` is true and resolution succeeds) or the
  /// best-effort path as managed by the `SourceManager`.
  ///
  /// \returns a path string appropriate for emission.
  std::string filePathForLocAbs(clang::SourceManager &SM,
                                clang::SourceLocation L, bool WantAbs);

  // Maps a macro-expanded token’s spelling location back to the invocation-site
  // argument index (0..N-1). Returns -1 if unknown / not in invocation file.
  int argIndexForSpellingLoc(const Item &MI, SourceLocation Loc,
                             SourceManager &SM, const LangOptions &Lang,
                             bool EmitAbsPaths);

  void addHeaderDecl(Item &Inc, StringRef Kind, StringRef Name,
                     StringRef HeaderFile, unsigned HeaderB,
                     unsigned HeaderE, unsigned PPBegin, unsigned PPEnd) {
    HeaderDecl D;
    D.Kind = Kind.str();
    D.Name = Name.str();
    D.File = HeaderFile.str();
    D.HeaderB = HeaderB;
    D.HeaderE = HeaderE;
    D.PPBegin = PPBegin;
    D.PPEnd = PPEnd;
    Inc.Decls.push_back(std::move(D));
  }

  void addHeaderDecl(Item &Inc, StringRef Name, StringRef HeaderFile,
                     unsigned HeaderB, unsigned HeaderE, unsigned PPBegin,
                     unsigned PPEnd) {
    addHeaderDecl(Inc, "unknown", Name, HeaderFile, HeaderB, HeaderE,
                  PPBegin, PPEnd);
  }

public:
  /// Construct a builder bound to a preprocessor and an output path.
  ///
  /// This builder records *spelled* paths (verbatim spellings) rather than
  /// canonicalized paths:
  ///   - the TU path spelling is taken from the command line (argv) and written
  ///     to JSON `source`,
  ///   - include file spellings are derived from the include search directory
  ///     spelling (e.g. `-I ./headers`) combined with Clang’s provided
  ///     `RelativePath` (yielding e.g. `./headers/bob.h`).
  ///
  /// When a spelling is relative, it is interpreted relative to `pp_ctx.cwd`.
  ///
  /// \param PP      Preprocessor to observe (tokens, directives, nesting).
  /// \param OutPath Destination file path for JSON output; when empty,
  ///                the builder is effectively disabled (no-ops).
  RefoldMapBuilder(Preprocessor &PP, StringRef OutPath);

  /// Extend (or open) the current contiguous token span for an item.
  ///
  /// Ensures spans for the given item are maintained as half-open intervals
  /// [Begin, End) over the *printed* PP-token index space. If the last span is
  /// currently open, this advances its End to `CurTokIndex + 1`; otherwise, a
  /// new open span starting at `CurTokIndex` is created.
  ///
  /// \param ItemIdx  Index into the internal `Items` vector; negative is
  ///                 ignored.
  /// \param CurTokIndex Zero-based index of the just-emitted printed token.
  void touchSpanForItem(int ItemIdx, uint64_t CurTokIndex) {
    if (ItemIdx < 0)
      return;
    auto &V = Items[(size_t)ItemIdx].Spans;
    if (!V.empty() && V.back().Open) {
      V.back().End = CurTokIndex + 1; // extend to one-past-current
    } else {
      TokenSpan S;
      S.Begin = CurTokIndex;
      S.End = CurTokIndex + 1;
      S.Open = true;
      V.push_back(S);
    }
  }

  /// \returns true iff an output path was provided and the builder is active.
  bool enabled() const { return !OutPath.empty(); }

  /// Callback for `#include`/`#include_next`.
  ///
  /// Records:
  ///  - The directive site’s byte range (including trailing newline).
  ///  - The as-written target token (e.g. `"e.h"` or `<vector>`).
  ///  - Whether it was angled or quoted.
  ///  - The resolved filesystem path when available.
  ///  - Parent include relationships (include stack).
  ///
  /// Also prepares a fresh include item (with no spans yet); spans are
  /// associated as subsequent tokens are printed while this include is active.
  void onIncludeDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange,
                          OptionalFileEntryRef File, StringRef SearchPath,
                          StringRef RelativePath);

  /// Callback for `#define`.
  ///
  /// Creates a directive item for the definition, records the macro’s name,
  /// exact text, and opens/extends its token spans as tokens are printed.
  void onMacroDefined(const Token &MacroNameTok, const MacroDirective *MD);

  /// Callback for `#undef`.
  ///
  /// Records an `#undef` directive item with exact text and site anchors.
  /// (Token spans for `#undef` are typically empty.)
  void onMacroUndefined(const Token &MacroNameTok, const MacroDefinition &MD,
                        const MacroDirective *Undef);

  /// Callback for macro expansion at a call site.
  ///
  /// Records a macro *item* with:
  ///  - Invocation text and byte range in its owning file,
  ///  - Owner include id (to disambiguate repeated includes),
  ///  - Token spans contributed by the expansion while it is active.
  void onMacroExpands(const Token &MacroNameTok, const MacroDefinition &MD,
                      SourceRange Range, const MacroArgs *Args);

  /// Callback for a raw `#pragma` line.
  ///
  /// Pragmas are recorded as directive items with exact source text and site
  /// byte anchors. They do not contribute to A-token spans.
  void onPragma(SourceLocation HashLoc, StringRef FullText);

  /// Entering a file (either TU or an included header).
  ///
  /// Pushes a new include context on the include stack and creates/updates
  /// the current file item for span attribution.
  void onEnterFile(SourceLocation IncludeLoc);

  /// Entering a file (either TU or an included header).
  ///
  /// Pushes a new include context on the include stack and creates/updates
  /// the current file item for span attribution.
  void onExitFile() {
    if (!enabled())
      return;
    if (!IncludeStack.empty())
      IncludeStack.pop_back();
  }

  /// Observe a single *printed* preprocessor token.
  ///
  /// Advances token index and attributes the token to the currently active
  /// items (file/include/macro) by extending their open spans.
  void onToken(const Token &Tok);

  /// End-of-stream notification.
  ///
  /// Closes any open spans so all recorded item spans become half-open and
  /// well-formed prior to serialization.
  void onEndOfStream() {
    if (!enabled())
      return;
    for (auto &It : Items)
      for (auto &S : It.Spans)
        S.Open = false;
  }

  void finalizeIncludeDecls();

  /// Serialize the accumulated map as JSON to `OutPath`.
  ///
  /// Emits:
  ///  - Version, TU path, and token count,
  ///  - Items (macros, directives, files) with spans and anchors,
  ///  - Primary PP-token map (`TokMap`),
  ///  - Conditional groups, if collected.
  ///
  /// This function performs no additional inference; it serializes exactly
  /// what has been recorded so far.
  void writeJSON();
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_FRONTEND_REFOLDMAPBUILDER_H
