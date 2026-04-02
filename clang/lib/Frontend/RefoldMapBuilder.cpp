//===- RefoldMapBuilder.cpp - Build clang-refold mapping --------*- C++ -*-===//
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
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//
#include "RefoldMapBuilder.h"
#include "PPMacroPrinting.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <iterator>
#include <type_traits>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

namespace {

// Helper to access macro parameter identifiers across Clang versions.
//
// Clang has changed MacroInfo's parameter accessor APIs over time:
//   * Some versions expose `MacroInfo::getParam(I)`.
//   * Some versions expose a raw `IdentifierInfo **` via `getParameterList()`.
//   * Some versions only provide iterators (`param_begin()` / `param_end()`).
//
// Instead of version-ifdefs or overload "tag dispatch", we use the C++17
// detection idiom and `if constexpr` to select the best available API at
// compile time.
template <typename T, typename = void> struct HasGetParam : std::false_type {};
template <typename T>
struct HasGetParam<
    T, std::void_t<decltype(std::declval<const T *>()->getParam(0U))>>
    : std::true_type {};

template <typename T, typename = void>
struct HasGetParameterList : std::false_type {};
template <typename T>
struct HasGetParameterList<
    T, std::void_t<decltype(std::declval<const T *>()->getParameterList())>>
    : std::true_type {};

template <typename MI>
static const IdentifierInfo *getMacroParamIdentifier(const MI *M, unsigned I) {
  if (!M)
    return nullptr;

  // Note: this must be a function template (rather than hard-coding MacroInfo)
  // so that the unused branches of the `if constexpr` remain in a dependent
  // context and do not require the selected Clang version to declare every
  // historical accessor API.
  if constexpr (HasGetParam<MI>::value) {
    return M->getParam(I);
  } else if constexpr (HasGetParameterList<MI>::value) {
    return M->getParameterList()[I];
  } else {
    auto It = M->param_begin();
    for (unsigned N = 0; N < I; ++N)
      ++It;
    return *It;
  }
}

// Normalize keys to file locations so InclusionDirective(HashLoc)
// and EnterFile(IncludeLoc) agree.
static std::string keyForLoc(const SourceManager &SM, SourceLocation Loc) {
  return std::to_string(SM.getFileLoc(Loc).getRawEncoding());
}

// Macro keys must NOT normalize through getFileLoc() or getSpellingLoc(): in
// nested expansions those can collapse distinct invocation sites onto the same
// key, causing MacroKey2Item collisions (e.g., __FILE__ overwriting PRINT_FILE).
// As in the example:
//
//   #define PRINT_FILE(FMT) printf(FMT, __FILE__, __LINE__)
//   PRINT_FILE("Error on file (%s) and line (%d)\n");
//
// Use the SourceLocation raw encoding directly (keeps MacroID locations distinct).
static std::string keyForMacroLoc(SourceLocation Loc) {
  if (Loc.isInvalid())
    return "0";
  return std::to_string(Loc.getRawEncoding());
}

// Returns [bol,eol+1) byte span of the line containing 'p'.
static std::pair<size_t, size_t> lineSpanOf(StringRef S, size_t p) {
  if (p >= S.size())
    return {S.size(), S.size()};

  size_t L = p, R = p;
  while (L > 0 && S[L - 1] != '\n')
    --L;
  while (R < S.size() && S[R] != '\n')
    ++R;
  if (R < S.size())
    ++R; // include '\n'
  return {L, R};
}

/// \brief Scan a source buffer for top-level preprocessor conditional groups.
///
/// This is a lightweight, deterministic line scanner that discovers `#if` /
/// `#ifdef` / `#ifndef`…`#endif` groups that occur at **nesting depth 0** in
/// \p Buf. For each such group it emits a \c CondGroup with:
///  - \c File set to \p FilePath,
///  - \c GroupB / \c GroupE bounding the byte range of the whole group from
///    the beginning of the opening directive line to the end of the closing
///    `#endif` line (half-open),
///  - an ordered list of arms (\c CondArm) for each peer directive at depth 0:
///    the initial \c #if / \c #ifdef / \c #ifndef arm, any number of
///    \c #elif arms, and an optional \c #else arm.
///    Each arm carries:
///      * \c Kind — the directive kind ("if", "ifdef", "ifndef", "elif",
///      "else"),
///      * \c Cond — the as-written condition text for
///      "if"/"elif"/"ifdef"/"ifndef"
///                  (trimmed of leading spaces after the keyword),
///      * \c BodyB / \c BodyE — the half-open byte interval of the arm’s body
///        (the text after the directive line up to—but not including—the next
///        peer directive line at depth 0 or the closing `#endif`).
///
/// The scanner is line-oriented:
///  - It treats a line as a potential directive line if, after optional leading
///    whitespace, it starts with \c '#'.
///  - A helper checks for a keyword match with an identifier boundary
///    (so \c "#ifdefx" does not match).
///  - Nested conditionals are tracked with a stack; nested groups are recorded
///    as their own CondGroup entries, while arm body ranges for outer groups
///    naturally include the text of any nested groups.
///
/// Error tolerance and edge cases:
///  - If a group is unterminated (missing \c #endif), the function closes it at
///    end-of-file so the scan makes progress.
///  - Non-directive lines, comments, and other directives inside a group are
///    ignored except for depth tracking.
///  - Body ranges exclude the directive lines themselves.
///  - Offsets are byte indices into \p Buf; all intervals are half-open
///    \f$[B,E)\f$.
///
/// This function performs **no** preprocessing or expression evaluation; it
/// records only syntactic structure as spelled in the buffer.
///
/// \param Buf       The full source text to scan (bytes).
/// \param FilePath  Path associated with \p Buf; copied into each \c CondGroup.
/// \returns         A vector of top-level \c CondGroup entries in lexical
///                  order.
///
/// \note This routine deliberately avoids any heuristic guessing and is fully
///       deterministic: the same input buffer always produces the same groups.
/// \sa CondGroup, CondArm
static std::vector<CondGroup> scanTopLevelConds(llvm::StringRef Buf,
                                                llvm::StringRef FilePath) {
  // Scans the raw source buffer for preprocessor conditional directive groups:
  //
  //   #if / #ifdef / #ifndef
  //     ... arm body ...
  //   #elif <cond>
  //     ... arm body ...
  //   #else
  //     ... arm body ...
  //   #endif
  //
  // For each group, we record:
  //   - the byte span covering the whole group (GroupB..GroupE),
  //   - and each arm’s directive kind, condition text, and body byte span.
  //
  // This is a lightweight, purely textual scan. It does *not* attempt to
  // preprocess/evaluate conditions, and it does not parse tokens; it only
  // recognizes directive keywords at the start of lines (after optional
  // whitespace) and tracks nesting with a stack.
  std::vector<CondGroup> Groups;
  const size_t N = Buf.size();
  size_t p = 0;

  // Check whether keyword `s` appears at Buf[start..eol) with a word boundary.
  // This prevents matching "#ifdefX" as "#ifdef", etc.
  auto kw_at = [&](size_t start, size_t eol, const char *s) -> bool {
    size_t t = start, k = 0;
    while (t < eol && s[k] && Buf[t] == s[k]) {
      ++t;
      ++k;
    }
    if (s[k])
      return false; // didn't consume full keyword
    if (t < eol) {
      unsigned char c = static_cast<unsigned char>(Buf[t]);
      if (llvm::isAlnum(static_cast<char>(c)) || c == '_') {
        return false; // word boundary: keyword must not be followed by ident
                      // char
      }
    }
    return true;
  };

  // Close the current (most recent) arm body for a group up to 'endAt'.
  //
  // We treat BodyB..BodyE as a half-open byte range in the file buffer,
  // where BodyB is set to the end-of-line of the arm's directive and BodyE is
  // extended when the next directive in the same group begins
  // (elif/else/endif).
  auto setPrevBodyEnd = [&](size_t groupIndex, size_t endAt) {
    if (groupIndex >= Groups.size())
      return;
    auto &G = Groups[groupIndex];
    if (!G.Arms.empty() && G.Arms.back().BodyE == G.Arms.back().BodyB)
      G.Arms.back().BodyE = endAt;
  };

  // Active conditional groups (nesting stack). Each stack entry refers to an
  // index in `Groups`. The top of the stack is the innermost active group.
  struct Active {
    size_t GroupIndex;
  };
  std::vector<Active> Stack;

  while (p < N) {
    // Determine the [bol, eol) byte span for the current line containing `p`.
    // lineSpanOf() returns the bounds *excluding* the newline.
    auto span = lineSpanOf(Buf, p);
    size_t bol = span.first, eol = span.second;
    if (eol <= bol) {
      // Degenerate or empty line; move past it safely.
      p = std::min(N, eol + 1);
      continue;
    }

    // Skip leading horizontal whitespace to detect directives that begin
    // anywhere after indentation.
    size_t s = bol;
    while (s < eol && isSpace</*kWithCR=*/true>(Buf[s]))
      ++s;

    // Recognize directives only when we see '#" after optional indentation.
    if (s < eol && Buf[s] == '#') {
      // Skip whitespace after '#'.
      size_t q = s + 1;
      while (q < eol && isSpace</*kWithCR=*/true>(Buf[q]))
        ++q;

      // The directive "kind" we recognize on this line.
      enum DirKind {
        DK_None,
        DK_If,
        DK_Ifdef,
        DK_Ifndef,
        DK_Elif,
        DK_Else,
        DK_Endif
      };
      DirKind Kind = DK_None;
      llvm::StringRef Tag;

      // Identify which directive keyword appears after '#'.
      if (kw_at(q, eol, "if")) {
        Kind = DK_If;
        Tag = "if";
      } else if (kw_at(q, eol, "ifdef")) {
        Kind = DK_Ifdef;
        Tag = "ifdef";
      } else if (kw_at(q, eol, "ifndef")) {
        Kind = DK_Ifndef;
        Tag = "ifndef";
      } else if (kw_at(q, eol, "elif")) {
        Kind = DK_Elif;
        Tag = "elif";
      } else if (kw_at(q, eol, "else")) {
        Kind = DK_Else;
        Tag = "else";
      } else if (kw_at(q, eol, "endif")) {
        Kind = DK_Endif;
        Tag = "endif";
      }

      switch (Kind) {
      case DK_If:
      case DK_Ifdef:
      case DK_Ifndef: {
        // Start a new conditional group. This may be top-level or nested
        // inside another active group (tracked by `Stack`).
        CondGroup G;
        G.File = FilePath.str();
        G.GroupB = bol;      // group begins at the opener line
        G.GroupE = G.GroupB; // filled when we see matching #endif

        // Create the first arm for this group (#if/#ifdef/#ifndef).
        CondArm A;
        A.Kind = Tag.str();

        // Extract the condition text for #if/#ifdef/#ifndef.
        // For #ifdef/#ifndef this is just an identifier expression; we keep
        // the raw remainder of the line verbatim (minus leading spaces).
        size_t condBeg =
            q + (Kind == DK_If ? 2
                               : (Kind == DK_Ifdef
                                      ? 5
                                      : 6)); // lengths: "if", "ifdef", "ifndef"
        while (condBeg < eol && isSpace</*kWithCR=*/true>(Buf[condBeg]))
          ++condBeg;
        A.Cond = std::string(Buf.substr(condBeg, eol - condBeg));

        // The arm body begins immediately after this directive line.
        // We use `eol` (not `eol+1`) so that the newline remains part of the
        // body depending on downstream reconstruction policy.
        A.BodyB = std::min(N, eol);
        A.BodyE = A.BodyB;
        G.Arms.push_back(std::move(A));

        // Record the group and push it onto the nesting stack.
        size_t idx = Groups.size();
        Groups.push_back(std::move(G));
        Stack.push_back(Active{idx});

        // Advance to the end of this line.
        p = std::min(N, eol);
        continue;
      }

      case DK_Elif:
      case DK_Else: {
        // Transition to a new arm of the current innermost group.
        // If there is no active group, this is a stray directive and we ignore
        // it.
        if (Stack.empty()) {
          p = std::min(N, eol);
          continue;
        }

        size_t idx = Stack.back().GroupIndex;
        CondGroup &G = Groups[idx];

        // Close previous arm at the start of this directive line.
        setPrevBodyEnd(idx, bol);

        // Start the new arm.
        CondArm A;
        A.Kind = Tag.str();
        if (Kind == DK_Elif) {
          // Capture the raw condition expression following "elif".
          size_t condBeg = q + 4; // "elif"
          while (condBeg < eol && isSpace</*kWithCR=*/true>(Buf[condBeg]))
            ++condBeg;
          A.Cond = std::string(Buf.substr(condBeg, eol - condBeg));
        } else {
          // "else" has no condition text.
          A.Cond.clear();
        }

        // Arm body begins immediately after this directive line.
        A.BodyB = std::min(N, eol);
        A.BodyE = A.BodyB;
        G.Arms.push_back(std::move(A));

        p = std::min(N, eol);
        continue;
      }

      case DK_Endif: {
        // Close the current innermost group.
        if (Stack.empty()) {
          p = std::min(N, eol);
          continue; // stray endif
        }

        size_t idx = Stack.back().GroupIndex;
        CondGroup &G = Groups[idx];

        // Close the final arm at the start of this '#endif' line.
        setPrevBodyEnd(idx, bol);

        // Group extent: we record through the end of the '#endif' line.
        // This allows downstream logic to treat the group as spanning the
        // directives themselves, not just the arm bodies.
        G.GroupE = std::min(N, eol);

        Stack.pop_back();

        p = std::min(N, eol);
        continue;
      }

      case DK_None:
        // Not a conditional directive we care about; treat it like a normal
        // line.
        break;
      }
    }

    // Not a recognized directive line; advance to end-of-line.
    p = std::min(N, eol);
  }

  // If the file ends without closing some groups, conservatively close them at
  // EOF. This preserves best-effort structural information even for malformed
  // files.
  for (const auto &A : Stack) {
    size_t idx = A.GroupIndex;
    if (idx >= Groups.size())
      continue;
    CondGroup &G = Groups[idx];
    setPrevBodyEnd(idx, N);
    if (G.GroupE < G.GroupB || G.GroupE > N)
      G.GroupE = N;
  }

  return Groups;
}

// Compute, for each *formal* parameter of a function-like macro invocation,
// the byte offset range in the *spelled* source file that corresponds to the
// *unexpanded* argument tokens at the call site.
//
// Output format:
//   Out[i] = {begin, end} where:
//     - begin is the file offset (in bytes) of the first token of argument i
//     - end   is the file offset (in bytes) immediately after the last token
//             of argument i (i.e., a half-open range [begin, end))
//
// We use optional offsets because:
//   * some arguments may be empty / missing / not representable as file offsets,
//   * some tokens may have invalid locations (e.g., synthesized tokens),
//   * some invocations may involve macro expansions where a "file offset" is
//     not meaningful without first mapping through SourceManager.
//
// Important: this routine deliberately uses Args->getUnexpArgument(ai), i.e.
// the argument token sequence as *spelled* at the invocation site, not the
// post-expansion stream. This is the form needed to refold edits back into
// the original source call text.
static std::string computeLangStr(const clang::LangOptions &Lang) {
  // Objective-C family
  if (Lang.ObjC)
    return Lang.CPlusPlus ? "objc++" : "objc";

  // CUDA/HIP/OpenCL (optional, but harmless to keep)
  if (Lang.CUDA)
    return "cuda";
  if (Lang.HIP)
    return "hip";
  if (Lang.OpenCL)
    return "cl";

  // C++
  if (Lang.CPlusPlus)
    return "c++";

  // Default
  return "c";
}

static void trimTrailingSeparators(llvm::SmallVectorImpl<char> &P) {
  while (!P.empty() && llvm::sys::path::is_separator(P.back()))
    P.pop_back();
}

static std::string normalizePathKey(llvm::StringRef Path, llvm::StringRef Cwd,
                                    bool IsDir) {
  if (Path.empty())
    return std::string();

  llvm::SmallString<256> P(Path);

  // Make absolute using Cwd when provided; otherwise use process cwd.
  if (llvm::sys::path::is_relative(P)) {
    if (!Cwd.empty()) {
      llvm::SmallString<256> Abs(Cwd);
      llvm::sys::path::append(Abs, P);
      P = Abs;
    } else {
      llvm::sys::fs::make_absolute(P);
    }
  }

  // Normalize `.` / `..`
  llvm::sys::path::remove_dots(P, /*remove_dot_dot=*/true);

  // For directory keys, remove trailing separator to canonicalize.
  if (IsDir)
    trimTrailingSeparators(P);

  // Prefer real_path when it exists (resolves symlinks); ignore errors.
  llvm::SmallString<256> RP;
  if (!llvm::sys::fs::real_path(P, RP)) {
    if (IsDir)
      trimTrailingSeparators(RP);
    return RP.str().str();
  }

  return P.str().str();
}

std::string joinSpelled(llvm::StringRef DirSpelling, llvm::StringRef Rel) {
  if (Rel.empty())
    return std::string();

  // If Rel is already absolute, keep it.
  if (llvm::sys::path::is_absolute(Rel))
    return Rel.str();

  if (DirSpelling.empty())
    return Rel.str();

  std::string Out = DirSpelling.str();

  // Preserve the include-dir spelling; just join with '/' if needed.
  char last = Out.empty() ? '\0' : Out.back();
  if (last != '/' && last != '\\')
    Out.push_back('/');

  Out.append(Rel.data(), Rel.size());
  return Out;
}

// Sanity-check helper: ensure any recorded invocation-argument byte ranges
// lie within the enclosing invocation span [InvBegin, InvEnd).
//
// This is a defensive validation step for producer-side metadata: it catches
// cases where argument offsets are missing, inverted, or escape the invocation
// region due to location mapping quirks (macro expansion, CRLF, etc.).
//
// Parse a macro invocation's spelled text and compute byte-offset ranges for
// each argument within that invocation.
//
// Inputs:
//   - InvText:      The full spelled invocation text (e.g. "FOO(a, b+1)").
//                   This is assumed to include the opening '(' and the matching
//                   ')'.
//   - InvBegin:     Absolute byte offset in the file where InvText begins.
//                   We add relative offsets within InvText to produce absolute
//                   ranges.
//   - ExpectedArgs: The number of arguments we expect to find (usually the
//                   number of formal parameters for the macro).
//
// Output:
//   - Out[i] = {begin, end} absolute byte offsets into the file for argument i,
//     using half-open ranges [begin, end). Missing/unknown args remain nullopt.
//
// Return value:
//   - true if we find a plausible top-level argument list that ends at a
//     matching ')' for the first '(' in InvText; false otherwise.
static bool computeInvArgRangesFromText(
    llvm::StringRef InvText, uint64_t InvBegin, size_t ExpectedArgs,
    const LangOptions &Lang,
    std::vector<std::pair<std::optional<uint64_t>, std::optional<uint64_t>>>
        &Out) {
  // Preserve the caller's current Out state on failure. (Callers may have
  // pre-sized Out and rely on it retaining its shape when parsing fails.)
  std::vector<std::pair<std::optional<uint64_t>, std::optional<uint64_t>>> Args;
  Args.reserve(ExpectedArgs);

  // Tokenize the *raw* invocation text with Clang's lexer. This ensures we
  // treat comments as whitespace and do not accidentally split on commas/parens
  // that appear inside comments, string/char literals, raw strings, etc.
  //
  // Offsets remain relative to InvText; we add InvBegin to form byte offsets
  // within the original file (inv_text semantics).
  const SourceLocation BaseLoc = SourceLocation::getFromRawEncoding(1);
  std::string LexBuf = InvText.str();
  LexBuf.push_back('\0');
  const char *BufStart = LexBuf.data();
  const char *BufEnd = BufStart + InvText.size();
  Lexer Lex(BaseLoc, Lang, BufStart, BufStart, BufEnd);

  auto tokOff = [&](const Token &Tok) -> size_t {
    return static_cast<size_t>(Tok.getLocation().getRawEncoding() -
                               BaseLoc.getRawEncoding());
  };

  auto recordArg = [&](size_t A0, size_t A1) {
    while (A0 < A1 &&
           std::isspace(static_cast<unsigned char>(InvText[A0]))) {
      ++A0;
    }
    while (A1 > A0 &&
           std::isspace(static_cast<unsigned char>(InvText[A1 - 1]))) {
      --A1;
    }
    Args.push_back({InvBegin + A0, InvBegin + A1});
  };

  Token Tok;
  bool SawLParen = false;
  size_t ArgStart = 0;

  unsigned ParenDepth = 0;
  unsigned BracketDepth = 0;
  unsigned BraceDepth = 0;

  // For 0-parameter function-like macros, accept invocations that have no
  // tokens between '(' and ')'. (Comments are lexed as whitespace unless
  // explicitly retained, so FOO(/*c*/) behaves like FOO().)
  bool SawAnyTokenBetweenParens = false;

  while (true) {
    Lex.LexFromRawLexer(Tok);
    if (Tok.is(tok::eof))
      break;

    if (!SawLParen) {
      if (Tok.is(tok::l_paren)) {
        SawLParen = true;
        ArgStart = tokOff(Tok) + Tok.getLength();
      }
      continue;
    }

    if (Tok.is(tok::comment))
      continue;

    const size_t Off = tokOff(Tok);

    if (Tok.is(tok::l_paren)) {
      ++ParenDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }
    if (Tok.is(tok::r_paren)) {
      if (ParenDepth == 0 && BracketDepth == 0 && BraceDepth == 0) {
        if (ExpectedArgs == 0) {
          if (!SawAnyTokenBetweenParens) {
            Out = std::move(Args);
            return true;
          }
          return false;
        }
        recordArg(ArgStart, Off);

        // Best-effort: if the parsed argument count does not match the macro's
        // formal parameter count, keep what we could parse and leave remaining
        // formals as null. This preserves the historical behavior for macro
        // dispatcher patterns like: (A,B,C,0)(__VA_ARGS__).
        std::vector<std::pair<std::optional<uint64_t>, std::optional<uint64_t>>>
            OutTmp;
        OutTmp.resize(ExpectedArgs, {std::nullopt, std::nullopt});

        const size_t Fill = std::min(Args.size(), ExpectedArgs);
        for (size_t I = 0; I < Fill; ++I)
          OutTmp[I] = Args[I];

        if (Args.size() > ExpectedArgs && ExpectedArgs > 0) {
          OutTmp[ExpectedArgs - 1] = {Args[ExpectedArgs - 1].first,
                                      Args.back().second};
        }

        Out = std::move(OutTmp);
        return true;
      }
      if (ParenDepth > 0)
        --ParenDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }

    if (Tok.is(tok::l_square)) {
      ++BracketDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }
    if (Tok.is(tok::r_square)) {
      if (BracketDepth > 0)
        --BracketDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }

    if (Tok.is(tok::l_brace)) {
      ++BraceDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }
    if (Tok.is(tok::r_brace)) {
      if (BraceDepth > 0)
        --BraceDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }

    if (Tok.is(tok::comma) && ParenDepth == 0 && BracketDepth == 0 &&
        BraceDepth == 0) {
      recordArg(ArgStart, Off);
      ArgStart = Off + Tok.getLength();
      continue;
    }

    SawAnyTokenBetweenParens = true;
  }

  return false;
}

// Parse a parenthesized tuple/signature text such as "(1, (4 + 1))" into
// per-element byte ranges relative to the provided text. The returned ranges
// exclude the outer parentheses and trim surrounding whitespace on each
// element. Nested parentheses/brackets/braces, string literals, and comments
// are handled by the raw lexer in the same way as computeInvArgRangesFromText.
static bool computeTupleElementRangesFromText(
    llvm::StringRef Text, const LangOptions &Lang,
    std::vector<std::pair<std::optional<uint32_t>, std::optional<uint32_t>>>
        &Out) {
  Out.clear();

  const SourceLocation BaseLoc = SourceLocation::getFromRawEncoding(1);
  std::string LexBuf = Text.str();
  LexBuf.push_back('\0');
  const char *BufStart = LexBuf.data();
  const char *BufEnd = BufStart + Text.size();
  Lexer Lex(BaseLoc, Lang, BufStart, BufStart, BufEnd);

  auto tokOff = [&](const Token &Tok) -> size_t {
    return static_cast<size_t>(Tok.getLocation().getRawEncoding() -
                               BaseLoc.getRawEncoding());
  };

  auto recordElem = [&](size_t B, size_t E) {
    while (B < E && std::isspace(static_cast<unsigned char>(Text[B])))
      ++B;
    while (E > B && std::isspace(static_cast<unsigned char>(Text[E - 1])))
      --E;
    Out.push_back({static_cast<uint32_t>(B), static_cast<uint32_t>(E)});
  };

  Token Tok;
  bool SawLParen = false;
  size_t ElemStart = 0;
  unsigned ParenDepth = 0;
  unsigned BracketDepth = 0;
  unsigned BraceDepth = 0;
  bool SawAnyTokenBetweenParens = false;

  while (true) {
    Lex.LexFromRawLexer(Tok);
    if (Tok.is(tok::eof))
      break;

    if (!SawLParen) {
      if (Tok.is(tok::l_paren)) {
        SawLParen = true;
        ElemStart = tokOff(Tok) + Tok.getLength();
      } else if (Tok.isNot(tok::comment)) {
        return false;
      }
      continue;
    }

    if (Tok.is(tok::comment))
      continue;

    const size_t Off = tokOff(Tok);

    if (Tok.is(tok::l_paren)) {
      ++ParenDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }
    if (Tok.is(tok::r_paren)) {
      if (ParenDepth == 0 && BracketDepth == 0 && BraceDepth == 0) {
        if (!SawAnyTokenBetweenParens) {
          Out.clear();
          return true;
        }
        recordElem(ElemStart, Off);
        return true;
      }
      if (ParenDepth > 0)
        --ParenDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }

    if (Tok.is(tok::l_square)) {
      ++BracketDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }
    if (Tok.is(tok::r_square)) {
      if (BracketDepth > 0)
        --BracketDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }

    if (Tok.is(tok::l_brace)) {
      ++BraceDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }
    if (Tok.is(tok::r_brace)) {
      if (BraceDepth > 0)
        --BraceDepth;
      SawAnyTokenBetweenParens = true;
      continue;
    }

    if (Tok.is(tok::comma) && ParenDepth == 0 && BracketDepth == 0 &&
        BraceDepth == 0) {
      recordElem(ElemStart, Off);
      ElemStart = Off + Tok.getLength();
      continue;
    }

    SawAnyTokenBetweenParens = true;
  }

  return false;
}

static std::optional<llvm::StringRef>
getItemInvocationArgText(const Item &It, uint32_t ArgIdx) {
  if (ArgIdx >= It.InvArgRanges.size() || !It.InvBegin)
    return std::nullopt;
  const auto &Rng = It.InvArgRanges[ArgIdx];
  if (!Rng.first || !Rng.second || *Rng.second < *Rng.first ||
      *Rng.first < *It.InvBegin)
    return std::nullopt;
  const uint64_t RelB = *Rng.first - *It.InvBegin;
  const uint64_t RelE = *Rng.second - *It.InvBegin;
  if (RelE < RelB || RelE > It.InvText.size())
    return std::nullopt;
  return llvm::StringRef(It.InvText).slice(static_cast<size_t>(RelB),
                                           static_cast<size_t>(RelE)).trim();
}

static std::optional<std::string>
getUnexpandedMacroArgText(const MacroArgs *Args, unsigned ArgIndex,
                          const SourceManager &SM, const LangOptions &Lang) {
  if (!Args)
    return std::nullopt;
  const Token *AT = Args->getUnexpArgument(ArgIndex);
  if (!AT)
    return std::nullopt;
  if (AT->is(tok::eof))
    return std::string();

  const Token *First = AT;
  const Token *Last = AT;
  for (const Token *T = AT; !T->is(tok::eof); ++T)
    Last = T;

  SourceLocation B = SM.getFileLoc(SM.getSpellingLoc(First->getLocation()));
  SourceLocation ESp = SM.getSpellingLoc(Last->getLocation());
  SourceLocation E = Lexer::getLocForEndOfToken(ESp, 0, SM, Lang);
  if (!B.isValid() || !E.isValid() || !SM.isWrittenInSameFile(B, E))
    return std::nullopt;

  return Lexer::getSourceText(CharSourceRange::getCharRange(B, E), SM, Lang)
      .str();
}

static void buildSyntheticFunctionLikeInvocationText(
    llvm::StringRef Name, llvm::ArrayRef<llvm::StringRef> ArgTexts,
    std::string &Out,
    std::vector<std::pair<std::optional<uint32_t>, std::optional<uint32_t>>>
        &ArgRanges) {
  Out.clear();
  ArgRanges.clear();

  Out.append(Name.data(), Name.size());
  Out.push_back('(');
  for (size_t I = 0; I < ArgTexts.size(); ++I) {
    if (I)
      Out.append(", ");
    const uint32_t B = static_cast<uint32_t>(Out.size());
    Out.append(ArgTexts[I].data(), ArgTexts[I].size());
    const uint32_t E = static_cast<uint32_t>(Out.size());
    ArgRanges.push_back({B, E});
  }
  Out.push_back(')');
}

struct DecodedPayloadMap {
  std::string Decoded;
  llvm::SmallVector<std::pair<uint32_t, uint32_t>, 64> PayloadToSpelling;
  bool Valid = false;
};

static inline void appendUtf8(uint32_t CP, std::string &Out) {
  // Encode a Unicode scalar value as UTF-8. Invalid scalars are encoded
  // as U+FFFD.
  if (CP > 0x10FFFFu || (CP >= 0xD800u && CP <= 0xDFFFu))
    CP = 0xFFFDu;

  if (CP <= 0x7Fu) {
    Out.push_back((char)CP);
    return;
  }
  if (CP <= 0x7FFu) {
    Out.push_back((char)(0xC0u | ((CP >> 6) & 0x1Fu)));
    Out.push_back((char)(0x80u | (CP & 0x3Fu)));
    return;
  }
  if (CP <= 0xFFFFu) {
    Out.push_back((char)(0xE0u | ((CP >> 12) & 0x0Fu)));
    Out.push_back((char)(0x80u | ((CP >> 6) & 0x3Fu)));
    Out.push_back((char)(0x80u | (CP & 0x3Fu)));
    return;
  }
  Out.push_back((char)(0xF0u | ((CP >> 18) & 0x07u)));
  Out.push_back((char)(0x80u | ((CP >> 12) & 0x3Fu)));
  Out.push_back((char)(0x80u | ((CP >> 6) & 0x3Fu)));
  Out.push_back((char)(0x80u | (CP & 0x3Fu)));
}

static inline bool isIdentByte(unsigned char C) {
  return (C == '_') || llvm::isAlnum(static_cast<char>(C));
}

// Decode the payload of a *single* string literal token spelling into a byte
// sequence, while also recording a mapping from each decoded byte back to the
// corresponding byte range in the original token spelling.
//
// This is intended for producer-side "paste-through-stringify" projection:
// mapping pasted-token spellings embedded inside a string literal (emitted by
// '#') back to the paste-producer macro invocation(s).
static DecodedPayloadMap decodeStringLiteralPayload(llvm::StringRef Spelling) {
  DecodedPayloadMap R;

  auto pushMappedByte = [&](char B, uint32_t SpellBegin, uint32_t SpellEnd) {
    R.Decoded.push_back(B);
    R.PayloadToSpelling.emplace_back(SpellBegin, SpellEnd);
  };

  auto pushMappedUtf8 = [&](uint32_t CP, uint32_t SpellBegin,
                            uint32_t SpellEnd) {
    size_t before = R.Decoded.size();
    appendUtf8(CP, R.Decoded);
    size_t after = R.Decoded.size();
    for (size_t i = before; i < after; ++i)
      R.PayloadToSpelling.emplace_back(SpellBegin, SpellEnd);
  };

  const size_t N = Spelling.size();
  if (N < 2)
    return R;

  size_t I = 0;

  // Optional prefix: u8, u, U, L.
  if (Spelling.starts_with("u8")) {
    I = 2;
  } else if (Spelling.starts_with("u") || Spelling.starts_with("U") ||
             Spelling.starts_with("L")) {
    I = 1;
  }

  // Raw string literal: (prefix)? R"delim(... )delim"
  if (I + 2 < N && Spelling[I] == 'R' && Spelling[I + 1] == '"') {
    size_t DelimBegin = I + 2;
    size_t OpenParen = Spelling.find("(", DelimBegin);
    if (OpenParen == llvm::StringRef::npos)
      return R;
    llvm::StringRef Delim = Spelling.slice(DelimBegin, OpenParen);
    llvm::SmallString<32> Closing;
    Closing.append(")");
    Closing.append(Delim);
    Closing.append("\"");
    size_t ClosePos = Spelling.find(Closing, OpenParen + 1);
    if (ClosePos == llvm::StringRef::npos)
      return R;

    size_t ContentBegin = OpenParen + 1;
    size_t ContentEnd = ClosePos;

    for (size_t P = ContentBegin; P < ContentEnd; ++P)
      pushMappedByte(Spelling[P], (uint32_t)P, (uint32_t)(P + 1));

    R.Valid = true;
    return R;
  }

  if (I >= N || Spelling[I] != '"')
    return R;

  ++I; // consume opening quote

  for (;;) {
    if (I >= N)
      return R;
    if (Spelling[I] == '"') {
      R.Valid = true;
      return R;
    }

    if (Spelling[I] != '\\') {
      pushMappedByte(Spelling[I], (uint32_t)I, (uint32_t)(I + 1));
      ++I;
      continue;
    }

    size_t EscBegin = I;
    ++I;
    if (I >= N)
      return R;

    // Source-level line splice: backslash followed by a physical newline
    // (LF or CRLF). This contributes no decoded payload byte.
    if (Spelling[I] == '\n') {
      ++I;
      continue;
    }
    if (Spelling[I] == '\r' && I + 1 < N && Spelling[I + 1] == '\n') {
      I += 2;
      continue;
    }

    char C = Spelling[I];

    // Hex escape: \x[0-9A-Fa-f]+
    if (C == 'x') {
      ++I;
      if (I >= N || !llvm::isHexDigit(Spelling[I]))
        return R;

      uint32_t V = 0;
      while (I < N && llvm::isHexDigit(Spelling[I])) {
        V = (V << 4) + llvm::hexDigitValue(Spelling[I]);
        ++I;
      }

      pushMappedByte((char)(V & 0xFFu), (uint32_t)EscBegin, (uint32_t)I);
      continue;
    }

    // Octal escape: \[0-7]{1,3}
    if (C >= '0' && C <= '7') {
      uint32_t V = 0;
      size_t Digits = 0;
      while (I < N && Digits < 3 &&
             (Spelling[I] >= '0' && Spelling[I] <= '7')) {
        V = (V << 3) + (uint32_t)(Spelling[I] - '0');
        ++I;
        ++Digits;
      }
      pushMappedByte((char)(V & 0xFFu), (uint32_t)EscBegin, (uint32_t)I);
      continue;
    }

    // Universal character name: \uXXXX or \UXXXXXXXX.
    if (C == 'u' || C == 'U') {
      const size_t Needed = (C == 'u') ? 4 : 8;
      size_t StartDigits = I + 1;
      size_t EndDigits = StartDigits + Needed;
      if (EndDigits > N)
        return R;

      uint32_t CP = 0;
      for (size_t J = StartDigits; J < EndDigits; ++J) {
        if (!llvm::isHexDigit(Spelling[J]))
          return R;
        CP = (CP << 4) + llvm::hexDigitValue(Spelling[J]);
      }
      I = EndDigits;
      pushMappedUtf8(CP, (uint32_t)EscBegin, (uint32_t)I);
      continue;
    }

    // Simple escapes (after we've consumed the backslash, and C = Spelling[I]).
    const uint32_t SB = static_cast<uint32_t>(EscBegin);
    const uint32_t SE = static_cast<uint32_t>(I + 1);

    char Out = C;
    switch (C) {
    case 'n':
      Out = '\n';
      break;
    case 'r':
      Out = '\r';
      break;
    case 't':
      Out = '\t';
      break;
    case 'v':
      Out = '\v';
      break;
    case 'b':
      Out = '\b';
      break;
    case 'f':
      Out = '\f';
      break;
    case 'a':
      Out = '\a';
      break;
    default:
      // Includes: \\ \" \' \? and any unknown escape → treat as the escaped
      // character verbatim (i.e., output 'C').
      break;
    }

    pushMappedByte(Out, SB, SE);

    ++I;
  }
}

/// \brief Precompute invocation-specific projection metadata for `#` and `##`.
///
/// For one concrete function-like macro invocation, this routine computes the
/// producer-side lookup tables needed to attribute later-emitted macro-body
/// tokens back to invocation arguments when those tokens are *projected*
/// through:
///
///   - stringification: `#param`
///   - token pasting:   `a ## b`
///
/// The result is stored on \p It in transient, non-serialized fields that are
/// consumed by `onToken()`:
///
///   - `StringifySpell2ArgIndices` maps the exact emitted string-literal
///     spelling produced by `#param` to the formal argument index or indices
///     that could have produced it for this invocation.
///   - `PasteTokens` records each token synthesized by `##`, together with the
///     byte subranges within the pasted spelling that came from argument-sourced
///     input tokens.
///   - `PasteSpell2TokenIndices` is a reverse index from pasted token spelling
///     to entries in `PasteTokens`, used to match emitted tokens
///     deterministically in expansion order.
///
/// The computation is invocation-specific:
///
///   - formal parameters in the replacement list are substituted with the
///     *unexpanded* actual argument token spellings from \p Args,
///   - stringification uses Clang's own `MacroArgs::StringifyArgument()` so the
///     stored spelling matches preprocessor output byte-for-byte,
///   - `##` is evaluated left-to-right over the substituted token sequence, and
///     provenance from argument-derived pieces is merged into the synthesized
///     token.
///
/// Non-function-like macros, null `MacroInfo`, or missing `MacroArgs` produce
/// no projection metadata.
///
/// \param It           Destination item for the current macro invocation.
/// \param PP           Preprocessor used to obtain exact token spellings and
///                     Clang-consistent stringification results.
/// \param MacroNameTok Invocation-site macro name token; its location is used
///                     when forming Clang's stringification token.
/// \param MI           Definition-time `MacroInfo` for the invoked macro.
/// \param Args         Invocation-specific actual arguments in unexpanded form.
/// \param Lang         Language options (currently unused here; kept for API
///                     symmetry with nearby helpers).
void computeMacroProjectionSites(Item &It, Preprocessor &PP,
                                 const Token &MacroNameTok, const MacroInfo *MI,
                                 const MacroArgs *Args,
                                 const LangOptions &Lang) {
  // Producer-side metadata for consumer projection through:
  //   - stringification sites:  #param
  //   - token pasting sites:    a ## b
  It.StringifySpell2ArgIndices.clear();
  It.PasteTokens.clear();
  It.PasteSpell2TokenIndices.clear();
  It.PasteTokenCursor = 0;

  if (!MI || !MI->isFunctionLike() || !Args)
    return;

  (void)Lang;

  bool HasStringify = false, HasHashHash = false;
  {
    const auto &RToks = MI->tokens();
    for (size_t i = 0, N = RToks.size(); i < N; ++i) {
      HasHashHash |= RToks[i].is(tok::hashhash);
      if (!HasStringify && RToks[i].is(tok::hash) && i + 1 < N &&
          RToks[i + 1].is(tok::identifier)) {
        const IdentifierInfo *II = RToks[i + 1].getIdentifierInfo();
        HasStringify = (II && MI->getParameterNum(II) >= 0);
      }
      if (HasStringify && HasHashHash)
        break;
    }
  }

  auto paramIndex = [&](const Token &T) -> int {
    if (!T.is(tok::identifier))
      return -1;
    if (const IdentifierInfo *II = T.getIdentifierInfo())
      return MI->getParameterNum(II);
    return -1;
  };

  // -----------------------------
  // Stringification: #param
  // -----------------------------
  if (HasStringify) {
    const SourceLocation Loc = MacroNameTok.getLocation();
    const auto &RT = MI->tokens();
    for (size_t i = 0, N = RT.size(); i + 1 < N; ++i) {
      if (!RT[i].is(tok::hash))
        continue;
      int PIdx = paramIndex(RT[i + 1]);
      if (PIdx < 0)
        continue;

      // Robust: rely on Clang's own macro stringification so the spelled token
      // matches the preprocessor output byte-for-byte.
      std::string Quoted = "\"\"";
      if (const Token *AT =
              Args->getUnexpArgument(static_cast<unsigned>(PIdx))) {
        Token StrTok = MacroArgs::StringifyArgument(AT, PP, /*Charify=*/false,
                                                    /*ExpansionLocStart=*/Loc,
                                                    /*ExpansionLocEnd=*/Loc);
        Quoted = PP.getSpelling(StrTok);
      }

      It.StringifySpell2ArgIndices[llvm::StringRef(Quoted)].push_back(
          static_cast<unsigned>(PIdx));
    }
  }

  // -----------------------------
  // Token pasting: a ## b
  // -----------------------------
  if (!HasHashHash)
    return;

  struct SubstTok {
    bool IsHashHash = false;
    bool IsPasteResult = false;
    std::string Text;
    SmallVector<PastePart, 4> Parts; // only arg-sourced parts are tracked
  };

  SmallVector<SubstTok, 64> Seq;

  auto pushTok = [&](std::string Text, std::optional<unsigned> ArgIdx) {
    SubstTok S;
    S.Text = std::move(Text);
    if (ArgIdx && !S.Text.empty()) {
      PastePart P;
      P.ArgIndex = static_cast<uint32_t>(*ArgIdx);
      P.ByteBegin = 0;
      P.ByteEnd = static_cast<uint32_t>(S.Text.size());
      S.Parts.push_back(P);
    }
    Seq.push_back(std::move(S));
  };

  // Substitute params with unexpanded argument token spellings.
  for (const Token &RTok : MI->tokens()) {
    if (RTok.is(tok::hashhash)) {
      SubstTok Op;
      Op.IsHashHash = true;
      Seq.push_back(std::move(Op));
      continue;
    }

    int PIdx = paramIndex(RTok);
    if (PIdx >= 0) {
      if (const Token *AT =
              Args->getUnexpArgument(static_cast<unsigned>(PIdx))) {
        for (; !AT->is(tok::eof); ++AT)
          pushTok(PP.getSpelling(*AT), static_cast<unsigned>(PIdx));
      }
      continue;
    }

    pushTok(PP.getSpelling(RTok), std::nullopt);
  }

  // Evaluate ## left-to-right.
  for (unsigned i = 0; i < Seq.size();) {
    if (!Seq[i].IsHashHash) {
      ++i;
      continue;
    }

    if (i == 0 || i + 1 >= Seq.size() || Seq[i - 1].IsHashHash ||
        Seq[i + 1].IsHashHash) {
      ++i;
      continue;
    }

    SubstTok &L = Seq[i - 1];
    SubstTok &R = Seq[i + 1];

    SubstTok M;
    M.IsPasteResult = true;
    M.Text.reserve(L.Text.size() + R.Text.size());
    M.Text.append(L.Text.data(), L.Text.size());
    M.Text.append(R.Text.data(), R.Text.size());

    M.Parts = L.Parts;
    const uint32_t Shift = static_cast<uint32_t>(L.Text.size());
    for (PastePart P : R.Parts) {
      P.ByteBegin += Shift;
      P.ByteEnd += Shift;
      M.Parts.push_back(P);
    }

    Seq[i - 1] = std::move(M);
    Seq.erase(Seq.begin() + i, Seq.begin() + i + 2);
    if (i > 0)
      --i;
  }

  // Record paste results with arg provenance.
  for (const SubstTok &S : Seq) {
    if (!S.IsPasteResult || S.Parts.empty())
      continue;

    PasteToken PT;
    PT.Spelling = S.Text;
    PT.Parts = S.Parts;

    const size_t Index = It.PasteTokens.size();
    It.PasteTokens.push_back(std::move(PT));
    It.PasteSpell2TokenIndices[It.PasteTokens.back().Spelling].push_back(Index);
  }
}
} // namespace

