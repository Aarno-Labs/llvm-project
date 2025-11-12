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
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

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
std::vector<CondGroup> scanTopLevelConds(llvm::StringRef Buf,
                                         llvm::StringRef FilePath) {
  std::vector<CondGroup> out;
  const size_t N = Buf.size();
  size_t p = 0;

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

  while (p < N) {
    auto span = lineSpanOf(Buf, p);
    size_t bol = span.first, eol = span.second;
    if (eol <= bol) {
      ++p;
      continue;
    } // zero-length/degenerate line

    size_t s = bol;
    while (s < eol && isSpace(Buf[s]))
      ++s;

    // Start of a conditional group?
    if (s < eol && Buf[s] == '#') {
      size_t q = s + 1;
      while (q < eol && isSpace(Buf[q]))
        ++q;

      bool starts = false;
      llvm::StringRef startTag;
      if (kw_at(q, eol, "if")) {
        starts = true;
        startTag = "if";
      } else if (kw_at(q, eol, "ifdef")) {
        starts = true;
        startTag = "ifdef";
      } else if (kw_at(q, eol, "ifndef")) {
        starts = true;
        startTag = "ifndef";
      }

      if (starts) {
        CondGroup G;
        G.File = FilePath.str();
        G.GroupB = bol;

        // First arm (#if/ifdef/ifndef)
        CondArm A;
        A.Tag = startTag.str();
        size_t condBeg = q + (A.Tag == "if" ? 2 : A.Tag == "ifdef" ? 5 : 6);
        while (condBeg < eol && isSpace(Buf[condBeg]))
          ++condBeg;
        A.Cond = std::string(Buf.substr(condBeg, eol - condBeg));
        A.BodyB = std::min(N, eol);
        A.BodyE = A.BodyB;
        G.Arms.push_back(std::move(A));

        // Walk to the peer #endif, handling nesting.
        size_t r = std::min(N, eol);
        size_t last_r = ~size_t(0);
        int depth = 0;
        bool closed = false;

        auto setPrevBodyEnd = [&](size_t endAt) {
          if (!G.Arms.empty() && G.Arms.back().BodyE == G.Arms.back().BodyB)
            G.Arms.back().BodyE = endAt;
        };

        while (r < N) {
          auto span2 = lineSpanOf(Buf, r);
          size_t bol2 = span2.first, eol2 = span2.second;
          if (eol2 <= bol2) {
            r = std::min(N, r + 1);
            continue;
          }

          size_t s2 = bol2;
          while (s2 < eol2 && isSpace(Buf[s2]))
            ++s2;

          if (s2 < eol2 && Buf[s2] == '#') {
            size_t t = s2 + 1;
            while (t < eol2 && isSpace(Buf[t]))
              ++t;

            // Nested open?
            if (kw_at(t, eol2, "if") || kw_at(t, eol2, "ifdef") ||
                kw_at(t, eol2, "ifndef")) {
              ++depth;
              r = std::min(N, eol2);
            }
            // Peer close?
            else if (kw_at(t, eol2, "endif")) {
              if (depth > 0) {
                --depth;
                r = std::min(N, eol2);
              } else {
                setPrevBodyEnd(bol2);
                G.GroupE = std::min(N, eol2);
                out.push_back(std::move(G));
                r = std::min(N, eol2);
                closed = true;
                break;
              }
            }
            // Peer elif/else at depth 0
            else if (depth == 0 && kw_at(t, eol2, "elif")) {
              setPrevBodyEnd(bol2);
              CondArm B;
              B.Tag = "elif";
              size_t condS = t + 4;
              while (condS < eol2 && isSpace(Buf[condS]))
                ++condS;
              B.Cond = std::string(Buf.substr(condS, eol2 - condS));
              B.BodyB = std::min(N, eol2);
              B.BodyE = B.BodyB;
              G.Arms.push_back(std::move(B));
              r = std::min(N, eol2);
            } else if (depth == 0 && kw_at(t, eol2, "else")) {
              setPrevBodyEnd(bol2);
              CondArm B;
              B.Tag = "else";
              B.BodyB = std::min(N, eol2);
              B.BodyE = B.BodyB;
              G.Arms.push_back(std::move(B));
              r = std::min(N, eol2);
            } else {
              r = std::min(N, eol2); // some other directive -> next line
            }
          } else {
            r = std::min(N, eol2); // non-pp line -> next
          }

          // progress guard
          if (r == last_r)
            r = std::min(N, r + 1);
          last_r = r;
        }

        // Unterminated group (missing #endif): close at EOF so we don't stall.
        if (!closed) {
          setPrevBodyEnd(N);
          G.GroupE = N;
          out.push_back(std::move(G));
        }

        // Continue outer scan after the group
        p = std::min(N, r);
        continue;
      }
    }

    // Not a group opener -> next line
    p = std::min(N, eol);
  }

  return out;
}
} // namespace

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
    return WantAbs ? absolutePathFor(*FER) : std::string(FER->getName());
  }
  return std::string();
}

