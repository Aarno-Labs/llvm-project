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
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

namespace {
// Normalize keys to file locations so InclusionDirective(HashLoc)
// and EnterFile(IncludeLoc) agree.
std::string keyForLoc(const SourceManager &SM, SourceLocation Loc) {
  return std::to_string(SM.getFileLoc(Loc).getRawEncoding());
}

// Macro keys must NOT normalize through getFileLoc() or getSpellingLoc(): in
// nested expansions those can collapse distinct invocation sites onto the same
// key, causing MacroKey2Item collisions (e.g., __FILE__ overwriting PRINT_FILE).
// As in th example:
//
//   #define PRINT_FILE(FMT) printf(FMT, __FILE__, __LINE__)
//   PRINT_FILE("Error on file (%s) and line (%d)\n");
//
// Use the SourceLocation raw encoding directly (keeps MacroID locations distinct).
std::string keyForMacroLoc(SourceLocation Loc) {
  if (Loc.isInvalid())
    return "0";
  return std::to_string(Loc.getRawEncoding());
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v'; }

// Returns [bol,eol+1) byte span of the line containing 'p'.
std::pair<size_t, size_t> lineSpanOf(StringRef S, size_t p) {
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
///      * \c Tag  — the directive kind ("if", "ifdef", "ifndef", "elif",
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
///  - Nested conditionals are tracked with a simple depth counter; nested
///    groups are **skipped over**, and only the outermost (depth 0) group is
///    recorded.
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
std::vector<CondGroup>
scanTopLevelConds(llvm::StringRef Buf, llvm::StringRef FilePath) {
  std::vector<CondGroup> Groups;
  const size_t N = Buf.size();
  size_t p = 0;

  auto lineSpanOf = [&](llvm::StringRef B, size_t Off) -> std::pair<size_t, size_t> {
    if (Off >= B.size())
      return {B.size(), B.size()};
    size_t bol = Off;
    while (bol > 0 && B[bol - 1] != '\n')
      --bol;
    size_t eol = Off;
    while (eol < B.size() && B[eol] != '\n')
      ++eol;
    if (eol < B.size())
      ++eol; // include newline
    return {bol, eol};
  };

  auto isSpace = [](char c) -> bool {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
  };

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
      if (::isalnum(c) || c == '_')
        return false; // word boundary
    }
    return true;
  };

  // Helper to close the current arm body for a group up to 'endAt'.
  auto setPrevBodyEnd = [&](int groupIndex, size_t endAt) {
    if (groupIndex < 0 || groupIndex >= (int)Groups.size())
      return;
    auto &G = Groups[groupIndex];
    if (!G.Arms.empty() && G.Arms.back().BodyE == G.Arms.back().BodyB)
      G.Arms.back().BodyE = endAt;
  };

  struct Active {
    int GroupIndex;
  };
  std::vector<Active> Stack;

  while (p < N) {
    auto span = lineSpanOf(Buf, p);
    size_t bol = span.first, eol = span.second;
    if (eol <= bol) {
      p = std::min(N, eol + 1);
      continue; // empty/degenerate line
    }

    size_t s = bol;
    while (s < eol && isSpace(Buf[s]))
      ++s;

    if (s < eol && Buf[s] == '#') {
      size_t q = s + 1;
      while (q < eol && isSpace(Buf[q]))
        ++q;

      enum DirKind { DK_None, DK_If, DK_Ifdef, DK_Ifndef, DK_Elif, DK_Else, DK_Endif };
      DirKind Kind = DK_None;
      llvm::StringRef Tag;

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
        // Starting a new conditional group (top-level or nested).
        CondGroup G;
        G.File = FilePath.str();
        G.GroupB = bol;       // from '#' of opener
        G.GroupE = G.GroupB;  // will be filled at #endif

        // First arm (#if/ifdef/ifndef)
        CondArm A;
        A.Tag = Tag.str();

        size_t condBeg =
            q + (Kind == DK_If
                     ? 2
                     : (Kind == DK_Ifdef ? 5 : 6)); // "if", "ifdef", "ifndef"
        while (condBeg < eol && isSpace(Buf[condBeg]))
          ++condBeg;
        A.Cond = std::string(Buf.substr(condBeg, eol - condBeg));
        A.BodyB = std::min(N, eol); // body starts after this line
        A.BodyE = A.BodyB;
        G.Arms.push_back(std::move(A));

        int idx = (int)Groups.size();
        Groups.push_back(std::move(G));

        // Any outer group should have its current arm body end before this '#if'.
        if (!Stack.empty())
          setPrevBodyEnd(Stack.back().GroupIndex, bol);

        Stack.push_back(Active{idx});

        p = std::min(N, eol);
        continue;
      }

      case DK_Elif:
      case DK_Else: {
        if (Stack.empty()) {
          p = std::min(N, eol);
          continue; // stray elif/else
        }

        int idx = Stack.back().GroupIndex;
        CondGroup &G = Groups[idx];

        // Close previous arm at the start of this line.
        setPrevBodyEnd(idx, bol);

        CondArm A;
        A.Tag = Tag.str();
        if (Kind == DK_Elif) {
          size_t condBeg = q + 4; // "elif"
          while (condBeg < eol && isSpace(Buf[condBeg]))
            ++condBeg;
          A.Cond = std::string(Buf.substr(condBeg, eol - condBeg));
        } else {
          A.Cond.clear(); // "else" has no condition text
        }
        A.BodyB = std::min(N, eol);
        A.BodyE = A.BodyB;
        G.Arms.push_back(std::move(A));

        p = std::min(N, eol);
        continue;
      }

      case DK_Endif: {
        if (Stack.empty()) {
          p = std::min(N, eol);
          continue; // stray endif
        }

        int idx = Stack.back().GroupIndex;
        CondGroup &G = Groups[idx];

        // Close the last arm at start of this '#endif' line.
        setPrevBodyEnd(idx, bol);
        // Group extends through the end of this line.
        G.GroupE = std::min(N, eol);

        Stack.pop_back();

        p = std::min(N, eol);
        continue;
      }

      case DK_None:
        break;
      }
    }

