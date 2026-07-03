//===--- RefoldSidebandPragmaEdits.cpp -------------------------*- C++ -*-===//
//
// Sideband pragma normalization and source-edit construction for clang-refold.
//
// See RefoldSidebandPragmaEdits.h for the public contract.  This file owns
// the internal canonicalization, JSON-collector, and edit-building helpers
// that produce sideband proof witnesses from the raw `-E -P` replay surfaces
// and the refold map JSON.
//
//===----------------------------------------------------------------------===//

#include "sideband/RefoldSidebandPragmaEdits.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "edit/RefoldTUEditPlanner.h"
#include "edit/RefoldTextEditAssembler.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTerminalProofSink.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldStructuralHunkDispatcher.h"
#include "source/RefoldToken.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

// --------------------- Sideband pragma normalization -------------------------

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

using SidebandSourceProof = OwnerLocalSourceEditProof;

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
std::vector<SidebandPragmaLine> collectSidebandPragmaLines(StringRef bytes) {
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

/// Certify each sideband pragma with the gap in the normal, non-sideband token
/// stream where the directive line appears.
///
/// Unknown pragmas can be textually identical.  Matching only the pragma text
/// makes deletion of the first `#pragma vendor note` indistinguishable from
/// deletion of the second.  The normal-token gap is a deterministic structural
/// anchor: it records whether the sideband directive appeared before token 0,
/// between tokens 5 and 6, and so on, after ignoring other sideband pragmas.
void annotateSidebandPragmaTokenGaps(std::vector<SidebandPragmaLine> &lines,
                                     ArrayRef<PPTok> toks,
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
/// position of the first normal B token after the sideband's gap, or EOF for
/// the final gap.  Sideband directive tokens are ignored when counting the gap.
static uint64_t byteOffsetForNormalTokenGap(StringRef bytes,
                                            ArrayRef<SidebandPragmaLine> lines,
                                            ArrayRef<PPTok> toks,
                                            ArrayRef<std::size_t> tokOff,
                                            uint64_t gap) {
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
/// The byte buffer itself is left untouched.  Kept token offsets therefore
/// still point into the original raw `.i` file, preserving correct B-slice
/// materializa- tion and terminal-fallback behavior while restoring the token
/// sequence that the refold map actually describes.
void filterSidebandPragmaTokens(ArrayRef<SidebandPragmaLine> lines,
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

static std::optional<uint64_t>
projectedBGapForSidebandLine(ArrayRef<int64_t> normalA2B,
                             const SidebandPragmaLine &line) {
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
    ArrayRef<PPTok> rawBToks, ArrayRef<std::size_t> rawBTokOff, uint64_t bStart,
    uint64_t bEnd) {
  assert(bStart < bEnd && "replacement block must contain B sideband lines");
  uint64_t end = bLines[static_cast<size_t>(bEnd - 1)].end;
  const uint64_t gap = bLines[static_cast<size_t>(bStart)].normalTokenGap;
  uint64_t limit =
      byteOffsetForNormalTokenGap(bBytes, bLines, rawBToks, rawBTokOff, gap);
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
  if (!stringutils::consumeIdentifier(canonicalText, pos, end, namespaceName) ||
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

static bool
tokenMapHasTokenInOwnerRange(StringRef file, uint64_t begin, uint64_t end,
                             ArrayRef<JsonTokMapEntryForSideband> tokmap) {
  return llvm::any_of(tokmap, [&](const JsonTokMapEntryForSideband &entry) {
    return StringRef(entry.file) == file &&
           rangesOverlap(entry.b, entry.e, begin, end);
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
inferSidebandSourceProof(const SidebandPragmaLine &line, uint64_t projectedAGap,
                         StringRef tuPath,
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

  return SidebandSourceProof::ZeroWidthInsertion(tuPath, *tuByte, std::nullopt);
}

/// Attach an owner include id to a header-owned pragma when the map does not
/// already provide one.
///
/// Older producer maps record raw `#pragma` source ranges but not the include
/// instance that opened the header.  A header sideband edit is still safe to
/// materialize when the physical header path has exactly one include instance
/// in the map.  If repeated includes make the owner ambiguous, leave the owner
/// unset so the consumer fails closed instead of editing the wrong instance.
static void
inferUniqueHeaderPragmaOwners(std::vector<JsonPragmaItem> &pragmas,
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
  constexpr uint64_t noOwner = std::numeric_limits<uint64_t>::max();

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

      const uint64_t ownerKey = owner ? *owner : noOwner;
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
        chosen.ownerIncludeId ? *chosen.ownerIncludeId : noOwner;
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
static std::vector<uint32_t>
buildSidebandOwnerDepthGaps(uint64_t tokenCount,
                            ArrayRef<JsonIncludeItemForSideband> includes) {
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
bool buildSidebandPragmaSourceEdits(
    const json::Object &rootJson, StringRef refoldMapPath,
    ArrayRef<SidebandPragmaLine> aLines, ArrayRef<SidebandPragmaLine> bLines,
    ArrayRef<PPTok> rawAToks, ArrayRef<std::size_t> rawATokOff,
    StringRef bBytes, ArrayRef<PPTok> rawBToks,
    ArrayRef<std::size_t> rawBTokOff, std::vector<SidebandPragmaEdit> &edits) {
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

    REFOLD_LOG_TRACE(
        "pragma/sideband",
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

  using SidebandBReplayProof = OwnerLocalBReplayProof;

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
    std::optional<SidebandPragmaEdit> edit =
        SidebandPragmaEdit::Create(std::move(source), std::move(replay),
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

bool appendSidebandPragmaSourceEdits(
    llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    llvm::StringRef tuPath, llvm::StringRef tuBytes,
    const RefoldPathIdentity &pathIdentity,
    const RefoldTextEditAssembler &textEditAssembler,
    const RefoldProofLattice &proofLattice,
    const RefoldTerminalProofSink &terminalSink,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher) {
  if (sidebandPragmaEdits.empty())
    return true;

  for (const SidebandPragmaEdit &sideband : sidebandPragmaEdits) {
    // The owner occurrence, source range, B byte envelope, and payload are the
    // emitted edit facts classified by OwnerLocalSourceEditProof.  Delegate to
    // the shared validation/reporting gate so the TU and include-owned paths
    // cannot drift in their terminal-fallback policy.
    if (!validateAndReportSidebandPragmaEditProof(
            sideband, static_cast<uint64_t>(tuBytes.size()), terminalSink,
            "pragma/sideband", /*traceSuccess=*/true))
      return false;

    const auto sourceRange = sideband.SourceByteRange();

    // The current clang-refold artifact is one emitted TU source file.  A
    // sideband pragma whose source location is inside a header must be handled
    // by an include/materialization proof; applying it blindly to the TU would
    // edit the wrong owner.  Reject that class explicitly instead of silently
    // preserving or dropping a header pragma.
    if (!pathIdentity.PathsEqual(sideband.SourcePath(), tuPath)) {
      if (!sideband.HasConcreteIncludeOwner()) {
        terminalSink.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::PragmaBoundaryKnown,
                TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary),
            "pragma/sideband",
            llvm::formatv(
                "sideband pragma edit targets non-TU owner path='{0}' "
                "site=[{1},{2}) without a unique include owner",
                sideband.SourcePath(), sourceRange.first, sourceRange.second)
                .str());
        return false;
      }

      continue;
    }

    if (!sideband.SourceIsWithinOwnerBytes(
            static_cast<uint64_t>(tuBytes.size()))) {
      terminalSink.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::PragmaBoundaryKnown,
              TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary),
          "pragma/sideband",
          llvm::formatv(
              "sideband pragma edit has invalid TU range site=[{0},{1}) "
              "tuSize={2}",
              sourceRange.first, sourceRange.second, tuBytes.size())
              .str());
      return false;
    }

    // Sideband pragmas are zero-normal-token artifacts: their raw directive
    // text appeared in the `.i` replay surface, but the producer deliberately
    // did not count that directive text as ordinary PP tokens.  Once the driver
    // removes the sideband directive tokens from A/B before diffing, the source
    // pragma itself still needs an explicit source edit so preserved comments
    // and nearby code can stay on the normal structural path.
    //
    // Apply the same line-state repair used for ordinary TU byte edits.  A
    // sideband block replacement can change the number of physical directive
    // lines before preserved TU suffix bytes; in --with-lines mode the suffix
    // must resume at its original logical TU line instead of drifting with the
    // replacement's physical line count.  The materialized edit-map range still
    // describes only the B-side sideband payload, not the synthetic #line
    // directive that may be appended for resynchronization.
    ResyncOutcome ro = textEditAssembler.ApplyResyncOrPend(
        tuBytes, sourceRange.first, sourceRange.second,
        sideband.ReplacementText(), tuPath);
    TextEdit edit{sourceRange.first,
                  sourceRange.second,
                  std::move(ro.text),
                  std::move(ro.pending),
                  std::nullopt,
                  {},
                  {},
                  {}};
    edit.lineControlPruneCandidates = std::move(ro.lineControlPruneCandidates);
    textEditAssembler.CertifyTextEditMaterializedBReplayProof(edit, sideband);
    textEditAssembler.AttachAcceptedResultCarrier(
        edit,
        proofLattice.AcceptedCandidateBuilder()
            .BuildAcceptedTUTextEditCandidate(
                AcceptedPathKind::TUByteSpanConservativeEdit, sourceRange.first,
                sourceRange.second, sideband.ReplacementText()));
    structuralHunkDispatcher.AddTUEdit(std::move(edit));
  }

  return true;
}

bool tuInsertionBeforeMaterializedInclude(
    const RefoldTUEditPlanner &planner, const RefoldModel &model,
    llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    const diffutils::Hunk &h, llvm::StringRef tuPath,
    const std::pair<uint64_t, uint64_t> &span, bool requireVisibleReplayText) {
  if (!h.isInsertOnly() || span.first != span.second)
    return false;
  if (!planner.AnchorToExactSlotBoundaryFromPPGap(tuPath, h.aStart))
    return false;

  const uint64_t maxPP = model.GetTokensCountA();
  std::optional<uint64_t> leftInc =
      (h.aStart > 0) ? model.InnermostIncludeAtPP(h.aStart - 1) : std::nullopt;
  std::optional<uint64_t> rightInc =
      h.aStart < maxPP ? model.InnermostIncludeAtPP(h.aStart) : std::nullopt;
  if (leftInc || !rightInc)
    return false;

  return llvm::any_of(
      sidebandPragmaEdits, [&](const SidebandPragmaEdit &sideband) {
        return sideband.TargetsInclude(*rightInc) &&
               (!requireVisibleReplayText || sideband.EmitsVisibleReplayText());
      });
}

} // namespace refold
} // namespace clang
