//===--- clang-refold.cpp - Main C source refolding utility -----*- C++ -*-===//
//
// Command-line front end for the C source refolding engine.
//
// This tool takes an original preprocessed stream **A** (.i), an edited
// preprocessed stream **B** (.i.mod), and a refold map **M** (JSON), and emits
// a partially expanded C translation unit (**.mod**) where only those
// preprocessor constructs whose expanded bytes were changed in **B** are
// re-materialized in source form.
//
// Synopsis:
//   clang-refold [--log-level=<value>] -p <A.i> -P <B.i.mod> -r <map.json> \
//     -o <out.c>
//
// Options:
//   -p, --pp           Path to original preprocessed input A (.i).
//   -P, --pp-mod       Path to edited preprocessed input B (.i.mod).
//   -r, --refold-map   Path to refold map JSON produced by the modified
//                      Clang preprocessor (.refold.json).
//   -o, --out          Path to write the refolded TU (.c.mod).
//   --emit-edit-map    Write B↔source byte ranges for materialized edits.
//  --log-level=<value> Set log level (default is --info)
//    =trace             -   Trace
//    =debug             -   Debug
//    =info              -   Info
//    =warn              -   Warn
//    =error             -   Error
//    =fatal             -   Fatal
//   --help             Display available options (--help-hidden for more)
//   --version          Display the version of this program
//
// Notes:
//   * All file paths must be absolute/canonical except textual include
//     targets, which are re-emitted as written in source.
//   * Output TU formatting is stable: internal whitespace of edited fragments
//     is preserved; only boundary spaces may be normalized to avoid double
//     spaces or token gluing at splice points.
//
// Determinism & Side Effects:
//   * No heuristics: if **M** lacks coverage for an edited region, the tool
//     fails with a specific diagnostic rather than guessing.
//   * Non-interactive; no network or environment-dependent state beyond
//     invoking Clang for raw token dumps.
//
// Example:
//   clang-refold \
//     -p test.c.i \
//     -P test.c.i.mod \
//     -r test.c.refold.json \
//     -o test.c.mod \
//     --verbose
//
// See also:
//   RefoldEngine
//   RefoldModel
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#include "RefoldLog.h"
#include "RefoldEngine.h"
#include "RefoldSchema.h"
#include "StringUtils.h"
#include "DiffAlgorithms.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/JSONSchemaValidator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

using namespace llvm;
using namespace clang;
using namespace clang::refold;

namespace {

// --------------------------- Tokenization ---------------------------------

/// \brief Lex a preprocessed byte buffer into Clang-style tokens.
///
/// Tokenizes the given preprocessed text using Clang's raw lexer so token
/// boundaries match `-E -P` behavior. The function clears and fills both
/// output arrays:
///  - `out` receives one `PPTok` per token with its exact byte spelling.
///  - `startOffs` receives the starting byte offset in the input buffer for
///    each token, plus a **sentinel** offset equal to `bytes.size()` at the
///    end (so `startOffs.size() == out.size() + 1`).
///
/// \details
///  - Ensures the buffer ends with a newline (Clang driver expects it).
///  - Uses `clang::Lexer` in raw-lex mode; whitespace tokens are **not**
///    emitted (`SetKeepWhitespaceMode(false)`), matching `-E -P`.
///  - Offsets are computed from a `clang::SourceManager` over a
///    `MemoryBuffer`, so they are stable and monotonic.
///  - UTF-8 is treated as raw bytes; spellings are byte slices of the input.
///  - Emits trace/debug logs for each token (kind, visible-WS spelling,
///    location) via the project logging helpers.
///  - On internal failures (e.g., invalid buffer retrieval) the function
///    calls `fatal(...)` and aborts; it does not throw.
///
/// \param bytes
///   Entire preprocessed file content to lex. If it does not end with '\n',
///   the function temporarily appends one for lexing.
/// \param[out] out
///   Destination vector of tokens (cleared on entry).
/// \param[out] startOffs
///   Destination vector of starting byte offsets per token, followed by a
///   one-past-end sentinel (cleared on entry).
///
/// \invariant
///   `startOffs.size() == out.size() + 1` and `startOffs` is non-decreasing.
///
/// \note
///   This path deliberately avoids the full preprocessor.
void lexPPTokens(const std::string &bytes, std::vector<PPTok> &out,
                 std::vector<std::size_t> &startOffs,
                 const LangOptions &lang) {
  out.clear();
  startOffs.clear();

  // Ensure a trailing newline.
  std::string buf = bytes;
  bool addedNL = false;
  if (buf.empty() || buf.back() != '\n') {
    buf.push_back('\n');
    addedNL = true;
  }

  REFOLD_LOG_DEBUG("lexer", "entered: bytes={0} (addedNL={1})", buf.size(),
        addedNL ? "YES" : "NO");

  using namespace clang;

  // Diagnostics: heap-owning client to avoid double free on destruction.
  DiagnosticOptions diagOpts;
  IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
  auto *client = new IgnoringDiagConsumer(); // owned by Diags
  DiagnosticsEngine diags(diagIDs, diagOpts, client, /*ShouldOwnClient*/ true);

  // FS & source management.
  FileSystemOptions fso;
  FileManager fm(fso);
  SourceManager sm(diags, fm);

  // Back the "file" with our bytes.
  std::unique_ptr<MemoryBuffer> mb =
      MemoryBuffer::getMemBuffer(StringRef(buf), "pp",
                                 /*RequiresNullTerminator*/ true);
  FileID fid = sm.createFileID(std::move(mb));

  bool invalid = false;
  StringRef data = sm.getBufferData(fid, &invalid);
  if (invalid) {
    REFOLD_LOG_FATAL("lexer", "getBufferData returned Invalid");
  }

  const char *b = data.begin();
  const char *e = data.end();

  Lexer lex(sm.getLocForStartOfFile(fid), lang, b, b, e);
  lex.SetKeepWhitespaceMode(false);
  lex.SetCommentRetentionState(true);

  // Tokenize
  for (;;) {
    Token tkn;
    lex.LexFromRawLexer(tkn);
    if (tkn.is(tok::eof))
      break;

    unsigned len = tkn.getLength();
    std::size_t off = sm.getFileOffset(sm.getFileLoc(tkn.getLocation()));
    std::size_t end = std::min(buf.size(), off + static_cast<std::size_t>(len));

    PPTok ppt;
    if (off <= end && end <= buf.size()) {
      ppt.spelling.assign(buf.data() + off, buf.data() + end);
    } else {
      ppt.spelling.clear(); // defensive
    }

    const char *kindName = tok::getTokenName(tkn.getKind());
    ppt.kind = kindName;

    if (inTraceMode()) {
      // Presumed location recovery and visible-whitespace spelling are only
      // used by the per-token lexer trace. Keep them out of the hot lexing path
      // when trace logging is disabled.
      PresumedLoc pl = sm.getPresumedLoc(tkn.getLocation());
      std::optional<unsigned> line;
      std::optional<unsigned> col;
      if (pl.isValid()) {
        line = pl.getLine();
        col = pl.getColumn();
      }

      trace("lexer/parsed",
            "kind={0} spelled={1} off={2} len={3} li={4} co={5}", kindName,
            stringutils::showWs(stringutils::clip(StringRef(ppt.spelling), 80)),
            off, len, line, col);
    }

    out.push_back(std::move(ppt));
    startOffs.push_back(off);
  }

  // Sentinel: one-past-end
  startOffs.push_back(buf.size());
  // Sanity: monotone offsets and size relationship.
  assert(startOffs.size() == out.size() + 1 && "need sentinel in startOffs");
  assert(std::is_sorted(startOffs.begin(), startOffs.end()));

  REFOLD_LOG_DEBUG("lexer", "done: tokens={0}", out.size());
}

// --------------------- Sideband pragma normalization -------------------------

/// Raw `-E -P` output can contain preserved pragma directive lines. Those lines
/// are directive sideband: they are printed in the replay surface but are not
/// ordinary preprocessor tokens in the producer's refold-map token count. Keep
/// them separate from the normal token stream so they can be diffed as
/// zero-token source artifacts rather than forcing whole-file fallback.
struct SidebandPragmaLine {
  std::string text;
  std::string canonicalText;
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t normalTokenGap = 0;
};

struct JsonPragmaItem {
  uint64_t id = 0;
  std::string text;
  std::string canonicalText;
  std::string sitePath;
  uint64_t siteB = 0;
  uint64_t siteE = 0;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
};

struct JsonZeroTokenDirectiveForSideband {
  std::string sitePath;
  uint64_t lineB = 0;
  uint64_t lineE = 0;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
};

struct JsonTokenSpanForSideband {
  uint64_t begin = 0;
  uint64_t end = 0;
};

struct JsonIncludeItemForSideband {
  uint64_t id = 0;
  std::string resolvedPath;
  std::vector<JsonTokenSpanForSideband> spans;
};

struct JsonTokMapEntryForSideband {
  std::string file;
  uint64_t pp = 0;
  uint64_t b = 0;
  uint64_t e = 0;
};

struct JsonSlotForSideband {
  std::string file;
  std::string kind;
  uint64_t b = 0;
  uint64_t e = 0;
  std::optional<uint64_t> pp = std::nullopt;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
};

using SidebandSourceProof = RefoldEngine::OwnerLocalSourceEditProof;

struct SidebandPragmaItemBinding {
  int64_t pragmaIndex = -1;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
};

static std::string stripCCommentsForPragmaCanonicalization(StringRef text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (stringutils::copyQuotedLiteral(text, i, out))
      continue;
    if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '*') {
      out.push_back(' ');
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/'))
        ++i;
      if (i + 1 < text.size())
        i += 2;
      continue;
    }
    if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/') {
      out.push_back(' ');
      while (i < text.size() && text[i] != '\n')
        ++i;
      if (i < text.size())
        out.push_back(text[i++]);
      continue;
    }
    out.push_back(text[i++]);
  }
  return out;
}

static std::string collapsePragmaWhitespacePreservingLiterals(StringRef text) {
  std::string out;
  out.reserve(text.size());
  bool pendingSpace = false;

  auto flushPendingSpace = [&]() {
    if (pendingSpace && !out.empty())
      out.push_back(' ');
    pendingSpace = false;
  };

  for (size_t i = 0; i < text.size();) {
    if (text[i] == '"' || text[i] == '\'') {
      flushPendingSpace();
      if (stringutils::copyQuotedLiteral(text, i, out))
        continue;
    }

    const char c = text[i++];
    if (stringutils::isWs(c)) {
      pendingSpace = true;
      continue;
    }

    flushPendingSpace();
    out.push_back(c);
  }

  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

/// Convert a physical or replayed pragma directive spelling to the canonical
/// sideband identity used for matching.
///
/// The source edit range and the replay identity are different facts.  The edit
/// range must preserve the full physical directive, including comments and line
/// continuations, but sideband matching must use the canonical spelling printed
/// in raw `.i`: comments removed, continued physical lines spliced, and
/// ordinary pragma whitespace normalized.  Keeping those facts separate
/// collapses the trailing-comment and line-continuation cases into the same
/// invariant as all other sideband pragma edits.
static std::string canonicalizeSidebandPragmaText(StringRef text) {
  std::string spliced;
  spliced.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\') {
      size_t j = i + 1;
      if (j < text.size() && text[j] == '\r')
        ++j;
      if (j < text.size() && text[j] == '\n') {
        spliced.push_back(' ');
        i = j;
        continue;
      }
    }
    spliced.push_back(text[i]);
  }

  std::string noComments =
      stripCCommentsForPragmaCanonicalization(StringRef(spliced));
  StringRef body(noComments);
  body = body.trim();

  if (body.empty())
    return "\n";

  size_t i = 0;
  while (i < body.size() && stringutils::isNonNewlineWs(body[i]))
    ++i;
  if (i < body.size() && body[i] == '#')
    ++i;
  while (i < body.size() && stringutils::isNonNewlineWs(body[i]))
    ++i;
  StringRef Pragma("pragma");
  if (body.size() - i >= Pragma.size() &&
      body.substr(i, Pragma.size()) == Pragma &&
      (body.size() == i + Pragma.size() ||
       !stringutils::isIdentPart(body[i + Pragma.size()]))) {
    i += Pragma.size();
    while (i < body.size() && stringutils::isNonNewlineWs(body[i]))
      ++i;
    std::string rest =
        collapsePragmaWhitespacePreservingLiterals(body.substr(i));
    if (rest.empty())
      return "#pragma\n";
    return ("#pragma " + rest + "\n");
  }

  std::string collapsed = collapsePragmaWhitespacePreservingLiterals(body);
  return collapsed + "\n";
}

static std::optional<std::string>
readFileForSidebandCanonicalization(StringRef path) {
  auto bufOrErr = MemoryBuffer::getFile(path);
  if (!bufOrErr)
    return std::nullopt;
  return (*bufOrErr)->getBuffer().str();
}

static std::optional<std::string>
readMappedSourceFileForSideband(StringRef sitePath, StringRef rootSourcePath,
                                StringRef refoldMapPath) {
  if (auto bytes = readFileForSidebandCanonicalization(sitePath))
    return bytes;

  if (!rootSourcePath.empty()) {
    SmallString<256> candidate(rootSourcePath);
    llvm::sys::path::remove_filename(candidate);
    if (!candidate.empty()) {
      llvm::sys::path::append(candidate, sitePath);
      if (auto bytes = readFileForSidebandCanonicalization(candidate))
        return bytes;
    }
  }

  if (!refoldMapPath.empty()) {
    SmallString<256> candidate(refoldMapPath);
    llvm::sys::path::remove_filename(candidate);
    if (!candidate.empty()) {
      llvm::sys::path::append(candidate, sitePath);
      if (auto bytes = readFileForSidebandCanonicalization(candidate))
        return bytes;
    }
  }

  return std::nullopt;
}

/// Collect physical `#pragma` lines from a raw `.i` replay surface.
static std::vector<SidebandPragmaLine>
collectSidebandPragmaLines(StringRef bytes) {
  std::vector<SidebandPragmaLine> out;
  size_t begin = 0;
  while (begin < bytes.size()) {
    size_t nl = bytes.find('\n', begin);
    size_t lineEndNoNL = nl == StringRef::npos ? bytes.size() : nl;
    size_t end = nl == StringRef::npos ? bytes.size() : nl + 1;
    StringRef line = bytes.slice(begin, lineEndNoNL);
    if (stringutils::lineStartsWithDirectiveKeyword(line, "pragma")) {
      SidebandPragmaLine rec;
      rec.text = bytes.slice(begin, end).str();
      rec.canonicalText = canonicalizeSidebandPragmaText(rec.text);
      rec.begin = static_cast<uint64_t>(begin);
      rec.end = static_cast<uint64_t>(end);
      out.push_back(std::move(rec));
    }
    begin = end;
  }
  return out;
}

/// Stamp each sideband pragma with the gap in the normal, non-sideband token
/// stream where the directive line appears.
///
/// Unknown pragmas can be textually identical.  Matching only the pragma text
/// makes deletion of the first `#pragma vendor note` indistinguishable from
/// deletion of the second.  The normal-token gap is a deterministic structural
/// anchor: it records whether the sideband directive appeared before token 0,
/// between tokens 5 and 6, and so on, after ignoring other sideband pragmas.
static void annotateSidebandPragmaTokenGaps(
    std::vector<SidebandPragmaLine> &lines, ArrayRef<PPTok> toks,
    ArrayRef<std::size_t> tokOff) {
  if (lines.empty())
    return;

  size_t lineIdx = 0;
  uint64_t normalTokensSeen = 0;
  for (size_t i = 0; i < toks.size(); ++i) {
    const size_t off = tokOff[i];
    while (lineIdx < lines.size() && off >= lines[lineIdx].end) {
      lines[lineIdx].normalTokenGap = normalTokensSeen;
      ++lineIdx;
    }

    const bool inSideband = lineIdx < lines.size() &&
                            off >= lines[lineIdx].begin &&
                            off < lines[lineIdx].end;
    if (!inSideband)
      ++normalTokensSeen;
  }

  while (lineIdx < lines.size()) {
    lines[lineIdx].normalTokenGap = normalTokensSeen;
    ++lineIdx;
  }
}

/// Return the raw byte offset in `bytes` for a gap in the normal token stream.
///
/// Deleting a sideband pragma has no B-side directive bytes.  For edit-map
/// output we still need a deterministic zero-length B envelope, so use the byte
/// position of the first normal B token after the sideband's gap, or EOF for the
/// final gap.  Sideband directive tokens are ignored when counting the gap.
static uint64_t byteOffsetForNormalTokenGap(
    StringRef bytes, ArrayRef<SidebandPragmaLine> lines, ArrayRef<PPTok> toks,
    ArrayRef<std::size_t> tokOff, uint64_t gap) {
  size_t lineIdx = 0;
  uint64_t normalTokensSeen = 0;
  for (size_t i = 0; i < toks.size(); ++i) {
    const size_t off = tokOff[i];
    while (lineIdx < lines.size() && off >= lines[lineIdx].end)
      ++lineIdx;
    const bool inSideband = lineIdx < lines.size() &&
                            off >= lines[lineIdx].begin &&
                            off < lines[lineIdx].end;
    if (inSideband)
      continue;

    if (normalTokensSeen == gap)
      return static_cast<uint64_t>(off);
    ++normalTokensSeen;
  }
  return static_cast<uint64_t>(bytes.size());
}

/// Remove pragma sideband tokens from an already-lexed raw replay stream.
///
/// The byte buffer itself is left untouched.  Kept token offsets therefore still
/// point into the original raw `.i` file, preserving correct B-slice materializa-
/// tion and terminal-fallback behavior while restoring the token sequence that
/// the refold map actually describes.
static void filterSidebandPragmaTokens(ArrayRef<SidebandPragmaLine> lines,
                                       std::vector<PPTok> &toks,
                                       std::vector<std::size_t> &tokOff,
                                       size_t sourceSize) {
  if (lines.empty() || toks.empty())
    return;

  std::vector<PPTok> filteredToks;
  std::vector<std::size_t> filteredOffs;
  filteredToks.reserve(toks.size());
  filteredOffs.reserve(tokOff.size());

  size_t lineIdx = 0;
  for (size_t i = 0; i < toks.size(); ++i) {
    const size_t off = tokOff[i];
    while (lineIdx < lines.size() && off >= lines[lineIdx].end)
      ++lineIdx;
    const bool inSideband = lineIdx < lines.size() &&
                            off >= lines[lineIdx].begin &&
                            off < lines[lineIdx].end;
    if (inSideband)
      continue;
    filteredOffs.push_back(off);
    filteredToks.push_back(std::move(toks[i]));
  }

  filteredOffs.push_back(sourceSize);
  toks = std::move(filteredToks);
  tokOff = std::move(filteredOffs);
}

static bool tokenOffsetIsInSidebandPragma(ArrayRef<SidebandPragmaLine> lines,
                                          size_t off) {
  for (const SidebandPragmaLine &line : lines) {
    if (off < line.begin)
      return false;
    if (line.begin <= off && off < line.end)
      return true;
  }
  return false;
}

