//===--- RefoldMacroTopology.cpp -------------------------------*- C++ -*-===//
//
// Macro-topology service implementation for clang-refold.
//
// This file builds and queries derived indices over the producer-recorded macro
// graph, conditional containment, #define containment, and __COUNTER__ event
// identity.  Proof and planning services use this narrow topology oracle instead
// of duplicating graph traversal logic.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroTopology.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "util/StringUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroTopology::RefoldMacroTopology(
    const RefoldModel &model, ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
    const RefoldSourceMapper &sourceMapper, const RefoldPathIdentity &paths)
    : model_(model), aToks_(aToks), paths_(paths) {
  // The constructor receives the complete dependency set for the topology
  // service.  The current indices use the A-token stream and path service; keep
  // the remaining parameters explicit for object-graph consistency while
  // avoiding unused-parameter warnings.
  (void)bToks;
  (void)sourceMapper;
  BuildMacroInvocationGraph();
}

void RefoldMacroTopology::BuildMacroInvocationGraph() {
  macroInvocationById_.clear();
  macroChildrenById_.clear();

  ArrayRef<RefoldModel::MacroInvocation> macroInvocations =
      model_.GetMacroInvocations();
  macroInvocationById_.reserve(macroInvocations.size());

  for (const auto &mi : macroInvocations) {
    // Preserve the old RefoldEngine linear scan's first-match behavior for
    // malformed producer maps with duplicate invocation IDs while making valid
    // model lookups constant-time.
    macroInvocationById_.insert(std::make_pair(mi.id, &mi));

    if (mi.callerMacroId)
      macroChildrenById_[*mi.callerMacroId].push_back(&mi);
  }
}

const RefoldModel::MacroInvocation *
RefoldMacroTopology::FindMacroInvocationById(uint64_t macroId) const {
  auto it = macroInvocationById_.find(macroId);
  if (it == macroInvocationById_.end())
    return nullptr;
  return it->second;
}

ArrayRef<const RefoldModel::MacroInvocation *>
RefoldMacroTopology::MacroChildrenOf(uint64_t parentId) const {
  auto it = macroChildrenById_.find(parentId);
  if (it == macroChildrenById_.end())
    return {};
  return it->second;
}

uint64_t RefoldMacroTopology::GetRootMacroId(uint64_t macroId) const {
  const RefoldModel::MacroInvocation *cur = FindMacroInvocationById(macroId);
  if (!cur)
    return macroId;

  // Walk callerMacroId links to the outermost invocation in this expansion
  // chain.  If a malformed model has a missing parent link, preserve the
  // historical fail-soft behavior and return the highest resolved invocation.
  while (cur->callerMacroId) {
    const RefoldModel::MacroInvocation *parent =
        FindMacroInvocationById(*cur->callerMacroId);
    if (!parent)
      break;
    cur = parent;
  }
  return cur->id;
}

std::string RefoldMacroTopology::ToAbsolutePath(StringRef spelledPath) const {
  SmallString<256> path(spelledPath);

  if (!sys::path::is_absolute(path)) {
    if (model_.GetPPCwd().empty()) {
      sys::fs::make_absolute(path);
    } else {
      // Use the producer working directory captured in the refold map rather
      // than the replay process CWD, matching LineDirectiveInserter::ToAbsolutePath.
      SmallString<256> base(model_.GetPPCwd());
      sys::path::append(base, path);
      path = base;
    }
  }

  sys::path::remove_dots(path, /*remove_dot_dot=*/true);
  return std::string(path.str());
}

