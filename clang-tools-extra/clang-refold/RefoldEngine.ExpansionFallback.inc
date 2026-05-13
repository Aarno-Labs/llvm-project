//===--- RefoldEngine.ExpansionFallback.inc ------------------*- C++ -*-===//
//
// `RefoldEngine` member definitions physically split out of
// `RefoldEngine.cpp` to reduce the size of the main implementation file.
//
// This fragment contains the post-structural, pre-terminal fallback stage for
// proof-backed partial expansion. These members do not participate in normal
// structural candidate construction; they are only consulted after the main
// structural refolding pass has already requested terminal fallback and before
// the engine emits raw edited preprocessed text `B`.
//
// This is a physical extraction only:
//   - the methods remain `RefoldEngine` members
//   - the file is included directly into `RefoldEngine.cpp`
//   - private access, translation-unit scope, and behavior are unchanged
//
// The purpose of this fragment is organizational only: keep the explicit
// partial-expansion fallback seam and its witness-formatting helpers out of the
// main engine file while preserving the exact implementation structure and
// semantics.
//
//===----------------------------------------------------------------------===//


/// Return true iff `text` is only whitespace and complete C/C++ comments.
///
/// TU include-closure may preserve such gaps as inert source trivia while
/// replacing the surrounding include directive run. The parser is deliberately
/// strict: unterminated comments and any lexer-visible spelling remain outside
/// this proof class and continue to fail closed.
static bool isWhitespaceOrCompleteCommentTrivia(StringRef text) {
  size_t i = 0;
  const size_t n = text.size();

  while (i < n) {
    if (stringutils::isWs(text[i])) {
      ++i;
      continue;
    }

    if (i + 1 >= n || text[i] != '/')
      return false;

    if (text[i + 1] == '*') {
      i += 2;
      bool closed = false;
      while (i + 1 < n) {
        if (text[i] == '*' && text[i + 1] == '/') {
          i += 2;
          closed = true;
          break;
        }
        ++i;
      }
      if (!closed)
        return false;
      continue;
    }

    if (text[i + 1] == '/') {
      i += 2;
      while (i < n && text[i] != '\n')
        ++i;
      if (i < n)
        ++i;
      continue;
    }

    return false;
  }

  return true;
}

/// Parse a conditional-control directive line admitted as inert include-gap
/// trivia. Only literal `#if 0` / `#if 1` groups are accepted; directives with
/// macro-dependent conditions, side effects, or escaped physical lines remain
/// outside this proof class.
static bool parseLiteralEmptyConditionalDirectiveLine(StringRef line,
                                                      unsigned &depth) {
  if (line.ends_with("\n"))
    line = line.drop_back();
  if (line.ends_with("\r"))
    line = line.drop_back();

  StringRef noTrailingHorizontalWS = line.rtrim(" \t\v\f");
  if (noTrailingHorizontalWS.ends_with("\\"))
    return false;

  StringRef rest = line.ltrim(" \t\v\f");
  if (!rest.consume_front("#"))
    return false;
  rest = rest.ltrim(" \t\v\f");

  StringRef keyword = rest.take_while([](char c) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
           ('0' <= c && c <= '9') || c == '_';
  });
  rest = rest.drop_front(keyword.size()).trim(" \t\v\f");

  auto isLiteralBit = [](StringRef text) { return text == "0" || text == "1"; };

  if (keyword == "if") {
    if (!isLiteralBit(rest))
      return false;
    ++depth;
    return true;
  }

  if (keyword == "elif") {
    return depth > 0 && isLiteralBit(rest);
  }

  if (keyword == "else") {
    return depth > 0 && rest.empty();
  }

  if (keyword == "endif") {
    if (depth == 0 || !rest.empty())
      return false;
    --depth;
    return true;
  }

  return false;
}

/// Return true iff `text` is trivia that may be preserved while closing over a
/// TU include directive run. In addition to whitespace/comments, this admits
/// complete empty conditional-control groups whose `#if` / `#elif` conditions
/// are literal `0` or `1`, so moving them with the preserved gap does not
/// depend on macro state removed by materializing the neighboring includes.
static bool isPreservableIncludeClosureGapTrivia(StringRef text) {
  if (isWhitespaceOrCompleteCommentTrivia(text))
    return true;

  size_t i = 0;
  const size_t n = text.size();
  unsigned depth = 0;
  bool sawConditionalDirective = false;

  auto skipCompleteComment = [&](size_t &cursor) -> bool {
    if (cursor + 1 >= n || text[cursor] != '/')
      return false;

    if (text[cursor + 1] == '*') {
      cursor += 2;
      while (cursor + 1 < n) {
        if (text[cursor] == '*' && text[cursor + 1] == '/') {
          cursor += 2;
          return true;
        }
        ++cursor;
      }
      return false;
    }

    if (text[cursor + 1] == '/') {
      cursor += 2;
      while (cursor < n && text[cursor] != '\n')
        ++cursor;
      if (cursor < n)
        ++cursor;
      return true;
    }

    return false;
  };

  bool atLineStart = true;
  while (i < n) {
    if (stringutils::isWs(text[i])) {
      atLineStart = text[i] == '\n';
      ++i;
      continue;
    }

    if (text[i] == '/') {
      if (!skipCompleteComment(i))
        return false;
      continue;
    }

    if (!atLineStart && text[i] != '#')
      return false;

    size_t lineEnd = i;
    while (lineEnd < n && text[lineEnd] != '\n')
      ++lineEnd;
    if (lineEnd < n)
      ++lineEnd;

    if (!parseLiteralEmptyConditionalDirectiveLine(
            text.slice(i, lineEnd), depth))
      return false;

    sawConditionalDirective = true;
    atLineStart = true;
    i = lineEnd;
  }

  return sawConditionalDirective && depth == 0;
}

/// Return true when preserved trivia begins with a preprocessing directive after
/// optional horizontal whitespace. Such trivia must be placed at the start of a
/// physical line when appended after materialized B tokens.
static bool startsWithPreprocessorDirectiveTrivia(StringRef text) {
  StringRef rest = text.ltrim(" \t\v\f");
  return rest.starts_with("#");
}

std::string RefoldEngine::FormatExpansionClosureWitness(
    const ExpansionClosureWitness &witness) const {
  // Keep fallback-witness logging compact and mechanically comparable so the
  // acceptance path can be audited from traces without inspecting the full
  // in-memory witness object.
  std::string out = formatv("kind={0}", witness.kind).str();
  if (witness.hasRootMacroId)
    out += formatv(" rootMacro={0}", witness.rootMacroId).str();
  if (witness.hasOwnerIncludeId)
    out += formatv(" ownerInclude={0}", witness.ownerIncludeId).str();
  out += formatv(" source=[{0},{1})", witness.sourceByteBegin,
                 witness.sourceByteEnd)
             .str();
  out += formatv(" coverA=[{0},{1})", witness.coverATokBegin,
                 witness.coverATokEnd)
             .str();
  out += formatv(" B=[{0},{1})", witness.bTokBegin, witness.bTokEnd).str();
  out += formatv(" selfContained={0} claimsClipped={1}",
                 witness.selfContained ? 1 : 0,
                 witness.claimsClipped ? 1 : 0)
             .str();
  return out;
}