/// Build the normal token stream used to anchor sideband directives.
///
/// Sideband pragma lines are deliberately absent from the producer token count,
/// so sideband matching must reason in the same coordinate space: the sequence
/// of ordinary preprocessed tokens after those directive lines are ignored.
static std::vector<StringRef>
buildNormalTokenRefsExcludingSideband(ArrayRef<SidebandPragmaLine> lines,
                                      ArrayRef<PPTok> toks,
                                      ArrayRef<std::size_t> tokOff) {
  std::vector<StringRef> out;
  out.reserve(toks.size());
  for (size_t i = 0; i < toks.size(); ++i) {
    if (tokenOffsetIsInSidebandPragma(lines, tokOff[i]))
      continue;
    out.push_back(toks[i].spelling);
  }
  return out;
}

/// Project an A-side normal-token gap into B through the ordinary-token LCS.
///
/// Absolute gaps are not stable under edits that insert or delete normal tokens
/// before a pragma.  The stable object is the boundary between neighboring
/// ordinary tokens.  This routine projects that boundary through the LCS and
/// succeeds only when the boundary has a unique image in B.
static std::optional<uint64_t> projectAGapToBGap(ArrayRef<int64_t> aToB,
                                                uint64_t aGap) {
  std::optional<uint64_t> leftB;
  std::optional<uint64_t> rightB;
  for (uint64_t a = 0; a < aToB.size(); ++a) {
    if (aToB[a] < 0)
      continue;
    const uint64_t b = static_cast<uint64_t>(aToB[a]);
    if (a < aGap) {
      leftB = b;
      continue;
    }
    rightB = b;
    break;
  }

  if (leftB && rightB) {
    if (*leftB + 1 != *rightB)
      return std::nullopt;
    return *rightB;
  }
  if (rightB)
    return *rightB;
  if (leftB)
    return *leftB + 1;
  return 0;
}

/// Project a B-side normal-token gap back to A through the ordinary-token LCS.
///
/// This is used to decide whether a sideband pragma that survived in B is the
/// same source pragma that appeared in A.  Matching by raw text is insufficient
/// for duplicate unknown pragmas; matching by absolute B gap is also unstable
/// when normal-token edits before the pragma changed the token count.
static std::optional<uint64_t> projectBGapToAGap(ArrayRef<int64_t> aToB,
                                                uint64_t bGap) {
  std::optional<uint64_t> leftA;
  std::optional<uint64_t> rightA;
  for (uint64_t a = 0; a < aToB.size(); ++a) {
    if (aToB[a] < 0)
      continue;
    const uint64_t b = static_cast<uint64_t>(aToB[a]);
    if (b < bGap) {
      leftA = a;
      continue;
    }
    rightA = a;
    break;
  }

  if (leftA && rightA) {
    if (*leftA + 1 != *rightA)
      return std::nullopt;
    return *rightA;
  }
  if (rightA)
    return *rightA;
  if (leftA)
    return *leftA + 1;
  return 0;
}

static bool isOnlyWhitespaceForSidebandBlock(StringRef text) {
  for (char c : text) {
    if (!stringutils::isNonNewlineWs(c) && c != '\n' && c != '\r')
      return false;
  }
  return true;
}

static std::optional<uint64_t> projectedBGapForSidebandLine(
    ArrayRef<int64_t> normalA2B, const SidebandPragmaLine &line) {
  return projectBGapToAGap(normalA2B, line.normalTokenGap);
}

/// Return the B-byte end for a sideband replacement block.
///
/// The sideband line records cover only the directive lines themselves.  A
/// block replacement also owns the raw B trivia between the last replacement
/// directive and the next replay artifact in the same normal-token gap: for
/// example, the blank line in `#pragma gamma\n\nint x`.  Include that trivia
/// when it is purely whitespace, but stop before the next sideband directive or
/// ordinary token so equal neighboring sideband lines cannot be duplicated.
static uint64_t sidebandBlockReplacementBEnd(
    StringRef bBytes, ArrayRef<SidebandPragmaLine> bLines,
    ArrayRef<PPTok> rawBToks, ArrayRef<std::size_t> rawBTokOff,
    uint64_t bStart, uint64_t bEnd) {
  assert(bStart < bEnd && "replacement block must contain B sideband lines");
  uint64_t end = bLines[static_cast<size_t>(bEnd - 1)].end;
  const uint64_t gap = bLines[static_cast<size_t>(bStart)].normalTokenGap;
  uint64_t limit = byteOffsetForNormalTokenGap(bBytes, bLines, rawBToks,
                                               rawBTokOff, gap);
  if (bEnd < bLines.size()) {
    const SidebandPragmaLine &next = bLines[static_cast<size_t>(bEnd)];
    if (next.normalTokenGap == gap)
      limit = std::min<uint64_t>(limit, next.begin);
  }

  if (end < limit && limit <= bBytes.size() &&
      isOnlyWhitespaceForSidebandBlock(bBytes.slice(end, limit)))
    end = limit;
  return end;
}

enum class SidebandDiagnosticPragmaAction { Push, Pop, Setting };

struct ParsedSidebandDiagnosticPragma {
  StringRef namespaceName;
  SidebandDiagnosticPragmaAction action =
      SidebandDiagnosticPragmaAction::Setting;
};

static bool sidebandDiagnosticSettingAction(StringRef action) {
  return action == "ignored" || action == "warning" || action == "error" ||
         action == "fatal" || action == "remark";
}

static bool consumeSidebandDiagnosticOptionString(StringRef text, size_t &pos,
                                                  size_t end) {
  stringutils::skipWsNoLF(text, pos, end);
  if (pos >= end || text[pos] != '"')
    return false;

  ++pos;
  while (pos < end) {
    const char c = text[pos++];
    if (c == '\\') {
      if (pos >= end)
        return false;
      ++pos;
      continue;
    }
    if (c == '"')
      return true;
    if (c == '\n' || c == '\r')
      return false;
  }
  return false;
}

/// Parse the small pragma-state language that may be carried by an ordinary
/// token replacement hunk.
///
/// Sideband pragma normalization happens before the refold engine has a chance
/// to prove source-gap ownership.  Therefore the frontend must know exactly
/// which pragma lines are safe to remove from the structural token stream when
/// those same raw B bytes are already inside an ordinary replacement payload.
/// Keep the admitted language identical to the engine-side invariant:
/// only Clang/GCC diagnostic push/pop/settings participate, settings are useful
/// only while a pushed frame is active, and unknown pragmas remain outside the
/// sideband carry proof.
static std::optional<ParsedSidebandDiagnosticPragma>
parseSidebandDiagnosticPragma(StringRef canonicalText) {
  const size_t end = canonicalText.size();
  size_t pos = 0;

  if (!stringutils::consumeDirectiveHash(canonicalText, pos, end))
    return std::nullopt;

  StringRef word;
  if (!stringutils::consumeIdentifier(canonicalText, pos, end, word) ||
      word != "pragma")
    return std::nullopt;

  stringutils::skipWsNoLF(canonicalText, pos, end);
  StringRef namespaceName;
  if (!stringutils::consumeIdentifier(canonicalText, pos, end,
                                      namespaceName) ||
      (namespaceName != "clang" && namespaceName != "GCC"))
    return std::nullopt;

  stringutils::skipWsNoLF(canonicalText, pos, end);
  if (!stringutils::consumeIdentifier(canonicalText, pos, end, word) ||
      word != "diagnostic")
    return std::nullopt;

  stringutils::skipWsNoLF(canonicalText, pos, end);
  StringRef action;
  if (!stringutils::consumeIdentifier(canonicalText, pos, end, action))
    return std::nullopt;

  ParsedSidebandDiagnosticPragma parsed;
  parsed.namespaceName = namespaceName;
  if (action == "push") {
    parsed.action = SidebandDiagnosticPragmaAction::Push;
  } else if (action == "pop") {
    parsed.action = SidebandDiagnosticPragmaAction::Pop;
  } else if (sidebandDiagnosticSettingAction(action)) {
    if (!consumeSidebandDiagnosticOptionString(canonicalText, pos, end))
      return std::nullopt;
    parsed.action = SidebandDiagnosticPragmaAction::Setting;
  } else {
    return std::nullopt;
  }

  if (!isOnlyWhitespaceForSidebandBlock(canonicalText.drop_front(pos)))
    return std::nullopt;
  return parsed;
}

struct BalancedSidebandDiagnosticPragmaIsland {
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t normalTokenGap = 0;
};

/// Find contiguous sideband pragma blocks with identity diagnostic state.
///
/// This is intentionally a replay-surface proof, not a source proof.  It only
/// says that the raw `.i` sideband block may be treated as state-neutral at its
/// boundaries and therefore may be carried by the ordinary B replacement bytes
/// if the source owner proof later accepts the matching source gap.  Every line
/// in the island must live in one normal-token gap; if ordinary tokens separate
/// two pragma lines, they are not one local state island for this purpose.
static std::vector<BalancedSidebandDiagnosticPragmaIsland>
collectBalancedSidebandDiagnosticPragmaIslands(
    ArrayRef<SidebandPragmaLine> lines) {
  std::vector<BalancedSidebandDiagnosticPragmaIsland> islands;

  for (uint64_t i = 0; i < lines.size();) {
    std::optional<ParsedSidebandDiagnosticPragma> first =
        parseSidebandDiagnosticPragma(
            lines[static_cast<size_t>(i)].canonicalText);
    if (!first || first->action != SidebandDiagnosticPragmaAction::Push) {
      ++i;
      continue;
    }

    const uint64_t gap = lines[static_cast<size_t>(i)].normalTokenGap;
    unsigned depth = 0;
    bool foundIsland = false;

    for (uint64_t j = i; j < lines.size(); ++j) {
      const SidebandPragmaLine &line = lines[static_cast<size_t>(j)];
      if (line.normalTokenGap != gap)
        break;

      std::optional<ParsedSidebandDiagnosticPragma> parsed =
          parseSidebandDiagnosticPragma(line.canonicalText);
      if (!parsed || parsed->namespaceName != first->namespaceName)
        break;

      bool validAction = true;
      switch (parsed->action) {
      case SidebandDiagnosticPragmaAction::Push:
        ++depth;
        break;
      case SidebandDiagnosticPragmaAction::Pop:
        if (depth == 0) {
          validAction = false;
          break;
        }
        --depth;
        break;
      case SidebandDiagnosticPragmaAction::Setting:
        if (depth == 0) {
          validAction = false;
          break;
        }
        break;
      }
      if (!validAction)
        break;

      if (depth == 0) {
        islands.push_back({i, j + 1, gap});
        i = j + 1;
        foundIsland = true;
        break;
      }
    }

    if (!foundIsland)
      ++i;
  }

  return islands;
}

static std::vector<JsonPragmaItem>
collectJsonPragmaItems(const json::Object &rootJson, StringRef refoldMapPath) {
  std::vector<JsonPragmaItem> out;
  const json::Array *items = rootJson.getArray("items");
  if (!items)
    return out;
  StringRef rootSourcePath = rootJson.getString("source").value_or(StringRef());

  for (const json::Value &value : *items) {
    const json::Object *obj = value.getAsObject();
    if (!obj)
      continue;
    auto kind = obj->getString("kind");
    auto subkind = obj->getString("subkind");
    if (!kind || !subkind || *kind != "directive" || *subkind != "#pragma")
      continue;

    auto id = obj->getInteger("id");
    auto text = obj->getString("text");
    auto path = obj->getString("site_path");
    auto b = obj->getInteger("site_b");
    auto e = obj->getInteger("site_e");
    if (!id || !text || !path || !b || !e || *id < 0 || *b < 0 || *e < 0 ||
        *b > *e)
      continue;

    // Built-in pseudo-files can contribute implementation pragmas to the map,
    // but the refolded TU source cannot edit those pseudo-ranges.  They must
    // not be considered as source targets for sideband pragma deletion.
    if (path->starts_with("<"))
      continue;

    JsonPragmaItem item;
    item.id = static_cast<uint64_t>(*id);
    item.text = text->str();
    item.canonicalText = canonicalizeSidebandPragmaText(item.text);
    item.sitePath = path->str();
    item.siteB = static_cast<uint64_t>(*b);
    item.siteE = static_cast<uint64_t>(*e);

    if (auto sourceBytes = readMappedSourceFileForSideband(
            item.sitePath, rootSourcePath, refoldMapPath)) {
      if (item.siteB <= item.siteE && item.siteE <= sourceBytes->size()) {
        const uint64_t extendedE = stringutils::extendRangeToLogicalDirective(
            StringRef(*sourceBytes), item.siteB, item.siteE);
        item.siteE = extendedE;

        // Prefer the producer-recorded replay text for matching.  A pragma
        // produced by `_Pragma(...)` has source bytes such as `EMIT_PRAGMA`,
        // while the replay surface contains `#pragma ...`.  The source range
        // is still the correct edit target, but replacing the replay canonical
        // text with the invocation spelling makes the sideband line
        // unbindable and forces terminal fallback.  Only direct source
        // `#pragma` directives are allowed to override the producer text.
        std::string sourceCanonical = canonicalizeSidebandPragmaText(
            StringRef(*sourceBytes).slice(item.siteB, item.siteE));
        if (StringRef(sourceCanonical).ltrim().starts_with("#pragma"))
          item.canonicalText = std::move(sourceCanonical);
      }
    }

    if (auto owner = obj->getInteger("owner_include_id"))
      if (*owner >= 0)
        item.ownerIncludeId = static_cast<uint64_t>(*owner);
    out.push_back(std::move(item));
  }
  return out;
}

/// Collect owner-local directive lines that are known not to contribute normal
/// preprocessed tokens.
///
/// Sideband insertion anchors may need to look through preserved source-only
/// state in one normal-token gap: macro definitions, undefs, empty includes, or
/// other directives that have no ordinary-token span in the refold map.  The
/// invariant is intentionally owner-polymorphic and fail-closed: only directive
/// items whose map entry has no spans are accepted, and the physical directive
/// line is recovered from the real owner bytes instead of trusting the callback
/// range to cover the leading `#`.
static std::vector<JsonZeroTokenDirectiveForSideband>
collectZeroTokenDirectivesForSideband(const json::Object &rootJson,
                                      StringRef refoldMapPath) {
  std::vector<JsonZeroTokenDirectiveForSideband> out;
  const json::Array *items = rootJson.getArray("items");
  if (!items)
    return out;
  StringRef rootSourcePath = rootJson.getString("source").value_or(StringRef());

  for (const json::Value &value : *items) {
    const json::Object *obj = value.getAsObject();
    if (!obj)
      continue;
    auto kind = obj->getString("kind");
    if (!kind || *kind != "directive")
      continue;
    auto path = obj->getString("site_path");
    auto b = obj->getInteger("site_b");
    auto e = obj->getInteger("site_e");
    if (!path || !b || !e || path->starts_with("<") || *b < 0 || *e < 0 ||
        *b > *e)
      continue;

    bool hasNormalTokenSpan = false;
    if (const json::Array *spans = obj->getArray("spans"))
      hasNormalTokenSpan = !spans->empty();
    if (hasNormalTokenSpan)
      continue;

    std::optional<std::string> sourceBytes =
        readMappedSourceFileForSideband(*path, rootSourcePath, refoldMapPath);
    if (!sourceBytes)
      continue;

    const uint64_t siteB = static_cast<uint64_t>(*b);
    const uint64_t siteE = static_cast<uint64_t>(*e);
    if (siteB > sourceBytes->size() || siteE > sourceBytes->size())
      continue;

    JsonZeroTokenDirectiveForSideband directive;
    directive.sitePath = path->str();
    directive.lineB =
        stringutils::lineBeginContainingOffset(*sourceBytes, siteB);
    directive.lineE = stringutils::extendLineToLogicalDirective(
        *sourceBytes, directive.lineB);
    if (auto owner = obj->getInteger("owner_include_id"))
      if (*owner >= 0)
        directive.ownerIncludeId = static_cast<uint64_t>(*owner);
    out.push_back(std::move(directive));
  }
  return out;
}

static std::vector<JsonIncludeItemForSideband>
collectJsonIncludesForSideband(const json::Object &rootJson) {
  std::vector<JsonIncludeItemForSideband> out;
  const json::Array *items = rootJson.getArray("items");
  if (!items)
    return out;

  for (const json::Value &value : *items) {
    const json::Object *obj = value.getAsObject();
    if (!obj)
      continue;
    auto kind = obj->getString("kind");
    auto subkind = obj->getString("subkind");
    if (!kind || !subkind || *kind != "directive" ||
        (*subkind != "#include" && *subkind != "#include_next"))
      continue;
    auto id = obj->getInteger("id");
    auto resolved = obj->getString("resolved_path");
    if (!id || !resolved || *id < 0)
      continue;

    JsonIncludeItemForSideband inc;
    inc.id = static_cast<uint64_t>(*id);
    inc.resolvedPath = resolved->str();
    if (const json::Array *spans = obj->getArray("spans")) {
      for (const json::Value &spanValue : *spans) {
        const json::Object *spanObj = spanValue.getAsObject();
        if (!spanObj)
          continue;
        auto begin = spanObj->getInteger("begin");
        auto end = spanObj->getInteger("end");
        if (!begin || !end || *begin < 0 || *end < 0 || *begin > *end)
          continue;
        inc.spans.push_back(JsonTokenSpanForSideband{
            static_cast<uint64_t>(*begin), static_cast<uint64_t>(*end)});
      }
    }
    out.push_back(std::move(inc));
  }
  return out;
}

static std::vector<JsonTokMapEntryForSideband>
collectJsonTokMapForSideband(const json::Object &rootJson) {
  std::vector<JsonTokMapEntryForSideband> out;
  const json::Array *tokmap = rootJson.getArray("tokmap");
  if (!tokmap)
    return out;

  for (const json::Value &value : *tokmap) {
    const json::Object *obj = value.getAsObject();
    if (!obj)
      continue;
    auto file = obj->getString("file");
    auto pp = obj->getInteger("pp");
    auto b = obj->getInteger("b");
    auto e = obj->getInteger("e");
    if (!file || !pp || !b || !e || *pp < 0 || *b < 0 || *e < 0 || *b > *e)
      continue;

    JsonTokMapEntryForSideband entry;
    entry.file = file->str();
    entry.pp = static_cast<uint64_t>(*pp);
    entry.b = static_cast<uint64_t>(*b);
    entry.e = static_cast<uint64_t>(*e);
    out.push_back(std::move(entry));
  }
  return out;
}

static std::vector<JsonSlotForSideband>
collectJsonSlotsForSideband(const json::Object &rootJson) {
  std::vector<JsonSlotForSideband> out;
  const json::Array *slots = rootJson.getArray("slots");
  if (!slots)
    return out;

  for (const json::Value &value : *slots) {
    const json::Object *obj = value.getAsObject();
    if (!obj)
      continue;
    auto file = obj->getString("file");
    auto kind = obj->getString("kind");
    auto b = obj->getInteger("b");
    auto e = obj->getInteger("e");
    if (!file || !kind || !b || !e || *b < 0 || *e < 0 || *b > *e)
      continue;

    JsonSlotForSideband slot;
    slot.file = file->str();
    slot.kind = kind->str();
    slot.b = static_cast<uint64_t>(*b);
    slot.e = static_cast<uint64_t>(*e);
    if (auto pp = obj->getInteger("pp"))
      if (*pp >= 0)
        slot.pp = static_cast<uint64_t>(*pp);
    if (auto owner = obj->getInteger("owner_include_id"))
      if (*owner >= 0)
        slot.ownerIncludeId = static_cast<uint64_t>(*owner);
    out.push_back(std::move(slot));
  }
  return out;
}

