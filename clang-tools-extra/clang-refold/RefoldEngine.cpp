//===--- RefoldEngine.cpp ---------------------------------------*- C++ -*-===//
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

#include "RefoldEngine.h"
#include "RefoldLog.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>
#include <limits>

using namespace llvm;

namespace clang {
namespace refold {

namespace {
constexpr bool INSERT_AFTER_LEFT_TOKEN_EOL = true;
} // namespace

// ========================== Public entry points ==========================

Expected<std::string>
RefoldEngine::Refold(const json::Object &rootJson, StringRef aSource,
                     const std::vector<PPTok> &aToks,
                     const std::vector<std::size_t> &aTokOff, StringRef bSource,
                     const std::vector<PPTok> &bToks,
                     const std::vector<std::size_t> &bTokOff) {
  // Build the refold model based on the parsed JSON object.
  auto mOrErr = RefoldModel::FromJson(rootJson);
  if (!mOrErr)
    return mOrErr.takeError();
  return Refold(*mOrErr, aSource, aToks, aTokOff, bSource, bToks, bTokOff);
}

std::string RefoldEngine::Refold(const RefoldModel &model, StringRef aSource,
                                 const std::vector<PPTok> &aToks,
                                 const std::vector<std::size_t> &aTokOffIn,
                                 StringRef bSource,
                                 const std::vector<PPTok> &bToks,
                                 const std::vector<std::size_t> &bTokOffIn) {
  const std::size_t expectedCount =
      static_cast<std::size_t>(model.GetTokensCountA());
  if (aToks.size() != expectedCount) {
    fatal(
        "tok",
        "token count mismatch: JSON says {0} expected, but driver produced {1}",
        expectedCount, aToks.size());
  }

  // Copy offsets so we can append sentinel if needed (we won’t mutate caller’s
  // vectors).
  std::vector<std::size_t> aTokOff = aTokOffIn;
  std::vector<std::size_t> bTokOff = bTokOffIn;

  const std::string tuPath = model.GetSourcePath();

  std::string tuBytes;
  {
    auto bufOrErr = MemoryBuffer::getFile(tuPath);
    if (!bufOrErr) {
      // Fatal and stop: unreachable past this point.
      fatal("src/load", "failed to read C source: {0} ({1})", tuPath,
            bufOrErr.getError().message());
    }

    // Copy the file bytes into a std::string (UTF-8 is treated as raw bytes
    // here).
    tuBytes.assign(bufOrErr.get()->getBufferStart(),
                   bufOrErr.get()->getBufferEnd());
  }

  // Append the sentinel to both source offsets.
  if (bTokOff.empty() || bTokOff.back() != bSource.size()) {
    if (bTokOff.empty() || bTokOff.back() < bSource.size())
      bTokOff.push_back(bSource.size());
  }
  if (aTokOff.empty() || aTokOff.back() != aSource.size()) {
    if (aTokOff.empty() || aTokOff.back() < aSource.size())
      aTokOff.push_back(aSource.size());
  }

  // 1) Generate token sequences and A->B anchor map.
  auto aSeq = MapLexemes(aToks, aTokOff);
  auto bSeq = MapLexemes(bToks, bTokOff);
  auto a2b = diffutils::lcsMapAB(aSeq, bSeq);

  auto writeMap = [](StringRef filename, const std::vector<int> &a2b) {
    std::error_code ec;
    llvm::raw_fd_ostream os(filename.str(), ec, llvm::sys::fs::OF_Text);
    if (ec) {
      fatal("a2b/write", "open file '{0}' failed: {1}", filename, ec.message());
    }

    for (int v : a2b)
      os << v << '\n';

    os.flush();
    if (os.has_error()) {
      os.clear_error();
      fatal("a2b/write", "write on file '{0}' failed", filename);
    }
  };

  SmallString<256> lcsMapFile;
  sys::fs::expand_tilde("~/lcsmap.cpp.txt", lcsMapFile);
  writeMap(lcsMapFile, a2b);

  // Sanity check: map must have a strict ordering.
  for (int i = 0, last = -1; i < static_cast<int>(a2b.size()); ++i) {
    if (a2b[i] < 0)
      continue;
    if (a2b[i] <= last)
      fatal("lcs", "non-monotone map at A[{0}]", i);
    last = a2b[i];
  }

  debug("lcs", "A={0} toks, B={1} toks", aSeq.size(), bSeq.size());
  int mapped = 0;
  for (int v : a2b) {
    if (v >= 0)
      mapped++;
  }
  debug("lcs", "mapped A->B = {0} ({1:F1}%)", mapped,
        100.0 * mapped / std::max<std::size_t>(1U, aSeq.size()));

  info("plan", "TU={0} includes={1} macroInvocations={2} tokmap={3}", tuPath,
       model.GetIncludes().size(), model.GetMacroInvocations().size(),
       model.GetTokmapByPP().size());

  // 2) Diff hunks (changed A-token intervals -> B-token intervals).
  auto hunks = diffutils::hunksFromMap(a2b, static_cast<int>(aSeq.size()),
                                       static_cast<int>(bSeq.size()));

  for (std::size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    const bool isIns = (h.aStart == h.aEnd) && (h.bStart < h.bEnd);
    const bool isDel = (h.aStart < h.aEnd) && (h.bStart == h.bEnd);
    const bool isRep = (h.aStart < h.aEnd) && (h.bStart < h.bEnd);

    const char *kind =
        isIns ? "INS" : (isDel ? "DEL" : (isRep ? "REP" : "UNK"));

    llvm::StringRef bfrag;
    if (h.bStart < h.bEnd) {
      const std::size_t b0 = bTokOff[static_cast<std::size_t>(h.bStart)];
      const std::size_t b1 = bTokOff[static_cast<std::size_t>(h.bEnd)];
      // Defensive clamping (should already be valid if offsets have sentinel):
      const std::size_t lo = std::max<std::size_t>(0U, b0);
      const std::size_t hi = std::max<std::size_t>(lo, b1);
      bfrag = bSource.substr(lo, hi - lo);
    }

    // If your helpers take std::string, convert as needed:
    std::string shown =
        stringutils::showWS(stringutils::clip(bfrag.str(), 160));

    debug("hunks", "#{0} {1} A[{2},{3}) -> B[{4},{5})  B='{6}'", i, kind,
          h.aStart, h.aEnd, h.bStart, h.bEnd, shown);
  }

  // 3) Classify hunks and collect per-target edits.
  std::vector<TextEdit> tuEdits;
  std::map<int, IncludeEdits> perInclude; // includeId -> edits
  std::map<std::optional<int>, std::vector<MacroPatch>> macroPatchesByOwner;

  // -> macro patches
  for (std::size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // a) Prefer include expansion if available.
    if (auto *inc = SmallestCoveringInclude(model, h.aStart, h.aEnd)) {
      debug("classify",
            "#{0} -> INCLUDE id={1} path={2}  A[{3},{4})->B[{5},{6})", i,
            inc->id,
            (inc->resolvedPath ? *inc->resolvedPath
                               : StripHeaderToken(inc->target)),
            h.aStart, h.aEnd, h.bStart, h.bEnd);
      auto patch = BuildIncludeInsertionPatch(*inc, h, bSource, bTokOff);
      perInclude[inc->id].include = inc;
      perInclude[inc->id].patches.push_back(std::move(patch));
      continue;
    }

    // b) Macro call-site?
    if (auto *m = SmallestCoveringMacro(model, h.aStart, h.aEnd)) {
      if (m->getInvB() != -1 && m->getInvE() != -1) {
        debug("classify",
              "#{0} -> MACRO inv='{1}' owner={2} invFile={3} "
              "A[{4},{5})->B[{6},{7})",
              i, m->invocationText ? *m->invocationText : "null",
              m->ownerIncludeId ? *m->ownerIncludeId : -1,
              m->invFile ? *m->invFile : "null", h.aStart, h.aEnd, h.bStart,
              h.bEnd);
        auto mp =
            BuildMacroInvocationPatchWholeCover(*m, h, a2b, bSource, bTokOff);
        macroPatchesByOwner[m->ownerIncludeId].push_back(std::move(mp));
        continue;
      }
    }

    // c) TU edit?
    if (HunkMapsToTU(model, h.aStart, h.aEnd, tuPath)) {
      auto span = TUByteSpan(model, h.aStart, h.aEnd, tuPath); // [b,e)
      std::string repl;
      if (span[0] >= 0 && h.bStart < h.bEnd) {
        std::size_t b0 = bTokOff[h.bStart], b1 = bTokOff[h.bEnd];
        repl.assign(bSource.data() + b0, bSource.data() + b1);
      }

      if (span[0] >= 0) {
        // Is this span replacing a TU "gap" (bytes that are all whitespace)?
        std::string original;
        if (span[1] > span[0])
          original.assign(tuBytes.data() + span[0], tuBytes.data() + span[1]);
        else if (span[1] < span[0])
          fatal("tu/span", "invalid TU byte span: [{0},{1})", span[0], span[1]);
        bool replacingGap =
            !original.empty() && stringutils::isAsciiWhitespace(original);

        // If we’re replacing a non-empty TU gap and the inserted text doesn’t
        // start with WS, prefix EXACTLY ONE space from the gap to preserve
        // “return injected” (no double spaces).
        if (replacingGap && !repl.empty() && !stringutils::isWs(repl.front()))
          repl.insert(repl.begin(), ' ');

        // Final boundary padding:
        // - allowLeft only if we did NOT already preserve a gap (avoids
        // double-space)
        // - always allowRight (covers cases like “…0” + “: 1” at zero-width
        // sites)
        repl = PadAtBoundaries(tuBytes, span[0], span[1], std::move(repl),
                               /*allowLeft*/ !replacingGap,
                               /*allowRight*/ true);

        debug("classify", "#{0} -> TU  bytes=[{1},{2}) repl='{3}'", i, span[0],
              span[1], stringutils::showWS(stringutils::clip(repl, 160)));

        tuEdits.push_back(TextEdit{span[0], span[1], std::move(repl)});
        continue;
      }
    }

    fatal(
        "hunks",
        "no covering item (macro/include/TU) for changed A-interval [{0},{1})",
        h.aStart, h.aEnd);
  }

  // Normalize/coalesce include-side insertions.
  NormalizeIncludeInsertions(perInclude, bSource, bTokOff);

  // 4) Materialize include expansions bottom-up (nested first). Build child
  // lists by parent include id.
  std::map<int, std::vector<const RefoldModel::IncludeItem *>> children;
  for (const auto &ii : model.GetIncludes()) {
    if (ii.parent)
      children[*ii.parent].push_back(&ii);
  }

  debug("include/tree", "BEGIN include children");
  for (const auto &[parentId, items] : children) {
    std::string pName = "#" + std::to_string(parentId);
    for (const RefoldModel::IncludeItem *child : items) {
      debug("include/tree",
            "{0} -> #{1} target={2} resolved={3} sitePath={4} site=[{5},{6}) "
            "cover=[{7},{8})",
            pName, child->id, child->target,
            child->resolvedPath ? *child->resolvedPath : "null",
            child->sitePath, child->siteB, child->siteE, child->cover.begin,
            child->cover.end);
    }
  }
  debug("include/tree", "END include children");

  // Cache for realized expansion text per include id.
  std::map<int, std::string> includeExpansion;

  // Build the set of include-ids that must be realized.
  std::set<int> seeds;

  // (a) Direct include edits.
  for (auto &kv : perInclude)
    seeds.insert(kv.first);

  // (b) Macro-owned work INSIDE headers (ownerIncludeId != null).
  for (auto &kv : macroPatchesByOwner) {
    if (kv.first)
      seeds.insert(*kv.first);
  }

  // (c) Pull in all ancestors up to the TU.
  for (int id : std::vector<int>(seeds.begin(), seeds.end())) {
    const auto *cur = model.GetIncludeById(id);
    while (cur && cur->parent) {
      seeds.insert(*cur->parent);
      cur = model.GetIncludeById(*cur->parent);
    }
  }

  // (d) Realize each include once (memoization lives inside
  // MaterializeIncludeExpansion).
  for (int incId : seeds) {
    MaterializeIncludeExpansion(model, incId, perInclude, macroPatchesByOwner,
                                children, includeExpansion);
  }

  // 5a) TU macro patches (ownerIncludeId == null) and include expansions at TU
  // sites.
  if (auto it = macroPatchesByOwner.find(std::nullopt);
      it != macroPatchesByOwner.end()) {
    for (const auto &mp : it->second) {
      auto text =
          PadAtBoundaries(tuBytes, mp.invStart, mp.invEnd, mp.replacement,
                          /*allowLeft*/ false, /*allowRight*/ true);
      tuEdits.push_back(TextEdit{mp.invStart, mp.invEnd, std::move(text)});
    }
  }

  // 5b) TU include expansions: includes with parent == null and site in TU,
  // only if we realized an expansion.
  for (const auto &kv : includeExpansion) {
    const auto *inc = model.GetIncludeById(kv.first);
    if (!inc)
      continue;
    if (!inc->parent && PathsEqual(inc->sitePath, tuPath)) {
      tuEdits.push_back(TextEdit{inc->siteB, inc->siteE, kv.second});
    }
  }

  // Apply TU edits in descending order of start offset.
  std::sort(
      tuEdits.begin(), tuEdits.end(),
      [](const TextEdit &a, const TextEdit &b) { return a.start > b.start; });

  std::string out(tuBytes);
  for (const auto &e : tuEdits) {
    if (e.start < 0 || e.end < e.start || e.end > static_cast<int>(out.size()))
      fatal("tu/edits", "bad TU edit bounds [{0},{1})", e.start, e.end);
    out.replace(static_cast<std::size_t>(e.start),
                static_cast<std::size_t>(e.end - e.start), e.text);
  }
  return out;
}

// ================== A ↔ B token mapping & diff utilities ==================

std::vector<std::string>
RefoldEngine::MapLexemes(const std::vector<PPTok> &toks,
                         const std::vector<std::size_t> &offs) {
  std::vector<std::string> out;
  out.reserve(toks.size());
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const auto &s = toks[i].spelling;
    if (stringutils::isAsciiWhitespace(s)) {
      // position-tied, cannot anchor elsewhere
      out.emplace_back("WS@" + std::to_string(offs[i]));
    } else {
      out.emplace_back(s);
    }
  }
  return out;
}

// ============ Boundary helpers ============

bool RefoldEngine::BoundaryGlues(char left, char right) {
  const bool leftId = stringutils::isIdentChar(left);
  const bool rightId = stringutils::isIdentChar(right);
  if (leftId && rightId)
    return true; // e.g., return + injected → returninjected
  if (leftId && right == '(')
    return true; // e.g., foo(…)
  // Operator-like punctuation right after an identifier/number benefits from a
  // space
  static constexpr char OPS[] = ":?+-*/%&|^<>=!";
  if (leftId && std::char_traits<char>::find(OPS, sizeof(OPS) - 1, right))
    return true; // e.g., 0: → 0 :
  return false;
}

std::string RefoldEngine::PadAtBoundaries(StringRef base, int start, int end,
                                          std::string text, bool allowLeft,
                                          bool allowRight) {
  const int f = stringutils::firstNonWsIdx(text);
  const int l = stringutils::lastNonWsIdx(text);
  const char leftC = (start > 0 && start <= static_cast<int>(base.size()))
                         ? base[start - 1]
                         : '\0';
  const char rightC = (end < static_cast<int>(base.size())) ? base[end] : '\0';

  const bool hasLeadingWS = (f > 0);
  const bool hasTrailingWS =
      (l >= 0 && l + 1 < static_cast<int>(text.size()) &&
       stringutils::isWs(text[static_cast<std::size_t>(l + 1)]));

  if (allowLeft && !hasLeadingWS && f >= 0) {
    if (BoundaryGlues(leftC, text[static_cast<std::size_t>(f)])) {
      text.insert(text.begin(), ' ');
    }
  }
  if (allowRight && !hasTrailingWS && l >= 0) {
    if (BoundaryGlues(text[static_cast<std::size_t>(l)], rightC)) {
      text.push_back(' ');
    }
  }
  return text;
}

bool RefoldEngine::IsAtLineIndent(StringRef s, int pos) {
  int i = pos - 1;
  while (i >= 0 && i < static_cast<int>(s.size()) &&
         s[static_cast<std::size_t>(i)] != '\n' &&
         s[static_cast<std::size_t>(i)] != '\r') {
    char c = s[static_cast<std::size_t>(i)];
    if (c != ' ' && c != '\t')
      return false;
    --i;
  }
  return true;
}

std::array<int, 3> RefoldEngine::LineAndFuncNameStart(StringRef text, int pos) {
  const int n = static_cast<int>(text.size());
  int bol = pos;
  while (bol > 0) {
    char c = text[static_cast<std::size_t>(bol - 1)];
    if (c == '\n' || c == '\r')
      break;
    --bol;
  }
  int eol = bol;
  while (eol < n) {
    char c = text[static_cast<std::size_t>(eol)];
    if (c == '\n' || c == '\r')
      break;
    ++eol;
  }
  // First '(' on the line
  int lparen = -1;
  for (int i = bol; i < eol; ++i) {
    if (text[static_cast<std::size_t>(i)] == '(') {
      lparen = i;
      break;
    }
  }
  int fnStart = bol; // default is none
  if (lparen >= 0) {
    int j = lparen - 1;
    // Skip spaces between name and '('
    while (j >= bol && (text[static_cast<std::size_t>(j)] == ' ' ||
                        text[static_cast<std::size_t>(j)] == '\t'))
      --j;
    // Skip pointer stars immediately to the left of name
    while (j >= bol && text[static_cast<std::size_t>(j)] == '*')
      --j;
    // Now back over identifier
    while (j >= bol &&
           stringutils::isIdentChar(text[static_cast<std::size_t>(j)]))
      --j;
    fnStart = std::max(bol, j + 1);
  }
  return {bol, fnStart, eol};
}

int RefoldEngine::ShiftAnchorToLineIndentIfAtFuncName(StringRef text, int pos) {
  auto b = LineAndFuncNameStart(text, pos);
  int bol = b[0], fnStart = b[1], eol = b[2];
  if (pos == fnStart) {
    int p = bol;
    while (p < eol) {
      char c = text[static_cast<std::size_t>(p)];
      if (c == ' ' || c == '\t') {
        ++p;
        continue;
      }
      break;
    }
    return p;
  }
  return pos;
}

// ===================== Owner resolution & TU mapping ======================

const RefoldModel::IncludeItem *
RefoldEngine::SmallestCoveringInclude(const RefoldModel &model, int aLo,
                                      int aHi) {
  const RefoldModel::IncludeItem *best = nullptr;
  int bestWidth = std::numeric_limits<int>::max();
  for (const auto &inc : model.GetIncludes()) {
    if (inc.cover.begin < 0 || inc.cover.end < 0)
      continue;
    if (inc.cover.begin <= aLo && aHi <= inc.cover.end) {
      int width = inc.cover.end - inc.cover.begin;
      if (width < bestWidth) {
        best = &inc;
        bestWidth = width;
      }
    }
  }
  return best;
}

const RefoldModel::MacroInvocation *
RefoldEngine::SmallestCoveringMacro(const RefoldModel &model, int aStart,
                                    int aEnd) {
  const RefoldModel::MacroInvocation *best = nullptr;
  for (const auto &m : model.GetMacroInvocations()) {
    int cb = m.cover.begin, ce = m.cover.end;
    if (cb < 0 || ce < 0)
      continue;
    if (cb <= aStart && aEnd <= ce) { // covers [aStart, aEnd)
      if (!best) {
        best = &m;
      } else {
        int len = ce - cb;
        int blen = best->cover.end - best->cover.begin;
        if (len < blen || (len == blen && m.id < best->id))
          best = &m;
      }
    }
  }
  return best;
}

bool RefoldEngine::HunkMapsToTU(const RefoldModel &model, int a0, int a1,
                                StringRef tuPath) {
  bool sawAnyTU = false;
  for (int pp = a0; pp < a1; ++pp) {
    auto it = model.GetTokmapByPP().find(pp);
    if (it == model.GetTokmapByPP().end())
      continue; // ignore unmapped (spaces/tabs/newlines)
    const auto &t = it->second;
    if (!PathsEqual(t.file, tuPath))
      return false; // spans a non-TU mapping
    sawAnyTU = true;
  }
  // Treat insertions (a0==a1) and whitespace-only ranges as TU-owned; span
  // computed by neighbors.
  return sawAnyTU || (a0 == a1);
}

std::array<int, 2> RefoldEngine::TUByteSpan(const RefoldModel &model, int a0,
                                            int a1, StringRef tuPath) {
  constexpr int MAX = std::numeric_limits<int>::max();
  int bMin = MAX, eMax = -1;

  // First pass: try to find mapped TU tokens inside [a0,a1)
  for (int pp = a0; pp < a1; ++pp) {
    auto it = model.GetTokmapByPP().find(pp);
    if (it == model.GetTokmapByPP().end())
      continue; // ignore unmapped (whitespace)
    const auto &t = it->second;
    if (PathsEqual(t.file, tuPath)) {
      if (t.b < bMin)
        bMin = t.b;
      if (t.e > eMax)
        eMax = t.e;
    }
  }
  if (bMin != MAX)
    return {bMin, eMax};

  uint64_t fileLen = 0;
  if (auto ec = sys::fs::file_size(tuPath, fileLen)) {
    fatal("tu/bytespan", "file_size() failed: {0} (value={1} category={2})",
          ec.message(), ec.value(), ec.category().name());
  }
  if (fileLen > std::numeric_limits<int>::max()) {
    fatal("tu/bytespan", "file '{0}' is too large to fit in int ({1} bytes)",
          tuPath, fileLen);
  }

  // Left boundary: end of the closest preceding TU-mapped token (or BOF).
  int leftE = -1;
  for (int p = a0 - 1; p >= 0; --p) {
    int e = ByteEndForPPInFile(model, tuPath, p, /*fallbackToEOF*/ false,
                               static_cast<int>(fileLen));
    if (e >= 0) {
      leftE = e;
      break;
    }
    auto it = model.GetTokmapByPP().find(p);
    if (it != model.GetTokmapByPP().end() &&
        !PathsEqual(it->second.file, tuPath))
      break; // crossed into non-TU region
  }
  if (leftE < 0)
    leftE = 0; // BOF

  // Right boundary: begin of the closest following TU-mapped token (or EOF).
  int rightB = -1;
  const int tokCount = static_cast<int>(model.GetTokmapByPP().size());
  for (int p = a1; p < tokCount; ++p) {
    int b = ByteStartForPPInFile(model, tuPath, p, /*fallbackToEOF*/ false,
                                 static_cast<int>(fileLen));
    if (b >= 0) {
      rightB = b;
      break;
    }
    auto it = model.GetTokmapByPP().find(p);
    if (it != model.GetTokmapByPP().end() &&
        !PathsEqual(it->second.file, tuPath))
      break; // crossed into non-TU region
  }
  if (rightB < 0)
    rightB = static_cast<int>(fileLen); // EOF

  return {leftE, rightB};
}

// ==================== Patch builders (include & macro) ====================

RefoldEngine::IncludePatch RefoldEngine::BuildIncludeInsertionPatch(
    const RefoldModel::IncludeItem &inc, const diffutils::Hunk &h,
    StringRef bSource, const std::vector<std::size_t> &bTokOff) {
  IncludePatch p;
  p.include = &inc;
  p.aStart = h.aStart;
  p.aEnd = h.aEnd;
  p.bStart = h.bStart;
  p.bEnd = h.bEnd;
  p.insertBytes.assign(bSource.data() + bTokOff[h.bStart],
                       bSource.data() + bTokOff[h.bEnd]);
  return p;
}

RefoldEngine::MacroPatch RefoldEngine::BuildMacroInvocationPatchWholeCover(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    const std::vector<int> &a2b, StringRef bSource,
    const std::vector<std::size_t> &bTokOff) {
  // Map the macro's A-cover [coverBegin, coverEnd) to a B-token interval via
  // LCS map.
  int bStartIdx = MapForwardToB(a2b, m.cover.begin);
  int bEndIdxEx = MapBackwardToB(a2b, m.cover.end - 1);

  // Fallback if unmapped OR inverted
  if (bStartIdx < 0 || bEndIdxEx < 0 || bStartIdx > bEndIdxEx) {
    // If B has no tokens in this hunk, the macro vanished → empty replacement
    if (h.bStart >= h.bEnd) {
      return MacroPatch{m.getInvB(), m.getInvE(), ""};
    }
    bStartIdx = h.bStart;
    bEndIdxEx = h.bEnd - 1;
  }

  // Final defensive clamp (should be redundant if bTokOff has sentinel)
  std::size_t b0 = bTokOff[bStartIdx];
  std::size_t b1 = bTokOff[bEndIdxEx + 1];
  if (b1 < b0) {
    // ultra-defensive check: produce empty replacement instead of crashing
    return MacroPatch{m.getInvB(), m.getInvE(), ""};
  }

  StringRef frag(bSource.data() + b0, static_cast<std::size_t>(b1 - b0));
  std::string repl = frag.ltrim(" \t").rtrim(" \t").str();
  return MacroPatch{m.getInvB(), m.getInvE(), std::move(repl)};
}

// =========== Include processing (normalize, materialize, apply) ===========

void RefoldEngine::NormalizeIncludeInsertions(
    std::map<int, IncludeEdits> &perInclude, StringRef bSource,
    const std::vector<std::size_t> &bTokOff) {
  for (auto &kv : perInclude) {
    auto &ie = kv.second;
    if (!ie.include || ie.patches.empty())
      continue;

    std::sort(ie.patches.begin(), ie.patches.end(),
              [](const IncludePatch &x, const IncludePatch &y) {
                if (x.aStart != y.aStart)
                  return x.aStart < y.aStart;
                return x.bStart < y.bStart;
              });

    std::vector<IncludePatch> merged;
    for (std::size_t i = 0; i < ie.patches.size();) {
      IncludePatch p = ie.patches[i];

      // Only coalesce pure insertions (A[as,ae) empty).
      if (p.aStart == p.aEnd) {
        int bStartIdx = p.bStart;
        int bEndIdxEx = p.bEnd;

        // Widen B-start leftwards across a same-line run of type-ish tokens.
        int k = bStartIdx - 1;
        bool sawNl = false;
        int earliestTypeTok = -1;

        // Walk left across whitespace tokens; track if we cross a newline.
        while (k >= 0) {
          std::size_t ts = bTokOff[k], te = bTokOff[k + 1];
          StringRef tok(bSource.data() + ts, te - ts);

          if (stringutils::isAsciiWhitespace(tok.str())) {
            if (tok.contains('\n') || tok.contains('\r')) {
              sawNl = true;
              break;
            }
            --k;
            continue;
          }

          // Non-whitespace token found
          if (sawNl)
            break;
          if (!stringutils::isTypeishToken(tok.str()))
            break;

          // Remember it and try to extend further left.
          earliestTypeTok = k;
          --k;
        }

        if (earliestTypeTok >= 0) {
          // include the entire type-ish prefix run (and trailing WS)
          bStartIdx = earliestTypeTok;
        }

        // We'll build the text incrementally from left + gap + right...
        std::string left(bSource.data() + bTokOff[bStartIdx],
                         bSource.data() + bTokOff[bEndIdxEx]);

        std::size_t j = i + 1;
        while (j < ie.patches.size()) {
          const auto &q = ie.patches[j];
          if (q.aStart != q.aEnd)
            break; // stop at non-insert
          if (q.aStart > ie.patches[i].aStart + 1)
            break; // keep it very local (same site)

          // Combine: include the matched B gap and the next insert bytes.
          std::string gap(bSource.data() + bTokOff[bEndIdxEx],
                          bSource.data() + bTokOff[q.bStart]);
          std::string right(bSource.data() + bTokOff[q.bStart],
                            bSource.data() + bTokOff[q.bEnd]);

          // If identifiers touch across the gap, ensure a single space.
          if (!gap.empty() && !right.empty() &&
              stringutils::isIdentChar(gap.back()) &&
              stringutils::isIdentChar(right.front())) {
            gap.push_back(' ');
          }

          left += gap;
          left += right;
          bEndIdxEx = q.bEnd;
          ++j;
        }

        // Keep the insertion line-local: trim anything after the first newline.
        auto nl = left.find('\n');
        if (nl != std::string::npos)
          left.resize(nl + 1);

        // Anchor at the first patch's A-position (insertion), B span is the
        // merged (possibly widened) range.
        merged.push_back(IncludePatch{ie.include, ie.patches[i].aStart,
                                      ie.patches[i].aStart, bStartIdx,
                                      bEndIdxEx, std::move(left)});
        i = j;
      } else {
        merged.push_back(std::move(p));
        ++i;
      }
    }

    ie.patches.swap(merged);
  }
}

void RefoldEngine::MaterializeIncludeExpansion(
    const RefoldModel &model, int includeId,
    const std::map<int, IncludeEdits> &perInclude,
    const std::map<std::optional<int>, std::vector<MacroPatch>>
        &macroPatchesByOwner,
    const std::map<int, std::vector<const RefoldModel::IncludeItem *>>
        &children,
    std::map<int, std::string> &includeExpansion) {
  // Already materialized?
  if (includeExpansion.count(includeId))
    return;

  const auto *inc = model.GetIncludeById(includeId);
  if (!inc) {
    fatal("include/mat", "unknown include id {0}", includeId);
  }

  debug("include/mat",
        "ENTER inc#{0} target={1} resolved={2} sitePath={3} site=[{4},{5}) "
        "cover=[{6},{7})",
        inc->id, inc->target, inc->resolvedPath ? *inc->resolvedPath : "null",
        inc->sitePath, inc->siteB, inc->siteE, inc->cover.begin,
        inc->cover.end);

  // Start from the raw header text that was preloaded into includeExpansion.
  // If it wasn’t preseeded for some reason, load deterministically by path.
  std::string bytes;
  if (auto it = includeExpansion.find(includeId); it != includeExpansion.end())
    bytes = it->second;
  if (bytes.empty()) {
    const std::string headerPath =
        (inc->resolvedPath && !inc->resolvedPath->empty())
            ? *inc->resolvedPath
            : StripHeaderToken(inc->target);

    auto bufOrErr = MemoryBuffer::getFile(headerPath);
    if (!bufOrErr) {
      fatal("include/mat", "failed to read header: {0} ({1})", headerPath,
            bufOrErr.getError().message());
    }

    // Treat as raw bytes; copy into std::string
    const MemoryBuffer &mb = **bufOrErr;
    bytes.assign(mb.getBufferStart(), mb.getBufferEnd());
  }

  // Collect byte-level edits to apply within this header.
  std::vector<TextEdit> edits;

  // 1) Macro-patch edits owned by this include (invocation byte ranges already
  // in the owner’s file space).
  if (auto it = macroPatchesByOwner.find(includeId);
      it != macroPatchesByOwner.end()) {
    for (const auto &mp : it->second)
      edits.push_back(TextEdit{mp.invStart, mp.invEnd, mp.replacement});
  }

  // 2) A/B include insert/delete/replace patches that belong to this include.
  if (auto it = perInclude.find(includeId); it != perInclude.end()) {
    if (!it->second.patches.empty())
      bytes = ApplyIncludeInsertions(model, it->second, std::move(bytes));
  }

  auto hasDescendantWork = [&](auto &&self, int id) -> bool {
    // Direct work at this node?
    bool selfWork = false;
    if (auto it = perInclude.find(id); it != perInclude.end())
      selfWork = !it->second.patches.empty();
    if (!selfWork) {
      if (auto it = macroPatchesByOwner.find(id);
          it != macroPatchesByOwner.end())
        selfWork = !it->second.empty();
    }

    if (selfWork) {
      debug("include/tree", "hasWork inc#{0}: selfWork=YES", id);
      return true;
    }

    // Otherwise, recurse into children
    if (auto it = children.find(id); it != children.end()) {
      for (const auto *child : it->second) {
        if (self(self, child->id)) {
          debug("include/tree", "hasWork inc#{0}: via child#{1} => YES", id,
                child->id);
          return true;
        }
      }
    }

    debug("include/tree", "hasWork inc#{0}: NO", id);
    return false;
  };

  // 3) Recurse into child includes that have any descendant work, and
  // replace each child directive with the child’s fully materialized text.
  if (auto it = children.find(includeId); it != children.end()) {
    for (const auto *child : it->second) {
      const bool todo = hasDescendantWork(hasDescendantWork, child->id);
      debug("include/mat",
            "inc#{0} -> child#{1} target={2} resolved={3} site=[{4},{5}) "
            "expand={6}",
            inc->id, child->id, child->target,
            child->resolvedPath ? *child->resolvedPath : "null", child->siteB,
            child->siteE, todo ? "YES" : "NO");
      if (!todo) {
        continue; // leave untouched: keep the original directive as-is
      }

      // Ensure the child is materialized first (depth-first).
      MaterializeIncludeExpansion(model, child->id, perInclude,
                                  macroPatchesByOwner, children,
                                  includeExpansion);

      // The child directive's site is recorded in the includer byte space.
      const auto &childText = includeExpansion[child->id];
      int siteStart =
          std::max(0, std::min(static_cast<int>(bytes.size()), child->siteB));
      int siteEnd = std::max(
          siteStart, std::min(static_cast<int>(bytes.size()), child->siteE));

      debug("include/mat",
            "REPLACE in inc#{0}: site=[{1},{2}) len(parent)={3} with child#{4} "
            "len(childText)={5}",
            inc->id, siteStart, siteEnd, bytes.size(), child->id,
            childText.size());

      debug("tu/replace",
            "TU replace site=[{0},{1}) with inc#{2} len={3} (target={4} "
            "resolved={5})",
            siteStart, siteEnd, child->id, childText.size(), child->target,
            child->resolvedPath ? *child->resolvedPath : "null");

      if (siteStart < siteEnd) {
        edits.push_back(TextEdit{siteStart, siteEnd, childText});
      } else {
        // Defensive fallback: if site is somehow unmapped, skip replacing.
        // (This keeps behavior deterministic instead of crashing.)
      }
    }
  }

  // Apply highest-offset-first.
  std::sort(
      edits.begin(), edits.end(),
      [](const TextEdit &a, const TextEdit &b) { return a.start > b.start; });
  for (const auto &e : edits) {
    if (e.start < 0 || e.end < e.start ||
        e.end > static_cast<int>(bytes.size()))
      fatal("inc/mat", "bad include edit [{0},{1})", e.start, e.end);
    bytes.replace(static_cast<std::size_t>(e.start),
                  static_cast<std::size_t>(e.end - e.start), e.text);
  }

  debug("include/mat", "EXIT inc#{0} resultLen={1}", inc->id, bytes.size());

  includeExpansion[includeId] = std::move(bytes);
}

std::string RefoldEngine::ApplyIncludeInsertions(const RefoldModel &model,
                                                 const IncludeEdits &ie,
                                                 std::string headerText) {
  const std::string file =
      (ie.include->resolvedPath && !ie.include->resolvedPath->empty())
          ? *ie.include->resolvedPath
          : StripHeaderToken(ie.include->target);

  const int fileLen = static_cast<int>(headerText.size());
  std::vector<TextEdit> edits;

  for (const auto &p : ie.patches) {
    const bool isInsert = (p.aStart == p.aEnd) && (p.bStart < p.bEnd);
    const bool isDelete = (p.aStart < p.aEnd) && (p.bStart == p.bEnd);
    const bool isReplace = (p.aStart < p.aEnd) && (p.bStart < p.bEnd);

    int startByte = -1, endByte = -1;

    if (isInsert) {
      int insertAt = -1;

      if (!INSERT_AFTER_LEFT_TOKEN_EOL) {
        // Prefer right-neighbor (start of token at aStart) in this file.
        insertAt = ByteStartForPPInFile(model, file, p.aStart,
                                        /*fallbackToEOF*/ false, fileLen);
        trace("include/anchor",
              "inc#{0} right-neighbor start: pp={1} -> byte={2}",
              ie.include->id, p.aStart, insertAt);

        // If that PP index doesn't belong to this header, try the left neighbor
        // end.
        if (insertAt < 0) {
          int leftPP = p.aStart - 1;
          if (leftPP >= ie.include->cover.begin) {
            int leftEnd = ByteEndForPPInFile(model, file, leftPP,
                                             /*fallbackToEOF*/ false, fileLen);
            if (leftEnd >= 0) {
              insertAt = HopPastOptionalNewline(headerText, leftEnd, fileLen);
              trace("include/anchor",
                    "inc#{0} left-neighbor end: leftPP={1} endByte={2} -> "
                    "byte={3}",
                    ie.include->id, leftPP, leftEnd, insertAt);
            }
          }
        }
      } else {
        // If the left token ends at EOL, hop the newline. Otherwise, if the
        // left token begins at line indent, insert at the START of that token
        // (start-of-line for the declaration). Else fall back to right token
        // start.
        int leftPP = std::max(ie.include->cover.begin, p.aStart - 1);
        int leftEnd = ByteEndForPPInFile(model, file, leftPP,
                                         /*fallbackToEOF*/ false, fileLen);
        int leftStart = ByteStartForPPInFile(model, file, leftPP,
                                             /*fallbackToEOF*/ false, fileLen);
        int rightStart = ByteStartForPPInFile(model, file, p.aStart,
                                              /*fallbackToEOF*/ false, fileLen);

        if (leftEnd >= 0) {
          bool atEOL = (leftEnd >= fileLen) ||
                       headerText[static_cast<std::size_t>(leftEnd)] == '\n' ||
                       headerText[static_cast<std::size_t>(leftEnd)] == '\r';
          if (atEOL) {
            insertAt = HopPastOptionalNewline(headerText, leftEnd, fileLen);
            trace(
                "include/anchor",
                "inc#{0} left-neighbor end: leftPP={1} endByte={2} -> byte={3}",
                ie.include->id, leftPP, leftEnd, insertAt);
          } else if (leftStart >= 0 && IsAtLineIndent(headerText, leftStart)) {
            insertAt = leftStart;
            trace("include/anchor",
                  "inc#{0} same-line: using left-neighbor START at byte={1} "
                  "(line indent)",
                  ie.include->id, insertAt);
          } else if (rightStart >= 0) {
            insertAt = rightStart;
            trace("include/anchor",
                  "inc#{0} same-line: using right-neighbor START at byte={1}",
                  ie.include->id, insertAt);
          } else {
            insertAt = leftEnd;
            trace("include/anchor",
                  "inc#{0} same-line fallback: left-neighbor END at byte={1}",
                  ie.include->id, insertAt);
          }
        } else {
          insertAt = rightStart;
          trace("include/anchor",
                "inc#{0} right-neighbor start: pp={1} -> byte={2} (no leftEnd)",
                ie.include->id, p.aStart, insertAt);
        }
      }

      // Last resort: first PP inside this header, else BOF=0
      if (insertAt < 0) {
        int rn = ByteStartForPPInFile(model, file, p.aStart,
                                      /*fallbackToEOF*/ false, fileLen);
        trace("include/anchor",
              "inc#{0} right-neighbor retry: pp={1} -> byte={2}",
              ie.include->id, p.aStart, rn);
        insertAt = rn;

        if (insertAt < 0) {
          int firstPP = -1;
          for (int pp = ie.include->cover.begin; pp < ie.include->cover.end;
               ++pp) {
            auto it = model.GetTokmapByPP().find(pp);
            if (it != model.GetTokmapByPP().end() &&
                PathsEqual(it->second.file, file)) {
              firstPP = pp;
              break;
            }
          }
          if (firstPP >= 0) {
            int firstByte = ByteStartForPPInFile(
                model, file, firstPP, /*fallbackToEOF*/ false, fileLen);
            insertAt = (firstByte >= 0) ? firstByte : 0;
            trace("include/anchor",
                  "inc#{0} cover-firstPP start: pp={1} -> byte={2} (fallback)",
                  ie.include->id, firstPP, insertAt);
          } else {
            insertAt = 0;
            trace("include/anchor", "inc#{0} BOF fallback -> byte=0",
                  ie.include->id);
          }
        }
      }

      // If the anchor is at the function-name start on this line, shift it to
      // the line indent so the type/qualifier/pointer prefix ("unsigned long ",
      // etc.) stays with the next line instead of being consumed by the
      // insertion.
      int shifted = ShiftAnchorToLineIndentIfAtFuncName(headerText, insertAt);
      if (shifted != insertAt) {
        trace("include/anchor",
              "inc#{0} shifted anchor to line indent before type prefix at "
              "byte={1}",
              ie.include->id, shifted);
        insertAt = shifted;
      }
      startByte = endByte = insertAt;
    } else {
      // DELETE/REPLACE: map [aStart,aEnd) to a byte span within *this file*
      // only.
      int s = -1, e = -1;

      for (int pp = p.aStart; pp < p.aEnd; ++pp) {
        auto it = model.GetTokmapByPP().find(pp);
        if (it != model.GetTokmapByPP().end() &&
            PathsEqual(it->second.file, file)) {
          s = pp;
          break;
        }
      }
      for (int pp = p.aEnd - 1; pp >= p.aStart; --pp) {
        auto it = model.GetTokmapByPP().find(pp);
        if (it != model.GetTokmapByPP().end() &&
            PathsEqual(it->second.file, file)) {
          e = pp;
          break;
        }
      }

      if (s == -1 || e == -1)
        continue; // nothing in this file
      startByte = ByteStartForPPInFile(model, file, s, /*fallbackToEOF*/ false,
                                       fileLen);
      endByte =
          ByteEndForPPInFile(model, file, e, /*fallbackToEOF*/ false, fileLen);
    }

    // Replacement bytes for INSERT/REPLACE; or "" for DELETE.
    std::string replacement = isDelete ? std::string() : p.insertBytes;

    debug("include/apply",
          "inc#{0} file={1} kind={2} A=[{3},{4}) B=[{5},{6}) start={7} end={8} "
          "repl='{9}'",
          ie.include->id, file,
          (isInsert ? "INSERT"
                    : (isDelete ? "DELETE" : (isReplace ? "REPLACE" : "???"))),
          p.aStart, p.aEnd, p.bStart, p.bEnd, startByte, endByte,
          stringutils::showWS(stringutils::clip(replacement, 120)));

    // Boundary hygiene (no identifier fusion), but NEVER add a space when
    // replacement already ends with a newline.
    if (isInsert && !replacement.empty()) {
      auto endsWithNl = (replacement.back() == '\n');

      int tail = static_cast<int>(replacement.size()) - 1;
      if (endsWithNl)
        --tail;
      while (tail >= 0) {
        char c = replacement[static_cast<std::size_t>(tail)];
        if (c == ' ' || c == '\t' || c == '\r')
          --tail;
        else
          break;
      }

      char lastIns =
          (tail >= 0) ? replacement[static_cast<std::size_t>(tail)] : '\0';
      char nextHdr = (startByte >= 0 &&
                      static_cast<std::size_t>(startByte) < headerText.size())
                         ? headerText[static_cast<std::size_t>(startByte)]
                         : '\0';

      // Right boundary: only add a space if NOT ending with newline.
      if (!endsWithNl && stringutils::isIdentChar(lastIns) &&
          stringutils::isIdentChar(nextHdr)) {
        replacement.push_back(' ');
        debug("include/anchor",
              "insert boundary: added trailing space between '{0}' and '{1}' "
              "at byte {2}",
              lastIns, nextHdr, startByte);
      }

      // Left boundary: avoid adding a space if immediately after a newline.
      char prevHdr = (startByte > 0)
                         ? headerText[static_cast<std::size_t>(startByte - 1)]
                         : '\0';

      std::size_t head = 0;
      while (head < replacement.size()) {
        char c = replacement[head];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
          ++head;
        else
          break;
      }

      char firstIns = (head < replacement.size()) ? replacement[head] : '\0';

      if (prevHdr != '\n' && prevHdr != '\r' &&
          stringutils::isIdentChar(prevHdr) &&
          stringutils::isIdentChar(firstIns)) {
        replacement.insert(replacement.begin(), ' ');
        debug("include/anchor",
              "insert boundary: added leading space between '{0}' and '{1}' at "
              "byte {2}",
              prevHdr, firstIns, startByte);
      }
    }

    if (isInsert && !replacement.empty()) {
      // Are we at line indent?
      auto [bol, fnStart, eol] = LineAndFuncNameStart(headerText, startByte);
      bool atIndent = IsAtLineIndent(headerText, startByte);

      // Does replacement look like "identifier(" after optional spaces?
      std::size_t ri = 0;
      while (ri < replacement.size()) {
        char c = replacement[ri];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
          ++ri;
        else
          break;
      }
      std::size_t rj = ri;
      while (rj < replacement.size() &&
             stringutils::isIdentChar(replacement[rj]))
        ++rj;
      bool looksFuncDecl =
          (rj > ri) && (rj < replacement.size()) && (replacement[rj] == '(');

      if (atIndent && looksFuncDecl) {
        // Compute full type-ish prefix on the target line.
        int hi = bol;
        while (hi < startByte) {
          char c = headerText[static_cast<std::size_t>(hi)];
          if (c == ' ' || c == '\t')
            ++hi;
          else
            break;
        }
        std::string prefix;
        if (fnStart > hi) {
          prefix.assign(headerText.begin() + hi, headerText.begin() + fnStart);
        }

        if (!prefix.empty()) {
          bool alreadyHas = ri <= replacement.size() &&
                            replacement.compare(ri, prefix.size(), prefix) == 0;
          if (!alreadyHas) {
            replacement.insert(ri, prefix);
            trace("include/apply",
                  "inc#{0} carried full type prefix '{1}' at byte {2}",
                  ie.include->id, stringutils::showWS(prefix), startByte);
          }
        }
      }
    }

    // If a REPLACE would delete ANY leading type/qualifier/pointer prefix on
    // the next line, and our replacement ends with a newline at a line-indent
    // anchor, treat it as a pure INSERT.
    if (isReplace && !replacement.empty() && replacement.back() == '\n' &&
        startByte >= 0 && endByte >= startByte &&
        IsAtLineIndent(headerText, startByte)) {
      auto [bol, fnStart, eol] = LineAndFuncNameStart(headerText, startByte);

      // Find first non-space after bol (true start of the prefix run).
      int p2 = bol;
      while (p2 < eol) {
        char c = headerText[static_cast<std::size_t>(p2)];
        if (c == ' ' || c == '\t')
          ++p2;
        else
          break;
      }
      // The deletable prefix region is [p2, fnStart).
      int prefLo = p2, prefHi = std::max(p2, std::min(fnStart, eol));

      // Consider deleting only if we're strictly within the prefix region and
      // not crossing newline.
      if (startByte >= prefLo && endByte <= prefHi) {
        endByte = startByte; // REPLACE → zero-width INSERT
        trace("include/apply",
              "inc#{0} REPLACE→INSERT at byte {1} to preserve prefix [{2},{3})",
              ie.include->id, startByte, prefLo, prefHi);
      }
    }

    edits.push_back(TextEdit{startByte, endByte, std::move(replacement)});
  }

  // Apply edits inside this header, highest offset first.
  std::sort(
      edits.begin(), edits.end(),
      [](const TextEdit &a, const TextEdit &b) { return a.start > b.start; });
  for (const auto &e : edits) {
    const int len = static_cast<int>(headerText.size());
    const bool bad = (e.start < 0) || (e.end < e.start) || (e.end > len);
    if (bad) {
      fatal("include/apply",
            "invalid edit range: start={0} end={1} (len={2}) inc#{3} "
            "file=\"{4}\"",
            e.start, e.end, len, ie.include->id,
            (ie.include->resolvedPath && !ie.include->resolvedPath->empty())
                ? *ie.include->resolvedPath
                : StripHeaderToken(ie.include->target));
    }
    headerText.replace(static_cast<std::size_t>(e.start),
                       static_cast<std::size_t>(e.end - e.start), e.text);
  }
  return headerText;
}

// =================== Low-level file & mapping utilities ===================

int RefoldEngine::HopPastOptionalNewline(StringRef text, int pos, int len) {
  if (pos < len) {
    char c = text[static_cast<std::size_t>(pos)];
    if (c == '\r') {
      return (pos + 1 < len && text[static_cast<std::size_t>(pos + 1)] == '\n')
                 ? pos + 2
                 : pos + 1;
    } else if (c == '\n') {
      return pos + 1;
    }
  }
  return pos;
}

bool RefoldEngine::PathsEqual(StringRef a, StringRef b) {
  if (a.empty() || b.empty())
    return a == b;

  std::error_code ecA, ecB;
  const auto ca =
      std::filesystem::weakly_canonical(std::filesystem::path(a.str()), ecA);
  const auto cb =
      std::filesystem::weakly_canonical(std::filesystem::path(b.str()), ecB);

  if (ecA) {
    fatal("path/canon", "failed to canonicalize '{0}': {1}", a, ecA.message());
  }
  if (ecB) {
    fatal("path/canon", "failed to canonicalize '{0}': {1}", b, ecB.message());
  }

  return ca == cb;
}

} // namespace refold
} // namespace clang