    // Non-directive line; just advance.
    p = std::min(N, eol);
  }

  // If file ended without closing some groups, close them at EOF.
  for (const auto &A : Stack) {
    int idx = A.GroupIndex;
    if (idx < 0 || idx >= (int)Groups.size())
      continue;
    CondGroup &G = Groups[idx];
    setPrevBodyEnd(idx, N);
    if (G.GroupE < G.GroupB || G.GroupE > N)
      G.GroupE = N;
  }

  return Groups;
}

void computeInvArgRanges(const MacroArgs *Args, const MacroInfo *MI,
                         const SourceManager &SM, const LangOptions &Lang,
                         std::vector<std::pair<long long, long long>> &Out) {
  Out.clear();
  if (!Args || !MI || !MI->isFunctionLike())
    return;

  unsigned N = MI->getNumParams();
  Out.reserve(N);

  for (unsigned ai = 0; ai < N; ++ai) {
    const Token *AT = Args->getUnexpArgument(ai);
    if (!AT) {
      Out.emplace_back(-1, -1);
      continue;
    }

    bool Have = false;
    SourceLocation First, Last;
    for (const Token *T = AT; !T->is(tok::eof); ++T) {
      SourceLocation TL = T->getLocation();
      if (!TL.isValid())
        continue;
      if (!Have) {
        First = TL;
        Have = true;
      }
      Last = TL;
    }

    if (!Have) {
      Out.emplace_back(-1, -1);
      continue;
    }

    SourceLocation FL = SM.getFileLoc(First);
    SourceLocation LL = SM.getFileLoc(Last);
    SourceLocation EndL = Lexer::getLocForEndOfToken(LL, 0, SM, Lang);
    SourceLocation EL = SM.getFileLoc(EndL);

    long long B = FL.isValid() ? (long long)SM.getFileOffset(FL) : -1;
    long long E = EL.isValid() ? (long long)SM.getFileOffset(EL) : -1;
    Out.emplace_back(B, E);
  }
}