void RefoldMacroTopology::BuildDefineDirectiveIndex() const {
  if (definesIndexBuilt_)
    return;

  // Mark the index built before populating it so this block remains a one-shot
  // cache initializer for the current model instance.
  definesIndexBuilt_ = true;
  defineFileTextCache_.clear();
  defineEndCache_.clear();
  definesByAbsPath_.clear();

  for (const auto &d : model_.GetMacroDirectives()) {
    // Only object/function macro definitions need widened directive extents.
    if ("#define" != d.subkind)
      continue;

    // Without a spelling path there is no source file to index.  Leave the
    // directive out rather than inventing an extent with no lookup key.
    if (d.sitePath.empty())
      continue;

    uint64_t defineEnd = d.siteE;

    auto itEnd = defineEndCache_.find(d.id);
    if (itEnd != defineEndCache_.end()) {
      defineEnd = itEnd->second;
    } else {
      const std::string absPath = ToAbsolutePath(d.sitePath);

      auto itTxt = defineFileTextCache_.find(absPath);
      if (itTxt == defineFileTextCache_.end()) {
        auto bufOrErr = MemoryBuffer::getFile(absPath);
        if (!bufOrErr) {
          // Best effort: keep the producer-provided one-line extent if the
          // replay-time source file cannot be loaded.
          defineEndCache_[d.id] = d.siteE;
          defineEnd = d.siteE;
        } else {
          defineFileTextCache_[absPath] = (**bufOrErr).getBuffer().str();
          itTxt = defineFileTextCache_.find(absPath);
        }
      }

      if (itTxt != defineFileTextCache_.end()) {
        StringRef bytes(itTxt->second);

        // Begin scanning at the directive start, clamped defensively in case the
        // producer offset is outside the replay-time source buffer.
        uint64_t i = d.siteB;
        if (i > bytes.size())
          i = bytes.size();

        // Walk physical lines until reaching a newline that is not escaped by a
        // C line splice.  Every escaped newline keeps the macro definition's
        // logical replacement list alive on the next physical line.
        while (i < bytes.size()) {
          size_t nl = bytes.find('\n', static_cast<size_t>(i));
          if (nl == StringRef::npos) {
            i = bytes.size();
            break;
          }

          i = static_cast<uint64_t>(nl + 1);
          if (!stringutils::isLineSplice(bytes, nl))
            break;
        }

        defineEnd = i;
        defineEndCache_[d.id] = defineEnd;
      }
    }

    // Store the widened extent under the replay-time absolute path used for
    // lookup.  The interval is half-open: [siteB, defineEnd).
    const std::string absPath = ToAbsolutePath(d.sitePath);
    definesByAbsPath_[absPath].push_back(
        DefineDirectiveExtent{d.siteB, defineEnd});
  }

  // Keep each per-file extent list ordered so containment queries remain
  // deterministic.  Ties by begin are ordered by end.
  for (auto &kv : definesByAbsPath_) {
    auto &vec = kv.getValue();
    llvm::sort(vec, [](const DefineDirectiveExtent &x,
                       const DefineDirectiveExtent &y) {
      if (x.begin != y.begin)
        return x.begin < y.begin;
      return x.end < y.end;
    });
  }
}

bool RefoldMacroTopology::IsInvocationInsideDefineDirective(
    const RefoldModel::MacroInvocation &m) const {
  if (!m.invFile || !m.invB || !m.invE)
    return false;

  BuildDefineDirectiveIndex();

  const std::string invAbs = ToAbsolutePath(*m.invFile);
  auto it = definesByAbsPath_.find(invAbs);
  if (it == definesByAbsPath_.end())
    return false;

  const uint64_t x = *m.invB;
  const auto &vec = it->second;
  if (vec.empty())
    return false;

  // Binary-search for the last define extent whose begin offset is <= x.  This
  // is the normal candidate in the non-overlapping case.
  size_t lo = 0;
  size_t hi = vec.size();
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (vec[mid].begin <= x)
      lo = mid + 1;
    else
      hi = mid;
  }

  auto contains = [&](const DefineDirectiveExtent &extent) {
    return x >= extent.begin && x < extent.end;
  };

  if (lo != 0 && contains(vec[lo - 1]))
    return true;

  // Preserve the previous adjacent-window scan for unusual overlapping #define
  // extents in producer metadata.
  for (size_t i = lo; i < vec.size() && i < lo + 4; ++i) {
    if (contains(vec[i]))
      return true;
  }
  for (size_t i = lo; i > 0 && i + 4 > lo; --i) {
    if (contains(vec[i - 1]))
      return true;
  }

  return false;
}

