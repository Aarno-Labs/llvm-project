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
/// This is intentionally a lexical-trivia predicate, not a preprocessing
/// predicate. Include-closure may carry these bytes through a replacement
/// because comments and whitespace do not contribute preprocessing tokens.
/// Anything that would lex as a real token, including malformed or
/// unterminated comments, remains outside this proof class.
static bool isWsOrCompleteCommentTrivia(StringRef text) {
  size_t i = 0;
  const size_t n = text.size();

  while (i < n) {
    // Ordinary whitespace is inert and may be skipped byte-for-byte.
    if (stringutils::isWs(text[i])) {
      ++i;
      continue;
    }

    // The only non-whitespace spelling admitted here is a complete comment.
    // A lone '/' or any other token spelling is therefore a hard rejection.
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

      // Do not treat a malformed block comment as trivia; fail closed so the
      // caller can choose a wider structural fallback or terminal B.
      if (!closed)
        return false;

      continue;
    }

    if (text[i + 1] == '/') {
      i += 2;

      // A line comment is complete at the physical newline, or at EOF for the
      // isolated gap buffer. The caller preserves the original bytes, so the
      // newline, if present, is retained.
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

/// Parse one admitted conditional-control directive line.
///
/// This recognizes only the small directive grammar that include-closure is
/// allowed to preserve as an inert gap:
///
///   #if 0
///   #if 1
///   #elif 0
///   #elif 1
///   #else
///   #endif
///
/// The `depth` argument tracks balance across the preserved gap. The function
/// rejects escaped physical lines, macro-dependent conditions, trailing tokens,
/// and every directive with side effects. In particular, this is not a general
/// preprocessor directive parser.
static bool parseLiteralEmptyConditionalDirectiveLine(StringRef line,
                                                      unsigned &depth) {
  if (line.ends_with("\n"))
    line = line.drop_back();
  if (line.ends_with("\r"))
    line = line.drop_back();

  // A trailing backslash would splice this physical line with the next one.
  // That makes the gap non-local, so it is outside the preserved-trivia proof.
  StringRef noTrailingHorizontalWs = line.rtrim(" \t\v\f");
  if (noTrailingHorizontalWs.ends_with("\\"))
    return false;

  StringRef rest = line.ltrim(" \t\v\f");
  if (!rest.consume_front("#"))
    return false;
  rest = rest.ltrim(" \t\v\f");

  // Keep directive recognition syntactic and state-free. We only need the
  // directive keyword and the remaining literal condition, not macro expansion.
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

/// Return true iff `text` may be carried through a TU include-closure edit.
///
/// This predicate defines the source-preservation proof for bytes between
/// touched top-level include directives. The gap is accepted only when every
/// byte is either:
///
///   * whitespace/comment trivia, or
///   * part of a complete empty literal conditional-control island.
///
/// Accepted bytes are preserved verbatim in the emitted replacement. They are
/// never silently deleted. Anything that can affect macro state, include state,
/// diagnostics, line state, or later preprocessing remains outside this proof
/// class and forces the caller to fail closed.
static bool isPreservableIncludeClosureGapTrivia(StringRef text) {
  if (isWsOrCompleteCommentTrivia(text))
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
      // Directives are only admitted at physical line start, modulo horizontal
      // whitespace already consumed by the directive parser.
      atLineStart = text[i] == '\n';
      ++i;
      continue;
    }

    if (text[i] == '/') {
      const size_t before = i;
      if (!skipCompleteComment(i))
        return false;

      // A line comment consumes its terminating newline when present, so the
      // next non-whitespace byte is physically at the start of a new line. A
      // block comment may contain newlines too; preserve directive legality by
      // checking the skipped spelling.
      StringRef skipped = text.slice(before, i);
      atLineStart = skipped.ends_with("\n");
      continue;
    }

    // Do not accept a '#' embedded after real spelling on the same line. That
    // would not be a preprocessing directive in the preserved source.
    if (!atLineStart && text[i] != '#')
      return false;

    size_t lineEnd = i;
    while (lineEnd < n && text[lineEnd] != '\n')
      ++lineEnd;
    if (lineEnd < n)
      ++lineEnd;

    // Only balanced, empty, literal conditional-control directives are
    // preservable. All other directives are macro/preprocessor state and must
    // remain outside this include-closure proof.
    if (!parseLiteralEmptyConditionalDirectiveLine(text.slice(i, lineEnd),
                                                   depth)) {
      return false;
    }

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
      if (cand == h)
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

  // The include-only closure case below materializes exactly the include cover.
  // A mixed TU/include hunk is wider: the edit consumes tokens on both sides of
  // the include cover, so the A materialization range must close over the whole
  // touched token interval.
  const uint64_t closureBegin = std::min(h.aStart, coverBegin);
  const uint64_t closureEnd = std::max(h.aEnd, coverEnd);
  const bool mixedTUIncludeClosure = !hunkInsideCover;

  // Defaults for the pure include-closure case.  The mixed case widens both the
  // materialized A token range and the source byte interval below.
  uint64_t materialBeginA = coverBegin;
  uint64_t materialEndA = coverEnd;
  uint64_t sourceBegin = first->siteB;
  uint64_t sourceEnd = last->siteE;

  // Return true iff the PP token is part of one of the include expansions that
  // this closure is explicitly consuming.
  auto tokenCoveredByTouchedInclude = [&](uint64_t pp) -> bool {
    for (const RefoldModel::IncludeItem *inc : touched)
      if (inc->cover.begin <= pp && pp < inc->cover.end)
        return true;
    return false;
  };

  // Return true iff this include directive belongs to the touched include run
  // whose expansion participates in the closure proof.
  auto includeIsTouched = [&](const RefoldModel::IncludeItem &inc) -> bool {
    for (const RefoldModel::IncludeItem *touchedInc : touched)
      if (touchedInc->id == inc.id)
        return true;
    return false;
  };

  // Return true iff the pragma is explicitly classified as source-neutral when
  // its containing include directive is consumed by a zero-token gap closure.
  //
  // Keep this whitelist deliberately small.  Most pragmas model compiler state
  // rather than token material, and the refold map does not currently carry the
  // state-region information needed to replay or repair them.  `#pragma once`
  // is the one admitted spelling because, for an otherwise source-neutral
  // zero-token include, suppressing future textual inclusion of the same
  // zero-token file cannot change the PP token stream.
  auto pragmaIsConsumableIncludeLocalState =
      [&](const RefoldModel::PragmaDirective &pragma) -> bool {
    StringRef text = pragma.text.trim();
    size_t pos = 0;

    auto consumeWord = [&](StringRef word) {
      if (!text.substr(pos).starts_with(word))
        return false;
      pos += word.size();
      if (pos < text.size() && stringutils::isIdentPart(text[pos]))
        return false;
      return true;
    };

    stringutils::skipNonNewlineWs(text, pos);
    if (pos >= text.size() || text[pos] != '#')
      return false;
    ++pos;

    stringutils::skipNonNewlineWs(text, pos);
    if (!consumeWord("pragma"))
      return false;

    stringutils::skipNonNewlineWs(text, pos);
    if (!consumeWord("once"))
      return false;

    stringutils::skipNonNewlineWs(text, pos);
    return pos == text.size();
  };

  // Return true iff the include has structure other than an explicitly
  // consumable pragma.  This is stricter than the general zero-token include
  // proof: `#pragma once` is considered source-neutral only for an otherwise
  // empty include.  If a pragma appears next to macro-state, conditionals,
  // nested includes, declarations, or any other recorded structure, keep the
  // include outside this proof class until the refold map records enough
  // pragma-state information to prove the combined effect.
  auto includeHasNonPragmaStructure =
      [&](const RefoldModel::IncludeItem &inc) -> bool {
    if (!inc.decls.empty())
      return true;

    for (const auto &child : model_.GetIncludes())
      if (child.parent && *child.parent == inc.id)
        return true;

    for (const auto &directive : model_.GetMacroDirectives())
      if (directive.ownerIncludeId && *directive.ownerIncludeId == inc.id)
        return true;

    for (const auto &group : model_.GetConds())
      if (group.parentIncludeId && *group.parentIncludeId == inc.id)
        return true;

    return false;
  };

  auto includePathForDiagnostic = [](const RefoldModel::IncludeItem &inc) {
    return inc.resolvedPath ? *inc.resolvedPath : inc.target;
  };

  // Return the first recorded reason that prevents treating this include as a
  // consumable zero-token source gap, or std::nullopt when the include tree is
  // source-neutral for this closure proof.
  //
  // The predicate is intentionally transitive: a top-level zero-token include
  // may contain nested zero-token includes, and the outer directive is still
  // safe to consume when every descendant is itself source-neutral.  Returning a
  // concrete reason keeps the later terminal-fallback trace actionable instead
  // of merely saying that an unrelated include was absorbed.
  std::function<std::optional<std::string>(const RefoldModel::IncludeItem &,
                                           DenseSet<uint64_t> &)>
      includeRecordedSideEffectReasonImpl;
  includeRecordedSideEffectReasonImpl =
      [&](const RefoldModel::IncludeItem &inc,
          DenseSet<uint64_t> &visiting) -> std::optional<std::string> {
    // A cycle would indicate malformed include-parent metadata.  Do not try to
    // prove closure through it; reject rather than deleting uncertain state.
    if (!visiting.insert(inc.id).second)
      return llvm::formatv("include-parent cycle reaches include id={0}",
                           inc.id)
          .str();

    auto finish = [&](std::optional<std::string> reason) {
      visiting.erase(inc.id);
      return reason;
    };

    // Header declarations are semantic structure, even if this include did not
    // contribute tokens to the particular A-side hunk being refolded.
    if (!inc.decls.empty())
      return finish(llvm::formatv("include id={0} path='{1}' has {2} "
                                  "recorded header declaration(s)",
                                  inc.id, includePathForDiagnostic(inc),
                                  inc.decls.size())
                        .str());

    // A nested include is not inherently a side effect.  It is consumable when
    // its own expansion is empty and its descendants/directives are consumable
    // by the same zero-token include-gap proof.  Record which child blocked the
    // proof so the mixed-closure rejection points at the real source structure.
    for (const auto &child : model_.GetIncludes()) {
      if (!child.parent || *child.parent != inc.id)
        continue;
      if (child.cover.IsValid())
        return finish(llvm::formatv("child include id={0} path='{1}' "
                                    "materializes PP cover=[{2},{3})",
                                    child.id, includePathForDiagnostic(child),
                                    child.cover.begin, child.cover.end)
                          .str());
      if (std::optional<std::string> childReason =
              includeRecordedSideEffectReasonImpl(child, visiting))
        return finish(llvm::formatv("child include id={0} path='{1}' rejected: "
                                    "{2}",
                                    child.id, includePathForDiagnostic(child),
                                    *childReason)
                          .str());
    }

    // Include-owned #define/#undef directives are macro-state transitions, not
    // PP tokens.  When their containing zero-token include lies inside the
    // replacement envelope, the directive is consumed with that envelope.  Any
    // surviving source that still observes the removed macro state is handled
    // later by the macro-state liveness repair passes: consumed #defines can be
    // preserved or replaced with whole-cover callsite realizations, and
    // consumed #undefs can be preserved when removing them would resurrect an
    // earlier definition.  Directives outside that nameable macro-state class
    // remain side-effect-bearing structure that this closure must not erase.
    for (const auto &directive : model_.GetMacroDirectives()) {
      if (directive.ownerIncludeId && *directive.ownerIncludeId == inc.id &&
          directive.subkind != "#define" && directive.subkind != "#undef") {
        return finish(
            llvm::formatv("include id={0} path='{1}' owns "
                          "non-consumable macro directive id={2} "
                          "subkind='{3}' text='{4}'",
                          inc.id, includePathForDiagnostic(inc), directive.id,
                          directive.subkind,
                          stringutils::showWsWithClip(directive.text, 120))
                .str());
      }
    }

    // Conditional control directives are also source structure, but an
    // include-local conditional group with no materialized A tokens is source
    // order only: it contributed no PP material and any real side effects
    // inside the group are represented separately as macro directives, child
    // includes, pragmas, or declarations.  When such a group lies inside the
    // source envelope being replaced, consuming it is the same kind of closure
    // as consuming an empty include gap.  Reject only a conditional group that
    // owns materialized tokens; that would make this a token-producing include,
    // not a zero-token gap.
    for (const auto &group : model_.GetConds()) {
      if (!group.parentIncludeId || *group.parentIncludeId != inc.id)
        continue;

      for (const RefoldModel::CondArm &arm : group.arms) {
        if (arm.span && arm.span->IsValid() &&
            arm.span->begin < arm.span->end) {
          return finish(llvm::formatv("include id={0} path='{1}' owns "
                                      "conditional group id={2} arm id={3} "
                                      "with materialized PP span=[{4},{5})",
                                      inc.id, includePathForDiagnostic(inc),
                                      group.id, arm.id, arm.span->begin,
                                      arm.span->end)
                            .str());
        }
      }
    }

    // Pragmas are keyed by physical file path rather than include id, so check
    // the resolved header path when available.  Unknown pragmas are always
    // side-effect-bearing here: they can affect diagnostics, layout,
    // optimization, visibility, attributes, or later preprocessing state while
    // contributing no PP tokens.  The only pragma admitted by this closure is
    // an explicitly parsed `#pragma once` in an otherwise source-neutral
    // zero-token include.  Anything richer must wait for a pragma-state proof
    // or a repair mechanism analogous to macro-state liveness.
    if (inc.resolvedPath)
      for (const auto &pragma : model_.GetPragmas()) {
        if (!PathsEqual(pragma.sitePath, *inc.resolvedPath))
          continue;
        if (!pragmaIsConsumableIncludeLocalState(pragma)) {
          return finish(
              llvm::formatv("include id={0} path='{1}' contains "
                            "non-consumable pragma id={2} text='{3}'",
                            inc.id, includePathForDiagnostic(inc), pragma.id,
                            stringutils::showWsWithClip(pragma.text, 120))
                  .str());
        }
        if (includeHasNonPragmaStructure(inc)) {
          return finish(llvm::formatv("include id={0} path='{1}' contains "
                                      "#pragma once with additional recorded "
                                      "source structure",
                                      inc.id, includePathForDiagnostic(inc))
                            .str());
        }
      }

    return finish(std::nullopt);
  };

  auto includeRecordedSideEffectReason =
      [&](const RefoldModel::IncludeItem &inc) -> std::optional<std::string> {
    DenseSet<uint64_t> visiting;
    return includeRecordedSideEffectReasonImpl(inc, visiting);
  };

  auto includeHasRecordedSideEffects =
      [&](const RefoldModel::IncludeItem &inc) -> bool {
    return includeRecordedSideEffectReason(inc).has_value();
  };

  struct MacroDefinitionReplacementList {
    const RefoldModel::MacroDirective *directive = nullptr;
    size_t nameTextBegin = 0;
    size_t replacementTextBegin = 0;
    uint64_t fileBase = 0;
    uint64_t fileBegin = 0;
    uint64_t fileEnd = 0;
  };

  auto findMacroDefinitionDirective =
      [&](const RefoldModel::MacroInvocation &m)
          -> const RefoldModel::MacroDirective * {
    if (!m.definitionDirectiveId)
      return nullptr;

    for (const auto &directive : model_.GetMacroDirectives())
      if (directive.id == *m.definitionDirectiveId)
        return &directive;
    return nullptr;
  };

  // Parse the recorded #define text and return the replacement-list range in
  // both directive-text coordinates and source-file byte coordinates.
  //
  // MacroDirective::text contains the full directive spelling, but siteB/siteE
  // anchor only the directive payload in the source file: siteB is the byte of
  // the macro name, not the byte of '#'.  Keep the text offset of the name so
  // nested macro callsites recorded inside a replacement list can be compared
  // against the correct source-file byte interval.
  auto macroDefinitionReplacementList =
      [&](const RefoldModel::MacroInvocation &m)
          -> std::optional<MacroDefinitionReplacementList> {
    const RefoldModel::MacroDirective *definition =
        findMacroDefinitionDirective(m);
    if (!definition || definition->subkind != "#define")
      return std::nullopt;

    StringRef text = definition->text;
    size_t pos = 0;

    // Re-parse only the directive prefix we need: optional horizontal/trivia
    // whitespace, '#', optional whitespace, and the 'define' directive keyword.
    stringutils::skipNonNewlineWs(text, pos);
    if (pos >= text.size() || text[pos] != '#')
      return std::nullopt;
    ++pos;
    stringutils::skipNonNewlineWs(text, pos);

    // Require the exact preprocessing directive keyword, not an identifier that
    // merely starts with "define".
    if (!text.substr(pos).starts_with("define"))
      return std::nullopt;
    pos += StringRef("define").size();
    if (pos < text.size() && stringutils::isIdentPart(text[pos]))
      return std::nullopt;
    stringutils::skipNonNewlineWs(text, pos);

    // The next token must be the macro name recorded on the invocation.  Record
    // its directive-text offset so we can translate later replacement-list
    // offsets back into source-file byte coordinates.
    const size_t nameTextBegin = pos;
    if (!text.substr(pos).starts_with(m.name))
      return std::nullopt;
    pos += m.name.size();

    // Avoid accepting a prefix of a longer macro identifier.
    if (pos < text.size() && stringutils::isIdentPart(text[pos]))
      return std::nullopt;

    if (m.subkind == "func") {
      // Function-like macro definitions require the parameter list to start
      // immediately after the name.  Skip the balanced parameter list; the
      // replacement list begins immediately after the closing paren.
      if (pos >= text.size() || text[pos] != '(')
        return std::nullopt;
      unsigned depth = 0;
      while (pos < text.size()) {
        char ch = text[pos++];
        if (ch == '(') {
          ++depth;
          continue;
        }
        if (ch == ')') {
          if (depth == 0)
            return std::nullopt;
          --depth;
          if (depth == 0)
            break;
        }
      }
      if (depth != 0)
        return std::nullopt;
    }

    // `siteB` is expected to be the source byte for the macro name.  If a map
    // violates that contract, fail closed rather than comparing nested callsite
    // bytes against a synthetic coordinate system.
    if (definition->siteB < nameTextBegin)
      return std::nullopt;

    MacroDefinitionReplacementList result;
    result.directive = definition;
    result.nameTextBegin = nameTextBegin;
    result.replacementTextBegin = pos;
    result.fileBase = definition->siteB - nameTextBegin;
    result.fileBegin = result.fileBase + pos;
    result.fileEnd = definition->siteE;
    if (result.fileBegin > result.fileEnd ||
        result.fileEnd > result.fileBase + text.size())
      return std::nullopt;
    return result;
  };

  // Return true iff this invocation contributed no A-side PP material.  Such a
  // callsite is source structure rather than token material for the current
  // hunk; if the complete invocation lies inside a proved replacement envelope,
  // the envelope may consume it just like a zero-token include gap.
  auto macroInvocationHasMaterializedPPTokens =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    if (m.cover.IsValid())
      return true;
    for (const auto &span : m.stringifySpans)
      if (span.IsValid())
        return true;
    for (const auto &span : m.pasteSpans)
      if (span.IsValid())
        return true;
    return false;
  };

  // Recursively prove that a zero-token macro invocation is source-neutral.
  //
  // The old admissible case was a literal empty-replacement-list macro:
  //
  //   #define EMPTY
  //   EMPTY
  //
  // This additionally admits object-like wrapper macros whose replacement list
  // is tiled by recorded nested zero-token macro invocations, for example:
  //
  //   #define EMPTY
  //   #define WRAP_EMPTY EMPTY
  //   WRAP_EMPTY
  //
  // The proof remains intentionally narrow.  Every non-trivia byte in the
  // wrapper's replacement list must belong to a nested macro invocation
  // recorded in the refold map, and every nested invocation must satisfy this
  // same proof. That avoids treating arbitrary non-empty replacement lists,
  // literals, operators, `_Pragma`, or unrecorded identifiers as source-neutral
  // merely because the final PP cover happens to be empty.
  std::function<bool(const RefoldModel::MacroInvocation &,
                     DenseSet<uint64_t> &)>
      macroInvocationIsSourceNeutralZeroTokenImpl;
  macroInvocationIsSourceNeutralZeroTokenImpl =
      [&](const RefoldModel::MacroInvocation &m,
          DenseSet<uint64_t> &visiting) -> bool {
    // A consumable gap macro must not contribute any final PP-token material.
    // If the invocation produced tokens, it is source-bearing and this proof
    // does not apply.
    if (macroInvocationHasMaterializedPPTokens(m))
      return false;

    // Reject recursive/cyclic macro invocation graphs fail-closed.  The proof
    // is structural recursion over the recorded macro DAG, so revisiting the
    // same invocation would mean the map cannot establish a finite neutral
    // tiling.
    if (!visiting.insert(m.id).second)
      return false;

    auto finish = [&](bool result) {
      visiting.erase(m.id);
      return result;
    };

    // Recover the defining #define and the replacement-list byte range. Without
    // a schema-valid definition anchor, nested invocation byte intervals cannot
    // be compared against the replacement list soundly.
    std::optional<MacroDefinitionReplacementList> replacement =
        macroDefinitionReplacementList(m);
    if (!replacement)
      return finish(false);

    StringRef replacementText =
        replacement->directive->text.substr(replacement->replacementTextBegin);

    // A literal empty replacement list is source-neutral by construction.
    if (isWsOrCompleteCommentTrivia(replacementText))
      return finish(true);

    struct ChildPiece {
      uint64_t begin;
      uint64_t end;
      const RefoldModel::MacroInvocation *child;
    };

    auto sortChildren = [](SmallVectorImpl<ChildPiece> &children) {
      llvm::sort(children, [](const ChildPiece &lhs, const ChildPiece &rhs) {
        if (lhs.begin != rhs.begin)
          return lhs.begin < rhs.begin;
        if (lhs.end != rhs.end)
          return lhs.end < rhs.end;
        return lhs.child->id < rhs.child->id;
      });
    };

    auto sourceRangeIsTiledByNeutralNestedMacros =
        [&](uint64_t begin, uint64_t end, StringRef file) -> bool {
      if (begin > end)
        return false;

      SmallVector<ChildPiece, 8> children;
      for (const auto &candidate : model_.GetMacroInvocations()) {
        if (!candidate.callerMacroId || *candidate.callerMacroId != m.id)
          continue;

        // Only nested callsites physically spelled inside this source range can
        // tile it.  Malformed or out-of-file candidates are harmless here: if
        // they were needed to cover non-trivia bytes, the later trivia-gap check
        // will reject the range fail-closed.
        if (!candidate.invFile || candidate.invFile->empty() ||
            !PathsEqual(*candidate.invFile, file))
          continue;
        if (!candidate.invB || !candidate.invE ||
            *candidate.invB >= *candidate.invE)
          continue;
        if (*candidate.invB < begin || end < *candidate.invE)
          continue;

        children.push_back({*candidate.invB, *candidate.invE, &candidate});
      }

      sortChildren(children);

      uint64_t cursor = begin;
      for (const ChildPiece &piece : children) {
        if (piece.begin < cursor)
          return false;

        StringRef gap;
        if (PathsEqual(file, replacement->directive->sitePath)) {
          const size_t gapBegin =
              static_cast<size_t>(cursor - replacement->fileBase);
          const size_t gapEnd =
              static_cast<size_t>(piece.begin - replacement->fileBase);
          gap = replacement->directive->text.slice(gapBegin, gapEnd);
        } else if (PathsEqual(file, tuPath)) {
          gap = tuBytes.slice(cursor, piece.begin);
        } else {
          return false;
        }

        if (!isWsOrCompleteCommentTrivia(gap))
          return false;

        if (!macroInvocationIsSourceNeutralZeroTokenImpl(*piece.child, visiting))
          return false;

        cursor = piece.end;
      }

      StringRef suffix;
      if (PathsEqual(file, replacement->directive->sitePath)) {
        const size_t suffixBegin =
            static_cast<size_t>(cursor - replacement->fileBase);
        const size_t suffixEnd = static_cast<size_t>(end - replacement->fileBase);
        suffix = replacement->directive->text.slice(suffixBegin, suffixEnd);
      } else if (PathsEqual(file, tuPath)) {
        suffix = tuBytes.slice(cursor, end);
      } else {
        return false;
      }

      return isWsOrCompleteCommentTrivia(suffix);
    };

    // Function-like zero-token wrappers are admissible only in a narrow
    // forwarding domain.  The replacement list may mention formals, and after
    // substitution each used argument must itself be tiled by recorded
    // zero-token macro invocations plus trivia.  This admits FORWARD(EMPTY)
    // without trying to model arbitrary macro semantics.
    if (m.subkind == "func") {
      if (m.defParams.empty() || m.invArgRanges.size() < m.defParams.size())
        return finish(false);

      DenseMap<StringRef, unsigned> paramIndexByName;
      for (unsigned i = 0, e = m.defParams.size(); i != e; ++i) {
        if (m.defParams[i].variadic)
          return finish(false);
        paramIndexByName.try_emplace(m.defParams[i].name, i);
      }

      SmallVector<unsigned, 4> usedParams;
      StringRef text = replacement->directive->text;
      size_t pos = replacement->replacementTextBegin;
      while (pos < text.size()) {
        if (stringutils::isWs(text[pos])) {
          ++pos;
          continue;
        }

        StringRef tail = text.substr(pos);
        if (tail.starts_with("/*")) {
          const size_t end = text.find("*/", pos + 2);
          if (end == StringRef::npos)
            return finish(false);
          pos = end + 2;
          continue;
        }
        if (tail.starts_with("//")) {
          const size_t end = text.find('\n', pos + 2);
          pos = end == StringRef::npos ? text.size() : end + 1;
          continue;
        }

        // The only non-trivia replacement-list tokens admitted here are formal
        // parameter names.  Operators, literals, _Pragma, #, ##, punctuation,
        // and unrecorded identifiers remain outside this proof class.
        if (!stringutils::isIdentStart(text[pos]))
          return finish(false);
        const size_t identBegin = pos++;
        while (pos < text.size() && stringutils::isIdentPart(text[pos]))
          ++pos;
        StringRef ident = text.slice(identBegin, pos);
        auto it = paramIndexByName.find(ident);
        if (it == paramIndexByName.end())
          return finish(false);
        usedParams.push_back(it->second);
      }

      if (usedParams.empty())
        return finish(false);

      llvm::sort(usedParams);
      usedParams.erase(std::unique(usedParams.begin(), usedParams.end()),
                       usedParams.end());

      for (unsigned argIdx : usedParams) {
        const auto &range = m.invArgRanges[argIdx];
        if (!range.first || !range.second || *range.first > *range.second)
          return finish(false);
        if (!m.invFile || m.invFile->empty())
          return finish(false);
        if (!sourceRangeIsTiledByNeutralNestedMacros(*range.first,
                                                     *range.second, *m.invFile))
          return finish(false);
      }

      return finish(true);
    }

    if (m.subkind != "obj")
      return finish(false);

    // Collect the nested macro invocations that the producer recorded inside
    // this macro's replacement list.  These are the only non-trivia source
    // bytes a non-empty object-like wrapper is allowed to contain.
    SmallVector<ChildPiece, 8> children;
    for (const auto &candidate : model_.GetMacroInvocations()) {
      if (!candidate.callerMacroId || *candidate.callerMacroId != m.id)
        continue;

      // Nested invocations must be physically spelled in the defining directive
      // for this macro.  Otherwise they cannot tile this replacement list.
      if (!candidate.invFile || candidate.invFile->empty() ||
          !PathsEqual(*candidate.invFile, replacement->directive->sitePath))
        return finish(false);

      // Require a concrete, non-empty source callsite interval for the child.
      // Zero-token refers to the produced PP coverage, not to the source
      // spelling of the nested macro name.
      if (!candidate.invB || !candidate.invE ||
          *candidate.invB >= *candidate.invE)
        return finish(false);

      // The child callsite must lie wholly inside the replacement-list source
      // interval.  A partial overlap would mean the wrapper contains source
      // bytes that this proof has not accounted for.
      if (*candidate.invB < replacement->fileBegin ||
          replacement->fileEnd < *candidate.invE)
        return finish(false);

      children.push_back({*candidate.invB, *candidate.invE, &candidate});
    }

    // A non-empty object-like wrapper with no recorded children contains
    // unaccounted source text, so it is not source-neutral under this proof.
    if (children.empty())
      return finish(false);

    sortChildren(children);

    uint64_t cursor = replacement->fileBegin;
    for (const ChildPiece &piece : children) {
      // Overlapping or out-of-order child intervals would make the tiling
      // ambiguous.  Reject rather than trying to normalize it heuristically.
      if (piece.begin < cursor)
        return finish(false);

      // Every byte before the next child invocation must be pure trivia.  Any
      // identifier, literal, operator, or _Pragma-like spelling here would be
      // source-bearing replacement-list material.
      const size_t gapBegin =
          static_cast<size_t>(cursor - replacement->fileBase);
      const size_t gapEnd =
          static_cast<size_t>(piece.begin - replacement->fileBase);
      StringRef gap = replacement->directive->text.slice(gapBegin, gapEnd);
      if (!isWsOrCompleteCommentTrivia(gap))
        return finish(false);

      // The child itself must satisfy the same zero-token source-neutral proof.
      // This is what admits WRAP_EMPTY -> EMPTY, while still rejecting wrappers
      // that eventually expand to real replacement-list text.
      if (!macroInvocationIsSourceNeutralZeroTokenImpl(*piece.child, visiting))
        return finish(false);

      cursor = piece.end;
    }

    // After the final child, the replacement-list suffix must also be trivia.
    // This completes the tiling proof for the wrapper's entire replacement
    // list.
    const size_t suffixBegin =
        static_cast<size_t>(cursor - replacement->fileBase);
    StringRef suffix = replacement->directive->text.substr(suffixBegin);
    if (!isWsOrCompleteCommentTrivia(suffix))
      return finish(false);

    return finish(true);
  };

  auto macroInvocationIsSourceNeutralZeroToken =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    DenseSet<uint64_t> visiting;
    return macroInvocationIsSourceNeutralZeroTokenImpl(m, visiting);
  };

  // Return true iff the macro invocation is a complete zero-token TU callsite
  // inside the candidate replacement envelope.  Partial overlaps remain
  // non-refoldable here because deleting only part of a macro call would not be
  // a proved source closure.
  auto macroInvocationIsConsumableZeroTokenGap =
      [&](const RefoldModel::MacroInvocation &m, uint64_t begin,
          uint64_t end) -> bool {
    if (m.invFile && !m.invFile->empty() && !PathsEqual(*m.invFile, tuPath))
      return false;
    if (!m.invB || !m.invE || *m.invB >= *m.invE)
      return false;
    if (*m.invB < begin || end < *m.invE)
      return false;
    return macroInvocationIsSourceNeutralZeroToken(m);
  };

  struct MacroDirectiveSourceInterval {
    uint64_t begin = 0;
    uint64_t end = 0;
  };

  // Recover the full physical source interval for a TU-spelled macro-state
  // directive.  MacroDirective::siteB is anchored at the macro name rather than
  // at '#', so use the recorded directive spelling to translate the name offset
  // back to the directive's real source begin.
  auto macroDirectiveFullSourceInterval =
      [&](const RefoldModel::MacroDirective &directive)
          -> std::optional<MacroDirectiveSourceInterval> {
    if (!PathsEqual(directive.sitePath, tuPath) || directive.ownerIncludeId)
      return std::nullopt;
    if (directive.subkind != "#define" && directive.subkind != "#undef")
      return std::nullopt;

    StringRef text = directive.text;
    if (text.empty())
      return std::nullopt;

    size_t pos = 0;
    stringutils::skipNonNewlineWs(text, pos);
    if (pos >= text.size() || text[pos] != '#')
      return std::nullopt;
    ++pos;
    stringutils::skipNonNewlineWs(text, pos);

    // Match the recorded directive kind exactly; accepting an identifier prefix
    // here would make the later byte-coordinate reconstruction unsound.
    StringRef keyword = directive.subkind.drop_front();
    if (!text.substr(pos).starts_with(keyword))
      return std::nullopt;
    pos += keyword.size();
    if (pos < text.size() && stringutils::isIdentPart(text[pos]))
      return std::nullopt;
    stringutils::skipNonNewlineWs(text, pos);

    // The next token is the macro name.  Its text offset is the bridge between
    // directive-text coordinates and source-file byte coordinates because the
    // producer records siteB at this token.
    const size_t nameTextBegin = pos;
    if (pos >= text.size() || !stringutils::isIdentStart(text[pos]))
      return std::nullopt;
    ++pos;
    while (pos < text.size() && stringutils::isIdentPart(text[pos]))
      ++pos;

    if (directive.siteB < nameTextBegin)
      return std::nullopt;

    const uint64_t fileBegin = directive.siteB - nameTextBegin;
    const uint64_t fileEnd = fileBegin + text.size();
    if (fileBegin >= fileEnd || fileEnd > tuBytes.size())
      return std::nullopt;

    // Require an exact byte match so this proof consumes the directive that the
    // producer recorded, not a synthetic interval inferred from stale anchors.
    if (tuBytes.slice(fileBegin, fileEnd) != text)
      return std::nullopt;

    return MacroDirectiveSourceInterval{fileBegin, fileEnd};
  };

  auto macroDirectiveIsConsumableStateGap =
      [&](const RefoldModel::MacroDirective &directive, uint64_t begin,
          uint64_t end) -> std::optional<MacroDirectiveSourceInterval> {
    std::optional<MacroDirectiveSourceInterval> interval =
        macroDirectiveFullSourceInterval(directive);
    if (!interval)
      return std::nullopt;
    if (interval->begin < begin || end < interval->end)
      return std::nullopt;
    return interval;
  };

  // Return a TU-spelled pragma that is wholly contained in the physical source
  // line being scanned, if the refold map recorded one there.
  //
  // Unlike #define/#undef, pragmas are deliberately not treated as consumable
  // source-neutral artifacts here.  A pragma may mutate compiler state without
  // producing PP tokens, and the refold map currently records only its spelling
  // and source anchors, not the state region needed to prove deletion safe.
  // This helper is therefore diagnostic-only: it lets the rejection log name the
  // exact pragma that blocked the closure instead of reporting an opaque
  // non-trivia gap.
  auto findTUPragmaOnSourceLine = [&](uint64_t lineBegin, uint64_t lineEnd)
      -> const RefoldModel::PragmaDirective * {
    for (const auto &pragma : model_.GetPragmas()) {
      if (!PathsEqual(pragma.sitePath, tuPath))
        continue;
      if (lineBegin <= pragma.siteB && pragma.siteE <= lineEnd &&
          pragma.siteB < pragma.siteE)
        return &pragma;
    }
    return nullptr;
  };

  // Return a concrete diagnostic when a TU gap contains a recorded pragma that
  // blocks closure.  This is checked at the outer rejection site as well as in
  // the line scanner, because a pragma-only gap has no consumable recorded
  // pieces; without this fast path the scanner is never entered and the user
  // only sees a generic "non-trivia gap" rejection.
  auto nonConsumableTUPragmaGapReason = [&](uint64_t gapBegin,
                                            uint64_t gapEnd)
      -> std::optional<std::string> {
    for (const auto &pragma : model_.GetPragmas()) {
      if (!PathsEqual(pragma.sitePath, tuPath))
        continue;
      if (gapBegin <= pragma.siteB && pragma.siteE <= gapEnd &&
          pragma.siteB < pragma.siteE) {
        return llvm::formatv("source gap [{0},{1}) contains non-consumable "
                             "TU pragma id={2} site=[{3},{4}) text='{5}'",
                             gapBegin, gapEnd, pragma.id, pragma.siteB,
                             pragma.siteE,
                             stringutils::showWsWithClip(pragma.text, 120))
            .str();
      }
    }
    return std::nullopt;
  };

  // Prove that a source gap inside the replacement envelope consists only of
  // lexical trivia plus complete source-neutral artifacts: zero-token include
  // trees, source-neutral zero-token macro invocation trees, and TU-spelled
  // macro-state directives whose later liveness obligations are handled by the
  // macro-state repair pass.
  //
  // These artifacts have no PP tokens, so token overlap alone cannot make them
  // part of the touched hunk.  If they lie physically between required source
  // pieces of a proved closure, however, they are inside the source interval
  // being replaced.  We therefore consume them with the closure; preserving
  // them before or after the B-side material would move source structure across
  // the edit boundary and would describe a different source-order program.
  auto gapIsConsumableZeroTokenSourceClosure = [&](uint64_t gapBegin,
                                                  uint64_t gapEnd) -> bool {
    if (gapBegin >= gapEnd || gapEnd > tuBytes.size())
      return false;

    struct GapPiece {
      uint64_t begin;
      uint64_t end;
      uint64_t id;
      StringRef kind;
    };

    SmallVector<GapPiece, 8> pieces;
    for (const auto &inc : model_.GetIncludes()) {
      if (!PathsEqual(inc.sitePath, tuPath) || includeIsTouched(inc))
        continue;
      if (inc.cover.IsValid() || includeHasRecordedSideEffects(inc))
        continue;
      if (gapBegin <= inc.siteB && inc.siteE <= gapEnd &&
          inc.siteB < inc.siteE)
        pieces.push_back({inc.siteB, inc.siteE, inc.id, "include"});
    }

    for (const auto &m : model_.GetMacroInvocations())
      if (macroInvocationIsConsumableZeroTokenGap(m, gapBegin, gapEnd))
        pieces.push_back({*m.invB, *m.invE, m.id, "macro"});

    for (const auto &directive : model_.GetMacroDirectives())
      if (std::optional<MacroDirectiveSourceInterval> interval =
              macroDirectiveIsConsumableStateGap(directive, gapBegin, gapEnd))
        pieces.push_back({interval->begin, interval->end, directive.id,
                          "macro-directive"});

    if (pieces.empty())
      return false;

    llvm::sort(pieces, [](const GapPiece &lhs, const GapPiece &rhs) {
      if (lhs.begin != rhs.begin)
        return lhs.begin < rhs.begin;
      // Put the widest piece first for a shared begin so an outer macro
      // invocation shadows any nested macro calls spelled inside its argument
      // list.  The outer macro proof is responsible for recursively proving
      // those nested calls source-neutral.
      if (lhs.end != rhs.end)
        return lhs.end > rhs.end;
      if (lhs.kind != rhs.kind)
        return lhs.kind < rhs.kind;
      return lhs.id < rhs.id;
    });

    SmallVector<GapPiece, 8> outerPieces;
    for (const GapPiece &piece : pieces) {
      if (outerPieces.empty() || piece.begin >= outerPieces.back().end) {
        outerPieces.push_back(piece);
        continue;
      }

      const GapPiece &outer = outerPieces.back();
      if (outer.begin <= piece.begin && piece.end <= outer.end &&
          outer.kind == "macro" && piece.kind == "macro") {
        // A function-like zero-token macro such as FORWARD(EMPTY) records both
        // the outer callsite and the nested EMPTY callsite in the same TU gap.
        // Once the outer macro has been proved source-neutral, the nested
        // callsite is part of that proof, not a second independent gap piece.
        continue;
      }

      // Any other overlap would make the source tiling ambiguous: includes,
      // directives, and unrelated macro calls must remain disjoint at this
      // level of the closure proof.
      return false;
    }
    pieces = std::move(outerPieces);

    uint64_t cursor = gapBegin;
    uint64_t includeCount = 0;
    uint64_t macroCount = 0;
    uint64_t macroDirectiveCount = 0;
    uint64_t conditionalCount = 0;
    unsigned conditionalDepth = 0;

    // Scan source bytes that are not covered by recorded zero-token artifacts.
    // These interstitial bytes may contain ordinary lexical trivia and literal
    // conditional-control directives.  The conditional depth is carried across
    // consumed artifacts, which is what admits shapes such as:
    //
    //   #if 1
    //   #define GAP_VALUE 99
    //   #endif
    //
    // when the #define line is itself a proved consumable macro-state artifact.
    bool atLineStart = stringutils::beginsLineAfterWs(tuBytes, gapBegin);
    auto scanNeutralControlTrivia = [&](uint64_t limit) -> bool {
      while (cursor < limit) {
        char ch = tuBytes[cursor];
        if (stringutils::isWs(ch)) {
          atLineStart = ch == '\n' || ch == '\r';
          ++cursor;
          continue;
        }

        if (ch == '/') {
          const uint64_t before = cursor;
          StringRef rest = tuBytes.drop_front(cursor);
          if (rest.starts_with("/*")) {
            cursor += 2;
            bool closed = false;
            while (cursor + 1 < limit) {
              if (tuBytes[cursor] == '*' && tuBytes[cursor + 1] == '/') {
                cursor += 2;
                closed = true;
                break;
              }
              ++cursor;
            }
            if (!closed)
              return false;
            StringRef skipped = tuBytes.slice(before, cursor);
            atLineStart = skipped.ends_with("\n") || skipped.ends_with("\r");
            continue;
          }

          if (rest.starts_with("//")) {
            cursor += 2;
            while (cursor < limit && tuBytes[cursor] != '\n' &&
                   tuBytes[cursor] != '\r')
              ++cursor;

            // A line comment may end at the end of the whole gap, but it may
            // not run into a following recorded artifact.  In that case the
            // artifact's spelling would be inside the comment, contradicting the
            // proof that it is an independently recorded source artifact.
            if (cursor >= limit)
              return limit == gapEnd;

            ++cursor;
            atLineStart = true;
            continue;
          }
        }

        // Literal conditional-control directives are source-neutral only at a
        // physical directive boundary.  Any other directive or token spelling is
        // real source that this closure has not proved safe to erase.
        if (ch != '#' || !atLineStart)
          return false;

        uint64_t lineEnd = cursor;
        while (lineEnd < limit && tuBytes[lineEnd] != '\n')
          ++lineEnd;
        if (lineEnd < limit)
          ++lineEnd;

        if (!parseLiteralEmptyConditionalDirectiveLine(
                tuBytes.slice(cursor, lineEnd), conditionalDepth)) {
          if (const RefoldModel::PragmaDirective *pragma =
                  findTUPragmaOnSourceLine(cursor, lineEnd)) {
            trace("fallback",
                  "TU/include closure rejected: source gap [{0},{1}) contains "
                  "non-consumable TU pragma id={2} site=[{3},{4}) text='{5}'",
                  gapBegin, gapEnd, pragma->id, pragma->siteB, pragma->siteE,
                  stringutils::showWsWithClip(pragma->text, 120));
          }
          return false;
        }

        ++conditionalCount;
        cursor = lineEnd;
        atLineStart = true;
      }
      return cursor == limit;
    };

    for (const GapPiece &piece : pieces) {
      if (piece.begin < cursor)
        return false;
      if (!scanNeutralControlTrivia(piece.begin))
        return false;

      // The artifact itself has already been independently proved zero-token and
      // complete.  Consume it as one opaque source-neutral piece, then continue
      // scanning the surrounding control/trivia bytes at the same conditional
      // depth.
      StringRef pieceBytes = tuBytes.slice(piece.begin, piece.end);
      cursor = piece.end;
      atLineStart = pieceBytes.ends_with("\n") || pieceBytes.ends_with("\r");
      if (piece.kind == "include")
        ++includeCount;
      else if (piece.kind == "macro")
        ++macroCount;
      else
        ++macroDirectiveCount;
    }

    if (!scanNeutralControlTrivia(gapEnd) || conditionalDepth != 0)
      return false;

    trace("fallback",
          "TU/include closure consuming zero-token source gap "
          "source=[{0},{1}) includes={2} macros={3} macroDirectives={4} "
          "conditionalDirectives={5}",
          gapBegin, gapEnd, includeCount, macroCount, macroDirectiveCount,
          conditionalCount);
    return true;
  };

  // Prove a mixed TU/include closure.
  //
  // This handles a single replacement hunk whose A-side tokens are split
  // between ordinary TU spelling and the complete expansion of a contiguous
  // top-level include run, for example:
  //
  //   int arr[] = { 1,
  //   #include "two.inc"
  //   };
  //
  // rewritten from preprocessed tokens:
  //
  //   { 1, 2 }  ->  { 3 }
  //
  // Replacing only the TU-owned bytes for "1," is unsound: the include
  // directive would remain in the source and replay its old "2" token.  The
  // only source realization we accept here is therefore one TU byte interval
  // that spans all TU tokens consumed by the hunk and all touched include
  // directive sites.
  if (mixedTUIncludeClosure) {
    materialBeginA = closureBegin;
    materialEndA = closureEnd;

    // Collect the source intervals that must be consumed by the closure:
    //   * every touched include directive site, and
    //   * every TU-owned token in the widened A token interval.
    //
    // Later checks prove these pieces merge into one safe TU replacement span
    // with only whitespace between pieces and no unrelated artifacts absorbed.
    SmallVector<std::pair<uint64_t, uint64_t>, 16> sourcePieces;
    sourcePieces.reserve(touched.size() + (closureEnd - closureBegin));
    for (const RefoldModel::IncludeItem *inc : touched)
      sourcePieces.push_back({inc->siteB, inc->siteE});

    bool sawTUToken = false;
    const auto &tokmapByPP = model_.GetTokmapByPP();
    for (uint64_t pp = closureBegin; pp < closureEnd; ++pp) {
      auto it = tokmapByPP.find(pp);
      if (it == tokmapByPP.end())
        continue;

      const auto &entry = it->second;
      if (PathsEqual(entry.file, tuPath)) {
        sourcePieces.push_back({entry.b, entry.e});
        sawTUToken = true;
        continue;
      }

      // Any non-TU token in the closure must be explained by one of the touched
      // include covers.  Otherwise this is not a TU/include closure; it would
      // be a mixed-owner edit involving some third artifact that this proof
      // does not know how to realize soundly.
      if (!tokenCoveredByTouchedInclude(pp)) {
        trace("fallback",
              "TU/include closure rejected: A token {0} maps to file={1} "
              "outside the touched include run A=[{2},{3})",
              pp, entry.file, coverBegin, coverEnd);
        return std::nullopt;
      }
    }

    // If the consumed A-token material came from inside a TU conditional arm,
    // consume the complete conditional group as a required source piece.  The
    // byte-level gap proof can already consume neutral conditional-control
    // islands whose bodies produce no tokens, but it cannot balance an opening
    // #if in one inter-piece gap against a closing #endif after a real source
    // piece.  A selected arm whose complete PP span is inside the replacement
    // material is different: its directive wrapper is part of the same source
    // interval whose produced tokens are being replaced, so the whole group must
    // be considered a required piece of the closure rather than trivia around
    // neighboring token pieces.
    for (const auto &group : model_.GetConds()) {
      if (!PathsEqual(group.file, tuPath) || group.parentIncludeId)
        continue;

      bool consumesSelectedArm = false;
      for (const RefoldModel::CondArm &arm : group.arms) {
        if (!arm.selected || !arm.span || !arm.span->IsValid() ||
            arm.span->begin >= arm.span->end)
          continue;
        if (materialBeginA <= arm.span->begin &&
            arm.span->end <= materialEndA) {
          consumesSelectedArm = true;
          break;
        }
      }

      if (!consumesSelectedArm)
        continue;
      if (group.groupB >= group.groupE || group.groupE > tuBytes.size()) {
        trace("fallback",
              "TU/include closure rejected: invalid consumed conditional "
              "group id={0} source=[{1},{2}) fileLen={3}",
              group.id, group.groupB, group.groupE, tuBytes.size());
        return std::nullopt;
      }

      sourcePieces.push_back({group.groupB, group.groupE});
    }

    // The mixed proof is only for hunks that actually consume ordinary TU
    // spelling.  If no TU token was seen, the pure include-closure path is the
    // appropriate proof shape instead.
    if (!sawTUToken) {
      trace("fallback",
            "TU/include closure rejected: mixed closure A=[{0},{1}) has no "
            "TU-owned source token",
            closureBegin, closureEnd);
      return std::nullopt;
    }

    // Normalize the collected source pieces so the rest of the proof can reason
    // about the single candidate replacement envelope.
    llvm::sort(sourcePieces, [](const auto &lhs, const auto &rhs) {
      if (lhs.first != rhs.first)
        return lhs.first < rhs.first;
      return lhs.second < rhs.second;
    });

    SmallVector<std::pair<uint64_t, uint64_t>, 16> mergedPieces;
    for (const auto &piece : sourcePieces) {
      if (piece.first >= piece.second || piece.second > tuBytes.size()) {
        trace("fallback",
              "TU/include closure rejected: invalid source piece [{0},{1}) "
              "fileLen={2}",
              piece.first, piece.second, tuBytes.size());
        return std::nullopt;
      }
      if (mergedPieces.empty() || piece.first > mergedPieces.back().second) {
        mergedPieces.push_back(piece);
        continue;
      }
      mergedPieces.back().second =
          std::max(mergedPieces.back().second, piece.second);
    }

    // The replacement will cover the whole byte envelope from the first
    // required source piece through the last.  Subsequent checks prove that
    // widening this far does not silently consume meaningful unrelated source.
    sourceBegin = mergedPieces.front().first;
    sourceEnd = mergedPieces.back().second;

    // Do not silently delete any include directive other than the run whose
    // expansion is explicitly part of this closure.  A complete unrelated
    // include with an empty PP cover may be consumed only when it is fully
    // inside the same source envelope and all recorded structure is either
    // absent or limited to source-neutral conditional control, nested
    // zero-token includes, and consumable macro-state directives.  That makes
    // it part of the replaced source interval, not trivia to preserve or
    // reorder around the B-side material.
    for (const auto &inc : model_.GetIncludes()) {
      if (!PathsEqual(inc.sitePath, tuPath))
        continue;
      if (inc.siteB >= sourceEnd || sourceBegin >= inc.siteE)
        continue;
      if (includeIsTouched(inc))
        continue;
      std::optional<std::string> sideEffectReason =
          includeRecordedSideEffectReason(inc);
      if (!inc.cover.IsValid() && !sideEffectReason &&
          sourceBegin <= inc.siteB && inc.siteE <= sourceEnd)
        continue;

      if (sideEffectReason) {
        trace("fallback",
              "TU/include closure rejected: source interval [{0},{1}) would "
              "absorb unrelated include id={2} site=[{3},{4}) cover=[{5},{6}): "
              "{7}",
              sourceBegin, sourceEnd, inc.id, inc.siteB, inc.siteE,
              inc.cover.begin, inc.cover.end, *sideEffectReason);
      } else {
        trace("fallback",
              "TU/include closure rejected: source interval [{0},{1}) would "
              "absorb unrelated include id={2} site=[{3},{4}) cover=[{5},{6})",
              sourceBegin, sourceEnd, inc.id, inc.siteB, inc.siteE,
              inc.cover.begin, inc.cover.end);
      }
      return std::nullopt;
    }

    auto ppSpanInsideMaterial = [&](const RefoldModel::PPSpan &span) -> bool {
      if (!span.IsValid() || span.begin >= span.end)
        return true;
      return materialBeginA <= span.begin && span.end <= materialEndA;
    };

    auto macroInvocationMaterialIsConsumed =
        [&](const RefoldModel::MacroInvocation &m) -> bool {
      if (!m.invB || !m.invE || *m.invB < sourceBegin || sourceEnd < *m.invE)
        return false;

      bool sawMaterializedSpan = false;
      auto checkSpan = [&](const RefoldModel::PPSpan &span) -> bool {
        if (!span.IsValid() || span.begin >= span.end)
          return true;
        sawMaterializedSpan = true;
        return ppSpanInsideMaterial(span);
      };

      // A source-bearing macro call may be consumed by this replacement only
      // when every PP span the producer attributes to that invocation is wholly
      // inside the A-side material being replaced.  Then the callsite is not an
      // unrelated artifact: it is one of the source pieces whose produced tokens
      // are removed by the hunk.  Any span escaping the material range means
      // deleting the call would also delete surviving PP tokens, so reject.
      for (const auto &span : m.spans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.bodySpans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.argSpans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.stringifySpans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.pasteSpans)
        if (!checkSpan(span))
          return false;

      return sawMaterializedSpan;
    };

    // Do not delete or partially overwrite unrelated macro invocations.  Two
    // complete-callsite cases are allowed inside the replacement envelope:
    //
    //   * zero-token source-neutral macro gaps, which contributed no PP tokens;
    //   * source-bearing macro calls whose entire produced PP material is
    //     already inside this hunk's A-side replacement range.
    //
    // The latter case is the source-bearing analogue of the zero-token gap: the
    // macro invocation is not being preserved or moved, it is part of the source
    // interval whose materialized tokens the user replaced.
    for (const auto &m : model_.GetMacroInvocations()) {
      if (m.invFile && !m.invFile->empty() && !PathsEqual(*m.invFile, tuPath))
        continue;
      if (!m.invB || !m.invE)
        continue;
      if (*m.invB < sourceEnd && sourceBegin < *m.invE) {
        if (macroInvocationIsConsumableZeroTokenGap(m, sourceBegin,
                                                   sourceEnd) ||
            macroInvocationMaterialIsConsumed(m))
          continue;
        trace("fallback",
              "TU/include closure rejected: source interval [{0},{1}) would "
              "absorb macro invocation id={2} name='{3}' site=[{4},{5})",
              sourceBegin, sourceEnd, m.id, m.name, *m.invB, *m.invE);
        return std::nullopt;
      }
    }

    // The envelope may widen across physical gaps between required pieces, but
    // only across source-neutral control/trivia or complete zero-token source
    // artifacts.  Complete comments and literal empty conditional-control
    // islands are consumed with the replacement for the same reason as
    // whitespace: they contribute no PP tokens and are physically inside the
    // source interval whose A-side material was replaced by the B-side hunk.
    // Any other byte would be meaningful source text that the proof has not
    // justified replacing.
    uint64_t cursor = sourceBegin;
    for (const auto &piece : mergedPieces) {
      if (piece.first > cursor) {
        StringRef gap = tuBytes.slice(cursor, piece.first);
        if (!isPreservableIncludeClosureGapTrivia(gap) &&
            !gapIsConsumableZeroTokenSourceClosure(cursor, piece.first)) {
          if (std::optional<std::string> pragmaReason =
                  nonConsumableTUPragmaGapReason(cursor, piece.first)) {
            trace("fallback", "TU/include closure rejected: {0}",
                  *pragmaReason);
          } else {
            trace("fallback",
                  "TU/include closure rejected: non-trivia TU gap inside "
                  "mixed source interval [{0},{1}) gap='{2}'",
                  cursor, piece.first,
                  stringutils::showWsWithClip(gap, 120));
          }
          return std::nullopt;
        }
      }
      cursor = piece.second;
    }

    trace("fallback",
          "TU/include closure source interval accepted: source=[{0},{1}) "
          "A=[{2},{3}) includeCover=[{4},{5})",
          sourceBegin, sourceEnd, materialBeginA, materialEndA, coverBegin,
          coverEnd);
  }

  if (!exactCover) {
    if (!hunkInsideCover && !mixedTUIncludeClosure) {
      trace("fallback",
            "TU include-closure rejected: touched include cover A=[{0},{1}) "
            "does not contain hunk A=[{2},{3})",
            coverBegin, coverEnd, h.aStart, h.aEnd);
      return std::nullopt;
    }

    // Widening from the unresolved hunk to the complete closure A-range is
    // admissible only when the widened-away prefix/suffix are observationally
    // unchanged in the token diff. That keeps this as a source-closure proof,
    // not a hidden multi-region merge: any independent token edit or insertion
    // in the extra cover must be handled by the normal partition/lattice path.
    const bool prefixClean = !rangeHasForeignTokenDiff(materialBeginA, h.aStart);
    const bool suffixClean = !rangeHasForeignTokenDiff(h.aEnd, materialEndA);
    if (!prefixClean || !suffixClean) {
      trace("fallback",
            "TU include-closure rejected: widening A=[{0},{1}) around hunk "
            "A=[{2},{3}) would absorb foreign token edits prefixClean={4} "
            "suffixClean={5}",
            coverBegin, coverEnd, h.aStart, h.aEnd, prefixClean ? 1 : 0,
            suffixClean ? 1 : 0);
      return std::nullopt;
    }

    if (sourceTouchesStagedEdit(sourceBegin, sourceEnd)) {
      trace("fallback",
            "TU include-closure rejected: widened source interval [{0},{1}) "
            "would absorb an already-staged source edit",
            sourceBegin, sourceEnd);
      return std::nullopt;
    }

    trace("fallback",
          "TU include-closure widening accepted: hunk A=[{0},{1}) widened to "
          "closure A=[{2},{3}) source=[{4},{5})",
          h.aStart, h.aEnd, materialBeginA, materialEndA, sourceBegin,
          sourceEnd);
  }

  if (sourceBegin >= sourceEnd || sourceEnd > tuBytes.size()) {
    trace("fallback",
          "TU include-closure rejected: invalid TU source interval [{0},{1}) "
          "fileLen={2}",
          sourceBegin, sourceEnd, tuBytes.size());
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

    // The pure include closure may absorb touched include directives plus
    // source-neutral whitespace between neighboring directives.  It may also
    // consume complete zero-token source artifacts, such as empty include trees
    // and invocations of empty-replacement-list macros, when they lie between
    // touched directives.  Those artifacts are inside the replaced source
    // envelope even though they
    // contributed no PP tokens.  Comments outside such complete zero-token gaps
    // are still preserved verbatim rather than silently deleted.
    if (inc->siteB > sourceCursor) {
      StringRef gap = tuBytes.slice(sourceCursor, inc->siteB);
      if (!gap.empty() && !stringutils::isWs(gap)) {
        if (gapIsConsumableZeroTokenSourceClosure(sourceCursor, inc->siteB)) {
          trace("fallback",
                "TU include-closure consuming zero-token source gap before "
                "inc#{0} gap='{1}'",
                inc->id, stringutils::showWsWithClip(gap, 120));
        } else if (!isPreservableIncludeClosureGapTrivia(gap)) {
          trace("fallback",
                "TU include-closure rejected: non-trivia TU gap between "
                "include directives before inc#{0} gap='{1}'",
                inc->id, stringutils::showWsWithClip(gap, 120));
          return std::nullopt;
        } else {
          preservedGapTrivia.append(gap.begin(), gap.end());
          trace("fallback",
                "TU include-closure preserving inert trivia gap before "
                "inc#{0} gap='{1}'",
                inc->id, stringutils::showWsWithClip(gap, 120));
        }
      }
    }

    sourceCursor = inc->siteE;
  }

  IncludeRealizationEvidenceKind evidenceKind =
      IncludeRealizationEvidenceKind::Unknown;
  auto bEnvelope = ResolveIncludeRealizationBTokenEnvelope(materialBeginA,
                                                           materialEndA,
                                                           &evidenceKind);
  if (!bEnvelope && materialBeginA == h.aStart && materialEndA == h.aEnd) {
    bEnvelope = std::make_pair(static_cast<size_t>(h.bStart),
                               static_cast<size_t>(h.bEnd));
    evidenceKind = IncludeRealizationEvidenceKind::CanonicalBCoverEnvelope;
    trace("fallback",
          "TU/include closure using exact diff hunk B envelope: "
          "A=[{0},{1}) B=[{2},{3})",
          materialBeginA, materialEndA, h.bStart, h.bEnd);
  }
  if (!bEnvelope) {
    trace("fallback",
          "TU include-closure rejected: no canonical-or-consensus B envelope "
          "for A-cover [{0},{1})",
          materialBeginA, materialEndA);
    return std::nullopt;
  }

  auto bMaterialBegin = bEnvelope->first;
  auto bMaterialEnd = bEnvelope->second;

  // Pure include-closure projection can include stable boundary tokens that are
  // outside the include cover, such as the `{` before an include run inside an
  // initializer. The source edit starts at the first include directive, so such
  // outside anchors must not be materialized or they would be duplicated in the
  // TU. A mixed TU/include closure consumes those TU-side boundary tokens as
  // part of the same source interval, so it keeps the full closure envelope.
  if (!mixedTUIncludeClosure) {
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
  }

  if (h.bStart < bMaterialBegin || h.bEnd > bMaterialEnd) {
    trace("fallback",
          "TU include-closure rejected: hunk B=[{0},{1}) is outside trimmed "
          "closure B material [{2},{3}) from envelope [{4},{5})",
          h.bStart, h.bEnd, bMaterialBegin, bMaterialEnd, bEnvelope->first,
          bEnvelope->second);
    return std::nullopt;
  }

  std::string rawReplacement = SliceBSource(bMaterialBegin, bMaterialEnd).str();
  std::string replacement = rawReplacement;

  // Preserve legacy inert trivia between touched include directives.  Empty
  // includes admitted by the gap proof above are deliberately not appended here:
  // they lie inside the source envelope replaced by this closure, so the B-side
  // material consumes them along with the adjacent TU/include tokens.
  if (!preservedGapTrivia.empty()) {
    if (startsWithPreprocessorDirectiveTrivia(preservedGapTrivia) &&
        !replacement.empty() && replacement.back() != '\n')
      replacement.push_back('\n');
    replacement.append(preservedGapTrivia.begin(), preservedGapTrivia.end());
  }

  // The closure begins at the proved source interval.  For a pure include
  // closure that is the include-directive run; for a mixed TU/include closure it
  // also includes the directly consumed TU tokens adjacent to that run.  We may
  // later extend the right edge over immediately following untouched TU line(s)
  // so any required physical-line compensation lands after copied suffix lines
  // such as `};` instead of inside the realized replacement itself.
  uint64_t closureSourceEnd = sourceEnd;
  std::string padded = PadAtBoundaries(
      tuBytes, static_cast<size_t>(sourceBegin),
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
      stringutils::countNewlines(tuBytes, sourceBegin, closureSourceEnd);
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
                lineBegin, lineEnd, stringutils::showWsWithClip(line, 120),
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
              sourceBegin, closureSourceEnd, remainingPadNl);
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
              sourceBegin, closureSourceEnd, remainingPadNl);
      }
      remainingPadNl = 0;
    }

    if (remainingPadNl > 0)
      padded.append(remainingPadNl, '\n');

    trace("fallback",
          "TU include-closure newline pad: source=[{0},{1}) origNl={2} "
          "replNl={3} physicalPadNl={4} finalSourceEnd={5} result='{6}'",
          sourceBegin, sourceEnd, origNl, initialReplNl, remainingPadNl,
          closureSourceEnd, stringutils::showWsWithClip(padded, 120));
  }

  debug("fallback",
        "TU include-closure accepted: includes=[{0},{1}] source=[{2},{3}) "
        "A=[{4},{5}) B=[{6},{7}) evidence={8} raw='{9}' padded='{10}'",
        first->id, last->id, sourceBegin, closureSourceEnd, materialBeginA,
        materialEndA, bEnvelope->first, bEnvelope->second, evidenceKind,
        stringutils::showWsWithClip(rawReplacement, 120),
        stringutils::showWsWithClip(padded, 120));

  ResyncOutcome ro = ApplyResyncOrPend(tuBytes, sourceBegin, closureSourceEnd,
                                       padded, tuPath);
  TextEdit edit{sourceBegin,           closureSourceEnd, std::move(ro.text),
                std::move(ro.pending), std::nullopt,     {}};

  // The emitted artifact is still a deterministic TU byte replacement even
  // though the witness came from an exact include-closure proof. Reuse the TU
  // textual-edit carrier until a dedicated closure-edit proof class exists.
  AttachAcceptedResultCarrier(
      edit, BuildAcceptedTUTextEditCandidate(
                AcceptedPathKind::TUByteSpanConservativeEdit, sourceBegin,
                closureSourceEnd, StringRef(rawReplacement)));
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
            stringutils::showWsWithClip(spelling, 80));
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