std::optional<RefoldEngine::ExpansionClosureWitness>
RefoldEngine::BuildMacroOwnerExpansionClosureWitness() const {
  if (!terminalFallbackRequested_) {
    trace("fallback", "macro-owner closure witness rejected: terminal fallback "
                      "not requested");
    return std::nullopt;
  }

  if (abTokHunks_.empty()) {
    trace("fallback",
          "macro-owner closure witness rejected: no token hunks available");
    return std::nullopt;
  }

  const StringRef tuPath = model_.GetSourcePath();
  if (tuPath.empty()) {
    trace("fallback",
          "macro-owner closure witness rejected: empty TU source path");
    return std::nullopt;
  }

  struct MacroOwnerClosureCandidate {
    const RefoldModel::MacroInvocation *root = nullptr;
    WholeCoverPlan plan;
  };

  // This witness deliberately proves only a whole-owner closure. A candidate root
  // is admissible only if every observed token hunk fits completely inside the
  // root's whole-cover envelope.
  //
  // For non-insert hunks, touching the boundary is allowed because the cover is
  // claimed inclusively over the replaced A-range. For insert-only hunks, we
  // require strict interior placement so the witness does not rely on a
  // boundary-adjacent ownership guess.
  auto hunkIsClosedByPlan = [&](const diffutils::Hunk &h,
                                const WholeCoverPlan &plan) -> bool {
    const bool aClosed =
        h.isInsertOnly() ? (plan.covLoA < h.aStart && h.aStart < plan.covHiA)
                         : (plan.covLoA <= h.aStart && h.aEnd <= plan.covHiA);
    if (!aClosed)
      return false;

    if (h.bStart < h.bEnd &&
        (h.bStart < plan.bTokStart || h.bEnd > plan.bTokEnd))
      return false;

    return true;
  };

  // Use explicit interval-level guards instead of relying on the
  // shape of the surviving candidate set alone. The fallback proof class is
  // only in-domain when its chosen owner frontier does not interact with any
  // foreign owner frontier.
  auto intervalsOverlap = [](uint64_t loA, uint64_t hiA, uint64_t loB,
                             uint64_t hiB) -> bool {
    return loA < hiB && loB < hiA;
  };

  auto isDescendantOf = [&](const RefoldModel::MacroInvocation &node,
                            const RefoldModel::MacroInvocation &root) -> bool {
    std::optional<uint64_t> cur = node.callerMacroId;
    DenseSet<uint64_t> visited;
    while (cur) {
      if (!visited.insert(*cur).second)
        break;
      if (*cur == root.id)
        return true;
      const RefoldModel::MacroInvocation *parent =
          FindMacroInvocationById(*cur);
      if (!parent)
        break;
      cur = parent->callerMacroId;
    }
    return false;
  };

  auto sourceIntervalIsSelfContainedTUOwned =
      [&](const RefoldModel::MacroInvocation &root) -> bool {
    if (!root.invB || !root.invE || *root.invE <= *root.invB)
      return false;

    ArrayRef<RefoldModel::Segment> segs = model_.GetSegmentsForFile(tuPath);
    if (segs.empty()) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' has no TU "
            "segments for source interval",
            root.id, root.name);
      return false;
    }

    // The first synthesized fallback class splices bytes directly back into the
    // original TU. Make that source frontier explicit: the full callsite byte
    // interval must be covered by TU-owned segments only, with no include-owner
    // transitions and no conditional-arm changes inside the splice span.
    uint64_t cur = *root.invB;
    std::optional<uint64_t> condArmId;
    bool sawCondArmId = false;
    bool sawCoveringSeg = false;
    for (const auto &seg : segs) {
      if (seg.e <= cur)
        continue;
      if (seg.b >= *root.invE)
        break;
      if (!intervalsOverlap(cur, *root.invE, seg.b, seg.e))
        continue;
      if (seg.b > cur) {
        trace("fallback",
              "macro-owner closure root rejected: id={0} name='{1}' source "
              "interval [{2},{3}) has uncovered gap before segment [{4},{5})",
              root.id, root.name, *root.invB, *root.invE, seg.b, seg.e);
        return false;
      }
      if (seg.ownerIncludeId) {
        trace("fallback",
              "macro-owner closure root rejected: id={0} name='{1}' source "
              "interval [{2},{3}) crosses include-owned segment [{4},{5}) "
              "includeId={6}",
              root.id, root.name, *root.invB, *root.invE, seg.b, seg.e,
              *seg.ownerIncludeId);
        return false;
      }
      if (!sawCondArmId) {
        condArmId = seg.ownerCondArmId;
        sawCondArmId = true;
      } else if (condArmId != seg.ownerCondArmId) {
        trace("fallback",
              "macro-owner closure root rejected: id={0} name='{1}' source "
              "interval [{2},{3}) crosses conditional-arm boundary between {4} "
              "and {5}",
              root.id, root.name, *root.invB, *root.invE, condArmId.value_or(0),
              seg.ownerCondArmId.value_or(0));
        return false;
      }
      sawCoveringSeg = true;
      cur = std::max<uint64_t>(cur, seg.e);
      if (cur >= *root.invE)
        return true;
    }

    if (!sawCoveringSeg) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' source "
            "interval [{2},{3}) has no covering TU segment",
            root.id, root.name, *root.invB, *root.invE);
      return false;
    }

    trace("fallback",
          "macro-owner closure root rejected: id={0} name='{1}' source "
          "interval [{2},{3}) is not fully covered by TU-owned segments",
          root.id, root.name, *root.invB, *root.invE);
    return false;
  };

  // Explicitly reject foreign macro interaction. For the first proof class,
  // the chosen owner closure must be the only macro-owner region participating
  // in the run; any intersecting non-descendant macro makes the frontier
  // ambiguous or mixed-owner and therefore out-of-domain.
  auto hasForeignOverlapOrMixedOwnership =
      [&](const RefoldModel::MacroInvocation &root,
          const WholeCoverPlan &plan) -> bool {
    for (const auto &other : model_.GetMacroInvocations()) {
      if (other.id == root.id)
        continue;

      const bool descendant = isDescendantOf(other, root);
      const bool sourceOverlap =
          other.invB && other.invE && *other.invE > *other.invB &&
          other.invFile && PathsEqual(*other.invFile, tuPath) &&
          intervalsOverlap(*root.invB, *root.invE, *other.invB, *other.invE);
      const bool coverOverlap =
          other.cover.IsValid() &&
          intervalsOverlap(plan.covLoA, plan.covHiA, other.cover.begin,
                           other.cover.end);

      if (!sourceOverlap && !coverOverlap)
        continue;

      if (descendant)
        continue;

      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' overlaps "
            "foreign macro id={2} name='{3}' sourceOverlap={4} "
            "coverOverlap={5} otherOwnerInclude={6}",
            root.id, root.name, other.id, other.name, sourceOverlap ? 1 : 0,
            coverOverlap ? 1 : 0, other.ownerIncludeId.value_or(0));
      return true;
    }
    return false;
  };

  SmallVector<MacroOwnerClosureCandidate, 4> candidates;
  for (const auto &m : model_.GetMacroInvocations()) {
    // The first fallback proof class is intentionally narrow: a single
    // top-level TU callsite macro whose whole-cover plan closes every edited
    // hunk by itself. Nested roots, include-owned callsites, define-body
    // invocations, and any clipped / non-self-contained plans remain
    // out-of-domain for this fallback proof.
    if (m.callerMacroId)
      continue;

    // The witness has to name an exact source callsite interval. If callsite
    // provenance is incomplete, we cannot soundly materialize the owner later.
    if (!m.invText || !m.invFile || !m.invB || !m.invE) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' missing "
            "callsite provenance",
            m.id, m.name);
      continue;
    }

    if (!PathsEqual(*m.invFile, tuPath)) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' callsite "
            "file='{2}' not TU='{3}'",
            m.id, m.name, StringRef(*m.invFile), tuPath);
      continue;
    }

    if (m.ownerIncludeId) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' belongs to "
            "include #{2}",
            m.id, m.name, *m.ownerIncludeId);
      continue;
    }

    if (*m.invE <= *m.invB) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' "
            "empty/invalid source range [{2},{3})",
            m.id, m.name, *m.invB, *m.invE);
      continue;
    }

    if (IsInvocationInsideDefineDirective(m)) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' invocation "
            "inside define directive",
            m.id, m.name);
      continue;
    }

    if (!InvocationSpanMatchesCallsitePrefix(*m.invText, m)) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' invocation "
            "text does not match callsite prefix",
            m.id, m.name);
      continue;
    }

    // Reuse the existing whole-cover computation as the closure/frontier
    // witness. Accept only roots whose existing plan already proves a
    // self-contained, unclipped edited B-surface.
    auto plan = ComputeWholeCoverPlan(m);
    if (!plan) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' has no "
            "whole-cover plan",
            m.id, m.name);
      continue;
    }

    if (!plan->selfContained || plan->claimsClipped ||
        plan->bTokEnd <= plan->bTokStart) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' "
            "selfContained={2} claimsClipped={3} B=[{4},{5})",
            m.id, m.name, plan->selfContained ? 1 : 0,
            plan->claimsClipped ? 1 : 0, plan->bTokStart, plan->bTokEnd);
      continue;
    }

    if (!sourceIntervalIsSelfContainedTUOwned(m))
      continue;

    // Make several previously implicit exclusions explicit. Reject any
    // candidate whose owner frontier overlaps a foreign macro closure or whose
    // callsite/source frontier crosses a mixed-ownership boundary. The single-
    // owner fallback class remains valid only when the proved closure is the
    // one and only owner region needed for the run.
    if (hasForeignOverlapOrMixedOwnership(m, *plan))
      continue;

    // The proof class is "single owner closes the entire edit set." Any hunk
    // left outside the candidate plan means this root is insufficient and must
    // be rejected instead of heuristically expanded upward.
    bool closesAllHunks = true;
    for (const auto &h : abTokHunks_) {
      if (hunkIsClosedByPlan(h, *plan))
        continue;

      trace(
          "fallback",
          "macro-owner closure root rejected: id={0} name='{1}' does not close "
          "hunk A=[{2},{3}) B=[{4},{5}) coverA=[{6},{7}) coverB=[{8},{9})",
          m.id, m.name, h.aStart, h.aEnd, h.bStart, h.bEnd, plan->covLoA,
          plan->covHiA, plan->bTokStart, plan->bTokEnd);
      closesAllHunks = false;
      break;
    }

    if (!closesAllHunks)
      continue;

    trace("fallback",
          "macro-owner closure root accepted: id={0} name='{1}' "
          "source=[{2},{3}) coverA=[{4},{5}) B=[{6},{7})",
          m.id, m.name, *m.invB, *m.invE, plan->covLoA, plan->covHiA,
          plan->bTokStart, plan->bTokEnd);
    candidates.push_back(MacroOwnerClosureCandidate{&m, *plan});
  }

  if (candidates.empty()) {
    trace("fallback", "macro-owner closure witness rejected: no single-owner "
                      "root candidate survived");
    return std::nullopt;
  }

  // Ambiguity is out-of-domain for the first proof class. The stage only
  // accepts one surviving root; multiple survivors require a later explicit
  // compatibility / minimal-frontier proof.
  if (candidates.size() != 1) {
    std::string ids;
    for (const auto &cand : candidates) {
      if (!ids.empty())
        ids += ",";
      ids += llvm::utostr(cand.root->id);
    }
    trace("fallback",
          "macro-owner closure witness rejected: ambiguous surviving roots={0}",
          ids);
    return std::nullopt;
  }

  const auto &cand = candidates.front();
  ExpansionClosureWitness witness;

  // Record only the proved owner interval and the cover/B frontiers needed by
  // later synthesis. This witness builder does not attempt to materialize text yet.
  witness.kind = ExpansionClosureKind::MacroOwnerWholeExpansion;
  witness.hasRootMacroId = true;
  witness.rootMacroId = cand.root->id;
  witness.hasOwnerIncludeId = false;
  witness.ownerIncludeId = 0;
  witness.sourceByteBegin = *cand.root->invB;
  witness.sourceByteEnd = *cand.root->invE;
  witness.coverATokBegin = cand.plan.covLoA;
  witness.coverATokEnd = cand.plan.covHiA;
  witness.bTokBegin = cand.plan.bTokStart;
  witness.bTokEnd = cand.plan.bTokEnd;
  witness.selfContained = cand.plan.selfContained;
  witness.claimsClipped = cand.plan.claimsClipped;
  return witness;
}