static bool slotOwnerMatches(const JsonSlotForSideband &slot,
                             std::optional<uint64_t> ownerIncludeId) {
  if (ownerIncludeId)
    return slot.ownerIncludeId && *slot.ownerIncludeId == *ownerIncludeId;
  return !slot.ownerIncludeId;
}

static bool rightTokenInFileAtGap(StringRef file, uint64_t gap,
                                  ArrayRef<JsonTokMapEntryForSideband> tokmap) {
  return llvm::any_of(tokmap, [&](const JsonTokMapEntryForSideband &entry) {
    return StringRef(entry.file) == file && entry.pp == gap;
  });
}

static bool leftTokenInFileAtGap(StringRef file, uint64_t gap,
                                 ArrayRef<JsonTokMapEntryForSideband> tokmap) {
  if (gap == 0)
    return false;
  return llvm::any_of(tokmap, [&](const JsonTokMapEntryForSideband &entry) {
    return StringRef(entry.file) == file && entry.pp + 1 == gap;
  });
}

/// Return a source byte that realizes a normal-token gap in one concrete owner.
///
/// Sideband pragma insertions have no A-side source directive to edit, so the
/// insertion point must come from explicit producer coordinates: neighboring
/// tokmap entries first, then file/include slots for zero-token boundaries.
/// The owner id is part of the key for header slots so repeated includes of the
/// same physical header do not collapse onto one arbitrary occurrence.
static std::optional<uint64_t> sourceByteForNormalTokenGap(
    StringRef file, std::optional<uint64_t> ownerIncludeId, uint64_t gap,
    ArrayRef<JsonTokMapEntryForSideband> tokmap,
    ArrayRef<JsonSlotForSideband> slots) {
  for (const JsonTokMapEntryForSideband &entry : tokmap)
    if (StringRef(entry.file) == file && entry.pp == gap)
      return entry.b;

  if (gap > 0)
    for (const JsonTokMapEntryForSideband &entry : tokmap)
      if (StringRef(entry.file) == file && entry.pp + 1 == gap)
        return entry.e;

  for (const JsonSlotForSideband &slot : slots) {
    if (StringRef(slot.file) != file || !slot.pp || *slot.pp != gap ||
        !slotOwnerMatches(slot, ownerIncludeId))
      continue;
    return slot.b;
  }

  // A zero-token header has no tokmap entry and its file_begin/file_end slots
  // may not carry a PP coordinate.  These slots are still explicit producer
  // anchors, but only when the caller has already selected a concrete include
  // owner.
  if (ownerIncludeId) {
    for (const JsonSlotForSideband &slot : slots) {
      if (StringRef(slot.file) != file ||
          !slotOwnerMatches(slot, ownerIncludeId))
        continue;
      if (slot.kind == "file_begin" || slot.kind == "after_last_include" ||
          slot.kind == "file_end")
        return slot.b;
    }
  }

  return std::nullopt;
}

static bool rangesOverlap(uint64_t lhsB, uint64_t lhsE, uint64_t rhsB,
                          uint64_t rhsE) {
  return lhsB < rhsE && rhsB < lhsE;
}

static bool ownerMatches(std::optional<uint64_t> lhs,
                         std::optional<uint64_t> rhs) {
  return lhs == rhs;
}

static bool tokenMapHasTokenInOwnerRange(
    StringRef file, uint64_t begin, uint64_t end,
    ArrayRef<JsonTokMapEntryForSideband> tokmap) {
  return llvm::any_of(tokmap, [&](const JsonTokMapEntryForSideband &entry) {
    return StringRef(entry.file) == file && rangesOverlap(entry.b, entry.e,
                                                          begin, end);
  });
}

static bool skipCCommentInSidebandGap(StringRef bytes, uint64_t &p,
                                      uint64_t end) {
  if (p + 1 >= end || bytes[p] != '/')
    return false;
  if (bytes[p + 1] == '/') {
    p += 2;
    while (p < end && bytes[p] != '\n')
      ++p;
    return true;
  }
  if (bytes[p + 1] == '*') {
    p += 2;
    while (p + 1 < end && !(bytes[p] == '*' && bytes[p + 1] == '/'))
      ++p;
    if (p + 1 >= end)
      return false;
    p += 2;
    return true;
  }
  return false;
}

static const JsonZeroTokenDirectiveForSideband *findZeroTokenDirectiveAt(
    StringRef file, std::optional<uint64_t> ownerIncludeId, uint64_t byte,
    ArrayRef<JsonZeroTokenDirectiveForSideband> directives) {
  for (const JsonZeroTokenDirectiveForSideband &directive : directives) {
    if (StringRef(directive.sitePath) == file &&
        ownerMatches(directive.ownerIncludeId, ownerIncludeId) &&
        directive.lineB <= byte && byte < directive.lineE)
      return &directive;
  }
  return nullptr;
}

/// Return true iff an owner-local interval can be preserved while placing a
/// sideband insertion at one of its boundaries.
///
/// This is the small invariant that replaces the whitespace-only neighbor
/// special case.  The interval is legal gap material only when it contains no
/// ordinary mapped tokens for the owner and every visible non-comment directive
/// line is explicitly recorded as a zero-normal-token directive in the refold
/// map.  That admits preserved macro-state directives such as `#define` and
/// `#undef` without allowing a sideband insertion to jump over normal source
/// text, condition bodies, or unmodelled directive state.
static bool sourceIntervalIsZeroTokenGapMaterial(
    StringRef file, std::optional<uint64_t> ownerIncludeId, uint64_t begin,
    uint64_t end, StringRef rootSourcePath, StringRef refoldMapPath,
    ArrayRef<JsonTokMapEntryForSideband> tokmap,
    ArrayRef<JsonZeroTokenDirectiveForSideband> zeroTokenDirectives) {
  if (begin > end)
    return false;
  if (begin == end)
    return true;

  std::optional<std::string> sourceBytes =
      readMappedSourceFileForSideband(file, rootSourcePath, refoldMapPath);
  if (!sourceBytes || end > sourceBytes->size())
    return false;

  if (tokenMapHasTokenInOwnerRange(file, begin, end, tokmap))
    return false;

  StringRef bytes(*sourceBytes);
  uint64_t p = begin;
  while (p < end) {
    if (stringutils::isNonNewlineWs(bytes[p]) || bytes[p] == '\n' ||
        bytes[p] == '\r') {
      ++p;
      continue;
    }
    if (skipCCommentInSidebandGap(bytes, p, end))
      continue;

    if (bytes[p] == '#') {
      const JsonZeroTokenDirectiveForSideband *directive =
          findZeroTokenDirectiveAt(file, ownerIncludeId, p,
                                   zeroTokenDirectives);
      if (!directive || directive->lineE > end)
        return false;
      p = directive->lineE;
      continue;
    }

    return false;
  }
  return true;
}

/// Return true iff the owner-local interval contains a producer-recorded
/// zero-normal-token directive line.
///
/// This is intentionally narrower than `sourceIntervalIsZeroTokenGapMaterial`:
/// whitespace and comments can be preserved by a plain right-boundary sideband
/// insertion, while macro-state directives require the special neighbor
/// coalescing path so that the directive stays outside the consumed edit range.
static bool sourceIntervalContainsZeroTokenDirective(
    StringRef file, std::optional<uint64_t> ownerIncludeId, uint64_t begin,
    uint64_t end, StringRef rootSourcePath, StringRef refoldMapPath,
    ArrayRef<JsonZeroTokenDirectiveForSideband> zeroTokenDirectives) {
  if (begin >= end)
    return false;

  std::optional<std::string> sourceBytes =
      readMappedSourceFileForSideband(file, rootSourcePath, refoldMapPath);
  if (!sourceBytes || end > sourceBytes->size())
    return false;

  StringRef bytes(*sourceBytes);
  uint64_t p = begin;
  while (p < end) {
    if (stringutils::isNonNewlineWs(bytes[p]) || bytes[p] == '\n' ||
        bytes[p] == '\r') {
      ++p;
      continue;
    }
    if (skipCCommentInSidebandGap(bytes, p, end))
      continue;

    if (bytes[p] != '#')
      return false;

    const JsonZeroTokenDirectiveForSideband *directive =
        findZeroTokenDirectiveAt(file, ownerIncludeId, p, zeroTokenDirectives);
    if (!directive || directive->lineE > end)
      return false;
    return true;
  }
  return false;
}

static std::optional<SidebandSourceProof>
inferSidebandSourceProof(
    const SidebandPragmaLine &line, uint64_t projectedAGap, StringRef tuPath,
    ArrayRef<JsonIncludeItemForSideband> includes,
    ArrayRef<JsonTokMapEntryForSideband> tokmap,
    ArrayRef<JsonSlotForSideband> slots) {
  struct IncludeClaim {
    const JsonIncludeItemForSideband *include = nullptr;
    uint64_t siteByte = 0;
  };
  std::vector<IncludeClaim> claims;

  for (const JsonIncludeItemForSideband &inc : includes) {
    for (const JsonTokenSpanForSideband &span : inc.spans) {
      if (!(span.begin <= projectedAGap && projectedAGap <= span.end))
        continue;

      // For B-only insertions, prefer a header owner only when the gap is
      // adjacent to a normal token from that header.  This proves that the
      // inserted sideband line is being interleaved with that include's replay
      // rather than merely sitting at a TU boundary around the include.
      const bool adjacentHeaderToken =
          rightTokenInFileAtGap(StringRef(inc.resolvedPath), projectedAGap,
                                tokmap) ||
          leftTokenInFileAtGap(StringRef(inc.resolvedPath), projectedAGap,
                               tokmap);
      if (!adjacentHeaderToken)
        continue;

      std::optional<uint64_t> siteByte = sourceByteForNormalTokenGap(
          StringRef(inc.resolvedPath), inc.id, projectedAGap, tokmap, slots);
      if (!siteByte)
        continue;
      claims.push_back(IncludeClaim{&inc, *siteByte});
    }
  }

  if (claims.size() > 1)
    return std::nullopt;
  if (claims.size() == 1) {
    const uint64_t siteByte = claims.front().siteByte;
    return SidebandSourceProof::ZeroWidthInsertion(
        claims.front().include->resolvedPath, siteByte,
        claims.front().include->id);
  }

  std::optional<uint64_t> tuByte = sourceByteForNormalTokenGap(
      tuPath, std::nullopt, projectedAGap, tokmap, slots);
  if (!tuByte)
    return std::nullopt;

  return SidebandSourceProof::ZeroWidthInsertion(tuPath, *tuByte,
                                                std::nullopt);
}

/// Attach an owner include id to a header-owned pragma when the map does not
/// already provide one.
///
/// Older producer maps record raw `#pragma` source ranges but not the include
/// instance that opened the header.  A header sideband edit is still safe to
/// materialize when the physical header path has exactly one include instance
/// in the map.  If repeated includes make the owner ambiguous, leave the owner
/// unset so the consumer fails closed instead of editing the wrong instance.
static void inferUniqueHeaderPragmaOwners(
    std::vector<JsonPragmaItem> &pragmas,
    ArrayRef<JsonIncludeItemForSideband> includes) {
  for (JsonPragmaItem &pragma : pragmas) {
    if (pragma.ownerIncludeId || pragma.sitePath.empty() ||
        StringRef(pragma.sitePath).starts_with("<")) {
      continue;
    }

    std::optional<uint64_t> uniqueInclude;
    bool ambiguous = false;
    for (const JsonIncludeItemForSideband &inc : includes) {
      if (inc.resolvedPath != pragma.sitePath)
        continue;
      if (uniqueInclude) {
        ambiguous = true;
        break;
      }
      uniqueInclude = inc.id;
    }
    if (uniqueInclude && !ambiguous)
      pragma.ownerIncludeId = *uniqueInclude;
  }
}

/// Infer the include instance for one header-owned sideband occurrence.
///
/// A physical header can be included more than once, so resolved-path
/// uniqueness is only a fast path.  For a concrete sideband line in A, the
/// normal-token gap identifies where that directive appeared in the replay
/// stream.  If exactly one include of the pragma's header owns that gap, the
/// edit can be routed through that include's normal materialization path.
///
/// The end boundary is intentionally considered for trailing pragmas: a pragma
/// after the last ordinary token in a header appears at `span.end`.  If the
/// same gap is also the start/end of another same-header include span, more
/// than one include will claim it and the occurrence remains
/// ambiguous/fail-closed.
static std::optional<uint64_t> inferHeaderPragmaOwnerForOccurrence(
    const JsonPragmaItem &pragma, const SidebandPragmaLine &line,
    ArrayRef<JsonIncludeItemForSideband> includes) {
  if (pragma.sitePath.empty() || StringRef(pragma.sitePath).starts_with("<"))
    return pragma.ownerIncludeId;

  std::optional<uint64_t> owner;
  bool sawHeaderInclude = false;
  for (const JsonIncludeItemForSideband &inc : includes) {
    if (inc.resolvedPath != pragma.sitePath)
      continue;
    sawHeaderInclude = true;
    for (const JsonTokenSpanForSideband &span : inc.spans) {
      // Leading pragmas live at span.begin, interior pragmas live strictly
      // inside the half-open span, and trailing pragmas live at span.end.
      // Endpoint ambiguity is handled by collecting claims from all same-path
      // include spans and accepting only when a single include id survives.
      if (span.begin <= line.normalTokenGap &&
          line.normalTokenGap <= span.end) {
        if (owner && *owner != inc.id)
          return std::nullopt;
        owner = inc.id;
      }
    }
  }

  if (owner)
    return owner;

  // `owner_include_id` on a physical header pragma is useful producer
  // provenance, but it is not by itself the replay occurrence.  A header can be
  // included more than once, and the same DirectivePragmaItem then replays once
  // per concrete include span.  Only fall back to the serialized owner when no
  // repeated-header occurrence inference is required; otherwise fail closed so
  // we do not bind the second replay to the first include instance.
  if (!sawHeaderInclude)
    return pragma.ownerIncludeId;

  return std::nullopt;
}

static bool
sitePathHasIncludeInstance(StringRef sitePath,
                           ArrayRef<JsonIncludeItemForSideband> includes) {
  for (const JsonIncludeItemForSideband &inc : includes)
    if (inc.resolvedPath == sitePath)
      return true;
  return false;
}

/// Pair A-side sideband pragma lines with their source `DirectivePragmaItem`.
///
/// A physical pragma item may replay multiple times when its header is included
/// multiple times.  The binding is therefore per replay occurrence, not just
/// per physical source item: the same `DirectivePragmaItem` can be reused once
/// for each concrete owner include id.  Within one owner occurrence, duplicate
/// identical pragma spellings are still consumed in recorded source order.
///
/// If the sideband line can be attributed to competing paths/owners, the line
/// is left unbound so the caller fails closed instead of editing the wrong
/// owner.
static std::vector<SidebandPragmaItemBinding>
mapSidebandLinesToPragmaItems(ArrayRef<SidebandPragmaLine> lines,
                              ArrayRef<JsonPragmaItem> pragmas,
                              ArrayRef<JsonIncludeItemForSideband> includes) {
  std::vector<SidebandPragmaItemBinding> out(lines.size());
  std::set<std::pair<size_t, uint64_t>> used;
  constexpr uint64_t NoOwner = std::numeric_limits<uint64_t>::max();

  struct Candidate {
    size_t pragmaIndex = 0;
    std::optional<uint64_t> ownerIncludeId = std::nullopt;
  };

  struct ZeroTokenReplayAtom {
    size_t pragmaIndex = 0;
    uint64_t ownerIncludeId = 0;
  };

  // Headers that emit only sideband pragma lines have no ordinary-token span,
  // so all of their replayed pragmas share the same normal-token gap.  Binding
  // those lines by "first unused physical pragma" is not enough for repeated
  // includes with duplicate pragma spelling: the second replay line could bind
  // either to the second physical pragma in the first include or the first
  // physical pragma in the second include.
  //
  // The refold map still gives a deterministic replay product:
  //
  //   include occurrence order × physical pragma source order
  //
  // Build that product once and use it as the binding witness for zero-token
  // headers.  This is not a preference heuristic; it is the producer's replay
  // order projected onto concrete include ids and physical pragma ordinals.
  std::vector<ZeroTokenReplayAtom> zeroTokenReplayOrder;
  for (const JsonIncludeItemForSideband &inc : includes) {
    if (!inc.spans.empty())
      continue;

    std::vector<size_t> physicalPragmas;
    for (size_t j = 0; j < pragmas.size(); ++j)
      if (pragmas[j].sitePath == inc.resolvedPath)
        physicalPragmas.push_back(j);

    std::stable_sort(physicalPragmas.begin(), physicalPragmas.end(),
                     [&](size_t lhs, size_t rhs) {
                       if (pragmas[lhs].siteB != pragmas[rhs].siteB)
                         return pragmas[lhs].siteB < pragmas[rhs].siteB;
                       return pragmas[lhs].id < pragmas[rhs].id;
                     });

    for (size_t pragmaIndex : physicalPragmas)
      zeroTokenReplayOrder.push_back(ZeroTokenReplayAtom{pragmaIndex, inc.id});
  }

  auto tryBindZeroTokenReplayAtom =
      [&](const SidebandPragmaLine &line) -> std::optional<Candidate> {
    for (const ZeroTokenReplayAtom &atom : zeroTokenReplayOrder) {
      const uint64_t ownerKey = atom.ownerIncludeId;
      if (used.find(std::make_pair(atom.pragmaIndex, ownerKey)) != used.end())
        continue;

      const JsonPragmaItem &pragma = pragmas[atom.pragmaIndex];
      if (pragma.canonicalText != line.canonicalText)
        continue;

      return Candidate{atom.pragmaIndex, atom.ownerIncludeId};
    }
    return std::nullopt;
  };

  for (size_t i = 0; i < lines.size(); ++i) {
    if (std::optional<Candidate> zeroTokenCandidate =
            tryBindZeroTokenReplayAtom(lines[i])) {
      const uint64_t ownerKey = *zeroTokenCandidate->ownerIncludeId;
      used.insert(std::make_pair(zeroTokenCandidate->pragmaIndex, ownerKey));
      out[i].pragmaIndex =
          static_cast<int64_t>(zeroTokenCandidate->pragmaIndex);
      out[i].ownerIncludeId = zeroTokenCandidate->ownerIncludeId;
      continue;
    }

    std::vector<Candidate> candidates;

    for (size_t j = 0; j < pragmas.size(); ++j) {
      const JsonPragmaItem &pragma = pragmas[j];
      if (pragma.canonicalText != lines[i].canonicalText)
        continue;

      std::optional<uint64_t> owner =
          inferHeaderPragmaOwnerForOccurrence(pragma, lines[i], includes);
      const bool isHeaderPragma =
          !pragma.sitePath.empty() &&
          !StringRef(pragma.sitePath).starts_with("<") &&
          sitePathHasIncludeInstance(pragma.sitePath, includes);
      if (isHeaderPragma && !owner)
        continue;

      const uint64_t ownerKey = owner ? *owner : NoOwner;
      if (used.find(std::make_pair(j, ownerKey)) != used.end())
        continue;
      candidates.push_back(Candidate{j, owner});
    }

    if (candidates.empty())
      continue;

    bool sameOwnerDomain = true;
    for (const Candidate &candidate : candidates) {
      if (pragmas[candidate.pragmaIndex].sitePath !=
              pragmas[candidates.front().pragmaIndex].sitePath ||
          candidate.ownerIncludeId != candidates.front().ownerIncludeId) {
        sameOwnerDomain = false;
        break;
      }
    }
    if (!sameOwnerDomain)
      continue;

    // Multiple identical physical pragmas in the same source owner replay in
    // map/source order.  Repeated zero-token include occurrences were handled
    // above by the stronger include-occurrence × physical-ordinal product.
    const Candidate chosen = candidates.front();
    const uint64_t ownerKey =
        chosen.ownerIncludeId ? *chosen.ownerIncludeId : NoOwner;
    used.insert(std::make_pair(chosen.pragmaIndex, ownerKey));
    out[i].pragmaIndex = static_cast<int64_t>(chosen.pragmaIndex);
    out[i].ownerIncludeId = chosen.ownerIncludeId;
  }
  return out;
}