RefoldMapBuilder::RefoldMapBuilder(Preprocessor &PP, llvm::StringRef OutputPath,
                                   bool EnableByteSpans)
    : PP(PP), SM(PP.getSourceManager()), Lang(PP.getLangOpts()),
      OutPath(OutputPath.str()), EnableByteSpans(EnableByteSpans) {
  const auto &PPO = PP.getPreprocessorOpts();

  // Prefer the driver-provided cwd spelling when available; fallback to process
  // cwd.
  Cwd = PPO.RefoldWorkingDir;
  llvm::SmallString<256> WD;
  if (!llvm::sys::fs::current_path(WD)) {
    Cwd = WD.str().str();
  } else {
    Cwd = ".";
  }

  EmitAbsPaths = false; // Prefer spellings; the consumer can resolve via cwd.

  // Parse include search spellings from the driver argv so we can reconstruct
  // include paths relative to the *spelled* `-I` entries.
  auto addIncludeDirSpelling = [&](llvm::StringRef DirSpelling) {
    if (DirSpelling.empty())
      return;

    std::string AbsKey = normalizePathKey(DirSpelling, Cwd, /*IsDir=*/true);
    if (!AbsKey.empty() &&
        IncludeDirAbs2Spelling.find(AbsKey) == IncludeDirAbs2Spelling.end()) {
      IncludeDirAbs2Spelling[AbsKey] = DirSpelling.str();
    }
  };

  for (size_t i = 0; i < PPO.RefoldPPArgv.size(); ++i) {
    llvm::StringRef A(PPO.RefoldPPArgv[i]);

    // Separate include-dir form: `-I <dir>`. Consume the following argv element
    // as the spelled directory and record only that directory text.
    if (A == "-I") {
      if (i + 1 < PPO.RefoldPPArgv.size())
        addIncludeDirSpelling(PPO.RefoldPPArgv[++i]);
      continue;
    }

    // Joined include-dir form: `-I<dir>`. Strip the `-I` prefix and record the
    // remaining spelling as the include-search directory text.
    if (A.starts_with("-I") && A.size() > 2) {
      addIncludeDirSpelling(A.drop_front(2));
      continue;
    }
  }

  // Resolve the TU path spelling from the driver argv. Prefer the exact argv
  // spelling when it is already absolute; otherwise recover a non-absolute argv
  // spelling only when it canonically resolves to the main file path.
  OptionalFileEntryRef MainFER = SM.getFileEntryRefForID(SM.getMainFileID());
  std::string MainAbs;
  if (MainFER)
    MainAbs = absolutePathFor(*MainFER);

  // 1. Try to find the exact absolute-path spelling in argv first.
  for (llvm::StringRef Arg : PPO.RefoldPPArgv) {
    if (Arg == MainAbs) {
      TUSourcePath = Arg.str();
      break;
    }
  }

  // 2. If there was no exact absolute-path match, accept a spelled argv path
  // only when it canonically resolves to the same main-file path. Require a
  // unique match here; otherwise fail closed to MainAbs instead of guessing.
  if (TUSourcePath.empty() && !MainAbs.empty()) {
    std::optional<llvm::StringRef> CanonicalMatch;
    bool Ambiguous = false;
    for (llvm::StringRef Arg : PPO.RefoldPPArgv) {
      if (Arg.empty() || Arg.starts_with("-"))
        continue;

      if (normalizePathKey(Arg, Cwd, /*IsDir=*/false) != MainAbs)
        continue;

      if (!CanonicalMatch) {
        CanonicalMatch = Arg;
        continue;
      }

      if (*CanonicalMatch != Arg) {
        Ambiguous = true;
        break;
      }
    }

    if (CanonicalMatch && !Ambiguous)
      TUSourcePath = CanonicalMatch->str();
  }

  // 3. Final safety net: use the canonical absolute main-file path.
  if (TUSourcePath.empty())
    TUSourcePath = MainAbs;

  if (TUSourcePath.empty()) {
    llvm::report_fatal_error("Critical Error: TU source path is empty.");
  }

  // Seed the spelling map so token locations in the main file report the TU
  // path spelling rather than an absolute canonical path.
  if (!MainAbs.empty())
    FileAbs2Spelling[MainAbs] = TUSourcePath;

  IgnoreComments = true;
}