bool RefoldEngine::MacroOwnerExpansionClosureWitnessIsDominated(
    const ExpansionClosureWitness &witness, std::string &detail) const {
  auto reject = [&](std::string why) -> bool {
    detail = std::move(why);
    return false;
  };

  // This audit is intentionally definitionally aligned with the fallback
  // witness domain. Any other witness kind is not evidence about the legacy
  // macro-owner expansion closure path, so it cannot prove dominance.
  if (witness.kind != ExpansionClosureKind::MacroOwnerWholeExpansion ||
      !witness.hasRootMacroId || witness.hasOwnerIncludeId) {
    return reject(
        formatv("witness is outside macro-owner whole-expansion domain: {0}",
                FormatExpansionClosureWitness(witness))
            .str());
  }

  const RefoldModel::MacroInvocation *root =
      FindMacroInvocationById(witness.rootMacroId);
  if (!root) {
    return reject(formatv("root macro id {0} is not present in the model",
                          witness.rootMacroId)
                      .str());
  }

  if (!root->invB || !root->invE || *root->invE <= *root->invB) {
    return reject(formatv("root macro id {0} name='{1}' lacks a valid callsite "
                          "source interval",
                          root->id, root->name)
                      .str());
  }

  // Recompute the same whole-cover plan used by normal macro candidate
  // construction. A dominated fallback witness must be reproducible from the
  // ordinary selector path without relying on post-terminal state.
  auto plan = ComputeWholeCoverPlan(*root);
  if (!plan) {
    return reject(formatv("root macro id {0} name='{1}' no longer has a normal "
                          "whole-cover plan",
                          root->id, root->name)
                      .str());
  }

  const bool planMatchesWitness =
      plan->covLoA == witness.coverATokBegin &&
      plan->covHiA == witness.coverATokEnd &&
      plan->bTokStart == witness.bTokBegin &&
      plan->bTokEnd == witness.bTokEnd &&
      plan->selfContained == witness.selfContained &&
      plan->claimsClipped == witness.claimsClipped;
  if (!planMatchesWitness) {
    return reject(
        formatv("normal whole-cover plan for root macro id {0} name='{1}' does "
                "not match fallback witness: plan coverA=[{2},{3}) B=[{4},{5}) "
                "selfContained={6} claimsClipped={7}; witness={8}",
                root->id, root->name, plan->covLoA, plan->covHiA,
                plan->bTokStart, plan->bTokEnd, plan->selfContained ? 1 : 0,
                plan->claimsClipped ? 1 : 0,
                FormatExpansionClosureWitness(witness))
            .str());
  }

  // Restamp the recomputed plan exactly as
  // BuildMacroInvocationPatchWholeCover() does for its normal
  // WholeCoverRealization candidate, then require that the normalized proof
  // carrier discharges. This proves the witness is not merely a raw replacement
  // string; it is visible through the first-class macro realization proof
  // class.
  MacroPatch patch{*root->invB, *root->invE, plan->clippedText, root->id};
  StampMacroWholeCoverRealizationPatch(patch, *plan, root->id);
  const AcceptedResultCandidate candidate = BuildAcceptedMacroCandidate(patch);
  const ProofSummary &summary = candidate.proofSummary;
  if (candidate.kind != AcceptedResultCandidateKind::MacroPatch ||
      summary.inventory.currentPath !=
          AcceptedPathKind::MacroWholeCoverRealization ||
      summary.discharge.status != ProofDischargeStatus::Discharged) {
    return reject(
        formatv("normal whole-cover candidate for root macro id {0} name='{1}' "
                "did not discharge as MacroWholeCoverRealization: {2}",
                root->id, root->name, FormatAcceptedResultCandidate(candidate))
            .str());
  }

  // Return true when the hunk is fully contained by the whole-cover plan's
  // A-side cover and, for non-empty B ranges, by the plan's B-token envelope.
  auto hunkIsClosedByPlan = [&](const diffutils::Hunk &h) -> bool {
    const bool aClosed =
        h.isInsertOnly() ? (plan->covLoA < h.aStart && h.aStart < plan->covHiA)
                         : (plan->covLoA <= h.aStart && h.aEnd <= plan->covHiA);
    if (!aClosed)
      return false;
    if (h.bStart < h.bEnd &&
        (h.bStart < plan->bTokStart || h.bEnd > plan->bTokEnd))
      return false;
    return true;
  };

  // Return true when `selected` reaches `root` by following ordinary
  // callerMacroId links without cycles or broken parent references.
  auto reachesRootThroughNormalClimb =
      [&](const RefoldModel::MacroInvocation *selected) -> bool {
    DenseSet<uint64_t> visited;
    for (const RefoldModel::MacroInvocation *cur = selected; cur;) {
      if (!visited.insert(cur->id).second)
        return false;
      if (cur->id == root->id)
        return true;
      if (!cur->callerMacroId)
        return false;
      cur = FindMacroInvocationById(*cur->callerMacroId);
    }
    return false;
  };

  const StringRef tuPath = model_.GetSourcePath();
  if (tuPath.empty())
    return reject(
        "empty TU source path while auditing macro-owner fallback dominance");

  unsigned closedHunks = 0;
  for (const diffutils::Hunk &h : abTokHunks_) {
    if (!hunkIsClosedByPlan(h)) {
      return reject(
          formatv(
              "root macro id {0} name='{1}' normal plan does not close hunk "
              "A=[{2},{3}) B=[{4},{5}) coverA=[{6},{7}) coverB=[{8},{9})",
              root->id, root->name, h.aStart, h.aEnd, h.bStart, h.bEnd,
              plan->covLoA, plan->covHiA, plan->bTokStart, plan->bTokEnd)
              .str());
    }

    // Mirror the ordinary hunk-classification selector exactly: classify the
    // hunk's current owner, ask for the smallest patchable covering macro, then
    // allow the existing normal path's caller-chain climb to reach the root.
    Owner owner = ClassifyOwnerWithSegments(tuPath, h);
    const RefoldModel::MacroInvocation *selected =
        SmallestCoveringPatchableMacro(h.aStart, h.aEnd, owner.includeId);
    if (!selected) {
      return reject(
          formatv("normal selector found no covering macro for hunk "
                  "A=[{0},{1}) B=[{2},{3}) ownerKind={4} ownerInclude={5}",
                  h.aStart, h.aEnd, h.bStart, h.bEnd, owner.kind,
                  owner.includeId.value_or(0))
              .str());
    }

    if (!selected->invB || !selected->invE) {
      return reject(formatv("normal selector chose macro id {0} name='{1}' for "
                            "hunk A=[{2},{3}) but it lacks callsite provenance",
                            selected->id, selected->name, h.aStart, h.aEnd)
                        .str());
    }

    if (!reachesRootThroughNormalClimb(selected)) {
      return reject(formatv("normal selector chose macro id {0} name='{1}' for "
                            "hunk A=[{2},{3}), but its caller chain does not "
                            "reach fallback root id {4} name='{5}'",
                            selected->id, selected->name, h.aStart, h.aEnd,
                            root->id, root->name)
                        .str());
    }
    ++closedHunks;
  }

  detail =
      formatv(
          "dominated by normal MacroWholeCoverRealization root id={0} "
          "name='{1}' source=[{2},{3}) coverA=[{4},{5}) B=[{6},{7}) hunks={8}",
          root->id, root->name, *root->invB, *root->invE, plan->covLoA,
          plan->covHiA, plan->bTokStart, plan->bTokEnd, closedHunks)
          .str();
  return true;
}