/// Build the A-side owner-depth profile used by sideband normal-token LCS.
///
/// Sideband placement projects each B-side sideband line through the ordinary
/// token LCS.  A plain token-only LCS is ambiguous when a B-side insertion
/// introduces tokens that duplicate the first tokens of an included header,
/// e.g. `int inserted; #pragma beta int value;`.  In that case mapping the
/// header's original `int` to the inserted declaration's `int` moves the
/// sideband insertion from the header boundary to the middle of the header.
///
/// Reuse the same small invariant as the main refolder: inserting across a
/// deeper owner boundary is more expensive than inserting at the boundary.
/// Interior gaps of include spans therefore carry positive depth, while the
/// include boundary gaps remain zero.  This keeps the LCS deterministic and
/// owner-polymorphic without peeking at pragma spelling or test-specific names.
static std::vector<uint32_t> buildSidebandOwnerDepthGaps(
    uint64_t tokenCount, ArrayRef<JsonIncludeItemForSideband> includes) {
  std::vector<uint32_t> gaps(static_cast<size_t>(tokenCount + 1), 0);

  for (const JsonIncludeItemForSideband &inc : includes) {
    for (const JsonTokenSpanForSideband &span : inc.spans) {
      const uint64_t begin = std::min<uint64_t>(span.begin, tokenCount);
      const uint64_t end = std::min<uint64_t>(span.end, tokenCount);
      if (begin >= end)
        continue;

      // Only gaps strictly inside the include are deeper than the parent
      // owner.  The boundary gaps are left at the parent depth so a pure
      // prefix/suffix insertion can remain a boundary insertion instead of
      // being attracted into the header body by an equal-token tie.
      for (uint64_t gap = begin + 1; gap < end; ++gap)
        ++gaps[static_cast<size_t>(gap)];
    }
  }

  return gaps;
}

/// Build source edits for sideband pragma changes and report whether the
/// sideband stream was fully modeled.
///
/// Supported structural cases are deliberately closed:
///   * equal sideband lines: source pragma remains untouched;
///   * A-only sideband lines: delete the corresponding recorded source pragma;
///   * one-for-one replacement: replace the recorded source pragma text;
///   * owner-local block replacement: replace one contiguous A-side directive
///     run with one contiguous B-side sideband block, even when their arities
///     differ;
///   * B-only insertion: insert at a map-backed normal-token gap in either the
///     TU or one concrete include owner.
///
/// Insertions are accepted only when the refold map gives a unique owner-local
/// placement: either a concrete normal-token gap anchor or matched neighboring
/// sideband atoms that refine order inside that gap.  Unequal replacement hunks
/// are accepted only when the A-side pragma lines bind to one owner/replay
/// occurrence and their source ranges form a whitespace-separated directive
/// run.  Preserved zero-token directives are admitted only as insertion-gap
/// material, where they remain outside the edited source range.  Ambiguous
/// include/TU boundaries remain fail-closed so the caller can route them
/// through the existing fallback path instead of manufacturing a source
/// placement.
static bool buildSidebandPragmaSourceEdits(
    const json::Object &rootJson, StringRef refoldMapPath,
    ArrayRef<SidebandPragmaLine> aLines, ArrayRef<SidebandPragmaLine> bLines,
    ArrayRef<PPTok> rawAToks, ArrayRef<std::size_t> rawATokOff,
    StringRef bBytes, ArrayRef<PPTok> rawBToks,
    ArrayRef<std::size_t> rawBTokOff,
    std::vector<RefoldEngine::SidebandPragmaEdit> &edits) {
  edits.clear();
  if (aLines.empty() && bLines.empty())
    return false;

  std::vector<JsonPragmaItem> pragmas =
      collectJsonPragmaItems(rootJson, refoldMapPath);
  std::vector<JsonIncludeItemForSideband> includes =
      collectJsonIncludesForSideband(rootJson);
  std::vector<JsonTokMapEntryForSideband> tokmap =
      collectJsonTokMapForSideband(rootJson);
  std::vector<JsonSlotForSideband> slots =
      collectJsonSlotsForSideband(rootJson);
  std::vector<JsonZeroTokenDirectiveForSideband> zeroTokenDirectives =
      collectZeroTokenDirectivesForSideband(rootJson, refoldMapPath);
  auto sourcePath = rootJson.getString("source");
  if (!sourcePath)
    return false;
  inferUniqueHeaderPragmaOwners(pragmas, includes);
  std::vector<SidebandPragmaItemBinding> aToPragma =
      mapSidebandLinesToPragmaItems(aLines, pragmas, includes);

  std::vector<StringRef> normalA =
      buildNormalTokenRefsExcludingSideband(aLines, rawAToks, rawATokOff);
  std::vector<StringRef> normalB =
      buildNormalTokenRefsExcludingSideband(bLines, rawBToks, rawBTokOff);
  std::vector<uint32_t> sidebandOwnerDepthGap =
      buildSidebandOwnerDepthGaps(normalA.size(), includes);
  std::vector<int64_t> normalA2B =
      diffutils::lcsMapAB(normalA, normalB, sidebandOwnerDepthGap);
  std::vector<diffutils::Hunk> normalHunks =
      diffutils::hunksFromMap(normalA2B, normalA.size(), normalB.size());

  auto sidebandIslandTextMatches = [&](ArrayRef<SidebandPragmaLine> lhsLines,
                                       uint64_t lhsBegin, uint64_t lhsEnd,
                                       ArrayRef<SidebandPragmaLine> rhsLines,
                                       uint64_t rhsBegin, uint64_t rhsEnd) {
    if (lhsEnd < lhsBegin || rhsEnd < rhsBegin ||
        lhsEnd - lhsBegin != rhsEnd - rhsBegin)
      return false;
    for (uint64_t i = 0; i < lhsEnd - lhsBegin; ++i) {
      if (lhsLines[static_cast<size_t>(lhsBegin + i)].canonicalText !=
          rhsLines[static_cast<size_t>(rhsBegin + i)].canonicalText)
        return false;
    }
    return true;
  };

  auto ordinaryReplacementCarriesSidebandIsland =
      [&](const BalancedSidebandDiagnosticPragmaIsland &aIsland,
          const BalancedSidebandDiagnosticPragmaIsland &bIsland) {
        for (const diffutils::Hunk &normalHunk : normalHunks) {
          if (!normalHunk.isReplace())
            continue;

          // The A-side gap must be strictly inside the replaced normal-token
          // interval: a boundary pragma belongs to neighboring preserved
          // source, not to the owner envelope consumed by this ordinary hunk.
          // The B-side gap must be inside the raw B replacement byte envelope.
          // A gap at `bEnd` is still carried, because the emitted byte slice
          // ends at the next preserved normal token and therefore includes
          // sideband directive lines immediately before that token.
          if (normalHunk.aStart < aIsland.normalTokenGap &&
              aIsland.normalTokenGap < normalHunk.aEnd &&
              normalHunk.bStart < bIsland.normalTokenGap &&
              bIsland.normalTokenGap <= normalHunk.bEnd)
            return true;
        }
        return false;
      };

  std::vector<SidebandPragmaLine> ordinaryCarriedKeptALines;
  std::vector<SidebandPragmaLine> ordinaryCarriedKeptBLines;

  auto dropOrdinaryCarriedBalancedSidebandIslands = [&]() -> bool {
    std::vector<BalancedSidebandDiagnosticPragmaIsland> aIslands =
        collectBalancedSidebandDiagnosticPragmaIslands(aLines);
    std::vector<BalancedSidebandDiagnosticPragmaIsland> bIslands =
        collectBalancedSidebandDiagnosticPragmaIslands(bLines);
    if (aIslands.empty() || bIslands.empty())
      return false;

    std::vector<bool> dropA(aLines.size(), false);
    std::vector<bool> dropB(bLines.size(), false);
    bool changed = false;

    // A balanced diagnostic island that is textually preserved in B and whose
    // replay bytes are already inside one ordinary replacement hunk should not
    // participate in the sideband-edit diff.  Its source placement is
    // discharged later by the owner-gap proof; keeping its directive tokens
    // here would make the frontend reject the refold map before that proof can
    // run.
    for (const BalancedSidebandDiagnosticPragmaIsland &aIsland : aIslands) {
      bool aAlreadyDropped = false;
      for (uint64_t a = aIsland.begin; a < aIsland.end; ++a)
        aAlreadyDropped |= dropA[static_cast<size_t>(a)];
      if (aAlreadyDropped)
        continue;

      for (const BalancedSidebandDiagnosticPragmaIsland &bIsland : bIslands) {
        bool bAlreadyDropped = false;
        for (uint64_t b = bIsland.begin; b < bIsland.end; ++b)
          bAlreadyDropped |= dropB[static_cast<size_t>(b)];
        if (bAlreadyDropped)
          continue;

        if (!sidebandIslandTextMatches(aLines, aIsland.begin, aIsland.end,
                                       bLines, bIsland.begin, bIsland.end))
          continue;
        if (!ordinaryReplacementCarriesSidebandIsland(aIsland, bIsland))
          continue;

        for (uint64_t a = aIsland.begin; a < aIsland.end; ++a)
          dropA[static_cast<size_t>(a)] = true;
        for (uint64_t b = bIsland.begin; b < bIsland.end; ++b)
          dropB[static_cast<size_t>(b)] = true;
        changed = true;
        break;
      }
    }

    if (!changed)
      return false;

    ordinaryCarriedKeptALines.clear();
    ordinaryCarriedKeptBLines.clear();
    ordinaryCarriedKeptALines.reserve(aLines.size());
    ordinaryCarriedKeptBLines.reserve(bLines.size());
    for (uint64_t a = 0; a < aLines.size(); ++a)
      if (!dropA[static_cast<size_t>(a)])
        ordinaryCarriedKeptALines.push_back(aLines[static_cast<size_t>(a)]);
    for (uint64_t b = 0; b < bLines.size(); ++b)
      if (!dropB[static_cast<size_t>(b)])
        ordinaryCarriedKeptBLines.push_back(bLines[static_cast<size_t>(b)]);

    REFOLD_LOG_TRACE("pragma/sideband",
          "ordinary hunk carries {0} A-side and {1} B-side balanced "
          "diagnostic pragma line(s); excluding them from sideband diff",
          aLines.size() - ordinaryCarriedKeptALines.size(),
          bLines.size() - ordinaryCarriedKeptBLines.size());

    // Re-slice the local ArrayRefs over owner storage that lives until this
    // function returns.  The caller still filters the original raw token
    // arrays; these narrowed views are only for deciding which sideband lines
    // require explicit source edits beyond ordinary-hunk replay.
    aLines = ArrayRef<SidebandPragmaLine>(ordinaryCarriedKeptALines);
    bLines = ArrayRef<SidebandPragmaLine>(ordinaryCarriedKeptBLines);
    return true;
  };

  if (dropOrdinaryCarriedBalancedSidebandIslands()) {
    if (aLines.empty() && bLines.empty())
      return true;
    aToPragma = mapSidebandLinesToPragmaItems(aLines, pragmas, includes);
  }

  auto sidebandInsertionIsCarriedByOrdinaryInsertion =
      [&](uint64_t bStart, uint64_t bEnd) -> bool {
    if (bStart >= bEnd || bEnd > bLines.size())
      return false;

    // B-only sideband lines can be replayed either by an explicit sideband
    // source edit or by an ordinary insertion hunk whose token envelope already
    // contains the raw sideband bytes.  For TU-owned pure insertions, prefer
    // the ordinary hunk when it owns the same B normal-token gap; otherwise the
    // same source byte receives two insertion edits with overlapping B replay
    // witnesses.  Header-owned insertions still need the explicit include
    // sideband proof so the include materializer can decide whether an
    // include-local ordinary patch also carries the bytes.
    for (uint64_t b = bStart; b < bEnd; ++b) {
      const uint64_t gap = bLines[static_cast<size_t>(b)].normalTokenGap;
      bool covered = false;
      for (const diffutils::Hunk &normalHunk : normalHunks) {
        if (!normalHunk.isInsertOnly() || normalHunk.bStart >= normalHunk.bEnd)
          continue;
        if (normalHunk.bStart <= gap && gap <= normalHunk.bEnd) {
          covered = true;
          break;
        }
      }
      if (!covered)
        return false;
    }
    return true;
  };

  std::vector<std::string> aKeys;
  std::vector<std::string> bKeys;
  std::vector<StringRef> aTextRefs;
  std::vector<StringRef> bTextRefs;
  aKeys.reserve(aLines.size());
  bKeys.reserve(bLines.size());
  aTextRefs.reserve(aLines.size());
  bTextRefs.reserve(bLines.size());

  for (const auto &line : aLines)
    aKeys.push_back(std::to_string(line.normalTokenGap) + "\x1f" +
                    line.canonicalText);

  for (const auto &line : bLines) {
    std::optional<uint64_t> projectedGap =
        projectBGapToAGap(normalA2B, line.normalTokenGap);
    if (!projectedGap)
      return false;
    bKeys.push_back(std::to_string(*projectedGap) + "\x1f" +
                    line.canonicalText);
  }

  for (const auto &key : aKeys)
    aTextRefs.push_back(key);
  for (const auto &key : bKeys)
    bTextRefs.push_back(key);

  std::vector<int64_t> lcs = diffutils::lcsMapAB(aTextRefs, bTextRefs);
  std::vector<diffutils::Hunk> hunks =
      diffutils::hunksFromMap(lcs, aLines.size(), bLines.size());

  std::vector<int64_t> bToA(bLines.size(), -1);
  for (size_t a = 0; a < lcs.size(); ++a)
    if (lcs[a] >= 0 && static_cast<size_t>(lcs[a]) < bToA.size())
      bToA[static_cast<size_t>(lcs[a])] = static_cast<int64_t>(a);

  using SidebandBReplayProof = RefoldEngine::OwnerLocalBReplayProof;

  struct SidebandBReplayBlockProof {
  private:
    uint64_t ownerGap = 0;
    SidebandBReplayProof replay;

    SidebandBReplayBlockProof(uint64_t ownerGap, SidebandBReplayProof replay)
        : ownerGap(ownerGap), replay(std::move(replay)) {}

  public:
    /// Build a B-side block proof after all lines in the block have projected
    /// to one normal-token owner gap and the replay proof has bound the emitted
    /// text to its raw-B witness range.
    static SidebandBReplayBlockProof Create(uint64_t ownerGap,
                                            SidebandBReplayProof replay) {
      return SidebandBReplayBlockProof(ownerGap, std::move(replay));
    }

    /// Return the normal-token gap that owns every sideband replay line in the
    /// block.
    uint64_t OwnerGap() const { return ownerGap; }

    /// Consume the replay proof carried by this block.
    SidebandBReplayProof TakeReplay() { return std::move(replay); }
  };

  auto appendProvedSidebandEdit = [&](std::optional<SidebandSourceProof> source,
                                      SidebandBReplayProof replay) -> bool {
    std::optional<RefoldEngine::SidebandPragmaEdit> edit =
        RefoldEngine::SidebandPragmaEdit::Create(
            std::move(source), std::move(replay),
            static_cast<uint64_t>(bBytes.size()));
    if (!edit)
      return false;
    edits.push_back(std::move(*edit));
    return true;
  };

  struct BoundSidebandSourceAtom {
    const JsonPragmaItem *pragma = nullptr;
    std::optional<uint64_t> ownerIncludeId = std::nullopt;
  };

  auto bindSourceAtom =
      [&](uint64_t aIdx) -> std::optional<BoundSidebandSourceAtom> {
    if (aIdx >= aToPragma.size())
      return std::nullopt;
    const SidebandPragmaItemBinding &binding =
        aToPragma[static_cast<size_t>(aIdx)];
    if (binding.pragmaIndex < 0)
      return std::nullopt;
    const JsonPragmaItem &pragma =
        pragmas[static_cast<size_t>(binding.pragmaIndex)];
    return BoundSidebandSourceAtom{&pragma, binding.ownerIncludeId};
  };

  auto proveBReplayBlock =
      [&](uint64_t bStart,
          uint64_t bEnd) -> std::optional<SidebandBReplayBlockProof> {
    if (bStart >= bEnd || bEnd > bLines.size())
      return std::nullopt;

    std::optional<uint64_t> ownerGap;
    for (uint64_t b = bStart; b < bEnd; ++b) {
      std::optional<uint64_t> projectedGap = projectedBGapForSidebandLine(
          normalA2B, bLines[static_cast<size_t>(b)]);
      if (!projectedGap || (ownerGap && *ownerGap != *projectedGap))
        return std::nullopt;
      ownerGap = *projectedGap;
    }

    const uint64_t begin = bLines[static_cast<size_t>(bStart)].begin;
    const uint64_t end = sidebandBlockReplacementBEnd(bBytes, bLines, rawBToks,
                                                      rawBTokOff, bStart, bEnd);
    if (!ownerGap || end < begin || end > static_cast<uint64_t>(bBytes.size()))
      return std::nullopt;
    return SidebandBReplayBlockProof::Create(
        *ownerGap,
        SidebandBReplayProof::FromText(bBytes.slice(begin, end), begin, end));
  };

  auto sourceGapMaterialIsPreservable =
      [&](StringRef sitePath, std::optional<uint64_t> owner, uint64_t leftEnd,
          uint64_t rightBegin) -> bool {
    return sourceIntervalIsZeroTokenGapMaterial(
        sitePath, owner, leftEnd, rightBegin, *sourcePath, refoldMapPath,
        tokmap, zeroTokenDirectives);
  };

  auto sourceGapIsWhitespace = [&](StringRef sitePath, uint64_t leftEnd,
                                   uint64_t rightBegin) -> bool {
    if (leftEnd > rightBegin)
      return false;
    if (leftEnd == rightBegin)
      return true;
    std::optional<std::string> sourceBytes =
        readMappedSourceFileForSideband(sitePath, *sourcePath, refoldMapPath);
    if (!sourceBytes || rightBegin > sourceBytes->size() ||
        leftEnd > sourceBytes->size())
      return false;
    return isOnlyWhitespaceForSidebandBlock(
        StringRef(*sourceBytes).slice(leftEnd, rightBegin));
  };

  auto proveSourceRun = [&](uint64_t aStart, uint64_t aEnd,
                            std::optional<uint64_t> ownerGap)
      -> std::optional<SidebandSourceProof> {
    if (aStart >= aEnd || aEnd > aLines.size())
      return std::nullopt;
    std::optional<BoundSidebandSourceAtom> first = bindSourceAtom(aStart);
    if (!first || !first->pragma)
      return std::nullopt;
    if (ownerGap &&
        aLines[static_cast<size_t>(aStart)].normalTokenGap != *ownerGap)
      return std::nullopt;

    SidebandSourceProof proof = SidebandSourceProof::ConsumedSourceRun(
        first->pragma->sitePath, first->pragma->siteB, first->pragma->siteE,
        first->ownerIncludeId, aEnd - aStart);

    // A source-run proof consumes one closed owner-local run.  Every A-side
    // atom must bind to the same physical file and concrete owner occurrence,
    // and the bytes between adjacent atoms must be whitespace-only. Source-only
    // directives in a gap are preservable insertion anchors, not bytes that a
    // sideband replacement run may silently consume.
    for (uint64_t a = aStart + 1; a < aEnd; ++a) {
      if (ownerGap &&
          aLines[static_cast<size_t>(a)].normalTokenGap != *ownerGap)
        return std::nullopt;
      std::optional<BoundSidebandSourceAtom> atom = bindSourceAtom(a);
      if (!atom || !atom->pragma ||
          !proof.CanExtendThroughSourceAtom(
              atom->pragma->sitePath, atom->pragma->siteB, atom->pragma->siteE,
              atom->ownerIncludeId) ||
          !sourceGapIsWhitespace(atom->pragma->sitePath, proof.SourceEnd(),
                                 atom->pragma->siteB) ||
          !proof.ExtendThroughSourceAtom(
              atom->pragma->sitePath, atom->pragma->siteB, atom->pragma->siteE,
              atom->ownerIncludeId))
        return std::nullopt;
    }
    return proof;
  };

  struct ProvedSidebandInsertion {
    SidebandSourceProof source;
    SidebandBReplayProof replay;
  };

  auto proveInsertion =
      [&](uint64_t bStart,
          uint64_t bEnd) -> std::optional<ProvedSidebandInsertion> {
    std::optional<SidebandBReplayBlockProof> insertedBlock =
        proveBReplayBlock(bStart, bEnd);
    if (!insertedBlock)
      return std::nullopt;
    const uint64_t ownerGap = insertedBlock->OwnerGap();

    auto matchedNeighborInInsertionGap =
        [&](uint64_t bIdx) -> std::optional<BoundSidebandSourceAtom> {
      if (bIdx >= bLines.size() || bToA[static_cast<size_t>(bIdx)] < 0)
        return std::nullopt;

      std::optional<uint64_t> projectedGap = projectedBGapForSidebandLine(
          normalA2B, bLines[static_cast<size_t>(bIdx)]);
      if (!projectedGap || *projectedGap != ownerGap)
        return std::nullopt;

      return bindSourceAtom(
          static_cast<uint64_t>(bToA[static_cast<size_t>(bIdx)]));
    };

    std::optional<BoundSidebandSourceAtom> prev =
        bStart > 0 ? matchedNeighborInInsertionGap(bStart - 1) : std::nullopt;
    std::optional<BoundSidebandSourceAtom> next =
        bEnd < bLines.size() ? matchedNeighborInInsertionGap(bEnd)
                             : std::nullopt;

    if (prev || next) {
      // A normal-token gap proves only coarse placement.  Matched sideband
      // neighbors refine the intra-gap order, but they must not force the new
      // B-only directive to cross preserved zero-token source state.  The
      // closed invariant is:
      //   * insert at the right matched atom when that is the narrowest source
      //     boundary for the B-only directive;
      //   * with only a previous atom, insert at the map-backed normal-token
      //     gap boundary when that boundary is after the previous atom;
      //   * coalesce a right atom only when a previous atom and the right atom
      //     enclose an explicitly proved zero-normal-token directive that must
      //     stay outside the edit range.
      // This preserves existing sideband lines when they are the first source
      // line after the insertion, while still handling macro-state sideband
      // gaps without consuming the preserved directive state.
      if ((prev && !prev->pragma) || (next && !next->pragma))
        return std::nullopt;

      if (next) {
        // Prefer the narrowest possible insertion point: when the inserted
        // sideband block is immediately before an already-preserved right
        // neighbor, a zero-width edit at the neighbor's source byte lets the
        // normal #line repair land between the new B-only directive and the
        // existing source directive.  That preserves the right neighbor's
        // original logical line instead of drifting it and repairing only the
        // later ordinary token stream.
        //
        // The only time we intentionally coalesce the right neighbor into the
        // sideband edit is the owner-local directive-gap case that motivated
        // the sideband-gap invariant: a previous matched sideband atom and the
        // right atom are separated by preserved zero-normal-token directive
        // state such as `#define`/`#undef`.  In that case the insertion is
        // ordered by both neighbors around source-only state, and replaying the
        // right atom keeps the resync after the complete sideband run while
        // still proving that the preserved directive is not consumed by the
        // edit.  Comments alone are not enough to trigger coalescing; they do
        // not carry macro state, so the narrower right-boundary insertion is
        // still preferred.
        if (!prev) {
          return ProvedSidebandInsertion{
              SidebandSourceProof::ZeroWidthInsertion(next->pragma->sitePath,
                                                      next->pragma->siteB,
                                                      next->ownerIncludeId),
              insertedBlock->TakeReplay()};
        }

        if (prev->pragma->sitePath != next->pragma->sitePath ||
            prev->ownerIncludeId != next->ownerIncludeId)
          return std::nullopt;

        const bool neighborGapIsPreservable = sourceGapMaterialIsPreservable(
            next->pragma->sitePath, next->ownerIncludeId, prev->pragma->siteE,
            next->pragma->siteB);
        if (!neighborGapIsPreservable)
          return std::nullopt;

        const bool neighborGapHasDirective =
            sourceIntervalContainsZeroTokenDirective(
                next->pragma->sitePath, next->ownerIncludeId,
                prev->pragma->siteE, next->pragma->siteB, *sourcePath,
                refoldMapPath, zeroTokenDirectives);
        if (!neighborGapHasDirective) {
          return ProvedSidebandInsertion{
              SidebandSourceProof::ZeroWidthInsertion(next->pragma->sitePath,
                                                      next->pragma->siteB,
                                                      next->ownerIncludeId),
              insertedBlock->TakeReplay()};
        }

        std::optional<SidebandBReplayBlockProof> replayWithNext =
            proveBReplayBlock(bStart, bEnd + 1);
        if (!replayWithNext)
          return std::nullopt;
        return ProvedSidebandInsertion{
            SidebandSourceProof::SourceAtom(
                next->pragma->sitePath, next->pragma->siteB,
                next->pragma->siteE, next->ownerIncludeId),
            replayWithNext->TakeReplay()};
      }

      const BoundSidebandSourceAtom &base = *prev;
      uint64_t siteByte = base.pragma->siteE;
      std::optional<uint64_t> gapByte = sourceByteForNormalTokenGap(
          base.pragma->sitePath, base.ownerIncludeId, ownerGap, tokmap, slots);
      if (gapByte && *gapByte >= base.pragma->siteE) {
        if (!sourceGapMaterialIsPreservable(base.pragma->sitePath,
                                            base.ownerIncludeId,
                                            base.pragma->siteE, *gapByte))
          return std::nullopt;
        siteByte = *gapByte;
      }
      return ProvedSidebandInsertion{
          SidebandSourceProof::ZeroWidthInsertion(
              base.pragma->sitePath, siteByte, base.ownerIncludeId),
          insertedBlock->TakeReplay()};
    }

    std::optional<SidebandSourceProof> anchor =
        inferSidebandSourceProof(bLines[static_cast<size_t>(bStart)], ownerGap,
                                 *sourcePath, includes, tokmap, slots);
    if (!anchor)
      return std::nullopt;
    return ProvedSidebandInsertion{std::move(*anchor),
                                   insertedBlock->TakeReplay()};
  };

  auto appendBarrierSeparatedReplacement =
      [&](const diffutils::Hunk &h) -> bool {
    if (!h.isReplace() || h.aStart >= h.aEnd || h.bStart >= h.bEnd)
      return false;

    std::optional<SidebandBReplayBlockProof> bBlock =
        proveBReplayBlock(h.bStart, h.bEnd);
    if (!bBlock)
      return false;

    const uint64_t ownerGap = bBlock->OwnerGap();
    std::vector<BoundSidebandSourceAtom> atoms;
    atoms.reserve(static_cast<size_t>(h.aEnd - h.aStart));

    bool sawPreservedDirectiveGap = false;
    for (uint64_t a = h.aStart; a < h.aEnd; ++a) {
      if (aLines[static_cast<size_t>(a)].normalTokenGap != ownerGap)
        return false;
      std::optional<BoundSidebandSourceAtom> atom = bindSourceAtom(a);
      if (!atom || !atom->pragma)
        return false;

      if (!atoms.empty()) {
        const BoundSidebandSourceAtom &prev = atoms.back();
        if (prev.pragma->sitePath != atom->pragma->sitePath ||
            prev.ownerIncludeId != atom->ownerIncludeId)
          return false;
        if (!sourceGapMaterialIsPreservable(
                atom->pragma->sitePath, atom->ownerIncludeId,
                prev.pragma->siteE, atom->pragma->siteB))
          return false;
        if (sourceIntervalContainsZeroTokenDirective(
                atom->pragma->sitePath, atom->ownerIncludeId,
                prev.pragma->siteE, atom->pragma->siteB, *sourcePath,
                refoldMapPath, zeroTokenDirectives))
          sawPreservedDirectiveGap = true;
      }

      atoms.push_back(*atom);
    }

    if (!sawPreservedDirectiveGap || atoms.empty())
      return false;

    // A replacement block cannot consume preserved macro-state directives that
    // sit between the matched sideband atoms.  Instead, decompose the source
    // side into atom-local edits: delete all earlier A-side atoms, then replay
    // the complete B-side sideband block at the last atom.  The preserved
    // zero-normal-token directive gap remains untouched and the sideband stream
    // is still modeled as one contiguous B replay witness.
    const size_t editsBefore = edits.size();
    const uint64_t emptyBAnchor = bLines[static_cast<size_t>(h.bStart)].begin;
    for (size_t i = 0; i + 1 < atoms.size(); ++i) {
      const JsonPragmaItem *pragma = atoms[i].pragma;
      if (!appendProvedSidebandEdit(
              SidebandSourceProof::SourceAtom(pragma->sitePath, pragma->siteB,
                                              pragma->siteE,
                                              atoms[i].ownerIncludeId),
              SidebandBReplayProof::EmptyAt(emptyBAnchor))) {
        edits.erase(edits.begin() + editsBefore, edits.end());
        return false;
      }
    }

    const BoundSidebandSourceAtom &last = atoms.back();
    if (!appendProvedSidebandEdit(SidebandSourceProof::SourceAtom(
                                      last.pragma->sitePath, last.pragma->siteB,
                                      last.pragma->siteE, last.ownerIncludeId),
                                  bBlock->TakeReplay())) {
      edits.erase(edits.begin() + editsBefore, edits.end());
      return false;
    }
    return true;
  };

  auto appendOwnerLocalSidebandHunk = [&](const diffutils::Hunk &h) -> bool {
    if (h.isInsertOnly()) {
      std::optional<ProvedSidebandInsertion> insertion =
          proveInsertion(h.bStart, h.bEnd);
      if (!insertion)
        return false;

      if (!insertion->source.OwnerIncludeId() &&
          sidebandInsertionIsCarriedByOrdinaryInsertion(h.bStart, h.bEnd)) {
        // The raw B bytes for this TU-owned sideband insertion are already
        // inside the token envelope of an ordinary insertion hunk.  Emitting a
        // second sideband edit at the same source byte would duplicate the
        // directive and usually destroy the edit-map B witness.  Treat this as
        // a discharged no-op sideband source edit: normalization has removed
        // the sideband tokens from the token stream, and the ordinary hunk is
        // now the sole replay owner for the visible bytes.
        return true;
      }

      return appendProvedSidebandEdit(std::move(insertion->source),
                                      std::move(insertion->replay));
    }

    if (h.isDeleteOnly()) {
      for (uint64_t a = h.aStart; a < h.aEnd; ++a) {
        std::optional<uint64_t> bGap = projectAGapToBGap(
            normalA2B, aLines[static_cast<size_t>(a)].normalTokenGap);
        if (!bGap)
          return false;
        const uint64_t bAnchor = byteOffsetForNormalTokenGap(
            bBytes, bLines, rawBToks, rawBTokOff, *bGap);
        if (!appendProvedSidebandEdit(proveSourceRun(a, a + 1, std::nullopt),
                                      SidebandBReplayProof::EmptyAt(bAnchor)))
          return false;
      }
      return true;
    }

    if (h.isReplace()) {
      // The owner-local block proof is the canonical replacement proof.  Keep
      // the equal-arity fallback only for legacy independent replacement hunks
      // that do not form one closed owner-local source run.
      std::optional<SidebandBReplayBlockProof> bBlock =
          proveBReplayBlock(h.bStart, h.bEnd);
      if (bBlock) {
        const uint64_t ownerGap = bBlock->OwnerGap();
        std::optional<SidebandSourceProof> source =
            proveSourceRun(h.aStart, h.aEnd, ownerGap);
        if (source &&
            appendProvedSidebandEdit(std::move(source), bBlock->TakeReplay()))
          return true;
      }
      if (appendBarrierSeparatedReplacement(h))
        return true;
      if ((h.aEnd - h.aStart) != (h.bEnd - h.bStart))
        return false;
      for (uint64_t a = h.aStart, b = h.bStart; a < h.aEnd; ++a, ++b) {
        const SidebandPragmaLine &line = bLines[static_cast<size_t>(b)];
        if (!appendProvedSidebandEdit(proveSourceRun(a, a + 1, std::nullopt),
                                      SidebandBReplayProof::FromText(
                                          line.text, line.begin, line.end)))
          return false;
      }
      return true;
    }

    // Any remaining sideband shape is outside the current proof domain.  In
    // particular, this rejects malformed empty hunks rather than manufacturing
    // a source placement.
    return false;
  };

  for (const diffutils::Hunk &h : hunks)
    if (!appendOwnerLocalSidebandHunk(h))
      return false;

  return true;
}

