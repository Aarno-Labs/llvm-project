//===--- RefoldNoLinesPruning.cpp -------------------------------*- C++ -*-===//
//
// `--no-lines` pruning recheck pipeline for clang-refold.
//
// See RefoldNoLinesPruning.h for the public contract.  This file owns the
// internal helpers that recover location-sensitive predefined macro spans
// (`__LINE__`, `__FILE__`, `__FILE_NAME__`, `__BASE_FILE__`), assemble the
// per-B-token ignore mask used by `--check --no-lines`, and run the
// mask-aware token comparison.  The generic preprocessor invocation
// (`preprocessToBytes`) and byte-exact token comparison (`compareTokens`)
// live in `core/RefoldPreprocessRecheck.{h,cpp}`; the raw-lexer producer
// (`lexPPTokens`) lives next to `PPTok` in `source/RefoldToken.{h,cpp}`.
//
//===----------------------------------------------------------------------===//

#include "line-control/RefoldNoLinesPruning.h"

#include "core/RefoldLangOptions.h"
#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "line-control/FinalLineControlModel.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"
#include "util/StringUtils.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

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
    REFOLD_LOG_DEBUG(
        "check",
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
/// Cycles or missing references terminate the chain so malformed metadata
/// cannot make recovery loop indefinitely.
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
static void
addPathSpellingsForNoLinesBuiltin(StringRef builtinName, StringRef path,
                                  std::set<std::string> &spellings) {
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
    // Fail closed when the allowed-spelling set is empty: it means the exact
    // physical __LINE__ value could not be computed (e.g. the invocation source
    // was unreadable or its offset was unrecoverable). Without the proven value
    // this token cannot be certified as a __LINE__ expansion. Matching any
    // all-digit token here would mark it builtin-sensitive and thereby ignore —
    // silently absorbing — a genuine numeric-literal edit at this position.
    // Requiring an exact proven spelling keeps recovery sound; the worst case is
    // a conservative spurious diff, never a dropped edit.
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
      REFOLD_LOG_DEBUG(
          "check",
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
    std::sort(
        events.begin(), events.end(),
        [](const NoLinesSensitiveEvent &lhs, const NoLinesSensitiveEvent &rhs) {
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
    std::vector<std::vector<uint8_t>> matches(eCount,
                                              std::vector<uint8_t>(cCount, 0));
    for (size_t ei = 0; ei < eCount; ++ei) {
      for (size_t ci = 0; ci < cCount; ++ci) {
        if (tokenMatchesNoLinesBuiltinEvent(a0Toks[candidates[ci]], events[ei]))
          matches[ei][ci] = 1;
      }
    }

    // Count monotone assignments, saturated at two. We only need to know
    // whether the assignment is absent, unique, or ambiguous.
    std::vector<std::vector<uint8_t>> ways(eCount + 1,
                                           std::vector<uint8_t>(cCount + 1, 0));
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
      REFOLD_LOG_DEBUG(
          "check",
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
          REFOLD_LOG_DEBUG(
              "check",
              "--no-lines: recovered zero-length builtin {0}#{1} at "
              "original token index {2} under anchor macro #{3}",
              events[ei].item->name, events[ei].item->id, tokIndex, anchor->id);
        }
        break;
      }
    }
  }
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
Expected<std::vector<uint8_t>>
buildNoLinesIgnoreMask(const json::Object &rootJson, const PPCtx &ctx,
                       ArrayRef<PPTok> bPPToks) {
  auto sourceOrErr = RefoldModel::ParseSourcePath(rootJson);
  if (!sourceOrErr)
    return sourceOrErr.takeError();

  auto ppOrErr = preprocessToBytes(*sourceOrErr, ctx);
  if (!ppOrErr)
    return ppOrErr.takeError();
  std::string a0Bytes = std::move(*ppOrErr);

  std::vector<PPTok> a0Toks;
  std::vector<std::size_t> a0Off;
  const LangOptions lexLang = makeRefoldLexLangOptions(ctx.lang);
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

Error compareTokensNoLinesAware(ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
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
          formatv("token mismatch at index {0}: A='{1}' B='{2}'", i, aDbg, bDbg)
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

} // namespace refold
} // namespace clang