std::optional<RefoldEngine::TextEdit>
RefoldEngine::BuildTUIncludeClosureEditForUnresolvedHunk(
    const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
    ArrayRef<std::pair<uint64_t, uint64_t>> stagedSourceIntervals) const {
  // This helper is intentionally narrower than ordinary include realization.
  // It is the first hybrid source-closure class used to keep already-proved
  // structural artifacts (for example a preserved macro callsite rewrite)
  // while materializing one otherwise-unresolved PP region directly into the
  // TU source. Stay fail-closed unless the closure is exact or can be widened
  // without absorbing any independent token diff.
  if (h.isInsertOnly()) {
    trace("fallback",
          "TU include-closure rejected: insert-only hunk A=[{0},{1}) "
          "B=[{2},{3}) is outside the first closure-edit domain",
          h.aStart, h.aEnd, h.bStart, h.bEnd);
    return std::nullopt;
  }

  SmallVector<const RefoldModel::IncludeItem *, 8> touched;
  for (const auto &inc : model_.GetIncludes()) {
    // Only top-level includes spelled directly in the TU are eligible for this
    // first source-closure class. Nested includes already have a dedicated
    // include-materialization path, while non-TU sites would need a different
    // embedding proof.
    if (inc.parent || !PathsEqual(inc.sitePath, tuPath))
      continue;
    if (inc.cover.end <= h.aStart || inc.cover.begin >= h.aEnd)
      continue;
    touched.push_back(&inc);
  }

  if (touched.empty()) {
    trace("fallback",
          "TU include-closure rejected: no top-level TU include touches hunk "
          "A=[{0},{1})",
          h.aStart, h.aEnd);
    return std::nullopt;
  }

  // Process touched includes in TU order so the closure can be proven as one
  // contiguous include cover rather than as a set of unrelated include ranges.
  llvm::sort(touched, [](const RefoldModel::IncludeItem *lhs,
                         const RefoldModel::IncludeItem *rhs) {
    if (lhs->siteB != rhs->siteB)
      return lhs->siteB < rhs->siteB;
    return lhs->id < rhs->id;
  });

  const RefoldModel::IncludeItem *first = touched.front();
  const RefoldModel::IncludeItem *last = touched.back();
  uint64_t coverBegin = first->cover.begin;
  uint64_t coverEnd = coverBegin;

  // Require the touched include covers to tile exactly. Any gap, overlap, or
  // empty include cover means the hunk cannot be represented by one
  // include-closure replacement.
  for (const RefoldModel::IncludeItem *inc : touched) {
    if (inc->cover.begin != coverEnd || inc->cover.end <= inc->cover.begin) {
      trace("fallback",
            "TU include-closure rejected: touched includes do not form one "
            "contiguous A-cover at inc#{0} cover=[{1},{2}) expectedBegin={3}",
            inc->id, inc->cover.begin, inc->cover.end, coverEnd);
      return std::nullopt;
    }
    coverEnd = inc->cover.end;
  }

  // `exactCover` means the edit already matches the include closure. The
  // wider `hunkInsideCover` case is a controlled widening: the hunk may be
  // absorbed only if later checks prove the wider include closure is safe.
  const bool exactCover = coverBegin == h.aStart && coverEnd == h.aEnd;
  const bool hunkInsideCover = coverBegin <= h.aStart && h.aEnd <= coverEnd;

  auto sameTokenHunk = [](const diffutils::Hunk &lhs,
                          const diffutils::Hunk &rhs) -> bool {
    return lhs.aStart == rhs.aStart && lhs.aEnd == rhs.aEnd &&
           lhs.bStart == rhs.bStart && lhs.bEnd == rhs.bEnd;
  };

  // Return true when `cand` would be absorbed by the closed A-side range
  // [begin, end]. Token-consuming edits use interval overlap; pure insertions
  // use the closed insertion-gap test.
  auto hunkTouchesAClosedRange = [](const diffutils::Hunk &cand, uint64_t begin,
                                    uint64_t end) -> bool {
    if (end < begin)
      end = begin;

    // A-consuming edits touch the extra cover when their token intervals
    // overlap it. Pure insertions touch the cover when their insertion anchor
    // lies in the closed gap interval: a widened include materialization would
    // otherwise absorb the insertion payload without going through the normal
    // candidate-conflict law.
    if (cand.aStart < cand.aEnd)
      return cand.aStart < end && begin < cand.aEnd;
    return cand.bStart < cand.bEnd && cand.aStart >= begin &&
           cand.aStart <= end;
  };

  // Return true when the proposed widened A range would cover any other token
  // diff hunk besides the hunk currently being realized.
  auto rangeHasForeignTokenDiff = [&](uint64_t begin, uint64_t end) -> bool {
    for (const diffutils::Hunk &cand : abTokHunks_) {
      if (sameTokenHunk(cand, h))
        continue;
      if (hunkTouchesAClosedRange(cand, begin, end))
        return true;
    }
    return false;
  };

  // Return true when the proposed widened source range would overlap an edit
  // that has already been staged. Zero-width staged edits are treated as closed
  // insertion points.
  auto sourceTouchesStagedEdit = [&](uint64_t begin, uint64_t end) -> bool {
    for (const auto &interval : stagedSourceIntervals) {
      if (interval.first < interval.second) {
        if (interval.first < end && begin < interval.second)
          return true;
        continue;
      }

      if (begin <= interval.first && interval.first <= end)
        return true;
    }
    return false;
  };

  if (!exactCover) {
    if (!hunkInsideCover) {
      trace("fallback",
            "TU include-closure rejected: touched include cover A=[{0},{1}) "
            "does not contain hunk A=[{2},{3})",
            coverBegin, coverEnd, h.aStart, h.aEnd);
      return std::nullopt;
    }

    // Widening from the unresolved hunk to the complete include-cover is
    // admissible only when the widened-away prefix/suffix are observationally
    // unchanged in the token diff. That keeps this as an include-closure proof,
    // not a hidden multi-region merge: any independent token edit or insertion
    // in the extra cover must be handled by the normal partition/lattice path.
    const bool prefixClean = !rangeHasForeignTokenDiff(coverBegin, h.aStart);
    const bool suffixClean = !rangeHasForeignTokenDiff(h.aEnd, coverEnd);
    if (!prefixClean || !suffixClean) {
      trace("fallback",
            "TU include-closure rejected: widening A=[{0},{1}) around hunk "
            "A=[{2},{3}) would absorb foreign token edits prefixClean={4} "
            "suffixClean={5}",
            coverBegin, coverEnd, h.aStart, h.aEnd, prefixClean ? 1 : 0,
            suffixClean ? 1 : 0);
      return std::nullopt;
    }

    if (sourceTouchesStagedEdit(first->siteB, last->siteE)) {
      trace("fallback",
            "TU include-closure rejected: widened source interval [{0},{1}) "
            "would absorb an already-staged source edit",
            first->siteB, last->siteE);
      return std::nullopt;
    }

    trace("fallback",
          "TU include-closure widening accepted: hunk A=[{0},{1}) widened to "
          "include cover A=[{2},{3})",
          h.aStart, h.aEnd, coverBegin, coverEnd);
  }

  if (first->siteB >= first->siteE || last->siteE > tuBytes.size() ||
      first->siteB >= last->siteE) {
    trace("fallback",
          "TU include-closure rejected: invalid TU source interval [{0},{1}) "
          "fileLen={2}",
          first->siteB, last->siteE, tuBytes.size());
    return std::nullopt;
  }

  std::string preservedGapTrivia;
  uint64_t sourceCursor = first->siteB;
  for (const RefoldModel::IncludeItem *inc : touched) {
    if (inc->siteB < sourceCursor || inc->siteE > tuBytes.size() ||
        inc->siteB >= inc->siteE) {
      trace("fallback",
            "TU include-closure rejected: invalid include site for inc#{0} "
            "site=[{1},{2}) cursor={3} fileLen={4}",
            inc->id, inc->siteB, inc->siteE, sourceCursor, tuBytes.size());
      return std::nullopt;
    }

    // The TU closure may absorb only include directives plus inert trivia
    // between neighboring directives. Whitespace is already source-neutral;
    // comments and literal empty conditional-control groups are admitted only
    // because the emitted edit preserves their bytes verbatim instead of
    // silently deleting user-written text.
    if (inc->siteB > sourceCursor) {
      StringRef gap = tuBytes.slice(sourceCursor, inc->siteB);
      if (!gap.empty() && !stringutils::isWhitespace(gap)) {
        if (!isPreservableIncludeClosureGapTrivia(gap)) {
          trace("fallback",
                "TU include-closure rejected: non-trivia TU gap between "
                "include directives before inc#{0} gap='{1}'",
                inc->id, stringutils::showWSWithClip(gap, 120));
          return std::nullopt;
        }

        preservedGapTrivia.append(gap.begin(), gap.end());
        trace("fallback",
              "TU include-closure preserving inert trivia gap before inc#{0} "
              "gap='{1}'",
              inc->id, stringutils::showWSWithClip(gap, 120));
      }
    }

    sourceCursor = inc->siteE;
  }

  IncludeRealizationEvidenceKind evidenceKind =
      IncludeRealizationEvidenceKind::Unknown;
  auto bEnvelope = ResolveIncludeRealizationBTokenEnvelope(coverBegin, coverEnd,
                                                           &evidenceKind);
  if (!bEnvelope) {
    trace("fallback",
          "TU include-closure rejected: no canonical-or-consensus B envelope "
          "for A-cover [{0},{1})",
          coverBegin, coverEnd);
    return std::nullopt;
  }

  auto bMaterialBegin = bEnvelope->first;
  auto bMaterialEnd = bEnvelope->second;

  // Canonical A-cover projection can include stable boundary tokens that are
  // outside the include cover, such as the `{` before an include run inside an
  // initializer. The source edit starts at the first include directive, so such
  // outside anchors must not be materialized or they would be duplicated in the
  // TU. Trim only B tokens that are proven stable and mapped to A tokens outside
  // the include cover; unmatched B tokens and stable tokens inside the cover
  // remain part of the realization payload.
  auto bTokenMapsOutsideCover = [&](size_t bTok) -> bool {
    if (bTok >= abTokMapB2A_.size())
      return false;
    const int64_t mappedA = abTokMapB2A_[bTok];
    if (mappedA < 0)
      return false;
    const uint64_t aTok = static_cast<uint64_t>(mappedA);
    return aTok < coverBegin || aTok >= coverEnd;
  };

  while (bMaterialBegin < bMaterialEnd &&
         bTokenMapsOutsideCover(bMaterialBegin))
    ++bMaterialBegin;
  while (bMaterialBegin < bMaterialEnd &&
         bTokenMapsOutsideCover(bMaterialEnd - 1))
    --bMaterialEnd;

  if (h.bStart < bMaterialBegin || h.bEnd > bMaterialEnd) {
    trace("fallback",
          "TU include-closure rejected: hunk B=[{0},{1}) is outside trimmed "
          "include-cover B material [{2},{3}) from envelope [{4},{5})",
          h.bStart, h.bEnd, bMaterialBegin, bMaterialEnd, bEnvelope->first,
          bEnvelope->second);
    return std::nullopt;
  }

  std::string rawReplacement = SliceBSource(bMaterialBegin, bMaterialEnd).str();
  std::string replacement = rawReplacement;
  if (!preservedGapTrivia.empty()) {
    if (startsWithPreprocessorDirectiveTrivia(preservedGapTrivia) &&
        !replacement.empty() && replacement.back() != '\n')
      replacement.push_back('\n');
    replacement.append(preservedGapTrivia.begin(), preservedGapTrivia.end());
  }

  // The closure begins at the exact include-directive run proved above, but we
  // may later extend its right edge over immediately following untouched TU
  // line(s). That keeps any required physical-line compensation after copied
  // suffix lines such as `};` instead of introducing a blank line inside the
  // realized replacement itself.
  uint64_t closureSourceEnd = last->siteE;
  std::string padded = PadAtBoundaries(
      tuBytes, static_cast<size_t>(first->siteB),
      static_cast<size_t>(closureSourceEnd), std::move(replacement),
      /*allowLeft=*/true, /*allowRight=*/true);

  // This closure class replaces only a contiguous run of top-level include
  // directives plus admitted inert trivia between them. Preserve the original
  // physical line count of the replaced source so the untouched suffix can
  // resume on its original logical line without needing a #line directive.
  // When there is a deficit, first try to sink it past immediately following
  // untouched TU
  // line(s) by absorbing those exact source bytes into the same TU edit.
  const size_t origNl =
      stringutils::countNewlines(tuBytes, first->siteB, closureSourceEnd);
  const size_t initialReplNl = stringutils::countNewlines(padded);
  if (origNl > initialReplNl) {
    size_t remainingPadNl = origNl - initialReplNl;

    // Return true when the source interval overlaps a spelled include directive
    // or macro invocation in this TU. Widened include-closure edits must not
    // absorb such artifacts implicitly.
    auto intervalOverlapsSpelledArtifact = [&](uint64_t begin,
                                               uint64_t end) -> bool {
      for (const auto &inc : model_.GetIncludes()) {
        if (!PathsEqual(inc.sitePath, tuPath))
          continue;
        if (inc.siteB < end && begin < inc.siteE)
          return true;
      }
      for (const auto &m : model_.GetMacroInvocations()) {
        if (m.invFile && !m.invFile->empty() && !PathsEqual(*m.invFile, tuPath))
          continue;
        if (!m.invB || !m.invE)
          continue;
        if (*m.invB < end && begin < *m.invE)
          return true;
      }
      return false;
    };

    if (remainingPadNl > 0 && closureSourceEnd < tuBytes.size()) {
      const uint64_t lineBegin = closureSourceEnd;
      uint64_t lineEnd = lineBegin;
      while (lineEnd < tuBytes.size() && tuBytes[lineEnd] != '\n')
        ++lineEnd;
      if (lineEnd < tuBytes.size())
        ++lineEnd;

      // Sink the unavoidable physical-line compensation past at most one
      // immediately following substantive TU line. This is enough to move the
      // blank-line surrogate past a copied suffix line such as `};`, while
      // remaining conservative: copied-line absorption is newline-deficit
      // invariant, so absorbing additional lines would only push the eventual
      // compensation point farther down the file without improving correctness.
      if (lineEnd > lineBegin &&
          !intervalOverlapsSpelledArtifact(lineBegin, lineEnd)) {
        StringRef line = tuBytes.slice(lineBegin, lineEnd);
        StringRef trimmed = line.trim();
        if (!trimmed.empty() && trimmed.front() != '#') {
          padded.append(line.begin(), line.end());
          closureSourceEnd = lineEnd;

          // Re-emitting one untouched trailing line verbatim moves the eventual
          // physical-line compensation point to just after that copied line,
          // but it does not reduce the outstanding newline deficit. The
          // original source span grows by the same newline count as the
          // replacement text, so the difference between them is invariant under
          // this exact copied-line absorption.
          trace("fallback",
                "TU include-closure newline sink: absorbed source=[{0},{1}) "
                "line='{2}' remainingPad={3} (single-line invariant-preserving "
                "sink)",
                lineBegin, lineEnd, stringutils::showWSWithClip(line, 120),
                remainingPadNl);
        }
      }
    }

    if (remainingPadNl > 0) {
      if (lineDirs_.Enabled()) {
        // Keep line-preservation semantic rather than physical in normal mode:
        // suppress synthetic blank padding here and let ApplyResyncOrPend()
        // observe the unchanged newline deficit over the final edit span. If a
        // resync is needed, it will be emitted as a #line directive after any
        // copied suffix line absorbed above.
        trace("fallback",
              "TU include-closure physical newline pad deferred to #line "
              "resync: source=[{0},{1}) suppressedNl={2}",
              first->siteB, closureSourceEnd, remainingPadNl);
      } else {
        // In --no-lines mode, clang-refold intentionally does not preserve
        // source-location semantics. The verification path already masks
        // location-sensitive predefined macro differences for this mode, so
        // newline-deficit padding would only manufacture visible blank lines in
        // the refolded source. Boundary padding above is still responsible for
        // lexical separation; this padding is solely for physical line count.
        trace("fallback",
              "TU include-closure physical newline pad suppressed by "
              "--no-lines: source=[{0},{1}) suppressedNl={2}",
              first->siteB, closureSourceEnd, remainingPadNl);
      }
      remainingPadNl = 0;
    }

    if (remainingPadNl > 0)
      padded.append(remainingPadNl, '\n');

    trace("fallback",
          "TU include-closure newline pad: source=[{0},{1}) origNl={2} "
          "replNl={3} physicalPadNl={4} finalSourceEnd={5} result='{6}'",
          first->siteB, last->siteE, origNl, initialReplNl, remainingPadNl,
          closureSourceEnd, stringutils::showWSWithClip(padded, 120));
  }

  debug("fallback",
        "TU include-closure accepted: includes=[{0},{1}] source=[{2},{3}) "
        "A=[{4},{5}) B=[{6},{7}) evidence={8} raw='{9}' padded='{10}'",
        first->id, last->id, first->siteB, closureSourceEnd, coverBegin,
        coverEnd, bEnvelope->first, bEnvelope->second, evidenceKind,
        stringutils::showWSWithClip(rawReplacement, 120),
        stringutils::showWSWithClip(padded, 120));

  ResyncOutcome ro = ApplyResyncOrPend(tuBytes, first->siteB, closureSourceEnd,
                                       padded, tuPath);
  TextEdit edit{first->siteB,          closureSourceEnd, std::move(ro.text),
                std::move(ro.pending), std::nullopt,     {}};

  // The emitted artifact is still a deterministic TU byte replacement even
  // though the witness came from an exact include-closure proof. Reuse the TU
  // textual-edit carrier until a dedicated closure-edit proof class exists.
  AttachAcceptedResultCarrier(
      edit, BuildAcceptedTUTextEditCandidate(
                AcceptedPathKind::TUByteSpanConservativeEdit, first->siteB,
                last->siteE, StringRef(rawReplacement)));
  return edit;
}