void RefoldMapBuilder::onIncludeDirective(SourceLocation HashLoc,
                                          const Token &IncludeTok,
                                          StringRef FileName, bool IsAngled,
                                          CharSourceRange FilenameRange,
                                          OptionalFileEntryRef File) {
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
    It.ResolvedPath =
        EmitAbsPaths ? absolutePathFor(*File) : std::string(File->getName());
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

  Items.push_back(std::move(It));
  if (!IncludeStack.empty())
    Items.back().OwnerIncludeId = IncludeStack.back();
  MacroKey2Item[keyForLoc(SM, MacroNameTok.getLocation())] =
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

  // Prefer a macro item when the token is inside a macro expansion.
  if (SM.isMacroArgExpansion(L) || SM.isMacroBodyExpansion(L)) {
    SourceLocation Caller = SM.getImmediateMacroCallerLoc(L);
    auto It = MacroKey2Item.find(keyForLoc(SM, Caller));
    if (It == MacroKey2Item.end()) {
      // Fallback: some paths prefer expansion loc
      Caller = SM.getExpansionLoc(L);
      It = MacroKey2Item.find(keyForLoc(SM, Caller));
    }
    if (It != MacroKey2Item.end())
      ItemIdx = It->second;
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
  //     - ArgSpans for tokens from actual arguments
  //     - BodySpans for tokens from the macro body
  if (Items[ItemIdx].Kind == IK_Macro) {
    if (SM.isMacroArgExpansion(L)) {
      touchTokSpan(Items[ItemIdx].ArgSpans, TokIndex);
    } else if (SM.isMacroBodyExpansion(L)) {
      touchTokSpan(Items[ItemIdx].BodySpans, TokIndex);
    }
    // Tokens that are neither arg nor body (rare, e.g. builtins) remain covered
    // by the primary Spans via touchSpanForItem above.
  }

  // 2) Grow all active include items transitively so a parent include
  //    covers its entire subtree (nested includes/macros).
  for (int idx : IncludeStack) {
    if (idx < 0 || idx == ItemIdx)
      continue;
    touchSpanForItem(idx, TokIndex);
  }

  // 3) Capture byte range in the spelling file of this token (main or header).
  {
    SourceLocation FL = SM.getFileLoc(L);
    if (FL.isValid()) {
      SourceLocation EndL =
          Lexer::getLocForEndOfToken(FL, /*Offset=*/0, SM, Lang);
      SourceLocation FEL = SM.getFileLoc(EndL);
      if (FEL.isValid()) {
        long long B = SM.getFileOffset(FL);
        long long E = SM.getFileOffset(FEL);
        TokMapEntry M;
        M.PPIndex = TokIndex;
        M.SrcBegin = B;
        M.SrcEnd = E;
        M.File = filePathForLocAbs(SM, FL, EmitAbsPaths); // e.g. "./e.h"
        TokMap.push_back(std::move(M));
      }
    }
  }

  ++TokIndex;
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
    JO.attribute("version", "1.2");
    JO.attribute("source", TUAbsPath);

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
            JO.attribute("path", TUAbsPath);

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

          // NEW: macro-token origin spans
          if (It.Kind == IK_Macro) {
            if (!It.ArgSpans.empty()) {
              JO.attributeArray("arg_spans", [&] {
                for (const TokenSpan &S : It.ArgSpans) {
                  JO.object([&] {
                    JO.attribute("begin", S.Begin);
                    JO.attribute("end",   S.End);
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
          Diags.Report(ID) << TUAbsPath;
        }

        long long after = computeAfterLastInclude(Buf);
        emitPoint(TUAbsPath, 0, "file_begin");
        emitPoint(TUAbsPath, (long long)size, "file_end");
        emitPoint(TUAbsPath, after, "after_last_include");
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
    });

    int NextCondGroupId = 0;
    int NextCondArmId = 0;

    // conds...
    JO.attributeArray("conds", [&] {
      auto emitGroups = [&](llvm::StringRef FilePath, int parentIncId) {
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
        llvm::errs() << "[refold] conds: " << FilePath << " -> "
                     << Groups.size() << " group(s)\n";

        for (const auto &G : Groups) {
          if (G.Arms.empty())
            continue;
          JO.object([&] {
            JO.attribute("id", NextCondGroupId++);
            JO.attribute("file", G.File);
            JO.attribute("group_b", (uint64_t)G.GroupB);
            JO.attribute("group_e", (uint64_t)G.GroupE);
            if (parentIncId >= 0)
              JO.attribute("parent_include_id", parentIncId);
            JO.attributeArray("arms", [&] {
              for (const auto &A : G.Arms) {
                JO.object([&] {
                  JO.attribute("id", NextCondArmId++);
                  JO.attribute("tag", A.Tag);
                  if (!A.Cond.empty())
                    JO.attribute("cond", A.Cond);
                  JO.attribute("body_b", (uint64_t)A.BodyB);
                  JO.attribute("body_e", (uint64_t)A.BodyE);
                });
              }
            });
          });
        }
      };

      // 1) Main translation unit
      {
        FileID MFID = SM.getMainFileID();
        if (auto MB = SM.getBufferOrNone(MFID)) {
          llvm::StringRef Buf = MB->getBuffer();
          auto Groups = scanTopLevelConds(Buf, TUAbsPath);
          llvm::errs() << "[refold] conds: TU " << TUAbsPath << " -> "
                       << Groups.size() << " group(s)\n";
          for (const auto &G : Groups) {
            if (G.Arms.empty())
              continue;
            JO.object([&] {
              JO.attribute("id", NextCondGroupId++);
              JO.attribute("file", G.File);
              JO.attribute("group_b", (uint64_t)G.GroupB);
              JO.attribute("group_e", (uint64_t)G.GroupE);
              JO.attributeArray("arms", [&] {
                for (const auto &A : G.Arms) {
                  JO.object([&] {
                    JO.attribute("id", NextCondArmId++);
                    JO.attribute("tag", A.Tag);
                    if (!A.Cond.empty())
                      JO.attribute("cond", A.Cond);
                    JO.attribute("body_b", (uint64_t)A.BodyB);
                    JO.attribute("body_e", (uint64_t)A.BodyE);
                  });
                }
              });
            });
          }
        } else {
          llvm::errs() << "[refold] conds: TU buffer missing for " << TUAbsPath
                       << "\n";
        }
      }

      // 2) Each include/include_next instance (per-instance, no dedup)
      for (const auto &It : Items) {
        if (!(It.Subkind == "#include" || It.Subkind == "#include_next"))
          continue;
        if (It.ResolvedPath.empty())
          continue;
        emitGroups(It.ResolvedPath, It.ID);
      }
    });
  });
}

} // namespace refold
} // namespace clang