bool RefoldMacroTopology::SourceRangeInsideConditionalArm(
    uint64_t condArmId, StringRef file, std::optional<uint64_t> ownerIncludeId,
    uint64_t begin, uint64_t end) const {
  const std::optional<RefoldModel::ArmRef> armRef =
      model_.GetArmRefById(condArmId);
  if (!armRef || !armRef->group || !armRef->arm)
    return false;

  // Preserve the historical RefoldPathIdentity::PathsEqual comparison used by this
  // proof predicate.  Conditional arm ownership is still source-byte evidence,
  // but file identity must use the same canonical path policy as the previous
  // engine helper.
  if (!paths_.PathsEqual(armRef->group->file, file))
    return false;
  if (armRef->group->parentIncludeId != ownerIncludeId)
    return false;

  // The whole site must be inside the producer-recorded active arm body before
  // it can inherit that conditional-owner state.
  return armRef->arm->bodyB <= begin && begin <= end && end <= armRef->arm->bodyE;
}


namespace {

/// Describes how strongly a PP range is covered by a macro invocation, from
/// most specific (`Body`) to no meaningful cover (`None`).
enum class MacroCoverRank : uint8_t {
  Body = 0,
  ArgLike = 1,
  Cover = 2,
  None = 3,
};

} // namespace

const RefoldModel::MacroInvocation *
RefoldMacroTopology::SmallestCoveringPatchableMacro(
    uint64_t aStart, uint64_t aEnd,
    std::optional<uint64_t> ownerIncludeId) const {
  const RefoldModel::MacroInvocation *best = nullptr;
  MacroCoverRank bestRank = MacroCoverRank::None;
  uint64_t bestLen = std::numeric_limits<uint64_t>::max();

  const bool isInsert = (aStart == aEnd);

  // Return true iff the candidate half-open span [b,e) covers the current A
  // target. For insertions, require the insertion point to lie strictly inside
  // the span (not exactly on its boundary). For non-insertions, require full
  // coverage of [aStart,aEnd).
  auto spanCovers = [&](uint64_t b, uint64_t e) -> bool {
    if (e <= b)
      return false;
    if (isInsert)
      return (b < aStart) && (aStart < e);
    return (b <= aStart) && (aEnd <= e);
  };

  // Among an arbitrary span collection, return the smallest covering span
  // length, or std::nullopt if none of the spans cover the target.
  auto minCoverLenIn = [&](auto &&spans) -> std::optional<uint64_t> {
    std::optional<uint64_t> out;
    for (const auto &sp : spans) {
      if (spanCovers(sp.begin, sp.end)) {
        const uint64_t len = sp.end - sp.begin;
        if (!out || len < *out)
          out = len;
      }
    }
    return out;
  };

  // Return the smallest covering span length among this invocation's argument-
  // derived projections: ordinary argument spans, stringify spans, and paste
  // spans. Used to prefer the tightest argument-local cover inside the macro.
  auto minCoverLenInArgs = [&](const RefoldModel::MacroInvocation &m)
      -> std::optional<uint64_t> {
    std::optional<uint64_t> out;
    for (const auto &sp : m.argSpans) {
      if (spanCovers(sp.begin, sp.end)) {
        const uint64_t len = sp.end - sp.begin;
        if (!out || len < *out)
          out = len;
      }
    }
    for (const auto &sp : m.stringifySpans) {
      if (spanCovers(sp.begin, sp.end)) {
        const uint64_t len = sp.end - sp.begin;
        if (!out || len < *out)
          out = len;
      }
    }
    for (const auto &sp : m.pasteSpans) {
      if (spanCovers(sp.begin, sp.end)) {
        const uint64_t len = sp.end - sp.begin;
        if (!out || len < *out)
          out = len;
      }
    }
    return out;
  };

  // Scan all macro invocations and choose the smallest patchable macro that
  // truthfully covers the requested A-range in the current owner context.
  // Only real callsites are eligible (never invocations spelled inside a
  // #define), and candidates are ranked by how directly they cover the target:
  // body-span cover first, then argument-derived cover, then broad cover as a
  // fallback. Ties are broken by smaller covering span, then lower macro id,
  // for deterministic selection.
  for (const auto &m : model_.GetMacroInvocations()) {
    // Owner filter (when known): avoids selecting a macro record that belongs
    // to a different include instance.
    if (ownerIncludeId) {
      if (!m.ownerIncludeId || *m.ownerIncludeId != *ownerIncludeId)
        continue;
    }

    if (!m.cover.IsValid() || m.cover.end <= m.cover.begin)
      continue;

    // Must be patchable at a real call site.
    if (!m.invB || !m.invE || !m.invText)
      continue;

    // CRITICAL: never patch invocations that are spelled inside a #define.
    if (IsInvocationInsideDefineDirective(m))
      continue;

    // Rank candidates by how directly their spans cover the requested range.
    //   Body:    body span covers the range (direct expansion token)
    //   ArgLike: argument-like span covers the range (arg/stringify/paste)
    //   Cover:   only the broad cover covers the range (fallback)
    MacroCoverRank rank = MacroCoverRank::Cover;
    uint64_t len = m.cover.end - m.cover.begin;

    if (auto bodyLen = minCoverLenIn(m.bodySpans)) {
      rank = MacroCoverRank::Body;
      len = *bodyLen;
    } else if (auto argLen = minCoverLenInArgs(m)) {
      rank = MacroCoverRank::ArgLike;
      len = *argLen;
    } else {
      if (!m.Covers(aStart, aEnd))
        continue;
      rank = MacroCoverRank::Cover;
      len = m.cover.end - m.cover.begin;
    }

    if (!best ||
        static_cast<unsigned>(rank) < static_cast<unsigned>(bestRank) ||
        (rank == bestRank &&
         (len < bestLen || (len == bestLen && m.id < best->id)))) {
      best = &m;
      bestRank = rank;
      bestLen = len;
    }
  }

  return best;
}