// Return the byte span [begin,end) in the *file buffer* that covers the entire
// physical source line containing `HashLoc` (typically the '#' of a directive),
// including the line-ending bytes (LF or CRLF) when present.
//
// Notes:
//  - We normalize to a file location (`getFileLoc`) so macro expansions map
//    back to a concrete FileID + offset.
//  - Offsets are *byte offsets* into the underlying file buffer returned by
//    SourceManager, which is exactly what the refold map schema wants for
//    site_begin/site_end.
std::optional<std::pair<uint64_t, uint64_t>>
RefoldMapBuilder::computeDirectiveLine(SourceLocation HashLoc) {
  // Convert to a file spelling location (not a macro expansion location).
  SourceLocation H = SM.getFileLoc(HashLoc);
  if (!H.isValid())
    return std::nullopt;

  FileID FID = SM.getFileID(H);

  // Fetch the entire file buffer for this FileID. If SourceManager can’t
  // provide it (e.g. invalid buffer), bail.
  bool Invalid = false;
  StringRef Buf = SM.getBufferData(FID, &Invalid);
  if (Invalid)
    return std::nullopt;

  // `B` is the byte offset of the directive hash within the file buffer.
  const size_t B = SM.getFileOffset(H);
  const size_t N = Buf.size();

  // Scan forward to find the end-of-line for the physical line containing `B`.
  // We stop at either '\n' or '\r' so we can handle both LF and CRLF.
  size_t P = B;
  while (P < N && Buf[P] != '\n' && Buf[P] != '\r')
    ++P;

  // Default end is the byte position of the line break (or EOF if none).
  size_t E = P;

  // If we stopped on a line break, include it in the returned span so the
  // caller can slice the whole directive line including its terminator.
  if (P < N) {
    // Windows-style CRLF: include both bytes.
    if (Buf[P] == '\r' && P + 1 < N && Buf[P + 1] == '\n')
      E = P + 2;
    else
      E = P + 1; // LF or bare CR
  }

  return {{B, E}};
}

std::string RefoldMapBuilder::absolutePathFor(const clang::FileEntryRef &FER) {
  if (auto RP = FER.getFileEntry().tryGetRealPathName(); !RP.empty())
    return RP.str();

  llvm::SmallString<256> P(FER.getName());

  // Pseudo paths
  if (!P.empty() && P.front() == '<')
    return P.str().str();

  // Try to resolve via host FS; if it fails (VFS), proceed with abs+normalize
  llvm::SmallString<256> Real;
  if (!llvm::sys::fs::real_path(P, Real))
    P = Real;

  if (llvm::sys::path::is_relative(P))
    llvm::sys::fs::make_absolute(P);

  llvm::sys::path::remove_dots(P, /*remove_dot_dot=*/true);
  return P.str().str();
}

std::string RefoldMapBuilder::filePathForLocAbs(clang::SourceManager &SM,
                                                clang::SourceLocation L,
                                                bool WantAbs) {
  if (!L.isValid())
    return std::string();

  if (SM.isWrittenInBuiltinFile(L))
    return "<built-in>";
  if (SM.isWrittenInCommandLineFile(L))
    return "<command-line>";

  clang::FileID FID = SM.getFileID(L);
  if (auto FER = SM.getFileEntryRefForID(FID)) {
    const std::string Abs = absolutePathFor(*FER);
    if (WantAbs)
      return Abs;

    if (auto It = FileAbs2Spelling.find(Abs); It != FileAbs2Spelling.end())
      return It->second;

    // Fallback: use whatever name the file manager recorded.
    return std::string(FER->getName());
  }
  return std::string();
}

std::optional<uint32_t> RefoldMapBuilder::argIndexForSpellingLoc(
    const Item &MI, SourceLocation Loc, SourceManager &Sm,
    const LangOptions &Lang, bool EmitAbsPaths) {
  // Goal:
  //   Given a token location (typically the spelling loc for a token that came
  //   out of a macro expansion), determine which *invocation-site argument
  //   slot* of macro invocation item MI produced that token.
  //
  // How:
  //   MI.InvFile names the physical file that contains the macro invocation
  //   text, and MI.InvArgRanges stores byte ranges (in that file) for each
  //   argument as written at the call site. We try to map the token's location
  //   back to a physical file location in MI.InvFile and then find the first
  //   argument range whose byte interval overlaps the token's byte interval.
  //
  // Return:
  //   * index of the argument (0-based) if we can prove the token originated
  //     from that argument at MI's call site
  //   * -1 otherwise
  if (Loc.isInvalid() || MI.InvFile.empty() || MI.InvArgRanges.empty())
    return std::nullopt;

  SourceLocation CurrentLoc = Loc;
  SourceLocation LastLoc; // Track the previous location to detect cycles

  // We primarily terminate via:
  //   (a) finding a matching invocation-file + overlapping byte range, or
  //   (b) leaving macro locations / hitting invalid loc / detecting a cycle.
  // The depth cap is just a safety net.
  for (unsigned Depth = 0; Depth < 100; ++Depth) {
    if (CurrentLoc.isInvalid() || CurrentLoc == LastLoc)
      break;

    LastLoc = CurrentLoc;

    // Step 1: Try to interpret the current location as a *physical file
    // location* and see if it is inside the macro invocation file (MI.InvFile).
    // If so, compute the token's byte span and test it against the recorded
    // invocation argument ranges.
    //
    // Note: if CurrentLoc is a MacroID, Sm.getFileLoc(CurrentLoc) collapses
    // through macro layers to a file location; otherwise it is already a file
    // location.
    SourceLocation Fl =
        CurrentLoc.isMacroID() ? Sm.getFileLoc(CurrentLoc) : CurrentLoc;

    std::string TokFile = filePathForLocAbs(Sm, Fl, EmitAbsPaths);
    if (TokFile == MI.InvFile) {
      auto TokB = Sm.getFileOffset(Fl);
      SourceLocation EndL = Lexer::getLocForEndOfToken(Fl, 0, Sm, Lang);
      auto TokE = EndL.isValid() ? Sm.getFileOffset(EndL) : TokB;
      if (TokE < TokB) TokE = TokB;

      // The argument ranges are stored as byte intervals in the invocation
      // file. If this token overlaps any argument interval, we attribute it to
      // that argument slot.
      for (size_t Ai = 0; Ai < MI.InvArgRanges.size(); ++Ai) {
        const auto &R = MI.InvArgRanges[Ai];
        if (!R.first || !R.second)
          continue;
        if (TokB < *R.second && TokE > *R.first) {
          if (Ai > std::numeric_limits<uint32_t>::max())
            return std::nullopt;
          return static_cast<uint32_t>(Ai);
        }
      }
    }

    // Step 2: "Zoom out" through macro provenance.
    //
    // Meaning of "zoom out":
    //   Move from the token's current macro-derived location toward *the call-
    //   site text that caused it*, so that eventually Sm.getFileLoc(...) lands
    //   in MI.InvFile and overlaps an MI.InvArgRanges interval.
    //
    // We do this by walking macro relationships outward:
    //   - If we are in a macro *argument expansion*, jump to the source range
    //     that was substituted at the call site (where the argument was passed).
    //   - Otherwise, prefer the immediate spelling location (useful for token-
    //     paste / copied-from situations) when it makes progress.
    //   - Otherwise, climb to the immediate macro caller location (one level
    //     outward in the expansion stack).
    if (CurrentLoc.isMacroID()) {
      if (Sm.isMacroArgExpansion(CurrentLoc)) {
        // Token comes from an argument expansion: hop to where that argument
        // was spelled at the call site (i.e., the expansion range begin in the
        // caller).
        CurrentLoc = Sm.getImmediateExpansionRange(CurrentLoc).getBegin();
      } else {
        SourceLocation SpellingLoc = Sm.getImmediateSpellingLoc(CurrentLoc);
        if (SpellingLoc.isValid() && SpellingLoc != CurrentLoc) {
          // Token's characters originate from a spelled token elsewhere (often
          // via paste/copy). Follow the spelling provenance if it actually
          // changes the location.
          CurrentLoc = SpellingLoc;
        } else {
          // Default: move one level outward to the macro caller.
          CurrentLoc = Sm.getImmediateMacroCallerLoc(CurrentLoc);
        }
      }
    } else {
      // We've reached a non-macro file location that is not in MI.InvFile (or
      // didn't overlap any argument ranges). No more provenance to walk.
      break;
    }
  }

  return std::nullopt;
}

void RefoldMapBuilder::onIncludeDirective(
    SourceLocation HashLoc, const Token &IncludeTok, StringRef FileName,
    bool IsAngled, CharSourceRange FilenameRange, OptionalFileEntryRef File,
    StringRef SearchPath, StringRef RelativePath) {
  if (!enabled())
    return;

  // Determine if the directive is an #include or an #include_next...
  tok::PPKeywordKind K = tok::pp_not_keyword;
  if (IncludeTok.is(tok::identifier)) {
    if (auto *II = IncludeTok.getIdentifierInfo())
      K = II->getPPKeywordID();
  }
  const bool IsInclude = (K == tok::pp_include);
  const bool IsIncludeNext = (K == tok::pp_include_next);
  assert((IsInclude || IsIncludeNext) &&
         "include directive must be one of: #include or #include_next");

  Item It;
  It.ID = Items.size();
  It.Kind = IK_Directive;
  It.Subkind = IsIncludeNext ? "#include_next" : "#include";
  It.Loc = HashLoc;
  It.IsAngled = IsAngled;
  std::string DirLine = "#";
  DirLine += PP.getSpelling(IncludeTok);
  DirLine += " ";
  DirLine += IsAngled ? "<" : "\"";
  DirLine += FileName.str();
  DirLine += IsAngled ? ">" : "\"";
  DirLine += "\n";
  It.Text = std::move(DirLine);
  It.TargetAsWritten = std::string(IsAngled ? ("<" + FileName.str() + ">")
                                            : ("\"" + FileName.str() + "\""));

  // Include-site anchors (definition site)
  auto Line = computeDirectiveLine(HashLoc);
  if (Line) {
    It.SiteBegin = Line->first;
    It.SiteEnd = Line->second;
  }
  It.SitePath = filePathForLocAbs(SM, HashLoc, EmitAbsPaths);

  // Resolved target path, when available
  if (File) {
    const std::string Abs = absolutePathFor(*File);

    std::string Spelled;
    if (!RelativePath.empty()) {
      const std::string SPKey =
          normalizePathKey(SearchPath, Cwd, /*IsDir=*/true);
      auto ItDir = IncludeDirAbs2Spelling.find(SPKey);

      llvm::StringRef DirSpell = (ItDir != IncludeDirAbs2Spelling.end())
                                     ? llvm::StringRef(ItDir->second)
                                     : llvm::StringRef(SearchPath);

      Spelled = joinSpelled(DirSpell, RelativePath);
    } else {
      // Fallback: Clang didn't provide a relative component; use whatever it
      // recorded.
      Spelled = std::string(File->getName());
    }

    // JSON should carry spellings by default; abs emission is optional.
    It.ResolvedPath = EmitAbsPaths ? Abs : Spelled;

    // Seed abs->spelling mapping for later __FILE__/__LINE__-style emission.
    if (!Abs.empty() && !Spelled.empty())
      FileAbs2Spelling[Abs] = Spelled;
  }

  Items.push_back(std::move(It));
  if (!IncludeStack.empty() && IncludeStack.back())
    Items.back().OwnerIncludeId = static_cast<uint64_t>(*IncludeStack.back());
  size_t ThisIdx = Items.size() - 1;

  // Remember multiple anchors for robustness.
  IncludeKey2Item[keyForLoc(SM, HashLoc)] = ThisIdx;
  IncludeKey2Item[keyForLoc(SM, FilenameRange.getBegin())] = ThisIdx;
  IncludeKey2Item[keyForLoc(SM, FilenameRange.getEnd())] = ThisIdx;
}

void RefoldMapBuilder::onMacroDefined(const Token &MacroNameTok,
                                      const MacroDirective *MD) {
  if (!enabled())
    return;

  const MacroInfo *MI = MD->getMacroInfo();

  // Skip Clang’s built-in/predefined macros (e.g. __LINE__, __FILE__, etc.).
  // These don’t have a meaningful user-authored #define site we can project
  // edits back onto in the original source.
  if (MI->isBuiltinMacro())
    return;

  // Record a directive "Item" describing this concrete #define in the source.
  Item It;
  It.ID = Items.size();
  It.Kind = IK_Directive;
  It.Subkind = "#define";

  // Use the macro definition location (not the expansion site) so the consumer
  // can map this item back to the defining file/line reliably.
  It.Loc = MI->getDefinitionLoc();

  // Materialize the directive text exactly as Clang would print it.
  // This keeps the producer’s directive spelling stable and avoids ad-hoc
  // reconstruction logic (function-like vs object-like, whitespace, etc.).
  std::string S;
  llvm::raw_string_ostream OS(S);
  PrintMacroDefinition(*MacroNameTok.getIdentifierInfo(), *MI, PP, &OS);
  OS << "\n"; // preserve directive line termination for refolding/diffing
  It.Text = OS.str();

  // Site info: capture the byte span of the whole directive line in its file,
  // so the consumer can do precise byte-based edits against the original source.
  auto Line = computeDirectiveLine(MI->getDefinitionLoc());
  if (Line) {
    It.SiteBegin = Line->first;
    It.SiteEnd = Line->second;
  }

  // Absolute (or configured) path of the file containing the #define.
  It.SitePath = filePathForLocAbs(SM, MI->getDefinitionLoc(), EmitAbsPaths);

  Items.push_back(std::move(It));

  // If we are currently inside an included file, attach ownership so the
  // consumer can attribute this directive to the include that brought it in.
  if (!IncludeStack.empty() && IncludeStack.back())
    Items.back().OwnerIncludeId = static_cast<uint64_t>(*IncludeStack.back());
}

