//===--- RefoldEngine.CounterStabilization.inc --------------*- C++ -*-===//
//
// `RefoldEngine` members physically split out of `RefoldEngine.cpp`.
//
// This fragment groups methods that preserve `__COUNTER__` semantics when
// edits would otherwise force later counter observations to shift. These
// helpers determine which surrounding macro invocations must be stabilized.
//
// This file is included directly by `RefoldEngine.cpp`, so this is only a
// physical file split. No ownership, access, or behavioral semantics change.
//
//===----------------------------------------------------------------------===//



std::vector<std::pair<uint64_t, uint64_t>>
RefoldEngine::CounterOutputRangesForInvocation(
    const RefoldModel::MacroInvocation &macro) const {
  std::vector<std::pair<uint64_t, uint64_t>> ranges;

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

std::string RefoldEngine::CounterValueForTokenRange(uint64_t begin,
                                                    uint64_t end) const {
  std::string value;
  llvm::raw_string_ostream os(value);
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

RefoldEngine::CounterEventIdentity RefoldEngine::BuildCounterEventIdentity(
    const RefoldModel::MacroInvocation &macro, uint64_t occurrenceOrdinal,
    uint64_t begin, uint64_t end,
    std::optional<uint64_t> ownerIncludeOverride) const {
  if (end == begin && begin < aToks_.size())
    end = begin + 1;

  CounterEventIdentity event;
  event.macroInvocationId = macro.id;
  event.occurrenceOrdinal = occurrenceOrdinal;
  event.ownerIncludeId = ownerIncludeOverride ? ownerIncludeOverride
                                              : macro.ownerIncludeId;
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

SmallVector<RefoldEngine::CounterOccurrence, 32>
RefoldEngine::CollectCounterOccurrences(StringRef tuPath) const {
  SmallVector<CounterOccurrence, 32> occs;

  uint64_t occurrenceOrdinal = 0;

  // Record a __COUNTER__ occurrence in PP-token space. Zero-length producer
  // anchors are widened to one token so pasted/stringified counter output still
  // participates in stabilization.  Phase 3G also attaches a stable
  // CounterEventIdentity so the stabilization proof names a concrete counter
  // event rather than a global textual guess.
  auto addOcc = [&](const RefoldModel::MacroInvocation &mi, uint64_t b,
                    uint64_t e) {
    if (e == b && b < aToks_.size())
      e = b + 1;
    if (e <= b)
      return;

    CounterEventIdentity event =
        BuildCounterEventIdentity(mi, occurrenceOrdinal++, b, e);
    occs.push_back(CounterOccurrence{&mi, b, e, std::nullopt, event});
  };

  // Collect all recorded __COUNTER__ outputs. Prefer the most precise producer
  // ranges first: bodySpans, then explicit spans, then the fallback cover.
  for (const auto &mi : model_.GetMacroInvocations()) {
    if (mi.name != "__COUNTER__")
      continue;
    if (!mi.invB || !mi.invE || !mi.invText)
      continue;

    if (!mi.bodySpans.empty()) {
      for (const auto &bs : mi.bodySpans)
        addOcc(mi, bs.begin, bs.end);
      continue;
    }

    if (!mi.spans.empty()) {
      for (const auto &range : mi.spans)
        addOcc(mi, range.begin, range.end);
      continue;
    }

    // Finally fall back to the producer cover.  Phase 8f classifies the
    // former zero-length fallback-cover path as ordinary counter-state
    // stabilization: a zero-width cover is not a byte-rescue fallback, it is a
    // producer anchor for the concrete counter literal token.  Normalize it
    // through addOcc() so the same one-token widening used for body/spans also
    // applies to fallback covers.
    if (mi.cover.IsValid())
      addOcc(mi, mi.cover.begin, mi.cover.end);
  }

  if (occs.empty())
    return {};

  // Determine the actual PP-owner include for each occurrence. This filters out
  // producer-merged duplicate records whose recorded ownerIncludeId does not
  // match the owner inferred from the current TU segmentation.
  SmallVector<CounterOccurrence, 32> filtered;
  filtered.reserve(occs.size());
  for (const CounterOccurrence &occ : occs) {
    diffutils::Hunk dummy;
    dummy.aStart = occ.aStart;
    dummy.aEnd = occ.aEnd;
    dummy.bStart = 0;
    dummy.bEnd = 0;

    Owner owner = ClassifyOwnerWithSegments(tuPath, dummy);
    std::optional<uint64_t> ownerInc;
    if (owner.kind == OwnerKind::Include)
      ownerInc = owner.includeId;

    // If the producer says this macro belongs to a specific include instance,
    // require the token occurrence to be owned by that same include instance.
    if (occ.macro->ownerIncludeId && ownerInc &&
        *occ.macro->ownerIncludeId != *ownerInc)
      continue;

    CounterOccurrence normalized = occ;
    normalized.ownerIncludeId = ownerInc;
    normalized.event.ownerIncludeId = ownerInc;
    filtered.push_back(std::move(normalized));
  }

  SortCounterOccurrences(filtered);
  return filtered;
}

void RefoldEngine::SortCounterOccurrences(
    SmallVectorImpl<RefoldEngine::CounterOccurrence> &occs) const {
  llvm::sort(occs, [](const CounterOccurrence &lhs,
                      const CounterOccurrence &rhs) {
    if (lhs.aStart != rhs.aStart)
      return lhs.aStart < rhs.aStart;
    if (lhs.aEnd != rhs.aEnd)
      return lhs.aEnd < rhs.aEnd;
    if (lhs.ownerIncludeId != rhs.ownerIncludeId)
      return lhs.ownerIncludeId < rhs.ownerIncludeId;
    return lhs.macro->id < rhs.macro->id;
  });
}


SmallVector<RefoldEngine::ForcedMacroPatchRequest, 32>
RefoldEngine::ComputeForcedCounterPatches(StringRef tuPath,
                                          ArrayRef<int64_t> a2b) const {
  SmallVector<CounterOccurrence, 32> filtered =
      CollectCounterOccurrences(tuPath);
  if (filtered.empty())
    return {};

  // A counter occurrence is considered edited if any A-side token in its
  // occurrence range is unmapped, maps outside B, or maps to a token with a
  // different spelling.
  auto isEditedOcc = [&](const CounterOccurrence &o) -> bool {
    for (uint64_t ai = o.aStart; ai < o.aEnd; ++ai) {
      if (static_cast<size_t>(ai) >= a2b.size() ||
          static_cast<size_t>(ai) >= aToks_.size())
        return true;
      const int64_t bj = a2b[static_cast<size_t>(ai)];
      if (bj < 0 || static_cast<size_t>(bj) >= bToks_.size())
        return true;
      if (aToks_[static_cast<size_t>(ai)].spelling !=
          bToks_[static_cast<size_t>(bj)].spelling)
        return true;
    }
    return false;
  };

  auto isEditedInvocationOutput =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    auto spanEdited = [&](uint64_t lo, uint64_t hi) -> bool {
      for (uint64_t ai = lo; ai < hi; ++ai) {
        if (static_cast<size_t>(ai) >= a2b.size() ||
            static_cast<size_t>(ai) >= aToks_.size())
          return true;
        const int64_t bj = a2b[static_cast<size_t>(ai)];
        if (bj < 0 || static_cast<size_t>(bj) >= bToks_.size())
          return true;
        if (aToks_[static_cast<size_t>(ai)].spelling !=
            bToks_[static_cast<size_t>(bj)].spelling)
          return true;
      }
      return false;
    };

    // Compare emitted output segments directly when the producer recorded them.
    // Do not collapse discontiguous bodySpans into one merged envelope: the
    // holes between body spans are caller-formal substitution sites, and edits
    // to those sites must not be misclassified as edits to an embedded
    // __COUNTER__ occurrence.
    if (!m.bodySpans.empty()) {
      for (const auto &s : m.bodySpans) {
        if (s.begin < s.end && spanEdited(s.begin, s.end))
          return true;
      }
      return false;
    }

    // Explicit spans may also include zero-length paste/stringify anchors. For
    // those anchors, compare the anchored token as the effective output slice.
    if (!m.spans.empty()) {
      for (const auto &s : m.spans) {
        if (s.begin < s.end) {
          if (spanEdited(s.begin, s.end))
            return true;
          continue;
        }
        if (s.begin == s.end && s.begin < aToks_.size()) {
          if (spanEdited(s.begin, s.begin + 1))
            return true;
        }
      }
      return false;
    }

    if (m.cover.IsValid()) {
      if (m.cover.end > m.cover.begin)
        return spanEdited(m.cover.begin, m.cover.end);
      if (m.cover.end == m.cover.begin && m.cover.begin < aToks_.size())
        return spanEdited(m.cover.begin, m.cover.begin + 1);
    }

    return false;
  };

  int firstEditedIdx = -1;
  for (size_t i = 0; i < filtered.size(); ++i) {
    if (isEditedOcc(filtered[i])) {
      firstEditedIdx = static_cast<int>(i);
      break;
    }
  }

  // Fallback: some nested-macro scenarios record __COUNTER__ with a zero-length
  // PP span anchored at a caller site that is not the edited output token
  // itself. In those cases, the direct __COUNTER__ occurrence comparison above
  // can miss the edit. Recover by lifting each __COUNTER__ occurrence to a
  // patchable caller macro whose bodySpans/cover correspond to the actual
  // expanded token slice, then redo edit detection.
  if (firstEditedIdx < 0) {
    DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
    invById.reserve(model_.GetMacroInvocations().size());
    for (const auto &mi : model_.GetMacroInvocations())
      invById[mi.id] = &mi;

    // Walk out of macro-definition-only frames until reaching the invocation
    // that can actually be patched in source.
    auto findPatchableCaller = [&](const RefoldModel::MacroInvocation &mi)
        -> const RefoldModel::MacroInvocation * {
      const RefoldModel::MacroInvocation *cur = &mi;
      while (cur) {
        if (!IsInvocationInsideDefineDirective(*cur))
          return cur;
        if (!cur->callerMacroId)
          break;
        auto it = invById.find(*cur->callerMacroId);
        if (it == invById.end())
          break;
        cur = it->second;
      }
      return nullptr;
    };

    auto computeOccRange = [&](const RefoldModel::MacroInvocation &m)
        -> std::optional<std::pair<uint64_t, uint64_t>> {
      uint64_t lo = std::numeric_limits<uint64_t>::max();
      uint64_t hi = 0;
      for (const auto &range : CounterOutputRangesForInvocation(m)) {
        uint64_t begin = range.first;
        uint64_t end = range.second;
        if (end == begin && begin < aToks_.size())
          end = begin + 1;
        if (end <= begin)
          continue;
        lo = std::min(lo, begin);
        hi = std::max(hi, end);
      }
      if (lo == std::numeric_limits<uint64_t>::max() || lo >= hi)
        return std::nullopt;
      return std::make_pair(lo, hi);
    };

    SmallVector<CounterOccurrence, 32> lifted;
    lifted.reserve(filtered.size());
    for (const auto &o : filtered) {
      const RefoldModel::MacroInvocation *root = findPatchableCaller(*o.macro);
      if (!root)
        continue;

      auto r = computeOccRange(*root);
      if (!r)
        continue;

      CounterEventIdentity event = BuildCounterEventIdentity(
          *root, o.event.occurrenceOrdinal, r->first, r->second,
          root->ownerIncludeId);
      lifted.push_back(CounterOccurrence{root, r->first, r->second,
                                         root->ownerIncludeId, event});
    }

    if (!lifted.empty()) {
      SortCounterOccurrences(lifted);

      int idx = -1;
      for (size_t i = 0; i < lifted.size(); ++i) {
        if (isEditedInvocationOutput(*lifted[i].macro)) {
          idx = static_cast<int>(i);
          break;
        }
      }

      if (idx >= 0) {
        trace("counter",
              "__COUNTER__: lifted edit detection found firstEditedIdx={0} at "
              "A=[{1},{2}) root='{3}'",
              idx, lifted[static_cast<size_t>(idx)].aStart,
              lifted[static_cast<size_t>(idx)].aEnd,
              lifted[static_cast<size_t>(idx)].macro->name);
        filtered.swap(lifted);
        firstEditedIdx = idx;
      }
    }
  }

  if (firstEditedIdx < 0) {
    trace("counter", "__COUNTER__: no edited occurrences; no forced expansion");
    return {};
  }

  // Unique key for forced patch requests. Counter stabilization may encounter
  // multiple counter occurrences covered by the same patchable macro; emit only
  // one forced request per owner/include + invocation byte range.
  SmallVector<ForcedMacroPatchRequest, 32> forced;
  llvm::DenseSet<llvm::hash_code> seen;

  // Once one __COUNTER__ occurrence is edited, every subsequent occurrence in
  // PP order must be forced through macro realization as well. Otherwise the
  // counter sequence after the edit could remain stale relative to the edited
  // preprocessed stream.
  for (const CounterOccurrence &o : llvm::drop_begin(filtered, firstEditedIdx)) {
    const auto *root =
        SmallestCoveringPatchableMacro(o.aStart, o.aEnd, o.ownerIncludeId);

    // Safety: never patch macro definitions.
    if (!root || IsInvocationInsideDefineDirective(*root))
      continue;

    const auto invStart = root->invB;
    const auto invEnd = root->invE;
    if (!invStart || !invEnd)
      continue;

    auto key = llvm::hash_combine(root->ownerIncludeId.value_or(0), *invStart,
                                  *invEnd);
    if (!seen.insert(key).second)
      continue;

    forced.push_back({root, o.aStart, o.aEnd, o.event});

    trace("counter",
          "__COUNTER__: force root id={0} name='{1}' ownerInc={2} "
          "inv=[{3},{4}) for occ A=[{5},{6})",
          root->id, root->name, root->ownerIncludeId, *invStart, *invEnd,
          o.aStart, o.aEnd);
  }

  debug("counter",
        "__COUNTER__: occurrences={0} firstEditedIdx={1} forced={2} "
        "firstOccA=[{3},{4})",
        filtered.size(), firstEditedIdx, forced.size(),
        filtered[static_cast<size_t>(firstEditedIdx)].aStart,
        filtered[static_cast<size_t>(firstEditedIdx)].aEnd);

  return forced;
}

SmallVector<RefoldEngine::ForcedMacroPatchRequest, 32>
RefoldEngine::ComputeForcedCounterPatchesFromExpandedMacros(
    StringRef tuPath,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &macroPatchByOwnerByMacroId) const {
  SmallVector<CounterOccurrence, 32> filtered =
      CollectCounterOccurrences(tuPath);
  if (filtered.empty())
    return {};

  // Return true when the patch already selected for `root` is an expanded
  // realization. Once one counter-bearing macro remains expanded, all following
  // counter-bearing roots must also be forced to preserve counter sequencing.
  auto hasExpandedPatchFor = [&](const RefoldModel::MacroInvocation &root)
      -> bool {
    const auto oit = macroPatchByOwnerByMacroId.find(root.ownerIncludeId);
    if (oit == macroPatchByOwnerByMacroId.end())
      return false;

    const auto invStart = root.invB;
    const auto invEnd = root.invE;
    if (!invStart || !invEnd)
      return false;

    for (const auto &kv : oit->second) {
      const MacroPatch &p = kv.second;
      if (p.invStart != *invStart || p.invEnd != *invEnd)
        continue;
      if (MacroPatchRemainsExpanded(p))
        return true;
    }
    return false;
  };

  // Find the first __COUNTER__ occurrence whose smallest patchable owner is
  // already going to remain expanded. That point starts the forced-stabilization
  // suffix.
  int firstExpandedIdx = -1;
  for (size_t i = 0; i < filtered.size(); ++i) {
    const auto *root =
        SmallestCoveringPatchableMacro(filtered[i].aStart, filtered[i].aEnd,
                                       filtered[i].ownerIncludeId);
    if (!root || IsInvocationInsideDefineDirective(*root))
      continue;
    if (hasExpandedPatchFor(*root)) {
      firstExpandedIdx = static_cast<int>(i);
      break;
    }
  }

  if (firstExpandedIdx < 0)
    return {};

  SmallVector<ForcedMacroPatchRequest, 32> forced;
  llvm::DenseSet<llvm::hash_code> seen;

  // Force every counter occurrence from the first expanded one onward. Dedup by
  // owner/include plus invocation byte span because multiple occurrences may be
  // covered by the same patchable macro invocation.
  for (const CounterOccurrence &o : llvm::drop_begin(filtered, firstExpandedIdx)) {
    const auto *root =
        SmallestCoveringPatchableMacro(o.aStart, o.aEnd, o.ownerIncludeId);
    if (!root || IsInvocationInsideDefineDirective(*root))
      continue;
    const auto invStart = root->invB;
    const auto invEnd = root->invE;
    if (!invStart || !invEnd)
      continue;

    auto key = llvm::hash_combine(root->ownerIncludeId.value_or(0), *invStart,
                                  *invEnd);
    if (!seen.insert(key).second)
      continue;

    forced.push_back({root, o.aStart, o.aEnd, o.event});
    trace("counter",
          "__COUNTER__: expanded-macro stabilization firstExpandedIdx={0} "
          "force root id={1} name='{2}' ownerInc={3} inv=[{4},{5}) for occ "
          "A=[{6},{7})",
          firstExpandedIdx, root->id, root->name, root->ownerIncludeId,
          *invStart, *invEnd, o.aStart, o.aEnd);
  }

  return forced;
}