bool RefoldEngine::ExpansionFallbackReplacementIsReplayStable(
    StringRef replacementText) const {
  if (replacementText.empty()) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: empty replacement text");
    return false;
  }

  // Expansion fallback emits ordinary source text, not a preprocessor token
  // stream with the original hide-set context. Therefore the replacement must
  // be stable if it is lexed again as source: it must not contain tokens that
  // could participate in another macro-expansion decision.
  //
  // For the current fallback class, reject raw identifiers and preprocessor
  // paste/directive operators. Accepting those would require a stronger proof
  // that rescanning the emitted text cannot trigger a different macro expansion
  // than the original expansion witness.
  std::string buffer = replacementText.str();
  if (buffer.empty() || buffer.back() != '\n')
    buffer.push_back('\n');

  using namespace clang;
  IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
  IntrusiveRefCntPtr<DiagnosticOptions> diagOpts(new DiagnosticOptions());
  auto *client = new IgnoringDiagConsumer();
  DiagnosticsEngine diags(diagIDs, diagOpts, client,
                          /*ShouldOwnClient=*/true);

  FileSystemOptions fso;
  FileManager fm(fso);
  SourceManager sm(diags, fm);
  auto mb =
      llvm::MemoryBuffer::getMemBufferCopy(buffer, "<expansion-fallback>");
  FileID fid = sm.createFileID(std::move(mb));

  // Raw-lex the synthesized replacement under the same language options used
  // by the recorded preprocessor model. This checks the text as source
  // spelling, without macro expansion or preprocessor state.
  const LangOptions lang = MakeLexLangOptions(model_.GetPPLang());
  Lexer lex(fid, sm.getBufferOrFake(fid), sm, lang);
  lex.SetKeepWhitespaceMode(false);

  const StringRef fileBytes = sm.getBufferOrFake(fid).getBuffer();
  Token token;
  while (true) {
    lex.LexFromRawLexer(token);
    if (token.is(clang::tok::eof))
      break;

    // These token kinds are replay-unstable in ordinary source text: an
    // identifier may expand as a macro, and #/## have preprocessor semantics
    // that are not justified by this fallback proof.
    if (token.is(clang::tok::raw_identifier) || token.is(clang::tok::hash) ||
        token.is(clang::tok::hashhash)) {
      const char *ptr = sm.getCharacterData(token.getLocation());
      const size_t off = static_cast<size_t>(ptr - fileBytes.data());
      const StringRef spelling(ptr, token.getLength());
      trace("fallback",
            "macro-owner fallback synthesis rejected: replay-unstable token "
            "kind={0} off={1} spelling='{2}'",
            clang::tok::getTokenName(token.getKind()), off,
            stringutils::showWSWithClip(spelling, 80));
      return false;
    }
  }

  return true;
}