void RefoldMapBuilder::onMacroUndefined(const Token &MacroNameTok,
                                        const MacroDefinition &MD,
                                        const MacroDirective *Undef) {
  if (!enabled())
    return;

  // Record this as a directive “Item” so the consumer can reconstruct / project
  // edits involving macro lifecycle directives (here: #undef).
  Item It;
  It.ID = Items.size();
  It.Kind = IK_Directive;
  It.Subkind = "#undef";
  It.Loc = MacroNameTok.getLocation();

  // Materialize the directive text exactly as it should appear in the refolded
  // source (including newline terminator).
  std::string S = "#undef ";
  S += MacroNameTok.getIdentifierInfo()->getName().str();
  S += "\n";
  It.Text = std::move(S);

  // Compute the byte-span of the full directive line in the source buffer so
  // the consumer can map this directive to an exact site range.
  auto Line = computeDirectiveLine(MacroNameTok.getLocation());
  if (Line) {
    It.SiteBegin = Line->first;
    It.SiteEnd = Line->second;
  }

  // Capture file provenance for the directive site (abs/rel controlled by
  // policy).
  It.SitePath = filePathForLocAbs(SM, MacroNameTok.getLocation(), EmitAbsPaths);

  Items.push_back(std::move(It));

  // If we're currently inside an #include, tag this directive with its owning
  // include item so the consumer can attribute it to the include context.
  if (!IncludeStack.empty() && IncludeStack.back())
    Items.back().OwnerIncludeId = static_cast<uint64_t>(*IncludeStack.back());
}

void RefoldMapBuilder::onMacroExpands(const Token &MacroNameTok,
                                      const MacroDefinition &MD,
                                      SourceRange Range,
                                      const MacroArgs *Args) {
  if (!enabled())
    return;

  const MacroInfo *MI = MD.getMacroInfo();

  // 1. Initialize a local Item.
  // We do this on the stack first to avoid any issues with vector reallocations
  // while we are still computing sub-fields.
  Item It;
  It.ID = static_cast<uint64_t>(Items.size());
  It.Kind = IK_Macro;
  It.Subkind = (MI && MI->isFunctionLike()) ? "func" : "obj";

  if (auto *II = MacroNameTok.getIdentifierInfo())
    It.Name = II->getName().str();

  // Record the macro's formal parameter list (as defined), so consumers can
  // build provenance edges across nested invocations without re-expanding
  // macros.
  if (MI && MI->isFunctionLike()) {
    It.DefParams.clear();
    It.DefParams.reserve(MI->getNumParams());
    const unsigned NumParams = MI->getNumParams();
    for (unsigned I = 0; I != NumParams; ++I) {
      MacroParam P;
      if (const IdentifierInfo *PI = getMacroParamIdentifier(MI, I))
        P.Name = PI->getName().str();
      P.Variadic = (MI->isVariadic() && I + 1 == NumParams);
      It.DefParams.push_back(std::move(P));
    }
  }

  It.Loc = Range.getBegin();
  It.IsBuiltinMacro = (MI != nullptr && MI->isBuiltinMacro());

  // --- Invocation text + byte range logic ---
  SourceLocation BeginTokLoc = Range.getBegin();
  SourceLocation EndTokLoc =
      Lexer::getLocForEndOfToken(Range.getEnd(), 0, SM, Lang);

  SourceLocation InvBeginLoc = BeginTokLoc;
  SourceLocation InvEndLoc = EndTokLoc;

  if (MacroNameTok.getLocation().isMacroID()) {
    InvBeginLoc = SM.getSpellingLoc(BeginTokLoc);
    SourceLocation SpEnd = SM.getSpellingLoc(Range.getEnd());
    InvEndLoc = Lexer::getLocForEndOfToken(SpEnd, 0, SM, Lang);
  }

  SourceLocation InvBeginFileLoc = SM.getFileLoc(InvBeginLoc);
  SourceLocation InvEndFileLoc = SM.getFileLoc(InvEndLoc);

  if (InvBeginFileLoc.isValid() && InvEndFileLoc.isValid()) {
    It.InvBegin = SM.getFileOffset(InvBeginFileLoc);
    It.InvEnd = SM.getFileOffset(InvEndFileLoc);
    It.InvFile = filePathForLocAbs(SM, InvBeginFileLoc, EmitAbsPaths);

    if (SM.isWrittenInSameFile(InvBeginFileLoc, InvEndFileLoc) &&
        *It.InvEnd >= *It.InvBegin) {
      It.InvText = Lexer::getSourceText(CharSourceRange::getCharRange(
                                            InvBeginFileLoc, InvEndFileLoc),
                                        SM, Lang)
                       .str();
    } else {
      It.InvText = It.Name;
    }
  } else {
    It.InvText = It.Name;
  }

  if (It.InvText.empty()) {
    auto &Diags = PP.getDiagnostics();
    unsigned DiagID = Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                            "macro invocation text is empty");
    Diags.Report(MacroNameTok.getLocation(), DiagID);
    It.InvText = It.Name; // Emergency fallback to keep schema happy
  }

  // 2. Compute invocation argument byte ranges (inv_arg_ranges).
  //
  // Coordinate system: absolute byte offsets within inv_file (same space as
  // inv_b/inv_e). To index into inv_text, subtract inv_b (inv_begin).
  //
  // We derive argument ranges by lexing the spelled invocation text (inv_text)
  // with Clang's raw lexer so that commas/parens inside comments, string/char
  // literals, raw strings, etc. are handled correctly. This yields ranges that
  // are consistent with inv_text and do not depend on SourceLocation mapping
  // through nested macro expansions.
  It.InvArgRanges.clear();
  if (MI && MI->isFunctionLike()) {
    const size_t NFormals = MI->getNumParams();
    It.InvArgRanges.resize(NFormals, {std::nullopt, std::nullopt});

    if (It.InvBegin) {
      (void)computeInvArgRangesFromText(It.InvText, *It.InvBegin, NFormals,
                                       PP.getLangOpts(), It.InvArgRanges);
    }
  }

  computeMacroProjectionSites(It, PP, MacroNameTok, MI, Args, Lang);

  // 3. Capture Owner ID from the stack before we lose the context
  if (!IncludeStack.empty() && IncludeStack.back())
    It.OwnerIncludeId = static_cast<uint64_t>(*IncludeStack.back());

  // 4. ATOMIC MOVE INTO COLLECTION
  // We determine the index before pushing.
  size_t NewIdx = Items.size();
  Items.push_back(std::move(It));

  // 5. Update the mapping for lookup during token attribution.
  //
  // Nested / higher-order expansions can later be observed through either the
  // macro name token location *or* the expansion range begin used by
  // SourceManager when walking immediate expansion hops. Register both, but do
  // so conservatively to avoid clobbering an existing more-specific mapping.
  auto RegisterMacroKey = [&](SourceLocation KLoc) {
    if (KLoc.isInvalid())
      return;
    MacroKey2Item[keyForMacroLoc(KLoc)] = NewIdx;
  };

  RegisterMacroKey(MacroNameTok.getLocation());
  RegisterMacroKey(Range.getBegin());

  // Register paste-produced spellings for paste-through-stringify projection.
  for (const auto &E : Items[NewIdx].PasteSpell2TokenIndices) {
    auto &Vec = PasteSpell2MacroItems[E.getKey()];
    if (std::find(Vec.begin(), Vec.end(), NewIdx) == Vec.end())
      Vec.push_back(NewIdx);
  }

  // Helper: map a macro-related location to the corresponding Item index.
  //
  // Nested higher-order expansions often surface the *formal occurrence* inside
  // the caller body (for example the `X` token in `FOO(X, 10)`) rather than the
  // caller invocation's name/range-begin location. When the direct key lookup
  // misses, fall back to the most-recent earlier macro invocation whose
  // invocation-site argument ranges can prove that this location originated
  // from one of its actual arguments. Cache the resolved alias so subsequent
  // lookups for the same raw MacroID location are O(1).
  auto LookupMacroItem = [&](SourceLocation Loc,
                             size_t SearchLimit = std::numeric_limits<size_t>::max())
      -> std::optional<size_t> {
    if (Loc.isInvalid())
      return std::nullopt;

    const std::string Key = keyForMacroLoc(Loc);
    auto It = MacroKey2Item.find(Key);
    if (It != MacroKey2Item.end())
      return It->second;

    if (!Loc.isMacroID())
      return std::nullopt;

    const size_t Limit = std::min(SearchLimit, Items.size());
    for (size_t I = Limit; I != 0; --I) {
      const size_t CandIdx = I - 1;
      const Item &Cand = Items[CandIdx];
      if (Cand.Kind != IK_Macro)
        continue;
      if (!argIndexForSpellingLoc(Cand, Loc, SM, Lang, EmitAbsPaths))
        continue;

      MacroKey2Item[Key] = CandIdx;
      return CandIdx;
    }

    return std::nullopt;
  };

  // If this macro invocation occurred while expanding another macro, record the
  // immediately enclosing (caller) macro invocation's item id and the origin of
  // the callee token itself. The consumer only performs generalized nested
  // args-only lifting through invocations whose callee token is proven literal;
  // higher-order or opaque callee origins are conservatively expanded.
  {
    Item &CurIt = Items[NewIdx];
    SourceLocation NameLoc = MacroNameTok.getLocation();
    if (!NameLoc.isMacroID() && Range.getBegin().isMacroID())
      NameLoc = Range.getBegin();

    if (!CurIt.CallerMacroId && NameLoc.isMacroID()) {
      SourceLocation L = NameLoc;
      for (unsigned Depth = 0; Depth != 16 && L.isMacroID(); ++Depth) {
        CharSourceRange ER = SM.getImmediateExpansionRange(L);
        SourceLocation CallerLoc = ER.getBegin();
        if (CallerLoc.isValid()) {
          if (auto CallerIdx = LookupMacroItem(CallerLoc, NewIdx)) {
            if (*CallerIdx != NewIdx)
              CurIt.CallerMacroId = Items[*CallerIdx].ID;
            break;
          }
        }

        SourceLocation Next = SM.getImmediateMacroCallerLoc(L);
        if (!Next.isValid() || Next == L)
          break;
        L = Next;
      }
    }

    CurIt.CalleeOrigin.Kind = MCO_LiteralMacroName;
    if (CurIt.CallerMacroId) {
      const Item *CallerIt = nullptr;
      for (const Item &Cand : Items) {
        if (Cand.ID == *CurIt.CallerMacroId) {
          CallerIt = &Cand;
          break;
        }
      }

      if (CallerIt) {
        if (auto ArgIdx = argIndexForSpellingLoc(*CallerIt, NameLoc, SM, Lang,
                                                 EmitAbsPaths)) {
          CurIt.CalleeOrigin.Kind = MCO_CallerParam;
          CurIt.CalleeOrigin.CallerParamIndices.push_back(*ArgIdx);
        } else if (NameLoc.isMacroID() && SM.isMacroArgExpansion(NameLoc)) {
          CurIt.CalleeOrigin.Kind = MCO_Opaque;
        }
      } else if (NameLoc.isMacroID()) {
        CurIt.CalleeOrigin.Kind = MCO_Opaque;
      }
    }

    // Higher-order signature forwarding: detect function-like nested calls
    // whose callee is literal but whose arguments are structurally unpacked
    // from a single caller formal, such as `(G z)` where `z` expands to a
    // parenthesized tuple. The current source-based invocation text/ranges can
    // be malformed for such cases, so record a normalized synthetic invocation
    // text and precise tuple-slice provenance for each callee argument.
    if (MI && MI->isFunctionLike() && Args && CurIt.CallerMacroId &&
        CurIt.CalleeOrigin.Kind == MCO_LiteralMacroName) {
      const Item *CallerIt = nullptr;
      for (const Item &Cand : Items) {
        if (Cand.ID == *CurIt.CallerMacroId) {
          CallerIt = &Cand;
          break;
        }
      }

      if (CallerIt) {
        SmallVector<std::string, 8> CalleeArgTexts;
        CalleeArgTexts.reserve(MI->getNumParams());
        bool MissingArgText = false;
        for (unsigned ArgIdx = 0; ArgIdx < MI->getNumParams(); ++ArgIdx) {
          auto ArgText =
              getUnexpandedMacroArgText(Args, ArgIdx, SM, PP.getLangOpts());
          if (!ArgText) {
            MissingArgText = true;
            break;
          }
          CalleeArgTexts.push_back(std::move(*ArgText));
        }

        if (!MissingArgText && !CalleeArgTexts.empty()) {
          std::optional<uint32_t> UniqueCallerFormal;
          std::vector<std::vector<InvArgTupleRef>> TupleRefs;
          std::string NormalizedInvText;
          std::vector<std::pair<std::optional<uint32_t>, std::optional<uint32_t>>>
              NormalizedRanges;

          for (uint32_t CallerFormal = 0;
               CallerFormal < CallerIt->InvArgRanges.size(); ++CallerFormal) {
            auto CallerArgText = getItemInvocationArgText(*CallerIt, CallerFormal);
            if (!CallerArgText)
              continue;

            std::vector<std::pair<std::optional<uint32_t>, std::optional<uint32_t>>>
                TupleRanges;
            if (!computeTupleElementRangesFromText(*CallerArgText, PP.getLangOpts(),
                                                   TupleRanges))
              continue;
            if (TupleRanges.size() != CalleeArgTexts.size())
              continue;

            bool Match = true;
            SmallVector<llvm::StringRef, 8> TupleArgTexts;
            std::vector<std::vector<InvArgTupleRef>> CandidateRefs;
            CandidateRefs.resize(TupleRanges.size());
            for (size_t I = 0; I < TupleRanges.size(); ++I) {
              const auto &TR = TupleRanges[I];
              if (!TR.first || !TR.second || *TR.second < *TR.first ||
                  *TR.second > CallerArgText->size()) {
                Match = false;
                break;
              }
              llvm::StringRef Slice =
                  CallerArgText->slice(*TR.first, *TR.second).trim();
              if (Slice != llvm::StringRef(CalleeArgTexts[I]).trim()) {
                Match = false;
                break;
              }
              TupleArgTexts.push_back(Slice);
              CandidateRefs[I].push_back(
                  InvArgTupleRef{CallerFormal, *TR.first, *TR.second});
            }
            if (!Match)
              continue;

            if (UniqueCallerFormal) {
              UniqueCallerFormal = std::nullopt;
              TupleRefs.clear();
              NormalizedInvText.clear();
              NormalizedRanges.clear();
              break;
            }

            UniqueCallerFormal = CallerFormal;
            TupleRefs = std::move(CandidateRefs);
            buildSyntheticFunctionLikeInvocationText(
                CurIt.Name, llvm::ArrayRef<llvm::StringRef>(TupleArgTexts),
                NormalizedInvText, NormalizedRanges);
          }

          if (UniqueCallerFormal && !NormalizedInvText.empty()) {
            CurIt.InvArgTupleRefs = std::move(TupleRefs);
            CurIt.NormalizedInvText = std::move(NormalizedInvText);
            CurIt.NormalizedInvArgTextRanges = std::move(NormalizedRanges);
          }
        }
      }
    }
  }

  // 6. THE SCHEMA FIX:
  // Seed the item with an *empty* open span at the current PP token index.
  //
  // Rationale:
  //   - Function-like macros can expand to nothing, or only to comment tokens
  //     (which we may skip), leaving the item with no observed printed tokens.
  //   - We still want a deterministic, schema-valid 'spans' array (minItems=1)
  //     without incorrectly claiming ownership of an unrelated printed token.
  //
  // Using an empty half-open range [TokIndex, TokIndex) satisfies the schema
  // and will be extended to include the first printed token if/when one is
  // attributed to this macro expansion.
  if (Items[NewIdx].Spans.empty()) {
    TokenSpan S;
    S.Begin = TokIndex;
    S.End = TokIndex; // empty/gap anchor
    S.Open = true;
    Items[NewIdx].Spans.push_back(S);
  }
}

void RefoldMapBuilder::onPragma(SourceLocation HashLoc, StringRef FullText) {
  if (!enabled())
    return;

  // Model a preprocessor pragma as a directive Item so the consumer can keep
  // it anchored to its original file/byte span when projecting edits.
  Item It;
  It.ID = Items.size();
  It.Kind = IK_Directive;
  It.Subkind = "#pragma";
  It.Loc = HashLoc;

  // Capture the directive's original text verbatim (as emitted/observed by the
  // preprocessor), including any trailing newline already present in FullText.
  It.Text = FullText.str();

  // Site info (line byte span and file path).
  auto Line = computeDirectiveLine(HashLoc);
  if (Line) {
    It.SiteBegin = Line->first;
    It.SiteEnd = Line->second;
  }
  It.SitePath = filePathForLocAbs(SM, HashLoc, EmitAbsPaths);

  Items.push_back(std::move(It));

  // If this pragma occurred while processing an included file, attach the
  // owning include Item id so the consumer can reconstruct include provenance.
  if (!IncludeStack.empty() && IncludeStack.back())
    Items.back().OwnerIncludeId = static_cast<uint64_t>(*IncludeStack.back());
}

void RefoldMapBuilder::onEnterFile(SourceLocation IncludeLoc) {
  if (!enabled())
    return;

  std::optional<size_t> Idx;
  if (IncludeLoc.isValid()) {
    auto It = IncludeKey2Item.find(keyForLoc(SM, IncludeLoc));
    if (It != IncludeKey2Item.end())
      Idx = It->second;
  }

  // Set parent relationship: the include we are about to enter is
  // conceptually a child of the current top of the stack (if any).
  if (Idx && !IncludeStack.empty() && IncludeStack.back())
    Items[*Idx].Parent = static_cast<uint64_t>(*IncludeStack.back());
  IncludeStack.push_back(Idx);
}

