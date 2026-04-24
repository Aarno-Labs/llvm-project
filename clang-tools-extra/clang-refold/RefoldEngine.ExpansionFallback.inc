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
    trace("fallback",
          "macro-owner closure witness rejected: terminal fallback not requested");
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

  // Step 2 deliberately proves only a whole-owner closure. A candidate root
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

  // Step 5 adds explicit interval-level guards instead of relying on the
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
      const RefoldModel::MacroInvocation *parent = FindMacroInvocationById(*cur);
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
            "macro-owner closure root rejected: id={0} name='{1}' has no TU segments for source interval",
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
              "macro-owner closure root rejected: id={0} name='{1}' source interval [{2},{3}) has uncovered gap before segment [{4},{5})",
              root.id, root.name, *root.invB, *root.invE, seg.b, seg.e);
        return false;
      }
      if (seg.ownerIncludeId) {
        trace("fallback",
              "macro-owner closure root rejected: id={0} name='{1}' source interval [{2},{3}) crosses include-owned segment [{4},{5}) includeId={6}",
              root.id, root.name, *root.invB, *root.invE, seg.b, seg.e,
              *seg.ownerIncludeId);
        return false;
      }
      if (!sawCondArmId) {
        condArmId = seg.ownerCondArmId;
        sawCondArmId = true;
      } else if (condArmId != seg.ownerCondArmId) {
        trace("fallback",
              "macro-owner closure root rejected: id={0} name='{1}' source interval [{2},{3}) crosses conditional-arm boundary between {4} and {5}",
              root.id, root.name, *root.invB, *root.invE,
              condArmId.value_or(0), seg.ownerCondArmId.value_or(0));
        return false;
      }
      sawCoveringSeg = true;
      cur = std::max<uint64_t>(cur, seg.e);
      if (cur >= *root.invE)
        return true;
    }

    if (!sawCoveringSeg) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' source interval [{2},{3}) has no covering TU segment",
            root.id, root.name, *root.invB, *root.invE);
      return false;
    }

    trace("fallback",
          "macro-owner closure root rejected: id={0} name='{1}' source interval [{2},{3}) is not fully covered by TU-owned segments",
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
            "macro-owner closure root rejected: id={0} name='{1}' overlaps foreign macro id={2} name='{3}' sourceOverlap={4} coverOverlap={5} otherOwnerInclude={6}",
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
    // out-of-domain for this step.
    if (m.callerMacroId)
      continue;

    // The witness has to name an exact source callsite interval. If callsite
    // provenance is incomplete, we cannot soundly materialize the owner later.
    if (!m.invText || !m.invFile || !m.invB || !m.invE) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' missing callsite provenance",
            m.id, m.name);
      continue;
    }

    if (!PathsEqual(*m.invFile, tuPath)) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' callsite file='{2}' not TU='{3}'",
            m.id, m.name, StringRef(*m.invFile), tuPath);
      continue;
    }

    if (m.ownerIncludeId) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' belongs to include #{2}",
            m.id, m.name, *m.ownerIncludeId);
      continue;
    }

    if (*m.invE <= *m.invB) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' empty/invalid source range [{2},{3})",
            m.id, m.name, *m.invB, *m.invE);
      continue;
    }

    if (IsInvocationInsideDefineDirective(m)) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' invocation inside define directive",
            m.id, m.name);
      continue;
    }

    if (!InvocationSpanMatchesCallsitePrefix(*m.invText, m)) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' invocation text does not match callsite prefix",
            m.id, m.name);
      continue;
    }

    // Reuse the existing whole-cover computation as the closure/frontier
    // witness. Step 2 only accepts roots whose existing plan already proves a
    // self-contained, unclipped edited B-surface.
    auto plan = ComputeWholeCoverPlan(m);
    if (!plan) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' has no whole-cover plan",
            m.id, m.name);
      continue;
    }

    if (!plan->selfContained || plan->claimsClipped ||
        plan->bTokEnd <= plan->bTokStart) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' selfContained={2} claimsClipped={3} B=[{4},{5})",
            m.id, m.name, plan->selfContained ? 1 : 0,
            plan->claimsClipped ? 1 : 0, plan->bTokStart, plan->bTokEnd);
      continue;
    }

    if (!sourceIntervalIsSelfContainedTUOwned(m))
      continue;

    // Step 5 makes several previously implicit exclusions explicit. Reject any
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

      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' does not close hunk A=[{2},{3}) B=[{4},{5}) coverA=[{6},{7}) coverB=[{8},{9})",
            m.id, m.name, h.aStart, h.aEnd, h.bStart, h.bEnd, plan->covLoA,
            plan->covHiA, plan->bTokStart, plan->bTokEnd);
      closesAllHunks = false;
      break;
    }

    if (!closesAllHunks)
      continue;

    trace("fallback",
          "macro-owner closure root accepted: id={0} name='{1}' source=[{2},{3}) coverA=[{4},{5}) B=[{6},{7})",
          m.id, m.name, *m.invB, *m.invE, plan->covLoA, plan->covHiA,
          plan->bTokStart, plan->bTokEnd);
    candidates.push_back(MacroOwnerClosureCandidate{&m, *plan});
  }

  if (candidates.empty()) {
    trace("fallback",
          "macro-owner closure witness rejected: no single-owner root candidate survived");
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
  // later synthesis. Step 2 does not attempt to materialize text yet.
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

std::optional<RefoldEngine::TextEdit>
RefoldEngine::BuildTUIncludeClosureEditForUnresolvedHunk(
    const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes) const {
  // This helper is intentionally narrower than ordinary include realization.
  // It is the first hybrid source-closure class used to keep already-proved
  // structural artifacts (for example a preserved macro callsite rewrite)
  // while materializing one otherwise-unresolved PP region directly into the
  // TU source. Stay fail-closed unless the closure is exact and self-contained.
  if (h.isInsertOnly()) {
    trace("fallback",
          "TU include-closure rejected: insert-only hunk A=[{0},{1}) B=[{2},{3}) is outside the first closure-edit domain",
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
          "TU include-closure rejected: no top-level TU include touches hunk A=[{0},{1})",
          h.aStart, h.aEnd);
    return std::nullopt;
  }

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

  for (const RefoldModel::IncludeItem *inc : touched) {
    if (inc->cover.begin != coverEnd || inc->cover.end <= inc->cover.begin) {
      trace("fallback",
            "TU include-closure rejected: touched includes do not form one contiguous A-cover at inc#{0} cover=[{1},{2}) expectedBegin={3}",
            inc->id, inc->cover.begin, inc->cover.end, coverEnd);
      return std::nullopt;
    }
    coverEnd = inc->cover.end;
  }

  // Keep the first hybrid closure exact: do not silently widen beyond the
  // unresolved PP hunk. If the touched include run would over-cover the hunk,
  // a larger search lattice is needed and remains out-of-domain for now.
  if (coverBegin != h.aStart || coverEnd != h.aEnd) {
    trace("fallback",
          "TU include-closure rejected: touched include cover A=[{0},{1}) does not exactly match hunk A=[{2},{3})",
          coverBegin, coverEnd, h.aStart, h.aEnd);
    return std::nullopt;
  }

  if (first->siteB >= first->siteE || last->siteE > tuBytes.size() ||
      first->siteB >= last->siteE) {
    trace("fallback",
          "TU include-closure rejected: invalid TU source interval [{0},{1}) fileLen={2}",
          first->siteB, last->siteE, tuBytes.size());
    return std::nullopt;
  }

  uint64_t sourceCursor = first->siteB;
  for (const RefoldModel::IncludeItem *inc : touched) {
    if (inc->siteB < sourceCursor || inc->siteE > tuBytes.size() ||
        inc->siteB >= inc->siteE) {
      trace("fallback",
            "TU include-closure rejected: invalid include site for inc#{0} site=[{1},{2}) cursor={3} fileLen={4}",
            inc->id, inc->siteB, inc->siteE, sourceCursor, tuBytes.size());
      return std::nullopt;
    }

    // The TU closure may absorb only the include directives themselves plus
    // whitespace between neighboring directives. Comments or other spelled TU
    // text would require a richer source-frontier proof.
    if (inc->siteB > sourceCursor) {
      StringRef gap = tuBytes.slice(sourceCursor, inc->siteB);
      if (!gap.empty() && !stringutils::isWhitespace(gap)) {
        trace("fallback",
              "TU include-closure rejected: non-whitespace TU gap between include directives before inc#{0} gap='{1}'",
              inc->id, stringutils::showWSWithClip(gap, 120));
        return std::nullopt;
      }
    }

    sourceCursor = inc->siteE;
  }

  IncludeRealizationEvidenceKind evidenceKind =
      IncludeRealizationEvidenceKind::Unknown;
  auto bEnvelope =
      ResolveIncludeRealizationBTokenEnvelope(coverBegin, coverEnd,
                                             &evidenceKind);
  if (!bEnvelope) {
    trace("fallback",
          "TU include-closure rejected: no canonical-or-consensus B envelope for A-cover [{0},{1})",
          coverBegin, coverEnd);
    return std::nullopt;
  }

  std::string replacement =
      SliceBSource(bEnvelope->first, bEnvelope->second).str();
  std::string rawReplacement = replacement;
  std::string padded = PadAtBoundaries(
      tuBytes, static_cast<size_t>(first->siteB), static_cast<size_t>(last->siteE),
      std::move(replacement), /*allowLeft=*/true, /*allowRight=*/true);

  debug("fallback",
        "TU include-closure accepted: includes=[{0},{1}] source=[{2},{3}) A=[{4},{5}) B=[{6},{7}) evidence={8} raw='{9}' padded='{10}'",
        first->id, last->id, first->siteB, last->siteE, coverBegin, coverEnd,
        bEnvelope->first, bEnvelope->second,
        FormatIncludeRealizationEvidenceKind(evidenceKind),
        stringutils::showWSWithClip(rawReplacement, 120),
        stringutils::showWSWithClip(padded, 120));

  ResyncOutcome ro =
      ApplyResyncOrPend(tuBytes, first->siteB, last->siteE, padded, tuPath);
  TextEdit edit{first->siteB, last->siteE, std::move(ro.text),
                std::move(ro.pending), std::nullopt, {}};

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

  // Step 3 keeps source synthesis narrower than the Step 2 witness. The first
  // emitted fallback class must be replay-stable when re-lexed as ordinary
  // source text, without relying on preprocessor hide sets. Reject any raw
  // identifier token for now: direct source replay of identifiers would need a
  // stronger proof that rescanning cannot trigger further macro expansion.
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
  auto mb = llvm::MemoryBuffer::getMemBufferCopy(buffer,
                                                 "<expansion-fallback>");
  FileID fid = sm.createFileID(std::move(mb));

  const LangOptions lang = MakeLexLangOptions(model_.GetPPLang());
  Lexer lex(fid, sm.getBufferOrFake(fid), sm, lang);
  lex.SetKeepWhitespaceMode(false);

  const StringRef fileBytes = sm.getBufferOrFake(fid).getBuffer();
  Token token;
  while (true) {
    lex.LexFromRawLexer(token);
    if (token.is(clang::tok::eof))
      break;

    if (token.is(clang::tok::raw_identifier) || token.is(clang::tok::hash) ||
        token.is(clang::tok::hashhash)) {
      const char *ptr = sm.getCharacterData(token.getLocation());
      const size_t off = static_cast<size_t>(ptr - fileBytes.data());
      const StringRef spelling(ptr, token.getLength());
      trace("fallback",
            "macro-owner fallback synthesis rejected: replay-unstable token kind={0} off={1} spelling='{2}'",
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
  if (witness.kind != ExpansionClosureKind::MacroOwnerWholeExpansion ||
      !witness.hasRootMacroId || witness.hasOwnerIncludeId) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: witness not in single-root macro domain");
    return std::nullopt;
  }

  if (!witness.selfContained || witness.claimsClipped ||
      witness.sourceByteEnd <= witness.sourceByteBegin ||
      witness.bTokEnd <= witness.bTokBegin) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: invalid witness bounds or clipped/selfContained flags source=[{0},{1}) B=[{2},{3}) selfContained={4} claimsClipped={5}",
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

  const std::string absTuPath = lineDirs_.ToAbsolutePath(tuPath);
  auto tuBufferOrErr = llvm::MemoryBuffer::getFile(absTuPath);
  if (!tuBufferOrErr) {
    debug("fallback",
          "macro-owner fallback synthesis rejected: failed to read TU source '{0}' ({1})",
          absTuPath, tuBufferOrErr.getError().message());
    return std::nullopt;
  }
  const StringRef tuBytes = (*tuBufferOrErr)->getBuffer();

  if (witness.sourceByteEnd > tuBytes.size()) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: source interval [{0},{1}) exceeds TU size={2}",
          witness.sourceByteBegin, witness.sourceByteEnd, tuBytes.size());
    return std::nullopt;
  }

  // Materialize the witness's full proved B-frontier. Unlike structural
  // whole-cover patches, this fallback synthesis is a single closed owner
  // splice, so it must not inherit the standalone-claim clipping used when B
  // insertions are being emitted through separate structural edits.
  std::string replacement = SliceBSource(witness.bTokBegin, witness.bTokEnd).str();
  if (replacement.empty()) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: materialization for B=[{0},{1}) is empty",
          witness.bTokBegin, witness.bTokEnd);
    return std::nullopt;
  }

  if (!ExpansionFallbackReplacementIsReplayStable(replacement))
    return std::nullopt;

  // Re-embed the materialized owner into the TU using the same boundary
  // spacing and line-drift machinery as ordinary TU byte edits. This keeps the
  // first synthesis class deterministic and avoids inventing a second emission
  // path with subtly different token-adjacency or #line behavior.
  std::string padded =
      PadAtBoundaries(tuBytes, static_cast<size_t>(witness.sourceByteBegin),
                      static_cast<size_t>(witness.sourceByteEnd),
                      std::move(replacement), /*allowLeft=*/true,
                      /*allowRight=*/true);
  ResyncOutcome ro = ApplyResyncOrPend(tuBytes, witness.sourceByteBegin,
                                       witness.sourceByteEnd, padded, tuPath);

  SmallString<0> out;
  out.reserve(tuBytes.size() -
              (witness.sourceByteEnd - witness.sourceByteBegin) +
              ro.text.size() + 64);
  std::optional<PendingResync> pending;
  pending = AppendOriginalSliceWithPending(out, tuBytes, 0,
                                           witness.sourceByteBegin,
                                           std::move(pending));
  out.append(ro.text);
  pending = ro.pending;
  pending = AppendOriginalSliceWithPending(out, tuBytes, witness.sourceByteEnd,
                                           tuBytes.size(),
                                           std::move(pending));

  return std::string(out.str());
}