// ----------------------------- JSON Parser -----------------------------------

/// \brief Parse a JSON value from a file path.
///
/// Opens \p filePath, reads its contents, and parses it as JSON using
/// `llvm::json::parse()`.
///
/// \param filePath Absolute or canonical path to a UTF-8 text file.
/// \returns
///   - On success: an `llvm::json::Value` containing the parsed JSON.
///   - On failure: an `llvm::Error` with a message such as:
///       * "failed to open file: <path>" if the file cannot be opened
///       * a parse error from `llvm::json::parse()` if the contents are
///         not valid JSON
///
/// \note The returned `Expected` may be consumed with `takeError()` to
///       propagate errors, or dereferenced on success.
Expected<json::Value> parseJSONFromFile(StringRef filePath) {
  auto bufOrErr = MemoryBuffer::getFile(filePath);
  if (!bufOrErr)
    return createStringError(inconvertibleErrorCode(),
                             "failed to open file: " + filePath);

  auto parsed = json::parse(bufOrErr.get()->getBuffer());
  if (!parsed)
    return parsed.takeError();

  return std::move(*parsed);
}

/// \brief Parse a JSON file and validate it against a JSON Schema.
///
/// Parses \p schemaJson as a JSON Schema, parses the JSON document at
/// \p jsonPath, validates the document against the schema, and returns the
/// validated top-level object.
///
/// \param jsonPath  Path to the JSON document to validate (UTF-8 text).
/// \param schemaJson JSON string containing the schema to validate against.
/// \returns
///   - On success: an `llvm::json::Object` representing the validated
///     top-level JSON object.
///   - On failure: an `llvm::Error` describing one of:
///       * Schema parse failure (invalid JSON in \p schemaJson)
///       * Schema is not an object
///       * Document read/parse failure (via `parseJSONFromFile`)
///       * Schema validation errors from `json::JSONSchemaValidator`
///       * Top-level JSON is not an object
///
/// \see parseJSONFromFile
Expected<json::Object> parseAndValidateJSON(StringRef jsonPath,
                                            StringRef schemaJson) {
  // Parse the schema from the provided string
  Expected<json::Value> schemaParsed = json::parse(schemaJson);
  if (!schemaParsed)
    return schemaParsed.takeError();

  const json::Object *schemaObj = schemaParsed->getAsObject();
  if (!schemaObj) {
    return createStringError(inconvertibleErrorCode(),
                             "provided schema is not a valid JSON object");
  }

  // Parse the JSON file to be validated
  Expected<json::Value> jsonParsed = parseJSONFromFile(jsonPath);
  if (!jsonParsed)
    return jsonParsed.takeError();

  // Validate
  json::JSONSchemaValidator Validator(*schemaObj);
  if (Error err = Validator.validate(*jsonParsed))
    return std::move(err);

  // Return the validated JSON object
  const json::Object *resultObj = jsonParsed->getAsObject();
  if (!resultObj)
    return createStringError(inconvertibleErrorCode(),
                             "top-level JSON is not an object");

  return *resultObj;
}

// ------------------------------ Misc Utils -----------------------------------

/// \brief Read the entire file into \p out.
///
/// Opens \p path and assigns its contents to \p out. On failure, emits a
/// fatal diagnostic and terminates.
///
/// \param path Filesystem path to read.
/// \param out  Destination string for the file contents.
void readFile(StringRef path, std::string &out) {
  auto bufOrErr = MemoryBuffer::getFile(path);
  if (!bufOrErr) {
    REFOLD_LOG_FATAL("file/load", "cannot read file: {0} ({1})", path,
          bufOrErr.getError().message());
  }
  out.assign(bufOrErr->get()->getBufferStart(),
             bufOrErr->get()->getBufferEnd());
}

/// \brief Require a CLI option that is intended to appear exactly once.
///
/// Verifies that \p opt was provided (intended for flags that must appear
/// exactly once). If the option is missing, emits a fatal diagnostic and
/// terminates.
///
/// \param flag Spelling used in diagnostics (e.g. "--config").
/// \param opt  Parsed option to check.
void requireExactlyOnce(StringRef flag, const cl::Option &opt) {
  if (opt.getNumOccurrences() != 1)
    REFOLD_LOG_FATAL("cli", "option '{0}' must be specified exactly once", flag);
}