void RefoldMapBuilder::onToken(const Token &Tok, uint64_t PPByteBegin,
                           uint64_t PPByteEnd) {
  if (!enabled())
    return;
  if (Tok.is(tok::eof))
    return;
  if (IgnoreComments && Tok.is(tok::comment))
    return;

  std::optional<size_t> ItemIdx;
  SourceLocation L = Tok.getLocation();

  // Precompute byte range in the spelling file of this token (main or header).
  // We reuse this both for TokMap and for robust macro arg/body attribution.
  std::string TokFile;
  std::optional<uint64_t> TokB;
  std::optional<uint64_t> TokE;
  bool HasTokMap = false;
  {
    SourceLocation FL = SM.getFileLoc(L);
    if (FL.isValid()) {
      SourceLocation EndL =
          Lexer::getLocForEndOfToken(FL, /*Offset=*/0, SM, Lang);
      SourceLocation FEL = SM.getFileLoc(EndL);
      if (FEL.isValid()) {
        TokB = SM.getFileOffset(FL);
        TokE = SM.getFileOffset(FEL);
        TokFile = filePathForLocAbs(SM, FL, EmitAbsPaths); // e.g. "./e.h"
        HasTokMap = !TokFile.empty();
      }
    }
  }

  // Prefer a macro item when the token is inside a macro expansion.
  if (SM.isMacroArgExpansion(L) || SM.isMacroBodyExpansion(L)) {
    auto LookupMacroItem = [&](SourceLocation Loc) -> std::optional<size_t> {
      if (Loc.isInvalid())
        return std::nullopt;

      const std::string Key = keyForMacroLoc(Loc);
      auto It = MacroKey2Item.find(Key);
      if (It != MacroKey2Item.end())
        return It->second;

      if (!Loc.isMacroID())
        return std::nullopt;

      for (size_t I = Items.size(); I != 0; --I) {
        const size_t CandIdx = I - 1;
        const Item &Cand = Items[CandIdx];
        if (Cand.Kind != IK_Macro)
          continue;
        if (!argIndexForSpellingLoc(Cand, Loc, SM, Lang, EmitAbsPaths))
          continue;

        MacroKey2Item[Key] = CandIdx;
        return CandIdx;
      }

      return std::nullopt;
    };

    // Innermost macro: immediate caller of this token location.
    //
    // For nested expansions, SourceManager::getImmediateMacroCallerLoc() can
    // “skip” past the immediate invocation (e.g. when the call site itself is
    // synthesized from an outer macro expansion). Prefer the begin of the
    // immediate expansion range, which corresponds to the macro-name token
    // location for the invocation that produced this token.
    SourceLocation Caller;
    if (auto R = SM.getImmediateExpansionRange(L); R.isValid())
      Caller = R.getBegin();
    if (!Caller.isValid())
      Caller = SM.getImmediateMacroCallerLoc(L);
    auto InnerIdx = LookupMacroItem(Caller);

    // Enclosing macro: walk up the macro caller chain (keeps MacroID hops
    // intact). Outer macro (if any): prefer ultimate expansion location. This
    // is robust when the immediate caller loc is a file location inside an
    // outer macro body (no MacroID chain to walk), which is exactly the nested
    // builtin case we care about.
    auto OuterIdx = LookupMacroItem(SM.getExpansionLoc(L));
    if (!OuterIdx) {
      // Conservative fallback: walk up the caller chain when the caller is a
      // MacroID.
      SourceLocation Cur = Caller;
      for (size_t Depth = 0; Depth < 16; ++Depth) {
        if (Cur.isInvalid() || !Cur.isMacroID())
          break;
        Cur = SM.getImmediateMacroCallerLoc(Cur);
        OuterIdx = LookupMacroItem(Cur);
        if (OuterIdx)
          break;
      }
    }

    // Fallback: some paths prefer expansion loc
    if (!InnerIdx) {
      Caller = SM.getExpansionLoc(L);
      InnerIdx = LookupMacroItem(Caller);
    }

    if (InnerIdx) {
      size_t Chosen = *InnerIdx;

      // Clang sometimes reports adjacent punctuation as being "inside" a
      // builtin macro expansion. In those cases, prefer the enclosing macro
      // item for non-expansion tokens.
      if (*InnerIdx < Items.size() && OuterIdx) {
        const Item &MI = Items[*InnerIdx];
        if (MI.Kind == IK_Macro && MI.IsBuiltinMacro) {
          // Builtin/predefined macros should contribute only their expansion
          // token(s). If Clang attributes adjacent punctuation or other
          // non-expansion tokens to the builtin macro location, prefer the
          // enclosing macro item for those tokens.
          bool Ok = Tok.isLiteral() || Tok.is(tok::numeric_constant);
          if (!Ok)
            Chosen = *OuterIdx;
        }
      }

      ItemIdx = Chosen;
    } else if (OuterIdx) {
      ItemIdx = OuterIdx;
    }
  }

  // Otherwise attribute to the innermost active include; else to the file item.
  if (!ItemIdx) {
    if (!IncludeStack.empty() && IncludeStack.back()) {
      ItemIdx = IncludeStack.back();
    } else {
      if (!CurrentFileItem) {
        Item F;
        F.ID = Items.size();
        F.Kind = IK_File;
        F.Subkind = "file";
        Items.push_back(std::move(F));
        CurrentFileItem = Items.size() - 1;
      }
      ItemIdx = CurrentFileItem;
    }
  }

  // 1) Attribute the token to its primary item (macro, include, or file).
  touchSpanForItem(ItemIdx, TokIndex);

  // 1a) If the primary item is a MACRO expansion, also record exact origin:
  //     - ArgSpans for tokens from actual arguments (func-like only)
  //     - BodySpans for tokens from the macro body (and for obj-like macros)
  if (ItemIdx && Items[*ItemIdx].Kind == IK_Macro) {
    auto &It = Items[*ItemIdx];

    if (SM.isMacroArgExpansion(L)) {
      if (It.Subkind == "func") {
        auto ArgIndex = argIndexForSpellingLoc(It, L, SM, Lang, EmitAbsPaths);
        if (ArgIndex)
          touchArgTokSpan(It.ArgSpans, TokIndex, *ArgIndex);
        else
          touchTokSpan(It.BodySpans, TokIndex); // fallback: keep schema-valid
      } else {
        // Object-like macros have no arguments; treat as body-origin.
        touchTokSpan(It.BodySpans, TokIndex);
      }
    } else if (SM.isMacroBodyExpansion(L)) {
      touchTokSpan(It.BodySpans, TokIndex);
    }

    // Record projections for stringification ("#X") and token-pasting
    // ("X##Y") for any macro invocation in the caller chain. We cannot reliably
    // discover these using spelling/callee locations alone (especially across
    // nested macro expansions), so we instead precompute the projection
    // spellings per macro invocation and match by the emitted token spelling
    // here.
    if (L.isMacroID()) {
      const std::string Sp = PP.getSpelling(Tok);

      // Record projections for stringification (#X) and token-pasting (X##Y).
      auto recordProjectionsForItem = [&](Item &MI) {
        if (MI.Kind != IK_Macro)
          return;

        // Fast reject.
        if (MI.StringifySpell2ArgIndices.empty() && MI.PasteTokens.empty())
          return;

        // Stringification: emitted token is a quoted string literal
        // corresponding to #Arg.
        if (!MI.StringifySpell2ArgIndices.empty() &&
            (Tok.is(tok::string_literal) || Tok.is(tok::wide_string_literal) ||
             Tok.is(tok::utf8_string_literal) ||
             Tok.is(tok::utf16_string_literal) ||
             Tok.is(tok::utf32_string_literal))) {
          auto ItS = MI.StringifySpell2ArgIndices.find(Sp);
          if (ItS != MI.StringifySpell2ArgIndices.end()) {
            for (unsigned A : ItS->second)
              touchArgTokSpan(MI.StringifySpans, TokIndex, A);
          }
        }

        // Token pasting: emitted token is the result of one or more "##"
        // operations.
        if (MI.PasteTokens.empty())
          return;
        auto ItP = MI.PasteSpell2TokenIndices.find(Sp);
        if (ItP == MI.PasteSpell2TokenIndices.end())
          return;

        const auto &Candidates = ItP->second;
        if (Candidates.empty())
          return;

        // Choose a candidate deterministically, respecting emission order.
        size_t Chosen = Candidates.front();
        const size_t Cursor = MI.PasteTokenCursor;

        // Prefer the "next" paste token if it matches.
        if (Cursor < MI.PasteTokens.size()) {
          for (size_t C : Candidates) {
            if (C == Cursor) {
              Chosen = C;
              break;
            }
          }
        }
        // Otherwise pick the first candidate at/after the cursor; else wrap
        // to the first.
        if (Chosen < Cursor) {
          for (size_t C : Candidates) {
            if (C >= Cursor) {
              Chosen = C;
              break;
            }
          }
        }

        // Advance cursor past the chosen index.
        if (MI.PasteTokenCursor < Chosen + 1)
          MI.PasteTokenCursor = Chosen + 1;

        const PasteToken &PT = MI.PasteTokens[Chosen];

        // Record one span per argument part (arg_index + byte range inside
        // the pasted token).
        auto appendPasteSpan = [&](std::optional<unsigned> ArgIndex,
                                   unsigned ByteBegin, unsigned ByteEnd) {
          ArgTokenSpan S;
          S.Begin = TokIndex;
          S.End = TokIndex + 1;
          S.ArgIndex = ArgIndex;
          S.ByteBegin = ByteBegin;
          S.ByteEnd = ByteEnd;
          S.HasByteRange = true;
          MI.PasteSpans.push_back(std::move(S));
        };

        for (const PastePart &P : PT.Parts) {
          if (!P.ArgIndex)
            continue;

          unsigned ByteBegin = P.ByteBegin;
          unsigned ByteEnd = P.ByteEnd;

          appendPasteSpan(P.ArgIndex, ByteBegin, ByteEnd);
        }
      };

      // Walk the immediate macro caller chain. Any macro invocation in the
      // chain might be the one that performed the stringification/paste that
      // produced this token (e.g., pasted in macro A, then passed through
      // macro B).
      SourceLocation Cur = L;
      for (unsigned Depth = 0; Depth < 32 && Cur.isMacroID(); ++Depth) {
        const SourceLocation Caller = SM.getImmediateMacroCallerLoc(Cur);
        if (Caller.isInvalid())
          break;

        auto ItM = MacroKey2Item.find(keyForMacroLoc(Caller));
        if (ItM != MacroKey2Item.end()) {
          const size_t I = ItM->second;
          if (I < Items.size())
            recordProjectionsForItem(Items[I]);
        }

        Cur = Caller;
      }
    }

    // Paste-through-stringify projection:
    //
    // Goal: if the *emitted* token is a string literal that itself came from a
    // stringify site (`#arg`), then edits inside that string literal may need
    // to be attributed back to earlier token-paste (`##`) results that
    // contributed substrings to the argument being stringified.
    //
    // This is specifically for cases where the `##`-producing macro is *not*
    // present on the immediate macro caller chain at the emission site (because
    // the paste happened earlier while building the argument text).
    if (L.isMacroID() &&
        Tok.isOneOf(tok::string_literal, tok::wide_string_literal,
                    tok::utf8_string_literal, tok::utf16_string_literal,
                    tok::utf32_string_literal) &&
        !PasteSpell2MacroItems.empty()) {

      // Sp = exact spelled bytes of the emitted string-literal token as the
      // preprocessor outputs it. This is what we key against in
      // StringifySpell2ArgIndices to identify the stringify owner.
      const std::string SpStr = PP.getSpelling(Tok);
      const llvm::StringRef Sp = SpStr;

      // Identify which macro invocation item "owns" this spelled string literal
      // as a result of a `#param` in its replacement list.
      std::optional<size_t> StringifyOwnerIdx;

      // Try to attribute this spelled string token to a specific item index I
      // by checking whether item I recorded a stringify-site mapping for Sp.
      auto trySetOwner = [&](size_t I) {
        if (StringifyOwnerIdx)
          return;
        auto It = Items[I].StringifySpell2ArgIndices.find(Sp);
        if (It != Items[I].StringifySpell2ArgIndices.end())
          StringifyOwnerIdx = I;
      };

      // First attempt: if the current emission is already associated with a
      // macro item (ItemIdx), prefer that.
      if (ItemIdx)
        trySetOwner(*ItemIdx);

      // Otherwise, walk up the macro caller chain (bounded) and see if any
      // enclosing macro invocation item performed the stringification that
      // produced this spelled string token.
      SourceLocation Cur2 = L;
      for (unsigned Depth = 0;
           Depth < 32 && Cur2.isMacroID() && !StringifyOwnerIdx; ++Depth) {
        SourceLocation Caller = SM.getImmediateMacroCallerLoc(Cur2);
        if (Caller.isInvalid())
          break;
        auto ItM = MacroKey2Item.find(keyForMacroLoc(Caller));
        if (ItM != MacroKey2Item.end())
          trySetOwner(ItM->second);
        Cur2 = Caller;
      }

      if (StringifyOwnerIdx) {
        // Decode the string literal into its "payload" (decoded characters) and
        // a mapping from decoded-payload indices back to (begin,end) spans in
        // the *spelled* token string. We only proceed if that mapping is a
        // per-byte/per-char 1:1 correspondence with Decoded.
        const auto Dec = decodeStringLiteralPayload(Sp);
        if (Dec.Valid && Dec.PayloadToSpelling.size() == Dec.Decoded.size()) {

          // rootOf(I):
          // Collapse an item index I up through CallerMacroId links until we
          // hit the root invocation in that call chain. This gives a stable
          // "macro family" identifier so we can avoid cross-chain ambiguity.
          auto rootOf = [&](size_t I) -> uint64_t {
            size_t Cur = I;
            for (unsigned Depth = 0; Depth < 128; ++Depth) {
              if (Cur >= Items.size())
                break;
              if (!Items[Cur].CallerMacroId)
                break;
              uint64_t CallerId = *Items[Cur].CallerMacroId;
              if (CallerId >= Items.size())
                break;
              Cur = (size_t)CallerId;
            }
            return Items[Cur].ID;
          };

          // RootId = root macro invocation for the stringify owner chain.
          // We only attribute paste matches to paste-items whose root matches
          // this RootId, to prevent accidental matches across unrelated macro
          // expansions that happen to share a spelling substring.
          const uint64_t RootId = rootOf(*StringifyOwnerIdx);

          // recordPastePart:
          // Append one ArgTokenSpan describing that bytes [SpellBegin,SpellEnd)
          // within the *spelled* string-literal token originate from argument
          // ArgIndex of the paste-producing macro item PMI, at output token
          // TokIndex.
          auto recordPastePart = [&](Item &PMI, uint64_t TokIndex,
                                     uint32_t ArgIndex, uint32_t SpellBegin,
                                     uint32_t SpellEnd) {
            ArgTokenSpan AS;
            AS.Begin = TokIndex;
            AS.End = TokIndex + 1;
            AS.ArgIndex = ArgIndex;
            AS.Open = false;
            AS.HasByteRange = true;
            AS.ByteBegin = SpellBegin;
            AS.ByteEnd = SpellEnd;
            PMI.PasteSpans.push_back(AS);
          };

          // Work over the decoded payload (not the spelled token) so that
          // escape sequences don't interfere with substring search. We'll map
          // back to spelled offsets using PayloadToSpelling.
          const llvm::StringRef D(Dec.Decoded);

          // Scan through the decoded payload, but only consider identifier-like
          // runs; pasting most commonly creates identifiers / identifier
          // chunks, and limiting to these runs reduces false positives
          // substantially.
          for (size_t P = 0; P < D.size();) {
            unsigned char C = (unsigned char)D[P];
            if (!isIdentByte(C)) {
              ++P;
              continue;
            }

            // [RunBegin,RunEnd) = one maximal identifier-ish run.
            size_t RunBegin = P;
            while (P < D.size() && isIdentByte((unsigned char)D[P]))
              ++P;
            size_t RunEnd = P;

            llvm::StringRef Word = D.substr(RunBegin, RunEnd - RunBegin);

            // A match is "paste spelling Key found at offset Off inside this
            // run".
            struct Match {
              llvm::StringRef Spell;
              size_t Offset; // offset within this identifier run
            };

            llvm::SmallVector<Match, 8> Matches;
            auto addMatch = [&](llvm::StringRef Spell, size_t Off) {
              // Deduplicate identical matches within the run to avoid doing the
              // same attribution work multiple times.
              for (const auto &M : Matches) {
                if (M.Spell == Spell && M.Offset == Off)
                  return;
              }
              Matches.push_back({Spell, Off});
            };

            // Find all paste-result spellings that occur as substrings within
            // this identifier run. PasteSpell2MacroItems maps:
            //   spelled_paste_token -> [candidate macro item indices that can
            //   produce it]
            //
            // Determinism: PasteSpell2MacroItems is a map (DenseMap/StringMap),
            // so iteration order is not stable. Sort the keys before scanning
            // so Match collection (and subsequent cursor advancement) is stable.
            llvm::SmallVector<llvm::StringRef, 64> PasteKeys;
            PasteKeys.reserve(PasteSpell2MacroItems.size());
            for (const auto &KV : PasteSpell2MacroItems)
              PasteKeys.push_back(KV.getKey());
            llvm::sort(PasteKeys);

            for (llvm::StringRef Key : PasteKeys) {
              if (Key.empty() || Key.size() > Word.size())
                continue;

              // Collect all occurrences of Key within this identifier run.
              size_t Off = Word.find(Key);
              while (Off != llvm::StringRef::npos) {
                addMatch(Key, Off);
                Off = Word.find(Key, Off + 1);
              }
            }

            // Determinism: handle overlaps in a stable order.
            llvm::sort(Matches, [](const Match &A, const Match &B) {
              if (A.Offset != B.Offset)
                return A.Offset < B.Offset;
              if (A.Spell.size() != B.Spell.size())
                return A.Spell.size() > B.Spell.size();
              return A.Spell < B.Spell;
            });

            auto handleMatch = [&](llvm::StringRef MatchSpell,
                                   size_t MatchOff) {
              // Identify which paste macro item(s) could have produced
              // MatchSpell.
              auto GI = PasteSpell2MacroItems.find(MatchSpell);
              if (GI == PasteSpell2MacroItems.end())
                return;

              // Filter candidates to those in the same macro-root chain as the
              // stringify owner, to enforce locality/uniqueness.
              llvm::SmallVector<size_t, 4> Cands;
              for (size_t Cand : GI->second) {
                if (Cand >= Items.size())
                  continue;
                if (rootOf(Cand) != RootId)
                  continue;
                Cands.push_back(Cand);
              }

              // If ambiguous (0 or >1), bail: we can't safely attribute this
              // substring to exactly one paste macro invocation.
              if (Cands.size() != 1)
                return;

              const size_t PasteItemIdx = Cands[0];
              Item &PMI = Items[PasteItemIdx];

              // PMI.PasteSpell2TokenIndices maps:
              //   paste_spelling -> [indices in PMI.PasteTokens vector]
              // (because the same spelling can be produced multiple times per
              // item).
              auto SI = PMI.PasteSpell2TokenIndices.find(MatchSpell);
              if (SI == PMI.PasteSpell2TokenIndices.end())
                return;

              const auto &TokenIndices = SI->second;
              if (TokenIndices.empty())
                return;

              // Choose a paste-token occurrence deterministically using the
              // per-item cursor:
              //   - prefer the token whose index equals the cursor
              //   - else prefer the first token >= cursor
              //   - else fall back to the first recorded token
              size_t Selected = TokenIndices.front();
              const size_t Cursor = PMI.PasteTokenCursor;

              if (Cursor < PMI.PasteTokens.size()) {
                for (size_t C : TokenIndices) {
                  if (C == Cursor) {
                    Selected = C;
                    break;
                  }
                }
              }
              if (Selected < Cursor) {
                for (size_t C : TokenIndices) {
                  if (C >= Cursor) {
                    Selected = C;
                    break;
                  }
                }
              }
              if (PMI.PasteTokenCursor < Selected + 1)
                PMI.PasteTokenCursor = Selected + 1;

              if (Selected >= PMI.PasteTokens.size())
                return;

              const auto &PT = PMI.PasteTokens[Selected];
              bool AnyRecorded = false;

              // Base = decoded-payload index where the matched paste spelling
              // begins.
              const size_t Base = RunBegin + MatchOff;

              // For each argument-derived part of the paste token, translate
              // its [ByteBegin,ByteEnd) within the paste spelling (decoded
              // space) into [SB,SE) within the *spelled* string literal token
              // using the payload-to-spelling map.
              for (const auto &Part : PT.Parts) {
                if (!Part.ArgIndex)
                  continue;

                const size_t PB = Base + Part.ByteBegin;
                const size_t PE = Base + Part.ByteEnd;
                if (PE <= PB || PE > Dec.PayloadToSpelling.size())
                  continue;

                // PayloadToSpelling[k] = {spell_begin, spell_end} for decoded
                // index k. We map [PB,PE) by taking begin at PB and end at
                // PE-1's end.
                const uint32_t SB = Dec.PayloadToSpelling[PB].first;
                const uint32_t SE = Dec.PayloadToSpelling[PE - 1].second;

                recordPastePart(PMI, TokIndex, (uint32_t)*Part.ArgIndex, SB,
                                SE);
                AnyRecorded = true;
              }

              // If we recorded any paste spans for this item, mark it as
              // “touched” at this output token index so later stages know this
              // item participates in projection at TokIndex.
              if (AnyRecorded)
                touchSpanForItem(PasteItemIdx, TokIndex);
            };

            // Process each unique match found in this identifier run.
            for (const auto &M : Matches)
              handleMatch(M.Spell, M.Offset);
          }
        }
      }
    }

    // Tokens that are neither arg nor body (rare, e.g. builtins) remain covered
    // by the primary Spans via touchSpanForItem above.
  }

  // 1b) Also attribute this token to any *enclosing* macro invocation in the
  // same spelling file whose invocation byte range contains [TokB,TokE).
  //
  // This fixes cases where a function-like macro argument is itself a macro
  // invocation (e.g., set_zero(..., BYTE4, ...)): the expanded token(s) are
  // primarily attributed to the inner macro item, but still need to be recorded
  // as argument tokens for the enclosing function-like macro.
  if (ItemIdx && HasTokMap && L.isMacroID()) {
    for (size_t I = 0; I < Items.size(); ++I) {
      if (I == *ItemIdx)
        continue;
      Item &MI = Items[I];
      if (MI.Kind != IK_Macro)
        continue;
      if (!MI.InvBegin || !MI.InvEnd || !TokB || !TokE)
        continue;
      if (MI.InvFile != TokFile)
        continue;
      if (*MI.InvBegin <= *TokB && *TokE <= *MI.InvEnd) {
        // Ensure the enclosing macro's primary token span covers nested
        // expansions.
        touchSpanForItem(I, TokIndex);

        // For function-like macros, anything after the name token is treated as
        // originating from an argument spelling region. Otherwise, treat as
        // body.
        if (MI.Subkind == "func") {
          auto ArgIndex = argIndexForSpellingLoc(MI, L, SM, Lang, EmitAbsPaths);
          if (ArgIndex)
            touchArgTokSpan(MI.ArgSpans, TokIndex, *ArgIndex);
          else
            touchTokSpan(MI.BodySpans, TokIndex); // fallback: keep schema-valid
        } else {
          touchTokSpan(MI.BodySpans, TokIndex);
        }
      }
    }
  }

  // 2) Grow all active include items transitively so a parent include covers
  //    its entire subtree (nested includes/macros).
  for (auto idx : IncludeStack) {
    if (!idx || idx == ItemIdx)
      continue;
    touchSpanForItem(idx, TokIndex);
  }

  // 3) Emit TokMap entry (reuse the precomputed spelling-file span).
  if (HasTokMap) {
    TokMapEntry M;
    M.PPIndex = TokIndex;
    M.SrcBegin = *TokB;
    M.SrcEnd = *TokE;
    M.File = TokFile;
    TokMap.push_back(std::move(M));
  }

  TokPPByteBegin.push_back(PPByteBegin);
  TokPPByteEnd.push_back(PPByteEnd);
  ++TokIndex;
}