std::optional<std::string> RefoldEngine::TryExpansionClosureFallback() {
  if (!terminalFallbackRequested_)
    return std::nullopt;

  // This intermediate stage remains proof-driven: build a declared witness
  // first, then synthesize source only if the witness survives the narrower
  // Step 3 replay-stability checks.
  if (auto witness = BuildMacroOwnerExpansionClosureWitness()) {
    // Step 6 keeps the new intermediate fallback class visible in the theorem
    // audit without restamping it as a theorem-facing carrier. These counters
    // are coverage-only: they report whether the run found a declared witness
    // and whether synthesis stayed inside the Step 3 replay-stable subset.
    ++lastTheoremAudit_.expansionFallbackWitnesses;
    debug("fallback", "expansion-closure witness discovered: {0}",
          FormatExpansionClosureWitness(*witness));

    if (auto source = SynthesizeMacroOwnerExpansionClosureSource(*witness)) {
      ++lastTheoremAudit_.expansionFallbackSynthesisSuccesses;
      debug("fallback",
            "expansion-closure source synthesized: rootMacro={0} source=[{1},{2}) B=[{3},{4}) outLen={5}",
            witness->rootMacroId, witness->sourceByteBegin,
            witness->sourceByteEnd, witness->bTokBegin, witness->bTokEnd,
            source->size());

      // The first synthesized fallback output is the original TU with exactly
      // one proved top-level macro owner expanded at its callsite.
      ResetAttemptStats();
      lastStats_.expandedMacros = 1;
      lastStats_.terminalFallbackToB = false;
      return source;
    }

    ++lastTheoremAudit_.expansionFallbackSynthesisRejections;
    debug("fallback",
          "expansion-closure stage: witness could not be synthesized into replay-stable source");
  } else {
    debug("fallback",
          "expansion-closure stage: no proved single-owner macro fallback witness available");
  }

  return std::nullopt;
}

std::string RefoldEngine::ResolvePostStructuralFallback() {
  if (!terminalFallbackRequested_)
    return std::string();

  // Step 4 makes the intermediate expansion-fallback stage authoritative. Once
  // structural emission has failed closed, the engine must try the proved
  // expansion closure before it records or emits the raw-B terminal carrier.
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
  const TerminalFallbackWitness terminalWitness = BuildTerminalFallbackWitness();
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