// ------------------------------ PP Context ----------------------------------

struct PPCtx {
  std::string cwd;
  std::vector<std::string> argv;
  std::string lang;
};

Expected<PPCtx> parsePPCtx(const json::Object &rootJson) {
  const json::Object *ctxObj = rootJson.getObject("pp_ctx");
  if (!ctxObj) {
    return createStringError(inconvertibleErrorCode(),
                             "missing required top-level key 'pp_ctx'");
  }

  PPCtx ctx;

  // cwd
  if (auto cwd = ctxObj->getString("cwd")) {
    ctx.cwd = cwd->str();
  } else {
    return createStringError(inconvertibleErrorCode(),
                             "pp_ctx.cwd must be a string");
  }

  // lang
  if (auto lang = ctxObj->getString("lang")) {
    ctx.lang = lang->str();
  } else {
    return createStringError(inconvertibleErrorCode(),
                             "pp_ctx.lang must be a string");
  }

  // argv
  const json::Array *argvArr = ctxObj->getArray("argv");
  if (!argvArr) {
    return createStringError(inconvertibleErrorCode(),
                             "pp_ctx.argv must be an array");
  }

  ctx.argv.reserve(argvArr->size());
  for (const json::Value &v : *argvArr) {
    auto s = v.getAsString();
    if (!s) {
      return createStringError(inconvertibleErrorCode(),
                               "pp_ctx.argv must contain only strings");
    }
    ctx.argv.emplace_back(s->str());
  }

  return ctx;
}

Expected<std::string> preprocessToBytes(StringRef inputPath, const PPCtx &ctx) {
  // Force an absolute input path so it remains valid after we chdir.
  SmallString<256> absInput(inputPath);
  if (std::error_code ec = sys::fs::make_absolute(absInput)) {
    return createStringError(
        ec, formatv("cannot resolve absolute path for '{0}'", inputPath));
  }

  // Create a temp file path for clang's preprocessor output.
  SmallString<256> tmpPath;
  if (std::error_code ec =
          sys::fs::createTemporaryFile("clang-refold-check", "i", tmpPath)) {
    return createStringError(ec, "failed to create temporary file");
  }

  auto removeTmp = make_scope_exit([&]() { (void)sys::fs::remove(tmpPath); });

  // Assemble a cc1-style argument list for in-process preprocessing.
  std::vector<std::string> args;
  args.reserve(ctx.argv.size() + 10);

  bool hasE = false;
  bool hasP = false;

  for (size_t i = 0; i < ctx.argv.size(); ++i) {
    StringRef a(ctx.argv[i]);

    // The recorded invocation may include refold-map or output flags used when
    // producing the refold map. Strip these so we can redirect output to our
    // temp file.
    if (a == "--refold-map") {
      ++i; // skip value
      continue;
    }
    if (a.starts_with("--refold-map="))
      continue;
    if (a == "-o") {
      ++i; // skip value
      continue;
    }

    if (a == "-E")
      hasE = true;
    else if (a == "-P")
      hasP = true;

    args.push_back(ctx.argv[i]);
  }

  // Force preprocess-only and suppress line markers, but do not duplicate flags
  // that are already present in the recorded cc1 argv.
  if (!hasP)
    args.insert(args.begin(), "-P");
  if (!hasE)
    args.insert(args.begin(), "-E");

  // Ensure language is specified (important for non-.c suffixes like '.mod').
  bool hasX = false;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "-x") {
      hasX = true;
      break;
    }
  }
  if (!hasX && !ctx.lang.empty()) {
    args.push_back("-x");
    args.push_back(ctx.lang);
  }

  // Set output and input path.
  args.push_back("-o");
  args.push_back(tmpPath.str().str());
  args.push_back(absInput.str().str());

  std::vector<const char *> cargs;
  cargs.reserve(args.size());
  for (const std::string &s : args)
    cargs.push_back(s.c_str());

  clang::CompilerInstance ci;

  // In Clang 21.x, getVirtualFileSystem() depends on an existing FileManager,
  // so diagnostics must be created against an external VFS first.
  auto vfs = llvm::vfs::getRealFileSystem();
  ci.createDiagnostics(*vfs);
  if (!ci.hasDiagnostics()) {
    return createStringError(inconvertibleErrorCode(),
                             "failed to create diagnostics engine");
  }

  if (!clang::CompilerInvocation::CreateFromArgs(
          ci.getInvocation(), ArrayRef<const char *>(cargs),
          ci.getDiagnostics())) {
    return createStringError(inconvertibleErrorCode(),
                             "failed to parse clang invocation");
  }

  // Be explicit: '-P' should suppress line markers.
  ci.getPreprocessorOutputOpts().ShowLineMarkers = false;
  ci.getFileSystemOpts().WorkingDir = ctx.cwd;

  ci.createFileManager();
  ci.createSourceManager(ci.getFileManager());

  // Run clang's preprocessor.
  clang::PrintPreprocessedAction action;
  if (!ci.ExecuteAction(action)) {
    return createStringError(inconvertibleErrorCode(),
                             "clang preprocessing failed");
  }

  // Read back in the temporary preprocessed output file that was created
  // by clang.
  std::string out;
  readFile(tmpPath, out);
  return out;
}

/// Write \p bytes to \p path as the source text that will be passed to
/// Clang's preprocessor for final line-control pruning validation.
static Error writePruneValidationSource(StringRef path, StringRef bytes) {
  std::error_code ec;
  raw_fd_ostream os(path, ec, sys::fs::OF_Text);
  if (ec)
    return createStringError(
        ec, formatv("cannot write pruning validation source '{0}'", path));
  os << bytes;
  os.close();
  if (os.has_error())
    return createStringError(
        os.error(),
        formatv("failed to flush pruning validation source '{0}'", path));
  return Error::success();
}

/// Return true iff two already-preprocessed `-E -P` byte streams have the same
/// token sequence.
///
/// Final line-control pruning normally requires byte-for-byte preprocessing
/// equivalence.  That is intentionally stronger than the refolding checker, but
/// it is too strong for stale `#line` directives whose only remaining effect is
/// Clang's cosmetic blank-line accounting around zero-token/comment-only source
/// lines.  In those cases the correct semantic oracle is the same one used by
/// `--check`: the emitted source must replay to the same preprocessed token
/// stream, including any materialized `__LINE__`, `__FILE__`, and
/// `__FILE_NAME__` expansions.
static bool preprocessedTokensEqualForLinePrune(StringRef currentPP,
                                                StringRef candidatePP,
                                                const PPCtx &ctx,
                                                std::string &reason) {
  const LangOptions lexLang = RefoldEngine::MakeLexLangOptions(ctx.lang);
  std::vector<PPTok> currentTokens;
  std::vector<PPTok> candidateTokens;
  std::vector<std::size_t> currentOffsets;
  std::vector<std::size_t> candidateOffsets;

  lexPPTokens(currentPP.str(), currentTokens, currentOffsets, lexLang);
  lexPPTokens(candidatePP.str(), candidateTokens, candidateOffsets, lexLang);

  const size_t n = std::min(currentTokens.size(), candidateTokens.size());
  for (size_t i = 0; i < n; ++i) {
    if (currentTokens[i].spelling == candidateTokens[i].spelling)
      continue;

    reason = formatv("preprocessed bytes differ and token streams differ at "
                     "index {0}: current='{1}' candidate='{2}'",
                     i,
                     stringutils::showWs(stringutils::clip(
                         StringRef(currentTokens[i].spelling), 100)),
                     stringutils::showWs(stringutils::clip(
                         StringRef(candidateTokens[i].spelling), 100)))
                 .str();
    return false;
  }

  if (currentTokens.size() != candidateTokens.size()) {
    reason = formatv("preprocessed bytes differ and token counts differ: "
                     "current={0} candidate={1}",
                     currentTokens.size(), candidateTokens.size())
                 .str();
    return false;
  }

  reason.clear();
  return true;
}

/// Create an internal final-pruning validation callback.
///
/// The callback does not implement a user-facing `--check` mode.  It is an
/// executable guard for one proposed `#line` deletion: preprocess the current
/// accepted final source and the candidate final source through the same
/// `clang -E -P` context, using one stable temporary source path located beside
/// `--out`.  Reusing the same source path for both inputs keeps `__FILE__` and
/// quoted-include lookup comparable.  Byte-for-byte equality is accepted first;
/// if the only difference is preprocessing trivia, token-sequence equality is
/// also accepted because the refolding soundness oracle is token equivalence.
static FinalLineControlValidationCallback
buildFinalLineControlValidationCallback(StringRef outputPath,
                                        const PPCtx &ctx) {
  SmallString<256> outputDir(outputPath);
  sys::path::remove_filename(outputDir);
  if (outputDir.empty())
    outputDir = ".";

  SmallString<256> model(outputDir);
  sys::path::append(model, ".clang-refold-line-prune-%%%%%%.c");
  std::string modelText = model.str().str();

  return [modelText, ctx](StringRef currentOutput, StringRef candidateOutput,
                          std::string &reason) -> bool {
    SmallString<256> tmpPath;
    int tmpFD = -1;
    if (std::error_code ec =
            sys::fs::createUniqueFile(modelText, tmpFD, tmpPath)) {
      reason = formatv("could not create pruning validation source '{0}': {1}",
                       modelText, ec.message())
                   .str();
      return false;
    }

    {
      raw_fd_ostream closeStream(tmpFD, /*shouldClose=*/true);
      closeStream.close();
    }

    auto cleanup = make_scope_exit([&]() { (void)sys::fs::remove(tmpPath); });

    if (Error err = writePruneValidationSource(tmpPath, currentOutput)) {
      reason = toString(std::move(err));
      return false;
    }

    auto currentPPOrErr = preprocessToBytes(tmpPath, ctx);
    if (!currentPPOrErr) {
      reason = formatv("failed to preprocess current final source: {0}",
                       toString(currentPPOrErr.takeError()))
                   .str();
      return false;
    }

    if (Error err = writePruneValidationSource(tmpPath, candidateOutput)) {
      reason = toString(std::move(err));
      return false;
    }

    auto candidatePPOrErr = preprocessToBytes(tmpPath, ctx);
    if (!candidatePPOrErr) {
      reason = formatv("failed to preprocess candidate final source: {0}",
                       toString(candidatePPOrErr.takeError()))
                   .str();
      return false;
    }

    if (*currentPPOrErr != *candidatePPOrErr) {
      if (!preprocessedTokensEqualForLinePrune(*currentPPOrErr,
                                               *candidatePPOrErr, ctx, reason))
        return false;
      reason.clear();
      return true;
    }

    reason.clear();
    return true;
  };
}

static bool canIgnoreNoLinesMismatch(const PPTok &aTok, const PPTok &bTok) {
  if (aTok.kind != bTok.kind)
    return false;
  return aTok.kind == "numeric_constant" || aTok.kind == "string_literal" ||
         aTok.kind == "header_name";
}

/// Minimal macro metadata needed by the --no-lines checker recovery path.
///
/// The producer can report zero-length `spans` for predefined location
/// builtins. When that happens, the checker uses the macro call chain and
/// caller `body_spans` to recover the generated token position without
/// broadening the normal ignore rule.
struct NoLinesMacroInfo {
  uint64_t id = 0;
  size_t ordinal = 0;
  std::string name;
  std::string invFile;
  std::optional<uint64_t> invBegin;
  std::optional<uint64_t> callerId;
  std::vector<RefoldModel::PPSpan> spans;
  std::vector<RefoldModel::PPSpan> bodySpans;
};

/// A zero-length predefined builtin expansion that still needs one token
/// marked as line-directive-sensitive in the original preprocessed token
/// stream.
struct NoLinesSensitiveEvent {
  const NoLinesMacroInfo *item = nullptr;
  const NoLinesMacroInfo *anchor = nullptr;
  size_t ordinal = 0;
  std::set<std::string> allowedSpellings;
};

/// Read a non-negative JSON integer as an unsigned metadata identifier/offset.
static std::optional<uint64_t> getJSONUInt64(const json::Object &obj,
                                             StringRef key) {
  auto value = obj.getInteger(key);
  if (!value || *value < 0)
    return std::nullopt;
  return static_cast<uint64_t>(*value);
}

/// Return true when any span covers at least one token after clamping to the
/// token stream length. Zero-length spans intentionally return false.
static bool spanMarksAnyToken(ArrayRef<RefoldModel::PPSpan> spans,
                              size_t tokenCount) {
  for (const auto &sp : spans) {
    uint64_t begin = std::min<uint64_t>(sp.begin, tokenCount);
    uint64_t end = std::min<uint64_t>(sp.end, tokenCount);
    if (end > begin)
      return true;
  }
  return false;
}

/// Return the basename spelling used by `__FILE_NAME__`.
static std::string filenameComponent(StringRef path) {
  return sys::path::filename(path).str();
}

/// Read a source file used for `__LINE__` recovery, caching both successes and
/// failures so repeated builtin events do not repeatedly touch the filesystem.
static std::optional<std::string>
readSourceForNoLines(StringRef path, const PPCtx &ctx,
                     std::map<std::string, std::optional<std::string>> &cache) {
  std::string key = path.str();
  auto it = cache.find(key);
  if (it != cache.end())
    return it->second;

  SmallString<256> resolved(path);
  if (sys::path::is_relative(resolved)) {
    resolved = ctx.cwd;
    sys::path::append(resolved, path);
  }

  auto bufOrErr = MemoryBuffer::getFile(resolved);
  if (!bufOrErr) {
    REFOLD_LOG_DEBUG("check",
          "--no-lines: could not read source '{0}' while recovering builtin "
          "location token spans",
          path);
    cache.emplace(std::move(key), std::nullopt);
    return std::nullopt;
  }

  std::string bytes(bufOrErr.get()->getBufferStart(),
                    bufOrErr.get()->getBufferEnd());
  auto inserted = cache.emplace(std::move(key), std::move(bytes));
  return inserted.first->second;
}

/// Compute the 1-based physical source line containing `offset`.
///
/// The value is used only as one exact candidate spelling for zero-length
/// `__LINE__` metadata. If the source cannot be read, callers fail closed by
/// leaving the allowed-spelling set empty.
static std::optional<uint64_t> lineNumberAtSourceOffset(
    StringRef path, uint64_t offset, const PPCtx &ctx,
    std::map<std::string, std::optional<std::string>> &cache) {
  auto bytes = readSourceForNoLines(path, ctx, cache);
  if (!bytes || offset > bytes->size())
    return std::nullopt;

  uint64_t line = 1;
  for (uint64_t i = 0; i < offset; ++i) {
    if ((*bytes)[static_cast<size_t>(i)] == '\n')
      ++line;
  }
  return line;
}

/// Walk `caller_macro_id` from a builtin expansion toward the outermost macro.
///
/// Cycles or missing references terminate the chain so malformed metadata cannot
/// make recovery loop indefinitely.
static std::vector<const NoLinesMacroInfo *>
callerChainForNoLines(const NoLinesMacroInfo &item,
                      const std::map<uint64_t, NoLinesMacroInfo> &macrosById) {
  std::vector<const NoLinesMacroInfo *> chain;
  std::set<uint64_t> seen;
  std::optional<uint64_t> cur = item.callerId;
  while (cur) {
    if (!seen.insert(*cur).second)
      break;
    auto it = macrosById.find(*cur);
    if (it == macrosById.end())
      break;
    chain.push_back(&it->second);
    cur = it->second.callerId;
  }
  return chain;
}

/// Add the exact string-token spellings a path-valued builtin may have produced
/// for a particular source path.
static void addPathSpellingsForNoLinesBuiltin(
    StringRef builtinName, StringRef path, std::set<std::string> &spellings) {
  if (path.empty())
    return;

  if (builtinName == "__FILE_NAME__") {
    spellings.insert(stringutils::quoteCString(filenameComponent(path)));
    return;
  }

  if (builtinName == "__FILE__" || builtinName == "__BASE_FILE__")
    spellings.insert(stringutils::quoteCString(path));
}

/// Build the finite set of token spellings that may correspond to a builtin
/// event under `--no-lines`.
///
/// For path-valued builtins, include the builtin's file, each caller's file,
/// and the TU source path where applicable. For `__LINE__`, compute exact
/// physical line numbers from invocation offsets when the source bytes are
/// available.
static std::set<std::string> buildAllowedSpellingsForNoLinesBuiltin(
    const NoLinesMacroInfo &item, ArrayRef<const NoLinesMacroInfo *> chain,
    StringRef sourcePath, const PPCtx &ctx,
    std::map<std::string, std::optional<std::string>> &sourceCache) {
  std::set<std::string> spellings;

  if (item.name == "__LINE__") {
    auto addLine = [&](const NoLinesMacroInfo &macro) {
      if (macro.invFile.empty() || !macro.invBegin)
        return;
      auto line = lineNumberAtSourceOffset(macro.invFile, *macro.invBegin, ctx,
                                           sourceCache);
      if (line)
        spellings.insert(std::to_string(*line));
    };

    addLine(item);
    for (const NoLinesMacroInfo *macro : chain)
      addLine(*macro);
    return spellings;
  }

  if (item.name == "__BASE_FILE__") {
    addPathSpellingsForNoLinesBuiltin(item.name, sourcePath, spellings);
    return spellings;
  }

  addPathSpellingsForNoLinesBuiltin(item.name, item.invFile, spellings);
  for (const NoLinesMacroInfo *macro : chain)
    addPathSpellingsForNoLinesBuiltin(item.name, macro->invFile, spellings);
  addPathSpellingsForNoLinesBuiltin(item.name, sourcePath, spellings);
  return spellings;
}

/// Test whether one original preprocessed token is a plausible expansion of a
/// particular zero-length builtin event.
static bool
tokenMatchesNoLinesBuiltinEvent(const PPTok &tok,
                                const NoLinesSensitiveEvent &event) {
  StringRef name(event.item->name);
  if (name == "__LINE__") {
    if (tok.kind != "numeric_constant")
      return false;
    if (event.allowedSpellings.empty())
      return std::all_of(tok.spelling.begin(), tok.spelling.end(),
                         [](char c) { return c >= '0' && c <= '9'; });
    return event.allowedSpellings.count(tok.spelling) != 0;
  }

  if (tok.kind != "string_literal" && tok.kind != "header_name")
    return false;
  return event.allowedSpellings.count(tok.spelling) != 0;
}

/// Materialize the sorted unique token indices covered by a caller macro body.
/// These are the only positions considered when recovering a child builtin
/// whose own span did not cover any token.
static std::vector<size_t>
bodyTokenCandidatesForNoLines(ArrayRef<RefoldModel::PPSpan> bodySpans,
                              size_t tokenCount) {
  std::vector<size_t> candidates;
  for (const auto &sp : bodySpans) {
    uint64_t begin = std::min<uint64_t>(sp.begin, tokenCount);
    uint64_t end = std::min<uint64_t>(sp.end, tokenCount);
    for (uint64_t i = begin; i < end; ++i)
      candidates.push_back(static_cast<size_t>(i));
  }

  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  return candidates;
}