std::optional<std::string>
RefoldEngine::SynthesizeMacroOwnerExpansionClosureSource(
    const ExpansionClosureWitness &witness) const {
  // This synthesis path is intentionally narrow: it only handles a single
  // self-contained top-level macro-owner expansion in the TU. Include-owned
  // witnesses, clipped witnesses, and multi-owner closures must use their
  // normal structural/whole-cover paths instead.
  if (witness.kind != ExpansionClosureKind::MacroOwnerWholeExpansion ||
      !witness.hasRootMacroId || witness.hasOwnerIncludeId) {
    trace("fallback", "macro-owner fallback synthesis rejected: witness not in "
                      "single-root macro domain");
    return std::nullopt;
  }

  // The witness must describe one complete, unclipped source interval and one
  // non-empty B-token envelope. If any of these invariants are missing, the
  // fallback would not be a closed owner splice.
  if (!witness.selfContained || witness.claimsClipped ||
      witness.sourceByteEnd <= witness.sourceByteBegin ||
      witness.bTokEnd <= witness.bTokBegin) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: invalid witness bounds or "
          "clipped/selfContained flags source=[{0},{1}) B=[{2},{3}) "
          "selfContained={4} claimsClipped={5}",
          witness.sourceByteBegin, witness.sourceByteEnd, witness.bTokBegin,
          witness.bTokEnd, witness.selfContained ? 1 : 0,
          witness.claimsClipped ? 1 : 0);
    return std::nullopt;
  }

  const StringRef tuPath = model_.GetSourcePath();
  if (tuPath.empty()) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: empty TU source path");
    return std::nullopt;
  }

  // Read the original TU source because this fallback emits a complete TU text,
  // not just a local replacement fragment.
  const std::string absTuPath = lineDirs_.ToAbsolutePath(tuPath);
  auto tuBufferOrErr = llvm::MemoryBuffer::getFile(absTuPath);
  if (!tuBufferOrErr) {
    debug("fallback",
          "macro-owner fallback synthesis rejected: failed to read TU source "
          "'{0}' ({1})",
          absTuPath, tuBufferOrErr.getError().message());
    return std::nullopt;
  }
  const StringRef tuBytes = (*tuBufferOrErr)->getBuffer();

  if (witness.sourceByteEnd > tuBytes.size()) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: source interval [{0},{1}) "
          "exceeds TU size={2}",
          witness.sourceByteBegin, witness.sourceByteEnd, tuBytes.size());
    return std::nullopt;
  }

  // Materialize the witness's full proved B frontier. Unlike structural
  // whole-cover patches, this fallback is a single closed owner splice, so it
  // must not inherit standalone-claim clipping that is only valid when B
  // insertions are emitted through separate structural edits.
  std::string replacement =
      SliceBSource(witness.bTokBegin, witness.bTokEnd).str();
  if (replacement.empty()) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: materialization for "
          "B=[{0},{1}) is empty",
          witness.bTokBegin, witness.bTokEnd);
    return std::nullopt;
  }

  // The synthesized source must be safe to replay as ordinary source text. In
  // particular, it must not rely on hidden preprocessor state from the original
  // expansion.
  if (!ExpansionFallbackReplacementIsReplayStable(replacement))
    return std::nullopt;

  // Re-embed the materialized owner into the TU using the same boundary
  // spacing and line-drift machinery as ordinary TU byte edits. This keeps the
  // synthesis class deterministic and avoids a separate emission path with
  // subtly different token-adjacency or #line behavior.
  std::string padded =
      PadAtBoundaries(tuBytes, static_cast<size_t>(witness.sourceByteBegin),
                      static_cast<size_t>(witness.sourceByteEnd),
                      std::move(replacement), /*allowLeft=*/true,
                      /*allowRight=*/true);
  ResyncOutcome ro = ApplyResyncOrPend(tuBytes, witness.sourceByteBegin,
                                       witness.sourceByteEnd, padded, tuPath);

  // Assemble the final TU text while preserving pending line-resync state
  // across copied original slices. The only changed region is the witnessed
  // source byte interval.
  SmallString<0> out;
  out.reserve(tuBytes.size() -
              (witness.sourceByteEnd - witness.sourceByteBegin) +
              ro.text.size() + 64);
  std::optional<PendingResync> pending;
  pending = AppendOriginalSliceWithPending(
      out, tuBytes, 0, witness.sourceByteBegin, std::move(pending));
  out.append(ro.text);
  pending = ro.pending;
  pending = AppendOriginalSliceWithPending(out, tuBytes, witness.sourceByteEnd,
                                           tuBytes.size(), std::move(pending));

  return std::string(out.str());
}