void RefoldMapBuilder::finalizeIncludeDecls() {
  if (!enabled())
    return;

  // Cache header file buffers so we only hit the filesystem once per path.
  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> FileBufCache;

  auto getHeaderBuffer = [&](llvm::StringRef Path) -> llvm::StringRef {
    auto It = FileBufCache.find(Path);
    if (It != FileBufCache.end())
      return It->second->getBuffer();

    auto MBOrErr = llvm::MemoryBuffer::getFile(Path);
    if (!MBOrErr)
      return llvm::StringRef();

    std::unique_ptr<llvm::MemoryBuffer> MB = std::move(*MBOrErr);
    llvm::StringRef Buf = MB->getBuffer();
    FileBufCache[Path] = std::move(MB);
    return Buf;
  };

  // Classify a single token slice using Clang's raw lexer.
  //
  // This replaces the prior character-class "identifier-like" heuristic.
  // We intentionally avoid assuming ASCII or hand-rolling identifier rules;
  // the lexer handles language mode, UCNs, trigraphs, etc.
  auto lexSingleKind = [&](llvm::StringRef S) -> tok::TokenKind {
    S = S.trim();
    if (S.empty())
      return tok::unknown;

    const SourceLocation BaseLoc = SourceLocation::getFromRawEncoding(1);
    std::string LexBuf = S.str();
    LexBuf.push_back('\0');
    const char *BufStart = LexBuf.data();
    const char *BufEnd = BufStart + S.size();

    Lexer Lex(BaseLoc, Lang, BufStart, BufStart, BufEnd);
    Token Tok;
    Lex.LexFromRawLexer(Tok);
    return Tok.getKind();
  };

  for (Item &It : Items) {
    // Only consider include directives that resolved to a real header.
    if (It.Kind != IK_Directive)
      continue;
    if (It.Subkind != "#include" && It.Subkind != "#include_next")
      continue;

    // Already populated (e.g. if we re-run for some reason)?
    if (!It.Decls.empty())
      continue;

    if (It.ResolvedPath.empty())
      continue;

    llvm::StringRef HeaderPath(It.ResolvedPath);

    // Collect all PP token indices that:
    //  - are in this include's spans, and
    //  - originate from the resolved header file.
    llvm::SmallVector<uint64_t, 64> PPIdxs;
    for (const TokenSpan &S : It.Spans) {
      if (S.Open)
        continue;
      uint64_t B = S.Begin;
      uint64_t E = S.End;
      if (E > TokMap.size())
        E = TokMap.size();

      for (uint64_t PP = B; PP < E; ++PP) {
        const TokMapEntry &TM = TokMap[PP];
        if (TM.File == HeaderPath.str())
          PPIdxs.push_back(PP);
      }
    }

    if (PPIdxs.empty())
      continue;

    // Sort and deduplicate in case spans overlap.
    std::sort(PPIdxs.begin(), PPIdxs.end());
    PPIdxs.erase(std::unique(PPIdxs.begin(), PPIdxs.end()), PPIdxs.end());

    // Get the header file buffer.
    llvm::StringRef HBuf = getHeaderBuffer(HeaderPath);
    if (HBuf.empty())
      continue;

    // Emit one HeaderDecl for PPIdxs[StartIdx..EndIdx-1].
    auto flushDecl = [&](unsigned StartIdx, unsigned EndIdx) {
      if (StartIdx >= EndIdx || EndIdx > PPIdxs.size())
        return;

      uint64_t PPBegin = PPIdxs[StartIdx];
      uint64_t PPEnd   = PPIdxs[EndIdx - 1] + 1; // half-open
      if (PPBegin >= PPEnd || PPEnd > TokMap.size())
        return;

      const TokMapEntry &First = TokMap[PPBegin];
      const TokMapEntry &Last  = TokMap[PPEnd - 1];

      uint64_t HeaderB = First.SrcBegin;
      uint64_t HeaderE = Last.SrcEnd;

      // --- Determine "kind" and "name" deterministically. ---

      llvm::StringRef Kind = "unknown";
      llvm::StringRef Name;

      llvm::StringRef LastIdentBeforeParen;
      llvm::StringRef LastIdentBeforeSemi;
      bool SawLParen = false;

      for (unsigned I = StartIdx; I < EndIdx; ++I) {
        unsigned PP = PPIdxs[I];
        const TokMapEntry &TM = TokMap[PP];
        if (TM.File != HeaderPath.str())
          continue;

        llvm::StringRef TokText =
            HBuf.slice(static_cast<size_t>(TM.SrcBegin),
                       static_cast<size_t>(TM.SrcEnd));
        llvm::StringRef Trimmed = TokText.trim();
        if (Trimmed.empty())
          continue;

        const tok::TokenKind K = lexSingleKind(Trimmed);

        // Track identifiers as we go.
        if (K == tok::identifier) {
          LastIdentBeforeSemi = Trimmed;
          if (!SawLParen)
            LastIdentBeforeParen = Trimmed;
        }

        // Opening paren: we are entering parameter / declarator list.
        if (K == tok::l_paren)
          SawLParen = true;

        // Semicolon ends the simple-declaration.
        if (K == tok::semi)
          break;
      }

      if (SawLParen && !LastIdentBeforeParen.empty()) {
        // Treat as a function-like declaration.
        Kind = "function";
        Name = LastIdentBeforeParen;
      } else if (!LastIdentBeforeSemi.empty()) {
        // Some other simple declaration; give it a best-effort name.
        Kind = "unknown";
        Name = LastIdentBeforeSemi;
      } else {
        // No useful identifier; give it a placeholder.
        Kind = "unknown";
        Name = "<decl>";
      }

      addHeaderDecl(It, Kind, Name, HeaderPath, HeaderB, HeaderE,
                    PPBegin, PPEnd);
    };

    // Walk tokens from the header, splitting at ';' in header text.
    unsigned Start = 0;
    for (unsigned I = 0; I < PPIdxs.size(); ++I) {
      unsigned PP = PPIdxs[I];
      const TokMapEntry &TM = TokMap[PP];
      if (TM.File != HeaderPath.str())
        continue;

      llvm::StringRef TokText =
          HBuf.slice(static_cast<size_t>(TM.SrcBegin),
                     static_cast<size_t>(TM.SrcEnd));

      // A semicolon token terminates a decl.
      if (lexSingleKind(TokText) == tok::semi) {
        flushDecl(Start, I + 1);
        Start = I + 1;
      }
    }

    // Any trailing tokens without a ';' are ignored for now.
  }
}