/// Recover ignore-mask bits for predefined location builtins with zero-length
/// producer spans.
///
/// Recovery is deliberately conservative: each builtin event is anchored to the
/// nearest caller macro with token-covering `body_spans`, then the events for
/// an anchor are matched to body tokens in producer order. Tokens are marked
/// only when the monotone assignment is unique; ambiguous or unanchored cases
/// remain unmarked so the validator fails closed.
static void recoverZeroLengthNoLinesBuiltinSpans(
    const json::Object &rootJson, const PPCtx &ctx, ArrayRef<PPTok> a0Toks,
    MutableArrayRef<uint8_t> a0Sensitive, StringRef sourcePath) {
  const json::Array *items = rootJson.getArray("items");
  if (!items)
    return;

  // Reparse only the macro metadata needed by the checker. The ordinal records
  // producer item order, which is the order used for monotone recovery below.
  std::map<uint64_t, NoLinesMacroInfo> macrosById;
  size_t ordinal = 0;
  for (const json::Value &it : *items) {
    const auto *obj = it.getAsObject();
    if (!obj)
      continue;
    auto kind = obj->getString("kind");
    if (!kind || *kind != "macro")
      continue;
    auto id = getJSONUInt64(*obj, "id");
    auto name = obj->getString("name");
    if (!id || !name)
      continue;

    NoLinesMacroInfo info;
    info.id = *id;
    info.ordinal = ordinal++;
    info.name = name->str();
    if (auto invFile = obj->getString("inv_file"))
      info.invFile = invFile->str();
    info.invBegin = getJSONUInt64(*obj, "inv_b");
    info.callerId = getJSONUInt64(*obj, "caller_macro_id");

    if (const json::Value *spansVal = obj->get("spans")) {
      auto spans = parsePPSpans(*spansVal, "no-lines.recover.macro.spans");
      if (!spans) {
        consumeError(spans.takeError());
        continue;
      }
      info.spans = std::move(*spans);
    }

    if (const json::Value *bodyVal = obj->get("body_spans")) {
      auto bodySpans =
          parsePPSpans(*bodyVal, "no-lines.recover.macro.body_spans");
      if (!bodySpans) {
        consumeError(bodySpans.takeError());
        continue;
      }
      info.bodySpans = std::move(*bodySpans);
    }

    macrosById.emplace(info.id, std::move(info));
  }

  std::map<std::string, std::optional<std::string>> sourceCache;
  std::map<uint64_t, std::vector<NoLinesSensitiveEvent>> eventsByAnchor;

  // Existing non-empty spans already participate in the normal mask builder.
  // Only zero-length sensitive builtins need the caller-body recovery path.
  for (const auto &entry : macrosById) {
    const NoLinesMacroInfo &item = entry.second;
    if (!stringutils::isLineDirectiveSensitiveBuiltin(item.name))
      continue;
    if (spanMarksAnyToken(item.spans, a0Toks.size()))
      continue;

    std::vector<const NoLinesMacroInfo *> chain =
        callerChainForNoLines(item, macrosById);

    // Anchor recovery to the nearest caller whose body covers concrete tokens.
    // That bounds matching to the macro surface that actually contains the
    // generated builtin token.
    const NoLinesMacroInfo *anchor = nullptr;
    for (const NoLinesMacroInfo *macro : chain) {
      if (spanMarksAnyToken(macro->bodySpans, a0Toks.size())) {
        anchor = macro;
        break;
      }
    }
    if (!anchor) {
      REFOLD_LOG_DEBUG("check",
            "--no-lines: no body-span anchor for zero-length builtin {0}#{1}",
            item.name, item.id);
      continue;
    }

    NoLinesSensitiveEvent event;
    event.item = &item;
    event.anchor = anchor;
    event.ordinal = item.ordinal;
    event.allowedSpellings = buildAllowedSpellingsForNoLinesBuiltin(
        item, chain, sourcePath, ctx, sourceCache);
    eventsByAnchor[anchor->id].push_back(std::move(event));
  }

  for (auto &entry : eventsByAnchor) {
    std::vector<NoLinesSensitiveEvent> &events = entry.second;

    // Match events to tokens in metadata order. This preserves the original
    // macro-expansion order and prevents arbitrary same-spelling token choices.
    std::sort(events.begin(), events.end(),
              [](const NoLinesSensitiveEvent &lhs,
                 const NoLinesSensitiveEvent &rhs) {
      return lhs.ordinal < rhs.ordinal;
    });

    const NoLinesMacroInfo *anchor = events.front().anchor;
    std::vector<size_t> candidates =
        bodyTokenCandidatesForNoLines(anchor->bodySpans, a0Toks.size());
    const size_t eCount = events.size();
    const size_t cCount = candidates.size();
    if (eCount == 0 || cCount == 0)
      continue;

    // matches[ei][ci] records whether event ei could be represented by the
    // candidate body token ci based on token kind and exact spelling.
    std::vector<std::vector<uint8_t>> matches(
        eCount, std::vector<uint8_t>(cCount, 0));
    for (size_t ei = 0; ei < eCount; ++ei) {
      for (size_t ci = 0; ci < cCount; ++ci) {
        if (tokenMatchesNoLinesBuiltinEvent(a0Toks[candidates[ci]], events[ei]))
          matches[ei][ci] = 1;
      }
    }

    // Count monotone assignments, saturated at two. We only need to know
    // whether the assignment is absent, unique, or ambiguous.
    std::vector<std::vector<uint8_t>> ways(
        eCount + 1, std::vector<uint8_t>(cCount + 1, 0));
    for (size_t ci = 0; ci <= cCount; ++ci)
      ways[eCount][ci] = 1;

    for (size_t ei = eCount; ei-- > 0;) {
      for (size_t ci = cCount; ci-- > 0;) {
        unsigned count = ways[ei][ci + 1];
        if (matches[ei][ci])
          count += ways[ei + 1][ci + 1];
        ways[ei][ci] = static_cast<uint8_t>(std::min<unsigned>(count, 2));
      }
    }

    // Ambiguity is not repaired heuristically: if more than one token sequence
    // satisfies the metadata constraints, leave the ignore mask unchanged.
    if (ways[0][0] != 1) {
      REFOLD_LOG_DEBUG("check",
            "--no-lines: refusing ambiguous zero-length builtin recovery for "
            "anchor macro #{0}: events={1} candidates={2} assignments={3}",
            anchor->id, eCount, cCount, ways[0][0]);
      continue;
    }

    // The unique assignment is now recoverable greedily because the DP table
    // proves there is exactly one valid suffix choice at every selected token.
    size_t ci = 0;
    for (size_t ei = 0; ei < eCount; ++ei) {
      for (; ci < cCount; ++ci) {
        if (!matches[ei][ci] || ways[ei + 1][ci + 1] == 0)
          continue;
        size_t tokIndex = candidates[ci++];
        if (tokIndex < a0Sensitive.size()) {
          a0Sensitive[tokIndex] = 1;
          REFOLD_LOG_DEBUG("check",
                "--no-lines: recovered zero-length builtin {0}#{1} at "
                "original token index {2} under anchor macro #{3}",
                events[ei].item->name, events[ei].item->id, tokIndex,
                anchor->id);
        }
        break;
      }
    }
  }
}

static Expected<std::string> parseSourcePath(const json::Object &rootJson) {
  auto s = rootJson.getString("source");
  if (!s || s->empty())
    return createStringError(inconvertibleErrorCode(),
                             "refold map missing required 'source' path");
  return s->str();
}

// In --check + --no-lines mode, allow token mismatches for *unmodified* tokens
// that originate from line-directive-sensitive predefined macros such as
// __LINE__/__FILE__/__FILE_NAME__/__BASE_FILE__.
//
// We identify these tokens structurally:
//   1) Preprocess the original TU from refold-map 'source' under pp_ctx.
//   2) Mark A-token indices covered by macro items whose name is one of the
//      sensitive builtins above. If producer metadata recorded a zero-length
//      span for such a builtin, recover the token from its caller body spans
//      only when there is a unique structural assignment.
//   3) Diff original A tokens to edited B tokens and mark the corresponding B
//      indices only for those tokens that remain unchanged (EQUAL steps).
static Expected<std::vector<uint8_t>>
buildNoLinesIgnoreMask(const json::Object &rootJson, const PPCtx &ctx,
                       ArrayRef<PPTok> bPPToks) {
  auto sourceOrErr = parseSourcePath(rootJson);
  if (!sourceOrErr)
    return sourceOrErr.takeError();

  auto ppOrErr = preprocessToBytes(*sourceOrErr, ctx);
  if (!ppOrErr)
    return ppOrErr.takeError();
  std::string a0Bytes = std::move(*ppOrErr);

  std::vector<PPTok> a0Toks;
  std::vector<std::size_t> a0Off;
  const LangOptions lexLang =
      RefoldEngine::MakeLexLangOptions(ctx.lang);
  lexPPTokens(a0Bytes, a0Toks, a0Off, lexLang);

  std::vector<uint8_t> a0Sensitive(a0Toks.size(), 0);
  if (auto items = rootJson.getArray("items")) {
    for (const auto &it : *items) {
      const auto *obj = it.getAsObject();
      if (!obj)
        continue;
      auto kind = obj->getString("kind");
      if (!kind || *kind != "macro")
        continue;
      auto name = obj->getString("name");
      if (!name || !stringutils::isLineDirectiveSensitiveBuiltin(*name))
        continue;
      auto *spansVal = obj->get("spans");
      if (!spansVal)
        continue;
      auto spansOrErr = parsePPSpans(*spansVal, "no-lines.items.macro.spans");
      if (!spansOrErr)
        return spansOrErr.takeError();
      for (const auto &sp : *spansOrErr) {
        uint64_t bb = sp.begin;
        uint64_t ee = sp.end;
        if (ee < bb)
          continue;
        if (bb > a0Sensitive.size())
          continue;
        ee = std::min<uint64_t>(ee, a0Sensitive.size());
        for (uint64_t i = bb; i < ee; ++i)
          a0Sensitive[static_cast<size_t>(i)] = 1;
      }
    }
  }

  recoverZeroLengthNoLinesBuiltinSpans(rootJson, ctx, a0Toks, a0Sensitive,
                                       *sourceOrErr);

  SmallVector<StringRef, 0> aSeq;
  SmallVector<StringRef, 0> bSeq;
  aSeq.reserve(a0Toks.size());
  bSeq.reserve(bPPToks.size());
  for (const auto &t : a0Toks)
    aSeq.push_back(StringRef(t.spelling));
  for (const auto &t : bPPToks)
    bSeq.push_back(StringRef(t.spelling));

  std::vector<uint8_t> ignore(bPPToks.size(), 0);
  auto steps = diffutils::diff(aSeq, bSeq);
  for (const auto &st : steps) {
    if (st.op != diffutils::Op::Equal)
      continue;
    const uint64_t len = st.aHi - st.aLo;
    for (uint64_t k = 0; k < len; ++k) {
      const uint64_t ai = st.aLo + k;
      const uint64_t bi = st.bLo + k;
      if (ai < a0Sensitive.size() && bi < ignore.size() && a0Sensitive[ai])
        ignore[bi] = 1;
    }
  }
  return ignore;
}

static Error compareTokens(ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks) {
  // Compare token spellings up to the min length first.
  const size_t n = std::min(aToks.size(), bToks.size());
  for (size_t i = 0; i < n; ++i) {
    if (aToks[i].spelling != bToks[i].spelling) {
      const std::string aDbg = stringutils::showWs(
          stringutils::clip(StringRef(aToks[i].spelling), 100));
      const std::string bDbg = stringutils::showWs(
          stringutils::clip(StringRef(bToks[i].spelling), 180));
      return createStringError(
          inconvertibleErrorCode(),
          formatv("token mismatch at index {0}: A='{1}' B='{2}'", i, aDbg,
                  bDbg)
              .str());
    }
    if (inDebugMode()) {
      const std::string aDbg = stringutils::showWs(
          stringutils::clip(StringRef(aToks[i].spelling), 100));
      const std::string bDbg = stringutils::showWs(
          stringutils::clip(StringRef(bToks[i].spelling), 180));
      debug("compare", "token match at index {0}: A='{1}' B='{2}'", i, aDbg,
            bDbg);
    }
  }

  if (aToks.size() != bToks.size()) {
    return createStringError(
        inconvertibleErrorCode(),
        formatv("token count mismatch: A={0} B={1}", aToks.size(), bToks.size())
            .str());
  }
  return Error::success();
}

static Error compareTokensNoLinesAware(ArrayRef<PPTok> aToks,
                                       ArrayRef<PPTok> bToks,
                                       ArrayRef<uint8_t> ignoreMask) {
  // Compare token spellings up to the min length first.
  const size_t n = std::min(aToks.size(), bToks.size());
  for (size_t i = 0; i < n; ++i) {
    if (aToks[i].spelling != bToks[i].spelling) {
      const bool ign = (i < ignoreMask.size()) && ignoreMask[i] &&
                       canIgnoreNoLinesMismatch(aToks[i], bToks[i]);
      if (ign) {
        if (inDebugMode()) {
          const std::string aDbg = stringutils::showWs(
              stringutils::clip(StringRef(aToks[i].spelling), 100));
          const std::string bDbg = stringutils::showWs(
              stringutils::clip(StringRef(bToks[i].spelling), 180));
          debug("compare",
                "--no-lines: ignoring builtin loc macro mismatch at index {0}: "
                "A='{1}' B='{2}'",
                i, aDbg, bDbg);
        }
        continue;
      }

      const std::string aDbg = stringutils::showWs(
          stringutils::clip(StringRef(aToks[i].spelling), 100));
      const std::string bDbg = stringutils::showWs(
          stringutils::clip(StringRef(bToks[i].spelling), 180));
      return createStringError(
          inconvertibleErrorCode(),
          formatv("token mismatch at index {0}: A='{1}' B='{2}'", i, aDbg,
                  bDbg)
              .str());
    }

    if (inDebugMode()) {
      const std::string aDbg = stringutils::showWs(
          stringutils::clip(StringRef(aToks[i].spelling), 100));
      const std::string bDbg = stringutils::showWs(
          stringutils::clip(StringRef(bToks[i].spelling), 180));
      debug("compare", "token match at index {0}: A='{1}' B='{2}'", i, aDbg,
            bDbg);
    }
  }

  if (aToks.size() != bToks.size()) {
    return createStringError(
        inconvertibleErrorCode(),
        formatv("token count mismatch: A={0} B={1}", aToks.size(), bToks.size())
            .str());
  }
  return Error::success();
}

static bool pathSpellingMatchesAfterAbsolute(StringRef a, StringRef b) {
  if (a.empty() || b.empty())
    return false;

  SmallString<256> absA(a);
  SmallString<256> absB(b);
  if (std::error_code ec = sys::fs::make_absolute(absA))
    return false;
  if (std::error_code ec = sys::fs::make_absolute(absB))
    return false;
  sys::path::remove_dots(absA, /*remove_dot_dot=*/true);
  sys::path::remove_dots(absB, /*remove_dot_dot=*/true);
  return absA == absB;
}

/// Write modified include-owner files required by automatic source-graph
/// refolding, and remove stale generated files from paths that this run proved
/// are no longer admissible.
///
/// Source-graph sidecars are part of the checker-visible replay surface because
/// quoted include lookup searches the directory containing the emitted `.c.mod`
/// before the captured `-I` paths.  Therefore a sidecar emitted by an older run
/// must not be allowed to shadow the original header after the current proof
/// has fallen back to TU materialization.
///
/// Cleanup entries are intentionally conservative: the driver removes a stale
/// file only when the existing bytes exactly match the rejected generated owner
/// bytes and the path is not the producer-resolved input header.  This lets the
/// backend clean up its own obsolete artifacts without deleting arbitrary user
/// headers that happen to sit beside `--out`.
static void writeSourceGraphOutputs(StringRef modifiedSrcPath,
                                    ArrayRef<SourceGraphOutput> outputs) {
  SmallString<256> outputDir(modifiedSrcPath);
  sys::path::remove_filename(outputDir);
  if (outputDir.empty())
    outputDir = ".";

  auto validateRelativePath = [](StringRef rel) {
    SmallVector<StringRef, 8> components;
    rel.split(components, '/', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
    const bool hasUnsafeComponent = llvm::any_of(components, [](StringRef c) {
      return c.empty() || c == "." || c == "..";
    });
    return !rel.empty() && !sys::path::is_absolute(rel) &&
           !hasUnsafeComponent && !rel.contains('\\') && !rel.contains('"');
  };

  std::map<std::string, SourceGraphOutput> cleanupOutputs;
  std::map<std::string, std::string> uniqueOutputs;
  for (const SourceGraphOutput &output : outputs) {
    StringRef rel(output.relativePath);
    if (!validateRelativePath(rel))
      REFOLD_LOG_FATAL("source-graph/write",
            "refusing unsafe source-graph output path: {0}",
            output.relativePath);

    if (output.cleanupOnly) {
      // Multiple rejected include sites can point at the same stale sidecar.
      // Keeping the first candidate is enough: cleanup is byte-exact, so a
      // nonmatching file is left alone rather than guessed about.
      cleanupOutputs.insert({output.relativePath, output});
      continue;
    }

    auto [it, inserted] =
        uniqueOutputs.insert({output.relativePath, output.bytes});
    if (!inserted && it->second != output.bytes)
      REFOLD_LOG_FATAL("source-graph/write",
            "conflicting source-graph contents for path: {0}",
            output.relativePath);
  }

  for (const auto &entry : cleanupOutputs) {
    if (uniqueOutputs.count(entry.first))
      continue;

    const SourceGraphOutput &cleanup = entry.second;
    SmallString<256> path(outputDir);
    sys::path::append(path, entry.first);

    if (!cleanup.resolvedPath.empty() &&
        pathSpellingMatchesAfterAbsolute(path, cleanup.resolvedPath)) {
      REFOLD_LOG_DEBUG("source-graph/write",
            "skip stale cleanup for {0}: output path names producer header {1}",
            path, cleanup.resolvedPath);
      continue;
    }

    auto existingOrErr = MemoryBuffer::getFile(path);
    if (!existingOrErr)
      continue;

    if ((*existingOrErr)->getBuffer() != cleanup.bytes) {
      REFOLD_LOG_DEBUG("source-graph/write",
            "leave possible stale source-graph file {0}: bytes no longer match "
            "rejected generated body for include #{1}",
            path, cleanup.includeId);
      continue;
    }

    if (std::error_code ec = sys::fs::remove(path))
      REFOLD_LOG_FATAL("source-graph/write",
            "cannot remove stale source-graph file {0}: {1}", path,
            ec.message());
    REFOLD_LOG_INFO("finished", "removed stale source-graph header: {0}", path);
  }

  for (const auto &entry : uniqueOutputs) {
    SmallString<256> path(outputDir);
    sys::path::append(path, entry.first);

    if (auto existingOrErr = MemoryBuffer::getFile(path)) {
      if ((*existingOrErr)->getBuffer() != entry.second)
        REFOLD_LOG_FATAL("source-graph/write",
              "refusing to overwrite existing different source-graph file: {0}",
              path);
      REFOLD_LOG_INFO("finished", "source-graph header already up to date: {0}", path);
      continue;
    }

    SmallString<256> parent(path);
    sys::path::remove_filename(parent);
    if (std::error_code ec = sys::fs::create_directories(parent))
      REFOLD_LOG_FATAL("source-graph/write", "cannot create {0}: {1}", parent,
            ec.message());

    std::error_code ec;
    raw_fd_ostream os(path, ec, sys::fs::OF_Text);
    if (ec)
      REFOLD_LOG_FATAL("source-graph/write", "cannot write {0}: {1}", path, ec.message());
    os << entry.second;
    os.close();
    REFOLD_LOG_INFO("finished", "wrote source-graph header: {0}", path);
  }
}

/// Write the optional materialized-edit map as stable, human-readable JSON.
///
/// The schema is deliberately small: each entry records one materialized edit
/// with a half-open B-byte range from `--pp-mod` and the half-open byte range in
/// the final `--out` source that represents that B-side materialization.  These
/// are source-envelope ranges: a structure-preserving macro edit may map an
/// expanded B envelope to the rewritten invocation argument that regenerates it.
static void
writeMaterializedEditMap(StringRef path, StringRef ppModPath,
                         StringRef modifiedSrcPath,
                         ArrayRef<MaterializedEditMapping> mappings) {
  std::error_code ec;
  raw_fd_ostream os(path, ec, sys::fs::OF_Text);
  if (ec)
    REFOLD_LOG_FATAL("edit-map/write", "cannot write {0}: {1}", path, ec.message());

  // Keep the range shape identical for both sides of every edit.  The
  // surrounding object names say which file the range belongs to.
  auto writeByteRangeObject = [](json::OStream &j, uint64_t begin,
                                 uint64_t end) {
    j.attribute("begin", begin);
    j.attribute("end", end);
  };

  json::OStream j(os, /*IndentSize=*/2);
  j.object([&] {
    j.attribute("modified_pp_source", ppModPath);
    j.attribute("refolded_output", modifiedSrcPath);

    j.attributeArray("edits", [&] {
      for (const MaterializedEditMapping &m : mappings) {
        j.object([&] {
          j.attributeObject("modified_pp_byte_range", [&] {
            writeByteRangeObject(j, m.modifiedPreprocessedBegin,
                                 m.modifiedPreprocessedEnd);
          });
          j.attributeObject("refolded_output_byte_range", [&] {
            writeByteRangeObject(j, m.refoldedSourceBegin, m.refoldedSourceEnd);
          });
        });
      }
    });
  });

  os << '\n';
  os.close();
}

} // end anonymous namespace