std::vector<RefoldMacroTopology::CounterOutputRange>
RefoldMacroTopology::CounterOutputRangesForInvocation(
    const RefoldModel::MacroInvocation &macro) const {
  std::vector<CounterOutputRange> ranges;

  auto addRange = [&](uint64_t begin, uint64_t end) {
    if (end >= begin)
      ranges.emplace_back(begin, end);
  };

  if (!macro.bodySpans.empty()) {
    for (const RefoldModel::PPSpan &span : macro.bodySpans)
      if (span.IsValid())
        addRange(span.begin, span.end);
    return ranges;
  }

  if (!macro.spans.empty()) {
    for (const RefoldModel::PPSpan &span : macro.spans)
      if (span.IsValid())
        addRange(span.begin, span.end);
    return ranges;
  }

  if (macro.cover.IsValid())
    addRange(macro.cover.begin, macro.cover.end);
  return ranges;
}

std::string RefoldMacroTopology::CounterValueForTokenRange(uint64_t begin,
                                                           uint64_t end) const {
  std::string value;
  raw_string_ostream os(value);
  if (end <= begin || begin >= aToks_.size())
    return value;
  const uint64_t clampedEnd = std::min<uint64_t>(end, aToks_.size());
  for (uint64_t i = begin; i < clampedEnd; ++i) {
    if (i != begin)
      os << ' ';
    os << aToks_[static_cast<size_t>(i)].spelling;
  }
  return os.str();
}

CounterEventIdentity RefoldMacroTopology::BuildCounterEventIdentity(
    const RefoldModel::MacroInvocation &macro, uint64_t occurrenceOrdinal,
    uint64_t begin, uint64_t end,
    std::optional<uint64_t> ownerIncludeOverride) const {
  if (end == begin && begin < aToks_.size())
    end = begin + 1;

  CounterEventIdentity event;
  event.macroInvocationId = macro.id;
  event.occurrenceOrdinal = occurrenceOrdinal;
  event.ownerIncludeId =
      ownerIncludeOverride ? ownerIncludeOverride : macro.ownerIncludeId;
  event.callerMacroId = macro.callerMacroId;
  event.aTokenBegin = begin;
  event.aTokenEnd = end;
  if (macro.invFile)
    event.expansionSiteFile = macro.invFile->str();
  event.expansionSiteBegin = macro.invB;
  event.expansionSiteEnd = macro.invE;
  event.aValue = CounterValueForTokenRange(begin, end);
  event.canStabilizeByLiteralization = begin < end && end <= aToks_.size();
  event.canStabilizeByMaterialization = macro.invFile && macro.invB && macro.invE;
  return event;
}

bool RefoldMacroTopology::MacroPatchRemainsExpanded(
    const MacroPatch &patch) const {
  const RefoldModel::MacroInvocation *macro =
      FindMacroInvocationById(patch.macroId);
  if (!macro)
    return true;
  return !RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
      patch.replacement, *macro);
}

} // namespace refold
} // namespace clang