std::optional<std::string> RefoldEngine::TryExpansionClosureFallback() {
  if (!terminalFallbackRequested_)
    return std::nullopt;

  // Try the post-terminal expansion-closure safety net.
  //
  // This path is still proof-driven, but it is no longer allowed to be an
  // independent completeness mechanism. A synthesized result is admissible only
  // if:
  //
  //   1. a declared single-owner macro expansion-closure witness exists,
  //   2. that witness is dominated by the normal MacroWholeCoverRealization
  //      candidate path, and
  //   3. the synthesized source text is replay-stable when re-lexed as
  //      ordinary source.
  //
  // In other words, this stage may materialize a dominated fallback witness,
  // but it must not discover a refolding class that the normal candidate system
  // failed to expose.
  if (auto witness = BuildMacroOwnerExpansionClosureWitness()) {
    // Record the intermediate fallback witness for theorem-audit coverage. This
    // does not restamp the witness as a theorem-facing accepted carrier; it
    // only reports that the fallback stage found a declared witness to inspect.
    ++lastTheoremAudit_.expansionFallbackWitnesses;
    debug("fallback", "expansion-closure witness discovered: {0}",
          FormatExpansionClosureWitness(*witness));

    // The dominance check is the key guardrail: if this witness was not visible
    // through the ordinary macro whole-cover candidate path, then this fallback
    // would be adding a separate completeness mechanism instead of merely
    // materializing an already-dominated proof shape.
    std::string dominanceDetail;
    if (MacroOwnerExpansionClosureWitnessIsDominated(*witness,
                                                     dominanceDetail)) {
      debug("fallback/audit",
            "post-terminal macro expansion closure is dominated: {0}",
            dominanceDetail);
    } else {
      const std::string diagnostic =
          formatv("post-terminal macro expansion closure witness was not "
                  "visible through the normal MacroWholeCoverRealization "
                  "candidate path: witness={0}; reason={1}",
                  FormatExpansionClosureWitness(*witness), dominanceDetail)
              .str();
      if (strict_)
        fatal("fallback/audit", "{0}", diagnostic);
      warn("fallback/audit", "{0}", diagnostic);
    }

    // Even a dominated witness must synthesize into replay-stable source. This
    // prevents emitting expansion text that would rely on original preprocessor
    // state such as hide sets.
    if (auto source = SynthesizeMacroOwnerExpansionClosureSource(*witness)) {
      ++lastTheoremAudit_.expansionFallbackSynthesisSuccesses;
      debug("fallback",
            "expansion-closure source synthesized: rootMacro={0} "
            "source=[{1},{2}) B=[{3},{4}) outLen={5}",
            witness->rootMacroId, witness->sourceByteBegin,
            witness->sourceByteEnd, witness->bTokBegin, witness->bTokEnd,
            source->size());

      // The synthesized output is the original TU with exactly one proved
      // top-level macro owner expanded at its callsite, not a terminal fallback
      // to the edited preprocessed stream.
      ResetAttemptStats();
      lastStats_.expandedMacros = 1;
      lastStats_.terminalFallbackToB = false;
      return source;
    }

    ++lastTheoremAudit_.expansionFallbackSynthesisRejections;
    debug("fallback", "expansion-closure stage: witness could not be "
                      "synthesized into replay-stable source");
  } else {
    debug("fallback", "expansion-closure stage: no proved single-owner macro "
                      "fallback witness available");
  }

  return std::nullopt;
}