// ------------------------- Command-Line Options ------------------------------

static cl::OptionCategory RefoldCategory("clang-refold options");

cl::opt<LogLevel> LogLevelOpt(
    "log-level", cl::desc("Set log level"),
    cl::values(clEnumValN(LogLevel::trace, "trace", "Trace"),
               clEnumValN(LogLevel::debug, "debug", "Debug"),
               clEnumValN(LogLevel::info, "info", "Info  (default)"),
               clEnumValN(LogLevel::warn, "warn", "Warn"),
               clEnumValN(LogLevel::error, "error", "Error"),
               clEnumValN(LogLevel::fatal, "fatal", "Fatal")),
    cl::init(LogLevel::info), cl::cat(RefoldCategory));

static cl::opt<std::string>
    PPPath("pp", // long name: --pp
           cl::desc("Path to preprocessed file (.c.i) [required(1)]"),
           cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -p (points to --pp)
static cl::alias PPPathShort("p", cl::desc("Alias for --pp"),
                             cl::aliasopt(PPPath), cl::cat(RefoldCategory));

static cl::opt<std::string> PPModPath(
    "pp-mod", // long name: --pp-mod
    cl::desc("Path to modified preprocessed file (.c.i.mod) [required(1)(2)]"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -P (points to --pp-mod)
static cl::alias PPModPathShort("P", cl::desc("Alias for --pp-mod"),
                                cl::aliasopt(PPModPath),
                                cl::cat(RefoldCategory));

static cl::opt<std::string> RefoldJSONPath(
    "refold-map", // long name: --refold-map
    cl::desc("Path to refold JSON file (.c.refold.json) [required(1)(2)]"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -r (points to --refold-map)
static cl::alias RefoldJSONPathShort("r", cl::desc("Alias for --refold-map"),
                                     cl::aliasopt(RefoldJSONPath),
                                     cl::cat(RefoldCategory));

static cl::opt<std::string> ModifiedSrcPath(
    "out", // long name: --out
    cl::desc("Path to refolded C source file (.c.mod) [required(1)]"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -o (points to --out)
static cl::alias ModifiedSrcPathShort("o", cl::desc("Alias for --out"),
                                      cl::aliasopt(ModifiedSrcPath),
                                      cl::cat(RefoldCategory));

static cl::opt<std::string> EmitEditMapPath(
    "emit-edit-map",
    cl::desc("Path to write materialized edit map JSON containing "
             "B-to-refolded-output byte ranges"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

static cl::opt<std::string> CheckSrcPath(
    "check", // long name: --check
    cl::desc("Verify a refolding by preprocessing this refolded C source and "
             "comparing tokens to --pp-mod"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -c (points to --check)
static cl::alias CheckSrcPathShort("c", cl::desc("Alias for --check"),
                                   cl::aliasopt(CheckSrcPath),
                                   cl::cat(RefoldCategory));

static cl::opt<bool> NoLines(
    "no-lines",
    cl::desc(
        "Don't include #line directives in refold source (off by default)"),
    cl::init(false), cl::cat(RefoldCategory));

// Short alias: -n (points to --no-lines)
static cl::alias NoLinesShort("n", cl::desc("Alias for --no-lines"),
                              cl::aliasopt(NoLines), cl::cat(RefoldCategory));

static cl::opt<bool> StrictMode(
    "strict",
    cl::desc("Enable strict refolding and authoritative proof audit"),
    cl::init(false), cl::cat(RefoldCategory));

// Short alias: -s (points to --strict)
static cl::alias StrictModeShort("s", cl::desc("Alias for --strict"),
                                 cl::aliasopt(StrictMode),
                                 cl::cat(RefoldCategory));

static cl::opt<std::string> ProofAuditModeOpt(
    "proof-audit",
    cl::desc("Proof-audit mode: off, probe, or strict. Defaults to strict "
             "with --strict and off otherwise."),
    cl::value_desc("mode"), cl::init(""), cl::cat(RefoldCategory));

static constexpr char Overview[] = R"(
  Deterministically reconstruct partially expanded C source from edited
  preprocessed output.

  Consumes:
    (1) the original preprocessed TU (.c.i),
    (2) a modified preprocessed output (.c.i.mod), and
    (3) the refold map JSON (.c.refold.json) emitted by clang’s --refold-map.

  It re-lexes and aligns the original and modified token streams, then projects
  the edits back through recorded includes, macros, and conditional structure to
  produce a stable, semantically equivalent C source that preserves the original
  preprocessing hierarchy.

  Modes:
    (1) Produce a refolding:
          clang-refold --pp foo.c.i --pp-mod foo.c.i.mod \
            --refold-map foo.c.refold.json --out foo.c.mod
    (2) Verify a refolding (token check):
          clang-refold --check foo.c.mod --refold-map foo.c.refold.json \
            --pp-mod foo.c.i.mod

       This mode re-preprocesses foo.c.mod using the captured pp_ctx (cwd/argv)
       embedded in the refold map JSON, lexes both token streams, and compares
       them while ignoring whitespace.
  )";

// ----------------------------- Main Program ----------------------------------

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  sys::PrintStackTraceOnErrorSignal(argv[0], true);

  cl::HideUnrelatedOptions(RefoldCategory);
  cl::ParseCommandLineOptions(argc, argv, Overview);

  REFOLD_LOG_INFO("log", "log level set to {0}", LogLevelOpt);

  // We output a custom error message if the following flags appear more than
  // once and remove cl::Required from the relevant cl::opt's. This is because
  // the error message would otherwise be misleading, stating that the option
  // must be specified at least once, which is not the case here.
  const bool onlyCheck = (CheckSrcPath.getNumOccurrences() != 0);

  auto parseProofAuditMode = []() -> RefoldEngine::ProofAuditMode {
    if (ProofAuditModeOpt.getNumOccurrences() == 0)
      return RefoldEngine::ProofAuditMode::Default;

    StringRef value(ProofAuditModeOpt.getValue());
    if (value.equals_insensitive("off"))
      return RefoldEngine::ProofAuditMode::Off;
    if (value.equals_insensitive("probe"))
      return RefoldEngine::ProofAuditMode::Probe;
    if (value.equals_insensitive("strict"))
      return RefoldEngine::ProofAuditMode::Strict;

    REFOLD_LOG_FATAL("options", "invalid --proof-audit value: {0} "
                     "(expected off, probe, or strict)",
          ProofAuditModeOpt);
    return RefoldEngine::ProofAuditMode::Default;
  };

  RefoldEngine::ProofAuditMode proofAuditMode = parseProofAuditMode();
  if (StrictMode &&
      proofAuditMode != RefoldEngine::ProofAuditMode::Default &&
      proofAuditMode != RefoldEngine::ProofAuditMode::Strict)
    REFOLD_LOG_FATAL("options", "--strict requires --proof-audit=strict");

  // Enforce exactly one supported invocation mode:
  //   (1) Refold:
  //       clang-refold --pp A.i --pp-mod B.i.mod --refold-map map.json --out \
  //         out.c.mod
  //   (2) Verify:
  //       clang-refold --check out.c.mod --refold-map map.json --pp-mod B.i.mod
  requireExactlyOnce("--refold-map", RefoldJSONPath);
  requireExactlyOnce("--pp-mod", PPModPath);

  const bool emitEditMap = EmitEditMapPath.getNumOccurrences() != 0;

  if (onlyCheck) {
    requireExactlyOnce("--check", CheckSrcPath);
    // Verify mode.
    if (PPPath.getNumOccurrences() != 0 ||
        ModifiedSrcPath.getNumOccurrences() != 0 || emitEditMap) {
      REFOLD_LOG_FATAL("cli", "invalid option combination: --check cannot be used with "
                   "--pp, --out, or --emit-edit-map");
    }
  } else {
    // Refold mode.
    requireExactlyOnce("--pp", PPPath);
    requireExactlyOnce("--out", ModifiedSrcPath);
    if (emitEditMap && EmitEditMapPath.getValue().empty())
      REFOLD_LOG_FATAL("cli", "--emit-edit-map requires a non-empty output path");
  }

  // Parse and validate the refold map JSON file.
  auto parsedObjOrErr = parseAndValidateJSON(RefoldJSONPath, RefoldSchema);
  if (!parsedObjOrErr) {
    handleAllErrors(parsedObjOrErr.takeError(), [&](const ErrorInfoBase &e) {
      REFOLD_LOG_FATAL("json", "failed to parse refold '{0}' JSON file: {1}",
            RefoldJSONPath, e.message());
    });
  } else {
    REFOLD_LOG_INFO("json", "refold JSON file '{0}' validated", RefoldJSONPath);
  }
  const json::Object &rootJson = *parsedObjOrErr;
  assert(!rootJson.empty() && "parsed refold map JSON object is empty");

  // Read files and tokenize.
  //
  // Refold mode deliberately treats --pp-mod as the raw edited PP replay
  // surface. That preserves comment/trivia insertions and directive-shaped
  // edits as source text for the existing owner/proof lattice. Check mode is
  // different: it preprocesses both the emitted source and --pp-mod because the
  // final oracle asks whether both replay to the same PP token stream.
  std::string aBytes, bBytes;
  auto ctxOrErr = parsePPCtx(rootJson);
  if (!ctxOrErr) {
    handleAllErrors(ctxOrErr.takeError(), [&](const ErrorInfoBase &e) {
      REFOLD_LOG_FATAL("model", "failed to parse pp_ctx from refold map: {0}",
            e.message());
    });
  }
  const PPCtx ctx = *ctxOrErr;
  const LangOptions lexLang =
      RefoldEngine::MakeLexLangOptions(ctx.lang);

  std::optional<PPCtx> checkCtx;
  if (onlyCheck) {
    checkCtx = ctx;

    // Preprocess the refolded C source:
    {
      auto ppOrErr = preprocessToBytes(CheckSrcPath, ctx);
      if (!ppOrErr) {
        handleAllErrors(ppOrErr.takeError(), [&](const ErrorInfoBase &e) {
          REFOLD_LOG_FATAL("pp", "failed to preprocess --check input: {0}", e.message());
        });
      }
      aBytes = std::move(*ppOrErr);
    }

    // Preprocess the edited preprocessed replay file.
    {
      auto ppOrErr = preprocessToBytes(PPModPath, ctx);
      if (!ppOrErr) {
        handleAllErrors(ppOrErr.takeError(), [&](const ErrorInfoBase &e) {
          REFOLD_LOG_FATAL("pp", "failed to preprocess --pp-mod input: {0}", e.message());
        });
      }
      bBytes = std::move(*ppOrErr);
    }
  } else {
    readFile(PPPath, aBytes);
    readFile(PPModPath, bBytes);
  }

  std::vector<PPTok> aToks, bToks;
  std::vector<std::size_t> aTokByteOff, bTokByteOff;
  std::vector<RefoldEngine::SidebandPragmaEdit> sidebandPragmaEdits;
  lexPPTokens(aBytes, aToks, aTokByteOff, lexLang);
  lexPPTokens(bBytes, bToks, bTokByteOff, lexLang);

  if (!onlyCheck) {
    std::vector<SidebandPragmaLine> aSidebandPragmas =
        collectSidebandPragmaLines(aBytes);
    std::vector<SidebandPragmaLine> bSidebandPragmas =
        collectSidebandPragmaLines(bBytes);
    annotateSidebandPragmaTokenGaps(aSidebandPragmas, aToks, aTokByteOff);
    annotateSidebandPragmaTokenGaps(bSidebandPragmas, bToks, bTokByteOff);

    if (!aSidebandPragmas.empty() || !bSidebandPragmas.empty()) {
      if (buildSidebandPragmaSourceEdits(
              rootJson, RefoldJSONPath, aSidebandPragmas, bSidebandPragmas, aToks,
              aTokByteOff, bBytes, bToks, bTokByteOff,
              sidebandPragmaEdits)) {
        // Keep the raw `.i` byte buffers intact, but remove preserved pragma
        // directive tokens from the sequences fed to the structural diff.  The
        // matching source directive edits are carried separately in
        // sidebandPragmaEdits, so comments/trivia around the original source
        // pragma stay on the normal TU edit path instead of being lost to
        // terminal raw-B fallback.
        filterSidebandPragmaTokens(aSidebandPragmas, aToks, aTokByteOff,
                                   aBytes.size());
        filterSidebandPragmaTokens(bSidebandPragmas, bToks, bTokByteOff,
                                   bBytes.size());
        REFOLD_LOG_DEBUG("pragma/sideband",
              "normalized sideband pragmas: A={0} B={1} sourceEdits={2}",
              aSidebandPragmas.size(), bSidebandPragmas.size(),
              sidebandPragmaEdits.size());
      } else {
        // Unsupported sideband forms, such as B-only pragma insertions without
        // a map-backed source anchor, remain in the token stream.  The existing
        // token-count/domain checks will route them through the explicit
        // fallback path rather than guessing a source placement.
        sidebandPragmaEdits.clear();
        REFOLD_LOG_DEBUG("pragma/sideband",
              "sideband pragma stream not fully modelled; keeping raw tokens "
              "for fallback classification");
      }
    }
  }

  REFOLD_LOG_DEBUG("lex", "{0} tokens={1} {2} tokens={3}", PPPath, aToks.size(), PPModPath,
        bToks.size());

  // Append the sentinel to both source offsets.
  if (bTokByteOff.empty() || bTokByteOff.back() != bBytes.size()) {
    if (bTokByteOff.empty() || bTokByteOff.back() < bBytes.size())
      bTokByteOff.push_back(bBytes.size());
  }
  if (aTokByteOff.empty() || aTokByteOff.back() != aBytes.size()) {
    if (aTokByteOff.empty() || aTokByteOff.back() < aBytes.size())
      aTokByteOff.push_back(aBytes.size());
  }

  if (onlyCheck && NoLines) {
    // Relax token comparison for location-sensitive predefined macros when
    // verifying a refolding produced with --no-lines.
    if (!checkCtx)
      REFOLD_LOG_FATAL("cli", "internal error: missing pp_ctx in --check mode");
    auto maskOrErr = buildNoLinesIgnoreMask(rootJson, *checkCtx, bToks);
    if (!maskOrErr) {
      handleAllErrors(maskOrErr.takeError(), [&](const ErrorInfoBase &e) {
        REFOLD_LOG_FATAL("check", "failed to build --no-lines ignore mask: {0}",
              e.message());
      });
    }
    if (Error err = compareTokensNoLinesAware(aToks, bToks, *maskOrErr)) {
      outs() << toString(std::move(err)) << "\n";
      outs() << "FAILURE!\n";
      return 1;
    }
    outs() << "SUCCESS!\n";
    return 0;
  }

  if (onlyCheck) {
    // Verification mode (normal comparison): compare preprocessed token streams
    // directly. The --no-lines special-case is handled above.
    if (Error err = compareTokens(aToks, bToks)) {
      outs() << toString(std::move(err)) << "\n";
      outs() << "FAILURE!\n";
      return 1;
    }
    outs() << "SUCCESS!\n";
    return 0;
  }

  // Default behavior: single refold.
  std::vector<MaterializedEditMapping> materializedEditMappings;
  std::vector<SourceGraphOutput> sourceGraphOutputs;
  auto refoldedOrErr = RefoldEngine::Refold(
      rootJson, aBytes, aToks, aTokByteOff, bBytes, bToks, bTokByteOff,
      NoLines, StrictMode, proofAuditMode, ModifiedSrcPath, sidebandPragmaEdits,
      emitEditMap ? &materializedEditMappings : nullptr,
      buildFinalLineControlValidationCallback(ModifiedSrcPath, ctx),
      &sourceGraphOutputs);
  if (!refoldedOrErr) {
    handleAllErrors(refoldedOrErr.takeError(), [&](const ErrorInfoBase &e) {
      REFOLD_LOG_FATAL("model", "failed to parse refold model: {0}", e.message());
    });
  }

  // Finally, serialize the refolded C source to the output file.
  std::error_code ec;
  raw_fd_ostream os(ModifiedSrcPath, ec, sys::fs::OF_Text);
  if (ec) {
    REFOLD_LOG_FATAL("src/write", "cannot write {0}: {1}", ModifiedSrcPath, ec.message());
  }
  os << *refoldedOrErr;
  os.close();
  REFOLD_LOG_INFO("finished", "wrote refolded C source: {0}", ModifiedSrcPath);

  writeSourceGraphOutputs(ModifiedSrcPath, sourceGraphOutputs);

  if (emitEditMap) {
    writeMaterializedEditMap(EmitEditMapPath.getValue(), PPModPath,
                             ModifiedSrcPath, materializedEditMappings);
    REFOLD_LOG_INFO("finished", "wrote materialized edit map: {0}",
         EmitEditMapPath.getValue());
  }
  return 0;
}