std::string computeLangStr(const clang::LangOptions &Lang) {
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

std::string normalizePathKey(llvm::StringRef Path, llvm::StringRef Cwd,
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

std::string joinSpelled(llvm::StringRef DirSpelling,
                               llvm::StringRef Rel) {
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
} // namespace

RefoldMapBuilder::RefoldMapBuilder(Preprocessor &PP, llvm::StringRef OutputPath)
    : PP(PP), SM(PP.getSourceManager()), Lang(PP.getLangOpts()),
      OutPath(OutputPath.str()) {
  const auto &PPO = PP.getPreprocessorOpts();

  // Prefer the driver-provided cwd spelling when available; fallback to process
  // cwd.
  Cwd = PPO.RefoldWorkingDir;
  llvm::SmallString<256> WD;
  if (!llvm::sys::fs::current_path(WD))
    Cwd = WD.str().str();
  else
    Cwd = ".";

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
    if (A == "-I") {
      if (i + 1 < PPO.RefoldPPArgv.size())
        addIncludeDirSpelling(PPO.RefoldPPArgv[++i]);
      continue;
    }
    if (A.starts_with("-I") && A.size() > 2) {
      addIncludeDirSpelling(A.drop_front(2));
      continue;
    }
  }

  // Resolve the TU path spelling from the driver argv. We match by base name to
  // avoid accidentally selecting an output file or other positional argument.
  OptionalFileEntryRef MainFER = SM.getFileEntryRefForID(SM.getMainFileID());
  std::string MainAbs;
  llvm::StringRef MainBase;
  if (MainFER) {
    MainAbs = absolutePathFor(*MainFER);
    MainBase = llvm::sys::path::filename(MainAbs);
  }

  // 1. Try to find the exact absolute path match in the arguments first.
  // This ensures we get the full path if provided.
  for (llvm::StringRef Arg : PPO.RefoldPPArgv) {
    if (Arg == MainAbs) {
      TUSourcePath = Arg.str();
      break;
    }
  }

  // 2. If no exact match, look for the argument that matches the filename.
  if (TUSourcePath.empty()) {
    for (llvm::StringRef Arg : PPO.RefoldPPArgv) {
      if (Arg.empty() || Arg.starts_with("-"))
        continue;

      if (!MainBase.empty() && llvm::sys::path::filename(Arg) == MainBase) {
        TUSourcePath = Arg.str();
        break; // Stop at the first match
      }
    }
  }

  // 3. Fallback logic remains as a safety net...
  if (TUSourcePath.empty()) {
    TUSourcePath = !MainAbs.empty() ? MainAbs : MainBase.str();
  }

  // Seed the spelling map so token locations in the main file report the TU
  // path spelling rather than an absolute canonical path.
  if (!MainAbs.empty())
    FileAbs2Spelling[MainAbs] = TUSourcePath;

  IgnoreComments = true;
}

std::pair<long long, long long>
RefoldMapBuilder::computeDirectiveLine(SourceLocation HashLoc) {
  SourceLocation H = SM.getFileLoc(HashLoc);
  if (!H.isValid())
    return {-1, -1};
  FileID FID = SM.getFileID(H);
  bool Invalid = false;
  StringRef Buf = SM.getBufferData(FID, &Invalid);
  if (Invalid)
    return {-1, -1};
  unsigned B = SM.getFileOffset(H);
  size_t N = Buf.size();
  size_t P = B;
  // Scan to end-of-line
  while (P < N && Buf[P] != '\n' && Buf[P] != '\r')
    ++P;
  size_t E = P;
  if (P < N) {
    // include EOL
    if (Buf[P] == '\r' && P + 1 < N && Buf[P + 1] == '\n')
      E = P + 2;
    else
      E = P + 1;
  }
  return {(long long)B, (long long)E};
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

int RefoldMapBuilder::argIndexForSpellingLoc(const Item &MI, SourceLocation Loc,
                                             SourceManager &Sm,
                                             const LangOptions &Lang,
                                             bool EmitAbsPaths) {
  // Goal:
  //   Given a token location (typically the spelling loc for a token that came
  //   out of a macro expansion), determine which *invocation-site argument slot*
  //   of macro invocation item MI produced that token.
  //
  // How:
  //   MI.InvFile names the physical file that contains the macro invocation text,
  //   and MI.InvArgRanges stores byte ranges (in that file) for each argument as
  //   written at the call site. We try to map the token's location back to a
  //   physical file location in MI.InvFile and then find the first argument range
  //   whose byte interval overlaps the token's byte interval.
  //
  // Return:
  //   * index of the argument (0-based) if we can prove the token originated
  //     from that argument at MI's call site
  //   * -1 otherwise
  if (Loc.isInvalid() || MI.InvFile.empty() || MI.InvArgRanges.empty())
    return -1;

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

    // Step 1: Try to interpret the current location as a *physical file location*
    // and see if it is inside the macro invocation file (MI.InvFile). If so,
    // compute the token's byte span and test it against the recorded invocation
    // argument ranges.
    //
    // Note: if CurrentLoc is a MacroID, Sm.getFileLoc(CurrentLoc) collapses
    // through macro layers to a file location; otherwise it is already a file
    // location.
    SourceLocation Fl = CurrentLoc.isMacroID() ? Sm.getFileLoc(CurrentLoc) : CurrentLoc;

    std::string TokFile = filePathForLocAbs(Sm, Fl, EmitAbsPaths);
    if (TokFile == MI.InvFile) {
      long long TokB = (long long)Sm.getFileOffset(Fl);
      SourceLocation EndL = Lexer::getLocForEndOfToken(Fl, 0, Sm, Lang);
      long long TokE = EndL.isValid() ? (long long)Sm.getFileOffset(EndL) : TokB;
      if (TokE < TokB) TokE = TokB;

      // The argument ranges are stored as byte intervals in the invocation file.
      // If this token overlaps any argument interval, we attribute it to that
      // argument slot.
      for (size_t Ai = 0; Ai < MI.InvArgRanges.size(); ++Ai) {
        const auto &R = MI.InvArgRanges[Ai];
        if (R.first < 0 || R.second < 0) continue;
        if (TokB < R.second && TokE > R.first)
          return (int)Ai;
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

  return -1;
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
  It.ID = (int)Items.size();
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
  auto [B, E] = computeDirectiveLine(HashLoc);
  It.SiteBegin = B;
  It.SiteEnd = E;
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
  if (!IncludeStack.empty())
    Items.back().OwnerIncludeId = IncludeStack.back();
  int ThisIdx = (int)Items.size() - 1;

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
  if (MI->isBuiltinMacro())
    return;

  Item It;
  It.ID = (int)Items.size();
  It.Kind = IK_Directive;
  It.Subkind = "#define";
  It.Loc = MI->getDefinitionLoc();
  std::string S;
  llvm::raw_string_ostream OS(S);
  PrintMacroDefinition(*MacroNameTok.getIdentifierInfo(), *MI, PP, &OS);
  OS << "\n";
  It.Text = OS.str();
  auto BE = computeDirectiveLine(MI->getDefinitionLoc());
  It.SiteBegin = BE.first;
  It.SiteEnd = BE.second;
  It.SitePath = filePathForLocAbs(SM, MI->getDefinitionLoc(), EmitAbsPaths);

  Items.push_back(std::move(It));
  if (!IncludeStack.empty())
    Items.back().OwnerIncludeId = IncludeStack.back();
}

void RefoldMapBuilder::onMacroUndefined(const Token &MacroNameTok,
                                        const MacroDefinition &MD,
                                        const MacroDirective *Undef) {
  if (!enabled())
    return;

  Item It;
  It.ID = (int)Items.size();
  It.Kind = IK_Directive;
  It.Subkind = "#undef";
  It.Loc = MacroNameTok.getLocation();
  std::string S = "#undef ";
  S += MacroNameTok.getIdentifierInfo()->getName().str();
  S += "\n";
  It.Text = std::move(S);
  auto BE = computeDirectiveLine(MacroNameTok.getLocation());
  It.SiteBegin = BE.first;
  It.SiteEnd = BE.second;
  It.SitePath = filePathForLocAbs(SM, MacroNameTok.getLocation(), EmitAbsPaths);

  Items.push_back(std::move(It));
  if (!IncludeStack.empty())
    Items.back().OwnerIncludeId = IncludeStack.back();
}

void RefoldMapBuilder::onMacroExpands(const Token &MacroNameTok,
                                      const MacroDefinition &MD,
                                      SourceRange Range,
                                      const MacroArgs *Args) {
  if (!enabled())
    return;

  const MacroInfo *MI = MD.getMacroInfo();
  Item It;
  It.ID = (int)Items.size();
  It.Kind = IK_Macro;
  It.Subkind = (MI && MI->isFunctionLike()) ? "func" : "obj";
  if (auto *II = MacroNameTok.getIdentifierInfo())
    It.Name = II->getName().str();
  It.Loc = Range.getBegin();

  SourceLocation EndTok =
      Lexer::getLocForEndOfToken(Range.getEnd(), /*Offset=*/0, SM, Lang);
  It.InvText =
      Lexer::getSourceText(
          CharSourceRange::getCharRange(Range.getBegin(), EndTok), SM, Lang)
          .str();
  It.IsBuiltinMacro = (MI != nullptr && MI->isBuiltinMacro());

  // byte offsets within the invocation's own file
  SourceLocation FB = SM.getFileLoc(Range.getBegin());
  SourceLocation FE = SM.getFileLoc(EndTok);
  if (FB.isValid() && FE.isValid()) {
    It.InvBegin = SM.getFileOffset(FB);
    It.InvEnd = SM.getFileOffset(FE);
    It.InvFile = filePathForLocAbs(SM, FB, EmitAbsPaths);
  } else {
    It.InvBegin = It.InvEnd = -1;
  }

  computeInvArgRanges(Args, MI, SM, Lang, It.InvArgRanges);

  Items.push_back(std::move(It));
  if (!IncludeStack.empty())
    Items.back().OwnerIncludeId = IncludeStack.back();
  MacroKey2Item[keyForMacroLoc(MacroNameTok.getLocation())] =
      (int)Items.size() - 1;
}

void RefoldMapBuilder::onPragma(SourceLocation HashLoc, StringRef FullText) {
  if (!enabled())
    return;

  Item It;
  It.ID = (int)Items.size();
  It.Kind = IK_Directive;
  It.Subkind = "#pragma";
  It.Loc = HashLoc;
  It.Text = FullText.str();
  // Site info (line byte span and file path).
  auto BE = computeDirectiveLine(HashLoc);
  It.SiteBegin = BE.first;
  It.SiteEnd = BE.second;
  It.SitePath = filePathForLocAbs(SM, HashLoc, EmitAbsPaths);

  Items.push_back(std::move(It));
  if (!IncludeStack.empty())
    Items.back().OwnerIncludeId = IncludeStack.back();
}

void RefoldMapBuilder::onEnterFile(SourceLocation IncludeLoc) {
  if (!enabled())
    return;

  int Idx = -1;
  if (IncludeLoc.isValid()) {
    auto It = IncludeKey2Item.find(keyForLoc(SM, IncludeLoc));
    if (It != IncludeKey2Item.end())
      Idx = It->second;
  }

  // Set parent relationship: the include we are about to enter is
  // conceptually a child of the current top of the stack (if any).
  if (Idx >= 0 && !IncludeStack.empty() && IncludeStack.back() >= 0) {
    Items[(size_t)Idx].Parent = IncludeStack.back();
  }

  IncludeStack.push_back(Idx);
}

void RefoldMapBuilder::onToken(const Token &Tok) {
  if (!enabled())
    return;
  if (Tok.is(tok::eof))
    return;
  if (IgnoreComments && Tok.is(tok::comment))
    return;

  int ItemIdx = -1;
  SourceLocation L = Tok.getLocation();

  // Precompute byte range in the spelling file of this token (main or header).
  // We reuse this both for TokMap and for robust macro arg/body attribution.
  std::string TokFile;
  long long TokB = -1;
  long long TokE = -1;
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
        HasTokMap = true;
      }
    }
  }

  // Prefer a macro item when the token is inside a macro expansion.
  if (SM.isMacroArgExpansion(L) || SM.isMacroBodyExpansion(L)) {
    auto LookupMacroItem = [&](SourceLocation Loc) -> int {
      if (Loc.isInvalid())
        return -1;
      auto It = MacroKey2Item.find(keyForMacroLoc(Loc));
      if (It != MacroKey2Item.end())
        return It->second;
      return -1;
    };

    // Innermost macro: immediate caller of this token location.
    SourceLocation Caller = SM.getImmediateMacroCallerLoc(L);
    int InnerIdx = LookupMacroItem(Caller);

    // Enclosing macro: walk up the macro caller chain (keeps MacroID hops intact).
    // Outer macro (if any): prefer ultimate expansion location. This is robust
    // when the immediate caller loc is a file location inside an outer macro body
    // (no MacroID chain to walk), which is exactly the nested builtin case we
    // care about.
    int OuterIdx = LookupMacroItem(SM.getExpansionLoc(L));
    if (OuterIdx < 0) {
      // Conservative fallback: walk up the caller chain when the caller is a MacroID.
      SourceLocation Cur = Caller;
      for (int Depth = 0; Depth < 16; ++Depth) {
        if (Cur.isInvalid() || !Cur.isMacroID())
          break;
        Cur = SM.getImmediateMacroCallerLoc(Cur);
        OuterIdx = LookupMacroItem(Cur);
        if (OuterIdx >= 0)
          break;
      }
    }

    // Fallback: some paths prefer expansion loc
    if (InnerIdx < 0) {
      Caller = SM.getExpansionLoc(L);
      InnerIdx = LookupMacroItem(Caller);
    }

    if (InnerIdx >= 0) {
      int Chosen = InnerIdx;

      // Clang sometimes reports adjacent punctuation as being "inside" a builtin
      // macro expansion. In those cases, prefer the enclosing macro item for
      // non-expansion tokens.
      if ((size_t)InnerIdx < Items.size() && OuterIdx >= 0) {
        const Item &MI = Items[(size_t)InnerIdx];
        if (MI.Kind == IK_Macro && MI.IsBuiltinMacro) {
          // Builtin/predefined macros should contribute only their expansion
          // token(s). If Clang attributes adjacent punctuation or other
          // non-expansion tokens to the builtin macro location, prefer the
          // enclosing macro item for those tokens.
          bool Ok = Tok.isLiteral() || Tok.is(tok::numeric_constant);
          if (!Ok)
            Chosen = OuterIdx;
        }
      }

      ItemIdx = Chosen;
    } else if (OuterIdx >= 0) {
      ItemIdx = OuterIdx;
    }
  }

  // Otherwise attribute to the innermost active include; else to the file item.
  if (ItemIdx == -1) {
    if (!IncludeStack.empty() && IncludeStack.back() != -1) {
      ItemIdx = IncludeStack.back();
    } else {
      if (CurrentFileItem == -1) {
        Item F;
        F.ID = (int)Items.size();
        F.Kind = IK_File;
        F.Subkind = "file";
        Items.push_back(std::move(F));
        CurrentFileItem = (int)Items.size() - 1;
      }
      ItemIdx = CurrentFileItem;
    }
  }

  // 1) Attribute the token to its primary item (macro, include, or file).
  touchSpanForItem(ItemIdx, TokIndex);

  // 1a) If the primary item is a MACRO expansion, also record exact origin:
  //     - ArgSpans for tokens from actual arguments (func-like only)
  //     - BodySpans for tokens from the macro body (and for obj-like macros)
  if (Items[ItemIdx].Kind == IK_Macro) {
    auto &It = Items[(size_t)ItemIdx];

    if (SM.isMacroArgExpansion(L)) {
      if (It.Subkind == "func") {
        int ArgIndex = argIndexForSpellingLoc(It, L, SM, Lang, EmitAbsPaths);
        if (ArgIndex >= 0)
          touchArgTokSpan(It.ArgSpans, TokIndex, ArgIndex);
        else
          touchTokSpan(It.BodySpans, TokIndex); // fallback: keep schema-valid
      } else {
        // Object-like macros have no arguments; treat as body-origin.
        touchTokSpan(It.BodySpans, TokIndex);
      }
    } else if (SM.isMacroBodyExpansion(L)) {
      touchTokSpan(It.BodySpans, TokIndex);
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
  if (HasTokMap && L.isMacroID()) {
    for (size_t I = 0; I < Items.size(); ++I) {
      if ((int)I == ItemIdx)
        continue;
      Item &MI = Items[I];
      if (MI.Kind != IK_Macro)
        continue;
      if (MI.InvBegin < 0 || MI.InvEnd < 0)
        continue;
      if (MI.InvFile != TokFile)
        continue;
      if (MI.InvBegin <= TokB && TokE <= MI.InvEnd) {
        // Ensure the enclosing macro's primary token span covers nested expansions.
        touchSpanForItem((int)I, TokIndex);

        // For function-like macros, anything after the name token is treated as
        // originating from an argument spelling region. Otherwise, treat as body.
        if (MI.Subkind == "func") {
          int ArgIndex = argIndexForSpellingLoc(MI, L, SM, Lang, EmitAbsPaths);
          if (ArgIndex >= 0)
            touchArgTokSpan(MI.ArgSpans, TokIndex, ArgIndex);
          else
            touchTokSpan(MI.BodySpans, TokIndex); // fallback: keep schema-valid
        } else {
          touchTokSpan(MI.BodySpans, TokIndex);
        }
      }
    }
  }

  // 2) Grow all active include items transitively so a parent include
  //    covers its entire subtree (nested includes/macros).
  for (int idx : IncludeStack) {
    if (idx < 0 || idx == ItemIdx)
      continue;
    touchSpanForItem(idx, TokIndex);
  }

  // 3) Emit TokMap entry (reuse the precomputed spelling-file span).
  if (HasTokMap) {
    TokMapEntry M;
    M.PPIndex = TokIndex;
    M.SrcBegin = TokB;
    M.SrcEnd = TokE;
    M.File = TokFile;
    TokMap.push_back(std::move(M));
  }

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

  auto isIdentLike = [](llvm::StringRef S) -> bool {
    if (S.empty())
      return false;
    auto isLetter = [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    auto isDigit = [](char c) { return (c >= '0' && c <= '9'); };

    char c0 = S.front();
    if (!(isLetter(c0) || c0 == '_'))
      return false;
    for (char c : S.drop_front()) {
      if (!(isLetter(c) || isDigit(c) || c == '_'))
        return false;
    }
    return true;
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
    llvm::SmallVector<unsigned, 64> PPIdxs;
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
          PPIdxs.push_back(static_cast<unsigned>(PP));
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

      unsigned PPBegin = PPIdxs[StartIdx];
      unsigned PPEnd   = PPIdxs[EndIdx - 1] + 1; // half-open
      if (PPBegin >= PPEnd || PPEnd > TokMap.size())
        return;

      const TokMapEntry &First = TokMap[PPBegin];
      const TokMapEntry &Last  = TokMap[PPEnd - 1];

      unsigned HeaderB = static_cast<unsigned>(First.SrcBegin);
      unsigned HeaderE = static_cast<unsigned>(Last.SrcEnd);

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

        // Track identifiers as we go.
        if (isIdentLike(Trimmed)) {
          LastIdentBeforeSemi = Trimmed;
          if (!SawLParen)
            LastIdentBeforeParen = Trimmed;
        }

        // Opening paren: we are entering parameter / declarator list.
        if (Trimmed.contains('(')) {
          SawLParen = true;
        }

        // Semicolon ends the simple-declaration.
        if (Trimmed.contains(';')) {
          break;
        }
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

      // Any token whose slice contains ';' terminates a decl.
      if (TokText.contains(';')) {
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
    JO.attribute("version", "1.3");

    const auto &PPO = PP.getPreprocessorOpts();
    std::string LangStr = computeLangStr(PP.getLangOpts());

    llvm::SmallString<256> CWD;
    std::string CwdStr;
    if (!llvm::sys::fs::current_path(CWD))
      CwdStr = CWD.str().str();
    llvm::SmallString<256> WD;
    if (!llvm::sys::fs::current_path(WD))
      CwdStr = WD.str().str();

    // Serailize the PP context of this clang instance
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
    JO.attributeObject("tokens", [&] { JO.attribute("count", TokIndex); });

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
          if (!It.InvText.empty())
            JO.attribute("inv_text", It.InvText);
          if (It.InvBegin >= 0 && It.InvEnd >= 0) {
            JO.attribute("inv_b", It.InvBegin);
            JO.attribute("inv_e", It.InvEnd);
            if (!It.InvFile.empty())
              JO.attribute("inv_file", It.InvFile);
          }

          if (It.Kind == IK_File)
            JO.attribute("path", TUSourcePath);

          // Emit site anchors for all directive kinds.
          if (It.Kind == IK_Directive) {
            if (It.SiteBegin >= 0 && It.SiteEnd >= 0) {
              JO.attribute("site_b", It.SiteBegin);
              JO.attribute("site_e", It.SiteEnd);
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

            if (It.Subkind == "#include" || It.Subkind == "#include_next") {
              if (!It.Decls.empty()) {
                JO.attributeArray("decls", [&] {
                  for (const auto &D : It.Decls) {
                    JO.object([&] {
                      JO.attribute("kind", D.Kind);
                      JO.attribute("name", D.Name);
                      JO.attributeObject("header_span", [&] {
                        JO.attribute("file", D.File);
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
            if (!It.TargetAsWritten.empty())
              JO.attribute("target", It.TargetAsWritten);
            if (!It.ResolvedPath.empty())
              JO.attribute("resolved_path", It.ResolvedPath);
            JO.attribute("angled", It.IsAngled);
            if (It.Parent >= 0)
              JO.attribute("parent", It.Parent);
          }

          // owner_include_id only for MACRO items and #define/#undef
          // directives
          if (It.OwnerIncludeId >= 0) {
            if (It.Kind == IK_Macro) {
              JO.attribute("owner_include_id", It.OwnerIncludeId);
            } else if (It.Kind == IK_Directive &&
                       (It.Subkind == "#define" || It.Subkind == "#undef")) {
              JO.attribute("owner_include_id", It.OwnerIncludeId);
            }
          }

          // Macro-token origin spans
          if (It.Kind == IK_Macro) {
            if (!It.ArgSpans.empty()) {
              JO.attributeArray("arg_spans", [&] {
                for (const ArgTokenSpan &S : It.ArgSpans) {
                  JO.object([&] {
                    JO.attribute("begin", S.Begin);
                    JO.attribute("end",   S.End);
                    if (S.ArgIndex >= 0)
                      JO.attribute("arg_index", (int64_t)S.ArgIndex);
                  });
                }
              });
            }
            if (!It.BodySpans.empty()) {
              JO.attributeArray("body_spans", [&] {
                for (const TokenSpan &S : It.BodySpans) {
                  JO.object([&] {
                    JO.attribute("begin", S.Begin);
                    JO.attribute("end",   S.End);
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
      int ArmId;
      long long BodyB;
      long long BodyE;
      int OwnerIncludeId; // -1 => no owning include (TU)
    };

    std::vector<ArmSlotSeed> ArmSlotSeeds;

    int NextCondGroupId = 0;
    int NextCondArmId = 0;

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
      auto computeArmPPSpan = [&](llvm::StringRef File, unsigned BodyB,
                                  unsigned BodyE, unsigned &PPBegin,
                                  unsigned &PPEnd) -> bool {
        bool FoundAny = false;
        unsigned Begin = 0;
        unsigned End = 0;

        // Normal case: collect all PP tokens whose source span overlaps [BodyB,
        // BodyE).
        for (const TokMapEntry &TM : TokMap) {
          if (TM.File != File)
            continue;

          // No overlap if token ends at/before BodyB, or starts at/after BodyE.
          if (TM.SrcEnd <= static_cast<long long>(BodyB) ||
              TM.SrcBegin >= static_cast<long long>(BodyE))
            continue;

          unsigned PP = static_cast<unsigned>(TM.PPIndex);
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
          unsigned Insert = 0;

          for (const TokMapEntry &TM : TokMap) {
            if (TM.File != File)
              continue;
            AnyForFile = true;

            if (!InsertSet) {
              if (TM.SrcBegin >= static_cast<long long>(BodyE)) {
                Insert = static_cast<unsigned>(TM.PPIndex);
                InsertSet = true;
                break;
              }

              // Track "just after" the last token strictly before BodyB.
              if (TM.SrcEnd <= static_cast<long long>(BodyB))
                Insert = static_cast<unsigned>(TM.PPIndex + 1);
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
      // selected/pp_span.
      auto emitGroups = [&](llvm::StringRef FilePath, int parentIncId,
                            bool isTU) {
        llvm::StringRef Buf;

        // Prefer SourceManager buffers (handles VFS and remaps).
        if (!FilePath.empty()) {
          if (auto FER = SM.getFileManager().getOptionalFileRef(FilePath)) {
            FileID FID = SM.translateFile(*FER); // FileID, not Optional
            if (FID.isValid()) {
              if (auto MB = SM.getBufferOrNone(FID))
                Buf = MB->getBuffer(); // Optional<MemoryBufferRef> ->
                                       // MB->getBuffer()
            }
          }
        }

        // Fallback to filesystem read.
        if (Buf.empty() && !FilePath.empty()) {
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

          const long long GroupB = static_cast<long long>(G.GroupB);
          const long long GroupE = static_cast<long long>(G.GroupE);

          // Find the innermost arm (if any) that textually contains this group.
          // We restrict to the same file + include-instance (parentIncId).
          int ParentArmId = -1;
          long long ParentArmBodyB = 0;
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

          int ThisGroupId = NextCondGroupId++;
          JO.object([&] {
            JO.attribute("id", ThisGroupId);
            JO.attribute("file", G.File);

            // parent = enclosing *arm* id, or null for top-level.
            if (HaveParent)
              JO.attribute("parent_arm_id", ParentArmId);
            else
              JO.attribute("parent_arm_id", nullptr);

            JO.attribute("group_b", static_cast<uint64_t>(G.GroupB));
            JO.attribute("group_e", static_cast<uint64_t>(G.GroupE));

            if (parentIncId >= 0)
              JO.attribute("parent_include_id",
                           static_cast<int64_t>(parentIncId));
            else
              JO.attribute("parent_include_id", nullptr);

            // *** NEW: single-arm groups get their arm body widened to the full
            // group range, so nested groups + trailing text are counted as part
            // of that arm.
            const bool SingleArmGroup = (G.Arms.size() == 1);

            JO.attributeArray("arms", [&] {
              for (const auto &A : G.Arms) {
                // Compute the effective body range we will use for:
                //  - computing pp_span (which tokens belong to this arm), and
                //  - parent lookup for nested groups (ArmSlotSeeds).
                long long ArmBodyB = static_cast<long long>(A.BodyB);
                long long ArmBodyE = static_cast<long long>(A.BodyE);

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

                int ArmId = NextCondArmId++;

                unsigned PPBegin = 0, PPEnd = 0;
                bool IsSelected = computeArmPPSpan(
                    G.File, static_cast<unsigned>(ArmBodyB),
                    static_cast<unsigned>(ArmBodyE), PPBegin, PPEnd);

                JO.object([&] {
                  JO.attribute("id", ArmId);
                  JO.attribute("tag", A.Tag);
                  if (!A.Cond.empty())
                    JO.attribute("cond", A.Cond);
                  JO.attribute("body_b", static_cast<uint64_t>(ArmBodyB));
                  JO.attribute("body_e", static_cast<uint64_t>(ArmBodyE));

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
                ArmSlotSeeds.push_back(ArmSlotSeed{G.File, ArmId, ArmBodyB,
                                                   ArmBodyE, parentIncId});
              }
            });
          });
        }
      };

      // 1) Main translation unit groups.
      emitGroups(TUSourcePath, /*parentIncId=*/-1, /*isTU=*/true);

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
      int SlotId = 0;
      auto emitPoint =
          [&](llvm::StringRef FilePath, long long off, const char *kind,
              std::optional<int> ref = std::nullopt, int owner = -1) {
            // const long long o = off < 0 ? 0 : off;  // clamp once here
            const long long o = off;
            JO.object([&] {
              JO.attribute("id", SlotId++);
              JO.attribute("file", FilePath.str());
              JO.attribute("kind", kind);
              JO.attribute("b", o);
              JO.attribute("e", o);
              if (ref.has_value())
                JO.attribute("ref", *ref);
              if (owner >= 0)
                JO.attribute("owner_include_id", owner);
            });
          };

      auto computeAfterLastInclude = [&](llvm::StringRef Buf) -> long long {
        if (Buf.empty())
          return 0;
        size_t N = Buf.size();
        size_t p = 0;
        int depth = 0;
        long long lastEnd = -1;
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
              lastEnd = (long long)eol;
            }
          }
          p = eol;
        }
        return lastEnd >= 0 ? lastEnd : 0;
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

        long long after = computeAfterLastInclude(Buf);
        emitPoint(TUSourcePath, 0, "file_begin");
        emitPoint(TUSourcePath, (long long)size, "file_end");
        emitPoint(TUSourcePath, after, "after_last_include");
      }

      // include before/after slots + file-level slots per included header
      // instance
      for (const auto &It : Items) {
        if (It.Subkind == "#include" || It.Subkind == "#include_next") {
          if (!It.SitePath.empty()) {
            if (It.SiteBegin >= 0)
              emitPoint(It.SitePath, It.SiteBegin, "before_include", It.ID);
            if (It.SiteEnd >= 0)
              emitPoint(It.SitePath, It.SiteEnd, "after_include", It.ID);
          }
          if (!It.ResolvedPath.empty()) {
            auto MB = llvm::MemoryBuffer::getFile(It.ResolvedPath);
            llvm::StringRef HBuf;
            size_t HSize = 0;
            if (MB) {
              HBuf = (*MB)->getMemBufferRef().getBuffer();
              HSize = HBuf.size();
            }
            long long after = computeAfterLastInclude(HBuf);
            emitPoint(It.ResolvedPath, 0, "file_begin", std::nullopt, It.ID);
            emitPoint(It.ResolvedPath, (long long)HSize, "file_end",
                      std::nullopt, It.ID);
            emitPoint(It.ResolvedPath, after, "after_last_include",
                      std::nullopt, It.ID);
          }
        }
      }

      // arm_begin / arm_end slots for each conditional arm body
      for (const auto &AS : ArmSlotSeeds) {
        // TU-local arms will have OwnerIncludeId == -1, so we omit
        // owner_include_id.
        int ownerId = AS.OwnerIncludeId; // -1 => no owner_include_id

        // Begin of the arm body
        emitPoint(AS.File, AS.BodyB, "arm_begin",
                  /*ref=*/AS.ArmId,
                  /*owner=*/ownerId);

        // End of the arm body
        emitPoint(AS.File, AS.BodyE, "arm_end",
                  /*ref=*/AS.ArmId,
                  /*owner=*/ownerId);
      }
    });
  });
}

} // namespace refold
} // namespace clang