std::string RefoldEngine::ResolvePostStructuralFallback() {
  if (!terminalFallbackRequested_)
    return std::string();

  // Once structural emission has failed closed, retain the old
  // expansion-closure safety net before raw-B terminal fallback, but require
  // any discovered witness to pass the dominance audit in
  // TryExpansionClosureFallback().
  if (auto expanded = TryExpansionClosureFallback()) {
    // Count the one case where the proved intermediate expansion stage
    // successfully intercepts a raw-B terminal fallback. This remains coverage
    // only: the synthesized source is not a theorem-facing carrier, but the
    // audit should still say that the terminal request was resolved by the
    // declared intermediate fallback class instead of by raw B.
    ++lastTheoremAudit_.expansionFallbackTerminalRescues;
    return *expanded;
  }

  // Reaching this point means both structural emission and the intermediate
  // proof-backed expansion stage failed closed. Only now do we normalize the
  // run onto the one explicit terminal out-of-domain carrier.
  RecordTerminalFallbackTheoremAudit();

  debug("fallback",
        "terminal fallback: emitting fully expanded edited preprocessed "
        "stream (B). reasons={0}",
        terminalFallbackReasons_.size());
  const TerminalFallbackWitness terminalWitness =
      BuildTerminalFallbackWitness();
  const AcceptedResultCandidate terminalCandidate =
      BuildAcceptedTerminalCandidate(terminalWitness);
  debug("proof/inventory", "terminal result {0}",
        FormatAcceptedResultCandidate(terminalCandidate));
  for (const auto &r : terminalFallbackReasons_)
    debug("fallback", "  {0}", r);

  // The raw-B terminal carrier is the fully expanded surface, so every include
  // and every top-level macro root remains expanded in the final result.
  ResetAttemptStats();
  lastStats_.expandedIncludes = lastStats_.totalIncludes;
  lastStats_.expandedMacros = lastStats_.totalMacros;
  lastStats_.terminalFallbackToB = true;
  return bSource_.str();
}