void RefoldMapBuilder::writeJSON() {
  if (!enabled())
    return;

  // Open the refold JSON file for writing.
  std::error_code EC;
  llvm::raw_fd_ostream OS(OutPath, EC, llvm::sys::fs::OF_Text);
  if (EC) {
    llvm::errs() << "refold: cannot open " << OutPath << ": " << EC.message()
                 << "\n";
    return;
  }

  llvm::json::OStream JO(OS, /*Indent=*/2);

  JO.object([&] {
    JO.attribute("version", "2.2");

    const auto &PPO = PP.getPreprocessorOpts();
    std::string LangStr = computeLangStr(PP.getLangOpts());

    // Capture a stable CWD string for provenance. Historically we attempted
    // this twice to tolerate rare transient failures.
    llvm::SmallString<256> CWD;
    std::string CwdStr;
    for (int Attempt = 0; Attempt < 2 && CwdStr.empty(); ++Attempt) {
      CWD.clear();
      if (!llvm::sys::fs::current_path(CWD))
        CwdStr = CWD.str().str();
    }

    // Serialize the PP context of this clang instance
    JO.attributeObject("pp_ctx", [&] {
      JO.attribute("cwd", CwdStr);
      JO.attributeArray("argv", [&] {
        for (const std::string &Arg : PPO.RefoldPPArgv)
          JO.value(Arg);
      });
      JO.attribute("lang", LangStr);
    });

    JO.attribute("source", TUSourcePath);

    // tokens...
    JO.attributeObject("tokens", [&] {
      JO.attribute("count", TokIndex);
      if (EnableByteSpans) {
        JO.attributeArray("pp_byte_begin", [&] {
          for (uint64_t B : TokPPByteBegin)
            JO.value(B);
        });
        JO.attributeArray("pp_byte_end", [&] {
          for (uint64_t E : TokPPByteEnd)
            JO.value(E);
        });
      }
    });

    // -------------------------------------------------------------------------
    // Macro provenance DAG helpers.
    //
    // We derive an immediate caller relation between macro invocations by
    // containment of their emitted token envelopes in the preprocessed output
    // (A-domain). We also compute (for nested invocations) per-argument
    // dependency sets on the caller's formals by scanning the raw argument text
    // for occurrences of caller parameter names. This is deliberately syntactic
    // and conservative: it records "could depend on" edges without
    // re-expanding.
    // -------------------------------------------------------------------------

    struct MacroEnv {
      uint64_t ID = 0;
      uint64_t BTok = 0;
      uint64_t ETok = 0;
    };

    llvm::DenseMap<uint64_t, const Item *> ItemByID;
    ItemByID.reserve(Items.size());
    for (const Item &It : Items)
      ItemByID.try_emplace(It.ID, &It);

    // Compute the half-open token envelope [BTok, ETok) covered by an item's
    // emitted token spans. This gives a simple nesting interval for later
    // caller-macro recovery.
    auto computeTokEnvelope = [](const Item &It, uint64_t &BTok,
                                 uint64_t &ETok) -> bool {
      uint64_t B = std::numeric_limits<uint64_t>::max();
      uint64_t E = 0;
      for (const TokenSpan &S : It.Spans) {
        B = std::min(B, S.Begin);
        E = std::max(E, S.End);
      }
      if (B == std::numeric_limits<uint64_t>::max() || E <= B)
        return false;
      BTok = B;
      ETok = E;
      return true;
    };

    llvm::SmallVector<MacroEnv, 128> MacroEnvs;
    for (const Item &It : Items) {
      if (It.Kind != IK_Macro)
        continue;
      uint64_t B = 0, E = 0;
      if (computeTokEnvelope(It, B, E))
        MacroEnvs.push_back({It.ID, B, E});
    }

    std::sort(MacroEnvs.begin(), MacroEnvs.end(),
              [](const MacroEnv &A, const MacroEnv &B) {
                if (A.BTok != B.BTok)
                  return A.BTok < B.BTok;
                // Longer (outer) first when equal begin.
                return A.ETok > B.ETok;
              });

    llvm::DenseMap<uint64_t, uint64_t> CallerMacroByID;
    llvm::SmallVector<MacroEnv, 32> MacroStack;

    // Recover a token-interval-based caller relation among macro items.
    // After sorting by begin token (and outer-before-inner on ties), maintain
    // a stack of currently open macro envelopes. The top of the stack is the
    // innermost enclosing macro, so if the current envelope is strictly
    // contained within it, that stack top is this macro's caller/parent in the
    // nesting chain. Equal envelopes are not a parent/child relation.
    for (const MacroEnv &Env : MacroEnvs) {
      while (!MacroStack.empty() && Env.BTok >= MacroStack.back().ETok)
        MacroStack.pop_back();
      if (!MacroStack.empty() && Env.BTok >= MacroStack.back().BTok &&
          Env.ETok <= MacroStack.back().ETok &&
          (Env.BTok > MacroStack.back().BTok ||
           Env.ETok < MacroStack.back().ETok))
        CallerMacroByID.try_emplace(Env.ID, MacroStack.back().ID);
      MacroStack.push_back(Env);
    }

    llvm::DenseMap<uint64_t, uint64_t> ResolvedCallerByID;
    ResolvedCallerByID.reserve(MacroEnvs.size());
    for (const Item &It : Items) {
      if (It.Kind != IK_Macro)
        continue;
      if (It.CallerMacroId) {
        ResolvedCallerByID[It.ID] = *It.CallerMacroId;
        continue;
      }
      auto CallerIt = CallerMacroByID.find(It.ID);
      if (CallerIt != CallerMacroByID.end())
        ResolvedCallerByID[It.ID] = CallerIt->second;
    }

    llvm::DenseSet<uint64_t> DoneCallerIds;
    for (const Item &It : Items) {
      if (It.Kind != IK_Macro || DoneCallerIds.find(It.ID) != DoneCallerIds.end())
        continue;

      llvm::SmallVector<uint64_t, 8> Path;
      llvm::DenseMap<uint64_t, unsigned> PathIndex;
      uint64_t Cur = It.ID;

      while (true) {
        if (DoneCallerIds.find(Cur) != DoneCallerIds.end())
          break;

        auto Inserted = PathIndex.try_emplace(Cur, Path.size());
        if (!Inserted.second) {
          for (unsigned I = Inserted.first->second; I < Path.size(); ++I)
            ResolvedCallerByID.erase(Path[I]);
          break;
        }

        Path.push_back(Cur);

        auto CurIt = ResolvedCallerByID.find(Cur);
        if (CurIt == ResolvedCallerByID.end())
          break;

        const uint64_t CallerId = CurIt->second;
        if (CallerId == Cur || ItemByID.find(CallerId) == ItemByID.end()) {
          ResolvedCallerByID.erase(CurIt);
          break;
        }

        Cur = CallerId;
      }

      for (uint64_t Id : Path)
        DoneCallerIds.insert(Id);
    }

    const LangOptions &Lang = PP.getLangOpts();

    // Scan raw invocation text for occurrences of the caller macro's *formal
    // parameter* identifiers. This is used to record which caller parameters
    // are referenced by the callee's (decoded) replacement text.
    //
    // We use Clang's raw lexer so that:
    //   * comments are treated as whitespace (not scanned for identifiers),
    //   * literals are tokenized as single tokens (so internal punctuation
    //     doesn't affect scanning), and
    //   * identifier rules match the language mode (C/C++).
    auto collectCallerFormalDeps =
        [&Lang](llvm::StringRef Text,
                const llvm::StringMap<uint32_t> &NameToIdx)
        -> std::vector<uint32_t> {
      llvm::SmallVector<uint32_t, 8> Deps;

      // Record each referenced formal at most once.
      auto addDep = [&](uint32_t I) {
        for (uint32_t E : Deps)
          if (E == I)
            return;
        Deps.push_back(I);
      };

      if (Text.empty())
        return {};

      // Lex the snippet as raw source text so we can find identifier-like
      // mentions of caller formal names without needing AST structure.
      const SourceLocation BaseLoc = SourceLocation::getFromRawEncoding(1);
      std::string LexBuf = Text.str();
      LexBuf.push_back('\0');
      const char *BufStart = LexBuf.data();
      const char *BufEnd = BufStart + Text.size();
      Lexer Lex(BaseLoc, Lang, BufStart, BufStart, BufEnd);
      Token Tok;

      while (true) {
        Lex.LexFromRawLexer(Tok);
        if (Tok.is(tok::eof))
          break;

        // Only identifiers can name caller formals.
        if (!(Tok.is(tok::identifier) || Tok.is(tok::raw_identifier)))
          continue;

        // Recover the identifier spelling directly from the original text using
        // the raw-lexer byte offset, then map it to a caller formal index.
        const unsigned Off =
            Tok.getLocation().getRawEncoding() - BaseLoc.getRawEncoding();
        llvm::StringRef Ident(Text.data() + Off, Tok.getLength());

        auto It = NameToIdx.find(Ident);
        if (It != NameToIdx.end())
          addDep(It->getValue());
      }

      // Return a stable sorted dependency list.
      std::sort(Deps.begin(), Deps.end());
      return std::vector<uint32_t>(Deps.begin(), Deps.end());
    };

    // Like collectCallerFormalDeps(), but records each identifier occurrence's
    // byte span within inv_text (absolute file bytes via BaseOff + token
    // offset).
    auto collectCallerFormalRefs =
        [&Lang](llvm::StringRef Text, uint64_t BaseOff,
                const llvm::StringMap<uint32_t> &NameToIdx)
        -> std::vector<Item::InvArgRef> {
      std::vector<Item::InvArgRef> Refs;

      if (Text.empty())
        return Refs;

      // Raw-lex the invocation text so we can find identifier occurrences and
      // recover their byte offsets directly from the original source snippet.
      const SourceLocation BaseLoc = SourceLocation::getFromRawEncoding(1);
      std::string LexBuf = Text.str();
      LexBuf.push_back('\0');
      const char *BufStart = LexBuf.data();
      const char *BufEnd = BufStart + Text.size();
      Lexer Lex(BaseLoc, Lang, BufStart, BufStart, BufEnd);
      Token Tok;

      while (true) {
        Lex.LexFromRawLexer(Tok);
        if (Tok.is(tok::eof))
          break;

        // Only identifiers can refer to caller formals.
        if (!(Tok.is(tok::identifier) || Tok.is(tok::raw_identifier)))
          continue;

        // Recover the matched identifier spelling from the original text using
        // the raw-lexer byte offset, then map it to a caller formal index.
        const unsigned Off =
            Tok.getLocation().getRawEncoding() - BaseLoc.getRawEncoding();
        llvm::StringRef Ident(Text.data() + Off, Tok.getLength());

        auto It = NameToIdx.find(Ident);
        if (It != NameToIdx.end()) {
          Item::InvArgRef R;
          R.CallerParamIndex = It->second;
          // Store the identifier's absolute byte span within the invocation
          // file, not just its offset within Text.v
          R.ByteBegin = static_cast<uint32_t>(BaseOff + Off);
          R.ByteEnd = static_cast<uint32_t>(BaseOff + Off + Tok.getLength());
          Refs.push_back(R);
        }
      }

      return Refs;
    };

    // Per-macro-invocation computed data:
    //   - ArgDepsByID[macro_item_id][arg_i] = sorted unique list of
    //     caller-formal parameter indices referenced (by name) inside
    //     invocation argument i.
    //   - ArgRefsByID[macro_item_id][arg_i] = concrete byte ranges (in
    //     inv_text) where those caller-formal names occur, for later
    //     projection/rewrites.
    //
    // We compute these for each macro invocation Item that has:
    //   * a known caller macro (CallerMacroId), and
    //   * caller definition parameter names (DefParams), and
    //   * invocation argument byte ranges (InvArgRanges) + inv_text.
    llvm::DenseMap<uint64_t, std::vector<std::vector<uint32_t>>> ArgDepsByID;
    llvm::DenseMap<uint64_t, std::vector<std::vector<Item::InvArgRef>>>
        ArgRefsByID;

    for (const Item &It : Items) {
      // Only macro invocation items participate in caller-arg dependency/ref
      // scanning.
      if (It.Kind != IK_Macro)
        continue;

      // Resolve the caller macro item ID from the sanitized caller graph.
      auto CallerIt = ResolvedCallerByID.find(It.ID);
      if (CallerIt == ResolvedCallerByID.end())
        continue; // no caller => cannot interpret “caller formal” references
      const uint64_t CallerID = CallerIt->second;

      // Look up the caller Item (the macro invocation that produced *this*
      // one). We need its formal parameter list to build name→index mapping.
      const Item *Caller = ItemByID.lookup(CallerID);
      if (!Caller || Caller->DefParams.empty() || It.InvArgRanges.empty())
        continue;

      // Build a map from caller formal parameter name -> parameter index.
      // Empty names are ignored (defensive: should not happen for normal
      // params).
      llvm::StringMap<uint32_t> NameToIdx;
      for (uint32_t I = 0; I < Caller->DefParams.size(); ++I) {
        llvm::StringRef Nm(Caller->DefParams[I].Name);
        if (!Nm.empty())
          NameToIdx.try_emplace(Nm, I);
      }
      if (NameToIdx.empty())
        continue;

      // Deps/Refs are indexed by invocation argument index (parallel to
      // InvArgRanges).
      std::vector<std::vector<uint32_t>> Deps;
      std::vector<std::vector<Item::InvArgRef>> Refs;
      Deps.reserve(It.InvArgRanges.size());
      Refs.reserve(It.InvArgRanges.size());

      // For each argument range in the invocation, slice the corresponding text
      // out of inv_text and scan it for occurrences of caller formal names.
      for (const auto &R : It.InvArgRanges) {
        // Validate all required spans:
        //  - argument begin/end must exist
        //  - invocation begin/end must exist
        //  - arg range must lie within invocation range
        //  - arg end must be >= arg begin
        //
        // If anything is off, record empty results for this arg and keep going.
        if (!R.first || !R.second || !It.InvBegin || !It.InvEnd ||
            *It.InvBegin > *R.first || *R.second > *It.InvEnd ||
            !(*R.second >= *R.first)) {
          Deps.push_back({});
          Refs.push_back({});
          continue;
        }

        // Convert absolute byte offsets in inv_file -> relative byte offsets in
        // inv_text. inv_text corresponds to [InvBegin, InvEnd) in the original
        // invocation file buffer.
        const uint64_t B = *R.first - *It.InvBegin;
        const uint64_t E = *R.second - *It.InvBegin;

        llvm::StringRef ArgText = It.InvText;

        // Defensive bounds: if begin is past inv_text, treat as missing.
        if (B >= ArgText.size()) {
          Deps.push_back({});
          Refs.push_back({});
          continue;
        }

        // Clamp the end to inv_text size to avoid out-of-bounds slices.
        const uint64_t EClamped = std::min(E, (uint64_t)ArgText.size());
        llvm::StringRef Slice = ArgText.slice(B, EClamped);

        // collectCallerFormalDeps:
        //   returns stable (sorted) unique caller-formal indices referenced in
        //   Slice.
        Deps.push_back(collectCallerFormalDeps(Slice, NameToIdx));

        // collectCallerFormalRefs:
        //   returns byte ranges in inv_text (BaseOff + local offsets) where
        //   each caller-formal name occurs, used for precise projection later.
        Refs.push_back(collectCallerFormalRefs(Slice, B, NameToIdx));
      }

      // Store per-item results keyed by macro invocation item ID.
      ArgDepsByID.try_emplace(It.ID, std::move(Deps));
      ArgRefsByID.try_emplace(It.ID, std::move(Refs));
    }

    // Materialize the macro nesting DAG and dependency edges onto the
    // corresponding macro items so downstream consumers can rely on the data
    // without recomputing it.
    for (Item &It : Items) {
      if (It.Kind != IK_Macro)
        continue;

      // Materialize the sanitized caller graph onto the serialized items.
      auto CIt = ResolvedCallerByID.find(It.ID);
      if (CIt != ResolvedCallerByID.end())
        It.CallerMacroId = CIt->second;
      else
        It.CallerMacroId.reset();

      It.InvArgDeps.clear();
      auto DIt = ArgDepsByID.find(It.ID);
      if (DIt != ArgDepsByID.end())
        It.InvArgDeps = DIt->second;

      auto RIt = ArgRefsByID.find(It.ID);
      if (RIt != ArgRefsByID.end())
        It.InvArgRefs = RIt->second;
    }

    auto hasRealTokenEnvelope = [](const Item &It) -> bool {
      for (const TokenSpan &S : It.Spans)
        if (S.End > S.Begin)
          return true;
      return false;
    };

    auto appendMergedTokenSpan = [](std::vector<TokenSpan> &Dst,
                                    const TokenSpan &S) {
      if (S.End <= S.Begin)
        return;
      if (!Dst.empty() && Dst.back().End >= S.Begin) {
        Dst.back().End = std::max(Dst.back().End, S.End);
        return;
      }
      TokenSpan T = S;
      T.Open = false;
      Dst.push_back(T);
    };

    // Some higher-order intermediate invocations (e.g. BAR(FUNC) -> FOO(X,10)
    // -> FUNC(...)) can legitimately produce no directly-attributed tokens: all
    // printed output is owned by nested child expansions. Once caller links are
    // known, synthesize a stable token envelope for such zero-width placeholder
    // items from their *direct* children only. This avoids broad ancestor
    // attribution while still preserving the intermediate nesting the consumer
    // needs.
    bool SynthChanged = false;
    for (unsigned Pass = 0; Pass < 8; ++Pass) {
      SynthChanged = false;
      for (Item &Parent : Items) {
        if (Parent.Kind != IK_Macro || hasRealTokenEnvelope(Parent))
          continue;

        llvm::SmallVector<TokenSpan, 8> ChildSpans;
        for (const Item &Child : Items) {
          if (Child.Kind != IK_Macro || !Child.CallerMacroId ||
              *Child.CallerMacroId != Parent.ID || !hasRealTokenEnvelope(Child))
            continue;
          for (const TokenSpan &S : Child.Spans)
            if (S.End > S.Begin)
              ChildSpans.push_back(S);
        }

        if (ChildSpans.empty())
          continue;

        llvm::sort(ChildSpans, [](const TokenSpan &A, const TokenSpan &B) {
          if (A.Begin != B.Begin)
            return A.Begin < B.Begin;
          return A.End < B.End;
        });

        Parent.Spans.clear();
        Parent.BodySpans.clear();
        for (const TokenSpan &S : ChildSpans) {
          appendMergedTokenSpan(Parent.Spans, S);
          appendMergedTokenSpan(Parent.BodySpans, S);
        }

        if (!Parent.Spans.empty())
          SynthChanged = true;
      }
      if (!SynthChanged)
        break;
    }

    // Propagate paste_spans upward through the macro nesting DAG only for a
    // narrow whole-argument direct-pass-through case.
    //
    // This is a producer-side optimization for paste bookkeeping only. The
    // generalized nested-macro refolding policy is implemented in the consumer
    // by inverting arg_refs/template slices hop-by-hop; it does not rely on
    // this upward propagation succeeding for wrapped or otherwise structured
    // arguments.
    llvm::DenseMap<uint64_t, Item *> ItemByIDMut;
    ItemByIDMut.reserve(Items.size());
    for (Item &It : Items)
      ItemByIDMut[It.ID] = &It;

    // Return true iff callee argument CalleeArgIdx is a direct pass-through of
    // exactly one caller formal parameter (allowing only trivial outer parens),
    // and report that caller parameter index via OutCallerParamIdx.
    auto isDirectCallerFormalRef =
        [&](const Item &Callee, uint32_t CalleeArgIdx, const Item &Caller,
            uint32_t &OutCallerParamIdx) -> bool {
      if (!Callee.CallerMacroId)
        return false;
      if (!Callee.InvBegin || Callee.InvText.empty())
        return false;
      if (CalleeArgIdx >= Callee.InvArgRefs.size())
        return false;
      const auto &Refs = Callee.InvArgRefs[CalleeArgIdx];
      if (Refs.size() != 1)
        return false;

      const uint32_t CallerParam = Refs[0].CallerParamIndex;
      if (CallerParam >= Caller.DefParams.size())
        return false;

      if (CalleeArgIdx >= Callee.InvArgRanges.size())
        return false;
      const auto &Range = Callee.InvArgRanges[CalleeArgIdx];
      if (!Range.first || !Range.second)
        return false;

      // InvArgRanges are absolute offsets in the invocation site file; convert
      // them to indices within inv_text.
      if (*Range.first < *Callee.InvBegin || *Range.second < *Range.first)
        return false;
      uint64_t RelB = *Range.first - *Callee.InvBegin;
      uint64_t RelE = *Range.second - *Callee.InvBegin;
      if (RelB > Callee.InvText.size() || RelE > Callee.InvText.size() ||
          RelE < RelB)
        return false;

      llvm::StringRef ArgText =
          llvm::StringRef(Callee.InvText).substr(RelB, RelE - RelB).trim();
      llvm::StringRef ParamName =
          llvm::StringRef(Caller.DefParams[CallerParam].Name);

      // Allow trivial parenthesis wrapping: "(a)", "((a))", etc.
      for (;;) {
        llvm::StringRef T = ArgText.trim();
        if (T.size() < 2 || T.front() != '(' || T.back() != ')')
          break;

        int Depth = 0;
        bool Balanced = true;
        for (char C : T) {
          if (C == '(')
            ++Depth;
          else if (C == ')') {
            --Depth;
            if (Depth < 0) {
              Balanced = false;
              break;
            }
          }
        }
        if (!Balanced || Depth != 0)
          break;

        ArgText = T.drop_front().drop_back().trim();
      }

      if (ArgText != ParamName)
        return false;

      OutCallerParamIdx = CallerParam;
      return true;
    };

    auto containsPasteSpan = [](const Item &It, const ArgTokenSpan &S) -> bool {
      for (const ArgTokenSpan &E : It.PasteSpans) {
        if (E.Begin != S.Begin || E.End != S.End)
          continue;
        if (E.ArgIndex != S.ArgIndex || E.Open != S.Open ||
            E.HasByteRange != S.HasByteRange)
          continue;
        if (S.HasByteRange &&
            (E.ByteBegin != S.ByteBegin || E.ByteEnd != S.ByteEnd))
          continue;
        return true;
      }
      return false;
    };

    for (Item &It : Items) {
      if (It.Kind != IK_Macro || It.PasteSpans.empty())
        continue;

      // Iterate over a snapshot to avoid interacting with spans we may add to
      // this invocation as a propagation target.
      llvm::SmallVector<ArgTokenSpan, 8> BaseSpans;
      BaseSpans.append(It.PasteSpans.begin(), It.PasteSpans.end());

      for (const ArgTokenSpan &S0 : BaseSpans) {
        if (!S0.ArgIndex)
          continue;

        uint64_t CurID = It.ID;
        uint32_t CurArg = *S0.ArgIndex;
        ArgTokenSpan CurSpan = S0;

        // Walk outward through caller_macro_id links only while the entire
        // invocation argument is a direct reference to a single caller formal.
        // More general wrapped/template-structured arguments are handled later
        // by the consumer using arg_refs.
        for (unsigned Depth = 0; Depth < 128; ++Depth) {
          Item *CurIt = ItemByIDMut.lookup(CurID);
          if (!CurIt || !CurIt->CallerMacroId)
            break;

          Item *Caller = ItemByIDMut.lookup(*CurIt->CallerMacroId);
          if (!Caller)
            break;

          uint32_t CallerParamIdx = 0;
          if (!isDirectCallerFormalRef(*CurIt, CurArg, *Caller, CallerParamIdx))
            break;

          CurSpan.ArgIndex = CallerParamIdx;
          if (!containsPasteSpan(*Caller, CurSpan))
            Caller->PasteSpans.push_back(CurSpan);

          CurID = Caller->ID;
          CurArg = CallerParamIdx;
        }
      }
    }

    auto sameArgTokenSpan = [](const ArgTokenSpan &A,
                               const ArgTokenSpan &B) -> bool {
      if (A.Begin != B.Begin || A.End != B.End)
        return false;
      if (A.ArgIndex != B.ArgIndex || A.Open != B.Open ||
          A.HasByteRange != B.HasByteRange)
        return false;
      if (A.HasByteRange &&
          (A.ByteBegin != B.ByteBegin || A.ByteEnd != B.ByteEnd))
        return false;
      return true;
    };

    auto containsArgTokenSpan = [&](const std::vector<ArgTokenSpan> &V,
                                    const ArgTokenSpan &S) -> bool {
      for (const ArgTokenSpan &E : V)
        if (sameArgTokenSpan(E, S))
          return true;
      return false;
    };

    // Propagate descendant arg-like provenance upward through lexical source
    // nesting inside invocation arguments. If a parent macro is invoked with an
    // argument that itself contains a macro invocation in source text, then any
    // arg/stringify/paste output produced by that nested child is also
    // descendant provenance for the enclosing parent formal.
    //
    // This is a producer-side convenience for the consumer's generalized DAG
    // lifting: the consumer can still recover the same structure by inspecting
    // source ranges directly, but serializing the upward-propagated spans makes
    // parent-level candidate discovery deterministic and local.
    bool Changed = false;
    for (unsigned Pass = 0; Pass < 8; ++Pass) {
      Changed = false;
      for (Item &Parent : Items) {
        if (Parent.Kind != IK_Macro || !Parent.InvBegin || !Parent.InvEnd ||
            Parent.InvText.empty() || Parent.InvArgRanges.empty() ||
            Parent.InvFile.empty())
          continue;

        for (uint32_t ArgIdx = 0; ArgIdx < Parent.InvArgRanges.size(); ++ArgIdx) {
          const auto &R = Parent.InvArgRanges[ArgIdx];
          if (!R.first || !R.second || *R.second < *R.first)
            continue;

          for (const Item &Child : Items) {
            if (Child.Kind != IK_Macro || Child.ID == Parent.ID ||
                !Child.InvBegin || !Child.InvEnd || Child.InvFile.empty())
              continue;
            if (Child.InvFile != Parent.InvFile)
              continue;
            if (*Child.InvBegin < *R.first || *Child.InvEnd > *R.second ||
                *Child.InvEnd <= *Child.InvBegin)
              continue;

            auto copySpans = [&](const std::vector<ArgTokenSpan> &Src,
                                 std::vector<ArgTokenSpan> &Dst) {
              for (const ArgTokenSpan &S : Src) {
                ArgTokenSpan T = S;
                T.ArgIndex = ArgIdx;
                if (!containsArgTokenSpan(Dst, T)) {
                  Dst.push_back(T);
                  Changed = true;
                }
              }
            };

            copySpans(Child.ArgSpans, Parent.ArgSpans);
            copySpans(Child.StringifySpans, Parent.StringifySpans);
            copySpans(Child.PasteSpans, Parent.PasteSpans);
          }
        }
      }
      if (!Changed)
        break;
    }

    // items...
    JO.attributeArray("items", [&] {
      for (const Item &It : Items) {
        JO.object([&] {
          JO.attribute("id", It.ID);
          const char *KindStr = (It.Kind == IK_Directive) ? "directive"
                                : (It.Kind == IK_Macro)   ? "macro"
                                                          : "file";
          JO.attribute("kind", KindStr);

          if (!It.Subkind.empty())
            JO.attribute("subkind", It.Subkind);
          if (!It.Name.empty())
            JO.attribute("name", It.Name);
          if (!It.Text.empty())
            JO.attribute("text", It.Text);

          // Invocation metadata only applies to macro items per the JSON
          // schema.
          if (It.Kind == IK_Macro) {
            // InvText can't be empty at this point
            JO.attribute("inv_text", It.InvText);

            if (It.NormalizedInvText)
              JO.attribute("normalized_inv_text", *It.NormalizedInvText);

            if (!It.InvFile.empty())
              JO.attribute("inv_file", It.InvFile);

            // inv_b / inv_e are byte offsets within inv_file and must be
            // non-negative. If the invocation range is unknown, we omit both
            // fields.
            if (!It.InvFile.empty() && It.InvBegin && It.InvEnd &&
                *It.InvEnd >= *It.InvBegin) {
              JO.attribute("inv_b", *It.InvBegin);
              JO.attribute("inv_e", *It.InvEnd);
            }

            if (!It.InvArgRanges.empty()) {
              JO.attributeArray("inv_arg_ranges", [&] {
                for (const auto &R : It.InvArgRanges) {
                  JO.object([&] {
                    JO.attribute("b", R.first ? llvm::json::Value(*R.first)
                                              : llvm::json::Value(nullptr));
                    JO.attribute("e", R.second ? llvm::json::Value(*R.second)
                                               : llvm::json::Value(nullptr));
                  });
                }
              });
            }

            if (!It.NormalizedInvArgTextRanges.empty()) {
              JO.attributeArray("normalized_inv_arg_text_ranges", [&] {
                for (const auto &R : It.NormalizedInvArgTextRanges) {
                  JO.object([&] {
                    JO.attribute("b", R.first ? llvm::json::Value(*R.first)
                                              : llvm::json::Value(nullptr));
                    JO.attribute("e", R.second ? llvm::json::Value(*R.second)
                                               : llvm::json::Value(nullptr));
                  });
                }
              });
            }

            // Derive inv_pp_byte_begin / inv_pp_byte_end from this macro's
            // PP-token envelope.
            //
            // We compute the A-token interval [minBeginTok, maxEndTok) over all
            // TokenSpan entries for this macro expansion, then map the
            // first/last token to PP-byte offsets.
            bool HaveTokEnv = false;
            uint64_t BTok = 0;
            uint64_t ETok = 0;

            for (const TokenSpan &S : It.Spans) {
              if (!HaveTokEnv) {
                BTok = S.Begin;
                ETok = S.End;
                HaveTokEnv = true;
              } else {
                BTok = std::min(BTok, S.Begin);
                ETok = std::max(ETok, S.End);
              }
            }

            if (HaveTokEnv && ETok > BTok) {
              const uint64_t LastTok = ETok - 1;
              if (BTok < TokPPByteBegin.size() &&
                  LastTok < TokPPByteEnd.size()) {
                JO.attribute("inv_pp_byte_begin", TokPPByteBegin[(size_t)BTok]);
                JO.attribute("inv_pp_byte_end", TokPPByteEnd[(size_t)LastTok]);
              }
            }
          }

          if (It.Kind == IK_File)
            JO.attribute("path", TUSourcePath);

          // Emit site anchors for all directive kinds.
          if (It.Kind == IK_Directive) {
            if (It.SiteBegin && It.SiteEnd) {
              JO.attribute("site_b", *It.SiteBegin);
              JO.attribute("site_e", *It.SiteEnd);
            } else {
              JO.attribute("site_b", nullptr);
              JO.attribute("site_e", nullptr);
            }
            // Always write site_path; provide deterministic fallback for
            // virtual buffers.
            std::string Path = It.SitePath;
            if (Path.empty()) {
              if (SM.isWrittenInBuiltinFile(It.Loc))
                Path = "<built-in>";
              else if (SM.isWrittenInCommandLineFile(It.Loc))
                Path = "<command-line>";
            }
            if (!Path.empty())
              JO.attribute("site_path", Path);
            else
              JO.attribute("site_path", "<unknown>");

            if (It.Subkind == "#include" || It.Subkind == "#include_next") {
              if (!It.Decls.empty()) {
                JO.attributeArray("decls", [&] {
                  for (const auto &D : It.Decls) {
                    JO.object([&] {
                      JO.attribute("kind", D.Kind);
                      JO.attribute("name", D.Name);
                      JO.attributeObject("header_span", [&] {
                        // Header file path is implied by the include's resolved_path.
                        // Omitting it here significantly reduces map size for large headers.
                        JO.attribute("b", D.HeaderB);
                        JO.attribute("e", D.HeaderE);
                      });
                      JO.attributeObject("pp_span", [&] {
                        JO.attribute("begin", D.PPBegin);
                        JO.attribute("end", D.PPEnd);
                      });
                    });
                  }
                });
              }
            }
          }

          // Include-site anchors and structure for partial refolding.
          if (It.Subkind == "#include" || It.Subkind == "#include_next") {
            JO.attribute("target", It.TargetAsWritten); // can't be empty
            if (!It.ResolvedPath.empty())
              JO.attribute("resolved_path", It.ResolvedPath);
            JO.attribute("angled", It.IsAngled);
            if (It.Parent)
              JO.attribute("parent", *It.Parent);
          }

          // owner_include_id only for MACRO items and #define/#undef
          // directives
          if (It.OwnerIncludeId) {
            if (It.Kind == IK_Macro) {
              JO.attribute("owner_include_id", *It.OwnerIncludeId);
            } else if (It.Kind == IK_Directive &&
                       (It.Subkind == "#define" || It.Subkind == "#undef")) {
              JO.attribute("owner_include_id", *It.OwnerIncludeId);
            }
          }

          // Macro-token origin spans
          if (It.Kind == IK_Macro) {
            if (!It.ArgSpans.empty()) {
              JO.attributeArray("arg_spans", [&] {
                for (const ArgTokenSpan &S : It.ArgSpans) {
                  JO.object([&] {
                    JO.attribute("begin", S.Begin);
                    JO.attribute("end", S.End);
                    if (S.ArgIndex)
                      JO.attribute("arg_index", *S.ArgIndex);
                    if (S.End > S.Begin && S.End <= TokPPByteBegin.size()) {
                      JO.attribute("pp_byte_begin", TokPPByteBegin[S.Begin]);
                      JO.attribute("pp_byte_end", TokPPByteEnd[S.End - 1]);
                    }
                  });
                }
              });
            }
            if (!It.StringifySpans.empty()) {
              JO.attributeArray("stringify_spans", [&] {
                for (const ArgTokenSpan &S : It.StringifySpans) {
                  JO.object([&] {
                    JO.attribute("begin", S.Begin);
                    JO.attribute("end", S.End);
                    if (S.ArgIndex)
                      JO.attribute("arg_index", *S.ArgIndex);
                    if (S.End > S.Begin && S.End <= TokPPByteBegin.size()) {
                      JO.attribute("pp_byte_begin", TokPPByteBegin[S.Begin]);
                      JO.attribute("pp_byte_end", TokPPByteEnd[S.End - 1]);
                    }
                  });
                }
              });
            }
            if (!It.PasteSpans.empty()) {
              JO.attributeArray("paste_spans", [&] {
                for (const ArgTokenSpan &S : It.PasteSpans) {
                  JO.object([&] {
                    JO.attribute("begin", S.Begin);
                    JO.attribute("end", S.End);
                    if (S.ArgIndex)
                      JO.attribute("arg_index", *S.ArgIndex);
                    if (S.HasByteRange) {
                      JO.attribute("byte_begin", S.ByteBegin);
                      JO.attribute("byte_end", S.ByteEnd);
                    }
                    if (S.End > S.Begin && S.End <= TokPPByteBegin.size()) {
                      JO.attribute("pp_byte_begin", TokPPByteBegin[S.Begin]);
                      JO.attribute("pp_byte_end", TokPPByteEnd[S.End - 1]);
                    }
                  });
                }
              });
            }

            // Macro provenance DAG fields.
            if (!It.DefParams.empty()) {
              JO.attributeArray("def_params", [&] {
                for (const MacroParam &P : It.DefParams) {
                  JO.object([&] {
                    JO.attribute("name", P.Name);
                    JO.attribute("variadic", P.Variadic);
                  });
                }
              });
            }

            if (It.CallerMacroId)
              JO.attribute("caller_macro_id", *It.CallerMacroId);

            JO.attributeObject("callee_origin", [&] {
              switch (It.CalleeOrigin.Kind) {
              case MCO_LiteralMacroName:
                JO.attribute("kind", "literal_macro_name");
                break;
              case MCO_CallerParam:
                JO.attribute("kind", "caller_param");
                break;
              case MCO_Paste:
                JO.attribute("kind", "paste");
                break;
              case MCO_Opaque:
                JO.attribute("kind", "opaque");
                break;
              }
              if (!It.CalleeOrigin.CallerParamIndices.empty()) {
                JO.attributeArray("caller_param_indices", [&] {
                  for (uint32_t Idx : It.CalleeOrigin.CallerParamIndices)
                    JO.value(Idx);
                });
              }
            });

            if (!It.InvArgDeps.empty()) {
              JO.attributeArray("arg_deps", [&] {
                for (const auto &ArgDeps : It.InvArgDeps) {
                  JO.array([&] {
                    for (uint32_t Dep : ArgDeps)
                      JO.value(Dep);
                  });
                }
              });
            }

            if (!It.InvArgRefs.empty()) {
              JO.attributeArray("arg_refs", [&] {
                for (const auto &ArgRefs : It.InvArgRefs) {
                  JO.array([&] {
                    for (const auto &Ref : ArgRefs) {
                      JO.object([&] {
                        JO.attribute("caller_param_index",
                                     Ref.CallerParamIndex);
                        JO.attribute("byte_begin", Ref.ByteBegin);
                        JO.attribute("byte_end", Ref.ByteEnd);
                      });
                    }
                  });
                }
              });
            }

            if (!It.InvArgTupleRefs.empty()) {
              JO.attributeArray("arg_tuple_refs", [&] {
                for (const auto &ArgRefs : It.InvArgTupleRefs) {
                  JO.array([&] {
                    for (const auto &Ref : ArgRefs) {
                      JO.object([&] {
                        JO.attribute("caller_param_index",
                                     Ref.CallerParamIndex);
                        JO.attribute("caller_byte_begin", Ref.CallerByteBegin);
                        JO.attribute("caller_byte_end", Ref.CallerByteEnd);
                      });
                    }
                  });
                }
              });
            }
            if (!It.BodySpans.empty()) {
              JO.attributeArray("body_spans", [&] {
                for (const TokenSpan &S : It.BodySpans) {
                  JO.object([&] {
                    JO.attribute("begin", S.Begin);
                    JO.attribute("end", S.End);
                  });
                }
              });
            }
          }

          JO.attributeArray("spans", [&] {
            for (const auto &S : It.Spans) {
              JO.object([&] {
                JO.attribute("begin", S.Begin);
                JO.attribute("end", S.End);
              });
            }
          });
        });
      }
    });

    // tokmap...
    JO.attributeArray("tokmap", [&] {
      for (const auto &M : TokMap) {
        JO.object([&] {
          JO.attribute("file", M.File);
          JO.attribute("pp", M.PPIndex);
          JO.attribute("b", M.SrcBegin);
          JO.attribute("e", M.SrcEnd);
        });
      }
    });

    // Collect per-arm slot seeds so we can emit "arm_begin"/"arm_end" slots
    // later.
    struct ArmSlotSeed {
      std::string File;
      uint64_t ArmId;
      uint64_t BodyB;
      uint64_t BodyE;
      std::optional<uint64_t>
          OwnerIncludeId; // nullopt => no owning include (TU)

      // Optional PP anchors for this arm's body in the A-side (preprocessed)
      // token stream. Only populated for the selected arm in this run.
      std::optional<uint64_t> PPBegin;
      std::optional<uint64_t> PPEnd;
    };

    std::vector<ArmSlotSeed> ArmSlotSeeds;

    uint64_t NextCondGroupId = 0;
    uint64_t NextCondArmId = 0;

    // conds...
    JO.attributeArray("conds", [&] {
      // Compute a PPSpan [PPBegin, PPEnd) for a byte range [BodyB, BodyE)
      // within a given source file, using TokMap (which is in PP order).
      //
      // Return value:
      //   - true  => at least one PP token from this file overlapped
      //   [BodyB,BodyE)
      //             (we treat the arm as "selected" for this run).
      //   - false => no such token; PPBegin/PPEnd will still be set to a
      //             deterministic insertion point, but the arm is not selected.
      auto computeArmPPSpan = [&](llvm::StringRef File, uint64_t BodyB,
                                  uint64_t BodyE, uint64_t &PPBegin,
                                  uint64_t &PPEnd) -> bool {
        bool FoundAny = false;
        uint64_t Begin = 0;
        uint64_t End = 0;

        // Normal case: collect all PP tokens whose source span overlaps [BodyB,
        // BodyE).
        for (const TokMapEntry &TM : TokMap) {
          if (TM.File != File)
            continue;

          // No overlap if token ends at/before BodyB, or starts at/after BodyE.
          if (TM.SrcEnd <= BodyB || TM.SrcBegin >= BodyE)
            continue;

          uint64_t PP = TM.PPIndex;
          if (!FoundAny) {
            Begin = PP;
            FoundAny = true;
          }
          // TokMap is in PP order, so this monotonically increases.
          End = PP + 1;
        }

        if (!FoundAny) {
          // Degenerate / empty body case: pick a deterministic insertion point.
          //
          // We choose the first PP index whose SrcBegin >= BodyE as the
          // insertion point; if none, we fall back to "after the last token"
          // for this file; if the file has no tokens at all, we use 0.
          bool AnyForFile = false;
          bool InsertSet = false;
          uint64_t Insert = 0;

          for (const TokMapEntry &TM : TokMap) {
            if (TM.File != File)
              continue;
            AnyForFile = true;

            if (!InsertSet) {
              if (TM.SrcBegin >= BodyE) {
                Insert = TM.PPIndex;
                InsertSet = true;
                break;
              }

              // Track "just after" the last token strictly before BodyB.
              if (TM.SrcEnd <= BodyB)
                Insert = TM.PPIndex + 1;
            }
          }

          if (!AnyForFile) {
            Insert = 0;
          } else if (!InsertSet) {
            // All tokens are before BodyE; Insert already holds "last + 1".
          }

          Begin = Insert;
          End = Insert;
        }

        PPBegin = Begin;
        PPEnd = End;
        return FoundAny;
      };

      // Helper: emit all conditional groups for a single file (TU or header),
      // computing the enclosing *arm* parent (if any) and per-arm
      // selected/pp_span. NOTE: FilePath must be non-empty
      auto emitGroups = [&](llvm::StringRef FilePath,
                            std::optional<uint64_t> parentIncId, bool isTU) {
        llvm::StringRef Buf;

        // Prefer SourceManager buffers (handles VFS and remaps).
        if (auto FER = SM.getFileManager().getOptionalFileRef(FilePath)) {
          FileID FID = SM.translateFile(*FER); // FileID, not Optional
          if (FID.isValid()) {
            if (auto MB = SM.getBufferOrNone(FID))
              Buf = MB->getBuffer(); // Optional<MemoryBufferRef> ->
                                     // MB->getBuffer()
          }
        }

        // Fallback to filesystem read.
        if (Buf.empty()) {
          if (auto MB = llvm::MemoryBuffer::getFile(FilePath))
            Buf = (*MB)->getMemBufferRef().getBuffer();
        }
        if (Buf.empty())
          return;

        auto Groups = scanTopLevelConds(Buf, FilePath);
        llvm::errs() << "[refold] conds: " << (isTU ? "TU " : "") << FilePath
                     << " -> " << Groups.size() << " group(s)\n";

        for (const auto &G : Groups) {
          if (G.Arms.empty())
            continue;

          const uint64_t GroupB = G.GroupB;
          const uint64_t GroupE = G.GroupE;

          // Find the innermost arm (if any) that textually contains this group.
          // We restrict to the same file + include-instance (parentIncId).
          std::optional<uint64_t> ParentArmId;
          uint64_t ParentArmBodyB = 0;
          bool HaveParent = false;
          for (const auto &Seed : ArmSlotSeeds) {
            if (Seed.File != G.File)
              continue;
            if (Seed.OwnerIncludeId != parentIncId)
              continue;

            if (Seed.BodyB <= GroupB && GroupE <= Seed.BodyE) {
              if (!HaveParent || Seed.BodyB >= ParentArmBodyB) {
                HaveParent = true;
                ParentArmBodyB = Seed.BodyB;
                ParentArmId = Seed.ArmId;
              }
            }
          }

          uint64_t ThisGroupId = NextCondGroupId++;
          JO.object([&] {
            JO.attribute("id", ThisGroupId);
            JO.attribute("file", G.File);

            // parent = enclosing *arm* id, or elide if no parent
            if (HaveParent)
              JO.attribute("parent_arm_id", *ParentArmId);

            JO.attribute("group_b", G.GroupB);
            JO.attribute("group_e", G.GroupE);

            if (parentIncId)
              JO.attribute("parent_include_id", *parentIncId);

            // Single-arm groups get their arm body widened to the full group
            // range, so nested groups + trailing text are counted as part of
            // that arm.
            const bool SingleArmGroup = (G.Arms.size() == 1);

            JO.attributeArray("arms", [&] {
              for (const auto &A : G.Arms) {
                // Compute the effective body range we will use for:
                //  - computing pp_span (which tokens belong to this arm), and
                //  - parent lookup for nested groups (ArmSlotSeeds).
                uint64_t ArmBodyB = A.BodyB;
                uint64_t ArmBodyE = A.BodyE;

                if (SingleArmGroup) {
                  // For #ifdef FOO ... #endif with no #else/#elif, the *entire*
                  // group body belongs to this single arm, including any nested
                  // conditionals and trailing lines before the closing #endif.
                  //
                  // We widen the body to [GroupB, GroupE) on the right, which:
                  //  - makes pp_span for the outer arm cover all PP tokens
                  //    produced under FOO, and
                  //  - ensures nested groups' [GroupB,GroupE) fall inside this
                  //    arm's [BodyB,BodyE) so they can correctly pick this as
                  //    their parent arm.
                  ArmBodyE = GroupE;
                }

                uint64_t ArmId = NextCondArmId++;

                uint64_t PPBegin = 0, PPEnd = 0;
                bool IsSelected = computeArmPPSpan(G.File, ArmBodyB, ArmBodyE,
                                                   PPBegin, PPEnd);

                JO.object([&] {
                  JO.attribute("id", ArmId);
                  JO.attribute("kind", A.Kind);
                  if (!A.Cond.empty())
                    JO.attribute("cond", A.Cond);
                  JO.attribute("body_b", ArmBodyB);
                  JO.attribute("body_e", ArmBodyE);

                  // Mark whether this arm actually contributed any PP tokens
                  // in this preprocessing run.
                  JO.attribute("selected", IsSelected);

                  // Only emit pp_span for the taken arm; for untaken arms we
                  // leave it out entirely.
                  if (IsSelected) {
                    JO.attributeObject("pp_span", [&] {
                      JO.attribute("begin", PPBegin);
                      JO.attribute("end", PPEnd);
                    });
                  }
                });

                // Remember where this arm's body lives, so we can:
                //  - resolve nested groups' parents deterministically, and
                //  - emit arm_begin/arm_end slots later.
                std::optional<uint64_t> ArmPPBegin;
                std::optional<uint64_t> ArmPPEnd;
                if (IsSelected) {
                  ArmPPBegin = PPBegin;
                  ArmPPEnd = PPEnd;
                }

                ArmSlotSeeds.push_back(ArmSlotSeed{G.File, ArmId, ArmBodyB,
                                                   ArmBodyE, parentIncId,
                                                   ArmPPBegin, ArmPPEnd});
              }
            });
          });
        }
      };

      // 1) Main translation unit groups.
      emitGroups(TUSourcePath, /*parentIncId=*/std::nullopt, /*isTU=*/true);

      // 2) Each include/include_next instance (per-instance, no dedup).
      for (const auto &It : Items) {
        if (!(It.Subkind == "#include" || It.Subkind == "#include_next"))
          continue;
        if (It.ResolvedPath.empty())
          continue;
        emitGroups(It.ResolvedPath, It.ID, /*isTU=*/false);
      }
    });

    // slots...
    JO.attributeArray("slots", [&] {
      uint64_t SlotId = 0;
      auto emitPoint = [&](llvm::StringRef FilePath, uint64_t off,
                           const char *kind,
                           std::optional<uint64_t> ref = std::nullopt,
                           std::optional<uint64_t> owner = std::nullopt,
                           std::optional<uint64_t> pp = std::nullopt) {
        JO.object([&] {
          JO.attribute("id", SlotId++);
          JO.attribute("file", FilePath.str());
          JO.attribute("kind", kind);
          JO.attribute("b", off);
          JO.attribute("e", off);
          if (ref)
            JO.attribute("ref", *ref);
          if (owner)
            JO.attribute("owner_include_id", *owner);
          if (pp)
            JO.attribute("pp", *pp);
        });
      };

      // Map a (file, byte offset) to a deterministic PP insertion gap index.
      //
      // Semantics:
      //   - returns the PP index of the first PP token whose source span either
      //     contains Off or begins at/after Off.
      //   - if no such token exists but the file contributed at least one PP
      //     token, returns (last PP index in file + 1), i.e. the gap after the
      //     file's final token.
      //   - if the file contributed no PP tokens (e.g. not included / not
      //     taken), returns nullopt (slot will omit "pp").
      auto ppIndexForFileOffset = [&](llvm::StringRef FilePath,
                                      uint64_t Off) -> std::optional<uint64_t> {
        bool Any = false;
        std::optional<uint64_t> Best;
        std::optional<uint64_t> Last;
        uint64_t MinBegin = std::numeric_limits<uint64_t>::max();

        for (const auto &TM : TokMap) {
          if (TM.File != FilePath)
            continue;
          Any = true;

          uint64_t Idx = TM.PPIndex;
          if (!Last || Idx > *Last)
            Last = Idx;

          if (TM.SrcBegin < MinBegin)
            MinBegin = TM.SrcBegin;

          // Prefer the earliest token that contains Off; otherwise the
          // earliest token that begins at/after Off.
          if ((TM.SrcBegin <= Off && Off < TM.SrcEnd) || (TM.SrcBegin >= Off)) {
            if (!Best || Idx < *Best)
              Best = Idx;
          }
        }

        if (!Any)
          return std::nullopt;

        // If Off lies *before* the first byte that contributes any printed
        // PP tokens for this file, then there is no sensible PP anchor for
        // Off (e.g. directive-only preambles like #include lines).
        if (MinBegin != std::numeric_limits<uint64_t>::max() && Off < MinBegin)
          return std::nullopt;

        if (Best)
          return Best;
        return Last ? *Last + 1 : 0;
      };

      // Find the byte offset immediately after the last top-level
      // #include/#include_next directive (ignoring directives nested under #if
      // blocks). This provides a stable anchor when we need to inject synthetic
      // include-like material.
      auto computeAfterLastInclude = [&](llvm::StringRef Buf) -> size_t {
        if (Buf.empty())
          return 0;
        size_t N = Buf.size();
        size_t p = 0;
        unsigned depth = 0;
        size_t lastEnd = 0;
        while (p < N) {
          auto span = lineSpanOf(Buf, p);
          size_t bol = span.first, eol = span.second;
          size_t q = bol;
          while (q < eol && isSpace(Buf[q]))
            ++q;
          if (q < eol && Buf[q] == '#') {
            ++q;
            while (q < eol && isSpace(Buf[q]))
              ++q;
            auto kw = [&](const char *s) -> bool {
              size_t t = q, k = 0;
              while (t < eol && s[k] && Buf[t] == s[k]) {
                ++t;
                ++k;
              }
              return !s[k] && (t == eol || !(isalnum((unsigned char)Buf[t]) ||
                                             Buf[t] == '_'));
            };
            if (kw("if") || kw("ifdef") || kw("ifndef")) {
              depth++;
            } else if (kw("endif")) {
              if (depth > 0)
                depth--;
            } else if (depth == 0 && (kw("include") || kw("include_next"))) {
              lastEnd = eol;
            }
          }
          p = eol;
        }
        return lastEnd;
      };

      // file-level slots for TU
      {
        llvm::StringRef Buf;
        size_t size = 0;

        if (auto MB = SM.getBufferOrNone(SM.getMainFileID())) {
          Buf = MB->getBuffer(); // authoritative (respects VFS/remaps)
          size = Buf.size();
        } else {
          auto &Diags = PP.getDiagnostics();
          unsigned ID = Diags.getCustomDiagID(
              clang::DiagnosticsEngine::Fatal,
              "[refold-map] TU buffer unavailable for '%0'");
          Diags.Report(ID) << TUSourcePath;
        }

        size_t after = computeAfterLastInclude(Buf);

        // Compute the PP-token boundary after the last include *output* in the
        // TU. Note: the bytes that contain the #include directive itself do not
        // appear in the printed PP stream, so tokmap cannot anchor these
        // offsets.
        std::optional<uint64_t> afterLastIncludePP = std::nullopt;
        {
          bool Have = false;
          uint64_t MaxE = 0;
          for (const auto &It : Items) {
            if (!(It.Subkind == "#include" || It.Subkind == "#include_next"))
              continue;
            if (It.SitePath != TUSourcePath)
              continue;

            for (const auto &S : It.Spans) {
              if (S.End > MaxE)
                MaxE = S.End;
              Have = true;
            }
          }
          if (Have)
            afterLastIncludePP = MaxE;
          else
            afterLastIncludePP = ppIndexForFileOffset(TUSourcePath, after);
        }

        emitPoint(TUSourcePath, 0, "file_begin", /*ref=*/std::nullopt,
                  /*owner=*/std::nullopt,
                  ppIndexForFileOffset(TUSourcePath, 0));
        emitPoint(TUSourcePath, size, "file_end", /*ref=*/std::nullopt,
                  /*owner=*/std::nullopt,
                  ppIndexForFileOffset(TUSourcePath, size));
        emitPoint(TUSourcePath, after, "after_last_include",
                  /*ref*/ std::nullopt,
                  /*owner=*/std::nullopt, afterLastIncludePP);
      }

      // include before/after slots + file-level slots per included header
      // instance
      for (const auto &It : Items) {
        if (It.Subkind == "#include" || It.Subkind == "#include_next") {
          if (!It.SitePath.empty()) {
            std::optional<uint64_t> ppB;
            std::optional<uint64_t> ppE;
            if (!It.Spans.empty()) {
              uint64_t MinB = std::numeric_limits<uint64_t>::max();
              uint64_t MaxE = 0;
              for (const auto &S : It.Spans) {
                if (S.Begin < MinB)
                  MinB = S.Begin;
                if (S.End > MaxE)
                  MaxE = S.End;
              }
              if (MinB != std::numeric_limits<uint64_t>::max()) {
                ppB = MinB;
                ppE = MaxE;
              }
            }
            if (It.SiteBegin) {
              emitPoint(It.SitePath, *It.SiteBegin, "before_include", It.ID,
                        /*owner=*/std::nullopt, ppB);
            }
            if (It.SiteEnd) {
              emitPoint(It.SitePath, *It.SiteEnd, "after_include", It.ID,
                        /*owner=*/std::nullopt, ppE);
            }
          }
          if (!It.ResolvedPath.empty()) {
            auto MB = llvm::MemoryBuffer::getFile(It.ResolvedPath);
            llvm::StringRef HBuf;
            size_t HSize = 0;
            if (MB) {
              HBuf = (*MB)->getMemBufferRef().getBuffer();
              HSize = HBuf.size();
            }
            size_t after = computeAfterLastInclude(HBuf);
            emitPoint(It.ResolvedPath, 0, "file_begin", std::nullopt, It.ID,
                      ppIndexForFileOffset(It.ResolvedPath, 0));
            emitPoint(It.ResolvedPath, HSize, "file_end", std::nullopt, It.ID,
                      ppIndexForFileOffset(It.ResolvedPath, HSize));
            emitPoint(It.ResolvedPath, after, "after_last_include",
                      std::nullopt, It.ID,
                      ppIndexForFileOffset(It.ResolvedPath, after));
          }
        }
      }

      // arm_begin / arm_end slots for each conditional arm body
      for (const auto &AS : ArmSlotSeeds) {
        // TU-local arms will have OwnerIncludeId == -1, so we omit
        // owner_include_id.
        auto ownerId = AS.OwnerIncludeId; // nullopt => no owner_include_id

        // Begin of the arm body
        emitPoint(AS.File, AS.BodyB, "arm_begin",
                  /*ref=*/AS.ArmId,
                  /*owner=*/ownerId,
                  /*pp=*/AS.PPBegin);

        // End of the arm body
        emitPoint(AS.File, AS.BodyE, "arm_end",
                  /*ref=*/AS.ArmId,
                  /*owner=*/ownerId,
                  /*pp=*/AS.PPEnd);
      }
    });
  });
}

} // namespace refold
} // namespace clang
