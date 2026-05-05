//===--- RefoldEngine.TailUtilities.inc -------------------------*- C++ -*-===//
//
// `RefoldEngine` member definitions physically split out of
// `RefoldEngine.cpp` to reduce the size of the main implementation file.
//
// This fragment contains the late-file utility members that support the
// refolding pipeline but do not define a separate proof or rewrite stage of
// their own. In practice, this file is intended for:
//
//   - trailing helper members used by the main refolding flow
//   - formatting / rendering helpers
//   - small normalization / adaptation routines
//   - other low-level support members that were previously grouped near the
//     end of `RefoldEngine.cpp`
//
// This is a physical extraction only:
//   - the methods remain `RefoldEngine` members
//   - the file is included directly into `RefoldEngine.cpp`
//   - private access, translation-unit scope, and behavior are unchanged
//
// The purpose of this fragment is organizational only: keep incidental
// support code out of the main engine file while preserving the exact
// implementation structure and semantics.
//
//===----------------------------------------------------------------------===//

bool RefoldEngine::InvocationSpanMatchesCallsitePrefix(
    StringRef invSpanText, const RefoldModel::MacroInvocation &m) const {
  if (invSpanText.empty() || m.name.empty())
    return false;

  const size_t n = invSpanText.size();
  size_t i = 0;

  // Skip leading whitespace.
  while (i < n && stringutils::isWs(invSpanText[i]))
    i++;

  if (i >= n)
    return false;

  // Verify identifier start.
  const char c0 = invSpanText[i];
  if (!stringutils::isIdentStart(c0))
    return false;

  // Find end of identifier.
  size_t j = i + 1;
  while (j < n && stringutils::isIdentPart(invSpanText[j]))
    j++;

  // Check if the identifier matches the macro name.
  StringRef ident = invSpanText.slice(i, j);
  if (ident != m.name)
    return false;

  // Object-like macro: identifier match at span start is sufficient.
  if (m.subkind != "func")
    return true;

  // Function-like macro: must be followed by '(' (allowing whitespace).
  while (j < n && stringutils::isWs(invSpanText[j]))
    j++;

  return (j < n && invSpanText[j] == '(');
}

uint64_t RefoldEngine::GetRootMacroId(uint64_t macroId) const {
  const RefoldModel::MacroInvocation *cur = FindMacroInvocationById(macroId);
  if (!cur)
    return macroId;

  while (cur->callerMacroId) {
    const RefoldModel::MacroInvocation *parent =
        FindMacroInvocationById(*cur->callerMacroId);
    if (!parent)
      break;
    cur = parent;
  }
  return cur->id;
}

std::optional<std::string> RefoldEngine::BuildInlineIncludeRealizationFromB(
    const RefoldModel::IncludeItem &inc, StringRef reason,
    AcceptedResultCandidate *acceptedCandidate) const {
  // Step 4A makes the include-realization boundary explicit. Inline
  // realization is in-domain only when the include cover yields either the
  // canonical A-cover -> B-envelope mapping or one deterministic
  // consensus-rescued B envelope. If neither witness exists, the engine stays
  // in the explicit out-of-domain terminal state instead of manufacturing a
  // weaker include-realization proof.
  IncludeRealizationEvidenceKind evidenceKind =
      IncludeRealizationEvidenceKind::Unknown;
  auto bEnvOpt = ResolveIncludeRealizationBTokenEnvelope(
      inc.cover.begin, inc.cover.end, &evidenceKind);
  if (!bEnvOpt) {
    RequestTerminalFallback(
        TerminalFallbackKind::IncludeRealizationUnmappableBCoverEnvelope,
        "include/mat",
        llvm::formatv("include realization from B failed to resolve a "
                      "canonical-or-consensus-resolvable B envelope for "
                      "A cover [{0},{1}) for inc#{2}; reason={3}",
                      inc.cover.begin, inc.cover.end, inc.id, reason)
            .str());
    return std::nullopt;
  }

  IncludeRealizationWitness realizationWitness;
  realizationWitness.evidence = evidenceKind;
  realizationWitness.hasIncludeId = true;
  realizationWitness.includeId = inc.id;
  realizationWitness.hasACover = true;
  realizationWitness.aCoverBegin = inc.cover.begin;
  realizationWitness.aCoverEnd = inc.cover.end;
  realizationWitness.hasBTokenEnvelope = true;
  realizationWitness.bTokBegin = bEnvOpt->first;
  realizationWitness.bTokEnd = bEnvOpt->second;

  AcceptedResultCandidate realizationCandidate =
      BuildAcceptedIncludeRealizationCandidate(
          AcceptedPathKind::IncludeRealizationInlineFromB, inc,
          &realizationWitness);
  debug("include/mat",
        "realize from B inc#{0} reason='{1}' bTok=[{2},{3}) candidate={4}",
        inc.id, reason, bEnvOpt->first, bEnvOpt->second,
        FormatAcceptedResultCandidate(realizationCandidate));
  if (acceptedCandidate)
    *acceptedCandidate = realizationCandidate;
  return SliceBSource(bEnvOpt->first, bEnvOpt->second).str();
}

// ============ Include processing (normalize, materialize, apply) =============

void RefoldEngine::MaterializeIncludeExpansion(
    uint64_t includeId, const DenseMap<uint64_t, IncludeEdits> &perInclude,
    const DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
        &macroPatchesByOwner,
    const DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>>
        &children,
    DenseMap<uint64_t, std::string> &includeExpansion,
    DenseMap<uint64_t, AcceptedResultCandidate>
        &includeExpansionAcceptedResults,
    DenseSet<uint64_t> *appliedExpandedMacroRootIds) const {
  // Already materialized?
  if (includeExpansion.count(includeId)) {
    debug("include/mat", "SKIP inc#{0} (already materialized)", includeId);
    return;
  }

  const RefoldModel::IncludeItem *inc = model_.GetIncludeById(includeId);
  if (!inc)
    fatal("include/mat", "unknown includeId {0}", includeId);

  debug("include/mat",
        "ENTER inc#{0} target={1} resolved={2} sitePath={3} site=[{4},{5}) "
        "cover=[{6},{7})",
        inc->id, inc->target, inc->resolvedPath, inc->sitePath, inc->siteB,
        inc->siteE, inc->cover.begin, inc->cover.end);

  const std::string headerPath = resolveHeaderPath(*inc);

  // Start from the raw header text that was preloaded into includeExpansion.
  // If it wasn’t preseeded for some reason, load deterministically by path.
  std::string bytes;
  if (auto it = includeExpansion.find(includeId); it != includeExpansion.end())
    bytes = it->second;
  if (bytes.empty()) {
    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(headerPath));
    if (!bufOrErr) {
      fatal("include/mat", "failed to read header: {0} ({1})", headerPath,
            bufOrErr.getError().message());
    }

    // Treat as raw bytes; copy into std::string
    const MemoryBuffer &mb = **bufOrErr;
    bytes.assign(mb.getBufferStart(), mb.getBufferEnd());
  } else {
    debug("include/mat", "inc#{0} using preseeded bytes len={1}", inc->id,
          bytes.size());
  }

  auto editsIt = perInclude.find(includeId);
  auto macroIt = macroPatchesByOwner.find(includeId);
  auto childIt = children.find(includeId);

  debug(
      "include/mat",
      "inc#{0} initialHeaderLen={1} patches={2} macroPatches={3} children={4}",
      inc->id, bytes.size(),
      (editsIt != perInclude.end()) ? editsIt->second.patches.size() : 0,
      (macroIt != macroPatchesByOwner.end()) ? macroIt->second.size() : 0,
      (childIt != children.end()) ? childIt->second.size() : 0);

  // Collect byte-level edits to apply within this header.
  std::vector<TextEdit> edits;

  // 1) Macro-patch edits owned by this include (invocation byte ranges already
  // in the owner’s file space).
  if (auto it = macroPatchesByOwner.find(includeId);
      it != macroPatchesByOwner.end()) {
    for (const auto &mp : it->second) {
      uint64_t mpEnd =
          extendChainedCallEnd(StringRef(bytes), mp.invEnd, mp.replacement);
      if (mpEnd != mp.invEnd) {
        debug("macro/chain",
              "inc#{0} extend chained callsite [{1},{2}) -> [{1},{3})", inc->id,
              mp.invStart, mp.invEnd, mpEnd);
      }
      debug("include/mat", "inc#{0} macroPatch inv=[{1},{2}) replLen={3}",
            inc->id, mp.invStart, mpEnd, mp.replacement.size());
      ResyncOutcome ro = ApplyResyncOrPend(bytes, mp.invStart, mpEnd,
                                           mp.replacement, headerPath);
      TextEdit edit{mp.invStart, mpEnd, std::move(ro.text),
                    std::move(ro.pending),
                    MacroPatchRemainsExpanded(mp)
                        ? std::make_optional(GetRootMacroId(mp.macroId))
                        : std::nullopt,
                    {}};
      AttachAcceptedResultCarrier(edit, BuildAcceptedEmittedMacroCandidate(mp));
      edits.push_back(std::move(edit));
    }
  }

  // 2) A/B include insert/delete/replace patches that belong to this include.
  if (auto it = perInclude.find(includeId); it != perInclude.end()) {
    if (!it->second.patches.empty()) {
      debug("include/mat", "inc#{0} adding {1} include patches as TextEdits",
            inc->id, it->second.patches.size());
      IncludeTextEditPlan plan = ComputeIncludeTextEdits(it->second, bytes);
      if (plan.requiresIncludeRealization) {
        AcceptedResultCandidate realizationCandidate;
        if (auto realized = BuildInlineIncludeRealizationFromB(
                *inc, plan.realizationReason, &realizationCandidate)) {
          includeExpansion[includeId] = std::move(*realized);
          includeExpansionAcceptedResults[includeId] =
              std::move(realizationCandidate);
          return;
        }
        includeExpansion[includeId] = std::string();
        return;
      }
      for (auto &te : plan.edits) {
        ResyncOutcome ro =
            ApplyResyncOrPend(bytes, te.start, te.end, te.text, headerPath);
        TextEdit edit{te.start, te.end, std::move(ro.text),
                      std::move(ro.pending), std::nullopt, {}};
        edit.acceptedResults = std::move(te.acceptedResults);
        edits.push_back(std::move(edit));
      }
    } else {
      debug("include/mat", "inc#{0} has no include patches", inc->id);
    }
  } else {
    debug("include/mat", "inc#{0} has no include patches", inc->id);
  }

  auto hasDescendantWork = [&](auto &&self, uint64_t id) -> bool {
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
            inc->id, child->id, child->target, child->resolvedPath,
            child->siteB, child->siteE, todo ? "YES" : "NO");
      if (!todo) {
        continue; // leave untouched: keep the original directive as-is
      }

      // Ensure the child is materialized first (depth-first).
      MaterializeIncludeExpansion(child->id, perInclude, macroPatchesByOwner,
                                  children, includeExpansion,
                                  includeExpansionAcceptedResults,
                                  appliedExpandedMacroRootIds);

      // The child directive's site is recorded in the includer byte space.
      const auto &childText = includeExpansion[child->id];
      const size_t n = bytes.size();
      const uint64_t siteStart = std::clamp<uint64_t>(child->siteB, 0ULL, n);
      const uint64_t siteEnd = std::clamp<uint64_t>(child->siteE, siteStart, n);

      debug("include/mat",
            "REPLACE in inc#{0}: site=[{1},{2}) len(parent)={3} with child#{4} "
            "len(childText)={5}",
            inc->id, siteStart, siteEnd, bytes.size(), child->id,
            childText.size());

      debug("tu/replace",
            "TU replace site=[{0},{1}) with inc#{2} len={3} (target={4} "
            "resolved={5})",
            siteStart, siteEnd, child->id, childText.size(), child->target,
            child->resolvedPath);

      if (siteStart < siteEnd) {
        std::string childHeaderPath = resolveHeaderPath(*child);
        std::string wrapped = lineDirs_.WrapIncludeExpansion(
            childHeaderPath, headerPath,
            stringutils::lineAtOffset(bytes, child->siteE), childText);
        TextEdit edit{siteStart, siteEnd, std::move(wrapped), std::nullopt,
                      std::nullopt, {}};
        auto itAccepted = includeExpansionAcceptedResults.find(child->id);
        if (itAccepted != includeExpansionAcceptedResults.end()) {
          AttachAcceptedResultCarrier(edit, itAccepted->second);
        } else {
          AttachAcceptedResultCarrier(
              edit,
              BuildAcceptedIncludeRealizationCandidate(
                  AcceptedPathKind::IncludeMaterializedExpansion, *child));
        }
        edits.push_back(std::move(edit));
      } else {
        // A degenerate child site cannot discharge an include-preserving edit
        // plan for the parent. Select the explicit include-realization proof
        // class for the whole parent include immediately.
        debug("include/mat",
              "inc#{0} child#{1} has degenerate site [start={2},end={3}]; "
              "realizing parent include from B",
              inc->id, child->id, siteStart, siteEnd);
        AcceptedResultCandidate realizationCandidate;
        if (auto realized = BuildInlineIncludeRealizationFromB(
                *inc,
                llvm::formatv("degenerate child replace site for parent inc#{0} "
                              "child#{1} site=[{2},{3})",
                              inc->id, child->id, siteStart, siteEnd)
                    .str(),
                &realizationCandidate)) {
          includeExpansion[includeId] = std::move(*realized);
          includeExpansionAcceptedResults[includeId] =
              std::move(realizationCandidate);
        } else {
          includeExpansion[includeId] = std::string();
        }
        return;
      }
    }
  }

  debug("include/mat", "inc#{0} applying {1} header TextEdits", inc->id,
        edits.size());
  std::string applied = ApplyTextEditsWithPendingResync(
      bytes, edits, appliedExpandedMacroRootIds, headerPath);
  if (terminalFallbackRequested_) {
    includeExpansion[includeId] = std::string();
    return;
  }
  includeExpansion[includeId] = std::move(applied);
  includeExpansionAcceptedResults[includeId] =
      BuildAcceptedIncludeRealizationCandidate(
          AcceptedPathKind::IncludeMaterializedExpansion, *inc);
  debug("include/mat", "EXIT inc#{0} resultLen={1}", inc->id,
        includeExpansion[includeId].size());
}

const RefoldModel::HeaderDecl *
RefoldEngine::FindHeaderDeclForPatch(const RefoldModel::IncludeItem &inc,
                                     const IncludePatch &p) {
  if (inc.decls.empty())
    return nullptr;

  const uint64_t aLo = p.aStart;
  const uint64_t aHi = p.aEnd;

  const RefoldModel::HeaderDecl *bestCover = nullptr;
  const RefoldModel::HeaderDecl *bestOverlap = nullptr;

  auto getSpanLen = [](const RefoldModel::HeaderDecl *d) -> uint64_t {
    // Standardize: ensure we don't underflow if a span is somehow malformed.
    if (d->span.end <= d->span.begin)
      return 0;
    return d->span.end - d->span.begin;
  };

  for (const auto &d : inc.decls) {
    const uint64_t dLo = d.span.begin;
    const uint64_t dHi = d.span.end;
    const uint64_t curSpanLen = dHi - dLo;

    // Pure insertion: treat as attached at aLo
    if (aLo == aHi) {
      // Special case: insertions at decl end are treated as inclusive
      if (aLo >= dLo && aLo <= dHi) {
        // Tie-breaker: prefer the "tightest" (smallest) declaration that covers
        // this point.
        if (!bestCover || curSpanLen < getSpanLen(bestCover))
          bestCover = &d;
      }
      continue;
    }

    // Non-empty A-interval
    const bool covers = (aLo >= dLo && aHi <= dHi);
    const bool overlaps = (aLo < dHi && aHi > dLo);

    if (covers) {
      // Prioritize "Cover": we want the smallest decl that completely contains
      // the patch.
      if (!bestCover || curSpanLen < getSpanLen(bestCover))
        bestCover = &d;
    } else if (overlaps) {
      // Fallback to "Overlap": if no decl covers it, find the smallest one that
      // touches it.
      if (!bestOverlap || curSpanLen < getSpanLen(bestOverlap))
        bestOverlap = &d;
    }
  }

  return bestCover ? bestCover : bestOverlap;
}

RefoldEngine::IncludeTextEditPlan
RefoldEngine::ComputeIncludeTextEdits(const IncludeEdits &ie,
                                      std::string headerText) const {
  const std::string file = resolveHeaderPath(*ie.include);

  const size_t fileLen = headerText.size();

  debug("include/apply",
        "ENTER computeIncludeTextEdits file={0} len={1} patches={2} "
        "cover=[{3},{4})",
        file, fileLen, ie.patches.size(), ie.include->cover.begin,
        ie.include->cover.end);

  // PP cover for this include inside the header; if not present, these will
  // already have been derived from spans when building the model.
  const uint64_t coverBegin =
      std::max(static_cast<uint64_t>(0), ie.include->cover.begin);
  const uint64_t coverEnd = std::max(coverBegin, ie.include->cover.end);

  // Single list of edits; we will apply them highest-offset-first so indices
  // remain stable as we mutate the StringBuilder.
  IncludeTextEditPlan plan;

  for (size_t idx = 0; idx < ie.patches.size(); ++idx) {
    const IncludePatch &p = ie.patches[idx];
    trace("include/patch", "computeIncludeTextEdits: patch={0}", p);

    // Step 3 records the local obligations discharged by each accepted
    // include-materialization path. The patch itself provides the class-local
    // witness data (for example whether the edit is an insertion or is owned
    // by a selected conditional arm).
    auto includeInventoryFor = [&](AcceptedPathKind path,
                                   const IncludeAnchorWitness *witness =
                                       nullptr) {
      return FormatAcceptedPathAudit(path, &p, /*tuAnchorWitness=*/nullptr,
                                     witness);
    };

    const bool isInsert = (p.aStart == p.aEnd) && (p.bStart < p.bEnd);
    const bool isDelete = (p.aStart < p.aEnd) && (p.bStart == p.bEnd);
    const bool isReplace = (p.aStart < p.aEnd) && (p.bStart < p.bEnd);

    debug("include/apply",
          "file={0} patch[{1}] raw A=[{2},{3}) B=[{4},{5}) isInsert={6} "
          "isDelete={7} isReplace={8}",
          file, idx, p.aStart, p.aEnd, p.bStart, p.bEnd, isInsert, isDelete,
          isReplace);

    if (!isInsert && !isDelete && !isReplace) {
      // Ignore empty or malformed patches defensively.
      debug("include/apply",
            "file={0} patch[{1}] ignored (no-op classification)", file, idx);
      continue;
    }

    // Decide which logical header decl "owns" this patch, if any.
    const auto *decl = FindHeaderDeclForPatch(*ie.include, p);

    if (decl) {
      debug("include/apply",
            "file={0} patch[{1}] ownerDecl kind={2} name={3} header=[{4},{5}) "
            "pp-span=[{6},{7})",
            file, idx, decl->kind, decl->name, decl->headerB, decl->headerE,
            decl->span.begin, decl->span.end);
    } else {
      debug("include/apply", "file={0} patch[{1}] ownerDecl=<none>", file, idx);
    }

    // Effective PP coverage in this header we're allowed to touch.
    // For INSERTs we deliberately work at include scope so that inserts that
    // land exactly at a declaration boundary (for example, between
    // `int yyy(...);` and `int zzz(...);`) can anchor to the first token
    // of the following declaration rather than being forced back inside the
    // previous one.  For DELETE / REPLACE we restrict to the owning decl.
    uint64_t ppLo = coverBegin;
    uint64_t ppHi = coverEnd;
    if (!isInsert && decl) {
      ppLo = std::max(ppLo, decl->span.begin);
      ppHi = std::min(ppHi, decl->span.end);
    }
    ppHi = std::max(ppHi, ppLo);

    trace("include/apply",
          "file={0} patch[{1}] effectivePP=[{2},{3}) cover=[{4},{5})", file,
          idx, ppLo, ppHi, coverBegin, coverEnd);

    std::optional<uint64_t> startByte;
    std::optional<uint64_t> endByte;
    IncludeAnchorWitness mappedHeaderWitness;

    const auto &tokmapByPP = model_.GetTokmapByPP();
    if (isInsert) {
      // INSERT: interpret A-position as "before the next token" in this header.
      const uint64_t pos = p.aStart;
      auto anchorMatchesCondArmCert = [&](uint64_t anchorByte) -> bool {
        if (!p.ownerHasCondArmCert)
          return true;
        auto armRef =
            model_.FindArmRefForByte(file, ie.include->id, anchorByte);
        return armRef && armRef->arm && armRef->arm->id == p.ownerCondArmIdCert;
      };

      struct InsertAnchorCandidate {
        AcceptedPathKind path = AcceptedPathKind::Unknown;
        IncludeAnchorWitness witness;
        uint64_t anchorByte = 0;
      };

      // Patch A introduced the normalized include candidate carrier. Patch B
      // moved this anchor competition onto the shared selector, and Patch C
      // now requires each wrapped anchor candidate to discharge its local
      // include-preserving proof obligations before it can participate.
      auto traceInsertCandidate = [&](const InsertAnchorCandidate &candidate,
                                      StringRef stage) {
        const AcceptedResultCandidate acceptedCandidate =
            BuildAcceptedIncludeCandidate(candidate.path, p,
                                          &candidate.witness);
        trace("include/apply",
              "file={0} patch[{1}] INSERT: candidate stage={2} anchorByte={3} "
              "candidate={4}",
              file, idx, stage, candidate.anchorByte,
              FormatAcceptedResultCandidate(acceptedCandidate));
      };

      auto selectBestInsertCandidate =
          [&](ArrayRef<InsertAnchorCandidate> candidates)
          -> std::optional<InsertAnchorCandidate> {
        if (candidates.empty())
          return std::nullopt;

        SmallVector<AcceptedResultCandidate, 4> acceptedCandidates;
        acceptedCandidates.reserve(candidates.size());
        for (const InsertAnchorCandidate &candidate : candidates) {
          acceptedCandidates.push_back(
              BuildAcceptedIncludeCandidate(candidate.path, p,
                                            &candidate.witness));
        }

        const std::optional<size_t> bestIdx =
            SelectPreferredAcceptedResultCandidateIndex(acceptedCandidates);
        if (!bestIdx) {
          trace("include/apply",
                "file={0} patch[{1}] INSERT: no candidate survived "
                "proof-discharge "
                "gating",
                file, idx);
          return std::nullopt;
        }

        trace("include/apply",
              "file={0} patch[{1}] INSERT: selected candidate anchorByte={2} "
              "candidate={3}",
              file, idx, candidates[*bestIdx].anchorByte,
              FormatAcceptedResultCandidate(acceptedCandidates[*bestIdx]));
        return candidates[*bestIdx];
      };

      auto commitInsertCandidate = [&](const InsertAnchorCandidate &candidate) {
        std::string text = PadAtBoundaries(
            headerText, static_cast<size_t>(candidate.anchorByte),
            static_cast<size_t>(candidate.anchorByte), p.insertBytes,
            /* allowLeft */ true, /* allowRight */ true);
        TextEdit edit = MakeTextEditWithResyncOrPending(
            headerText, candidate.anchorByte, candidate.anchorByte, text, file);
        AttachAcceptedResultCarrier(
            edit, BuildAcceptedIncludeCandidate(candidate.path, p,
                                                &candidate.witness));
        plan.edits.push_back(std::move(edit));
      };

      // If this INSERT gap is exactly the begin of the currently selected
      // conditional arm in this header, treat it as the *boundary before* the
      // conditional group rather than as "before the next token" inside the
      // arm body. In preprocessed token space, both positions collapse onto the
      // same PP gap because the controlling directive itself contributes no PP
      // tokens. For include-owned pure insertions, the boundary policy is to
      // keep the insertion outside the conditional unless the insertion is
      // explicitly arm-owned.
      auto selectedArmBeginBoundaryByte = [&]() -> std::optional<uint64_t> {
        auto rightArmRef = model_.FindArmRefAtPP(pos);
        if (!rightArmRef || !rightArmRef->group || !rightArmRef->arm)
          return std::nullopt;
        if (!rightArmRef->arm->selected || !rightArmRef->arm->span)
          return std::nullopt;
        if (rightArmRef->arm->span->begin != pos)
          return std::nullopt;
        if (!PathsEqual(rightArmRef->group->file, file))
          return std::nullopt;
        if (!rightArmRef->group->parentIncludeId ||
            *rightArmRef->group->parentIncludeId != ie.include->id)
          return std::nullopt;

        auto leftArmRef = (pos > 0) ? model_.FindArmRefAtPP(pos - 1)
                                    : std::optional<RefoldModel::ArmRef>{};
        if (leftArmRef && leftArmRef->arm &&
            leftArmRef->arm->id == rightArmRef->arm->id)
          return std::nullopt;

        const bool explicitArmOwned =
            p.ownerHasCondArmCert &&
            p.ownerCondArmIdCert == rightArmRef->arm->id;
        if (explicitArmOwned)
          return std::nullopt;

        return std::clamp<uint64_t>(rightArmRef->group->groupB, 0ULL, fileLen);
      };

      {
        SmallVector<InsertAnchorCandidate, 2> topTierCandidates;

        if (const std::optional<uint64_t> insertByte =
                selectedArmBeginBoundaryByte()) {
          if (*insertByte <= fileLen && anchorMatchesCondArmCert(*insertByte)) {
            InsertAnchorCandidate candidate;
            candidate.path =
                AcceptedPathKind::IncludeInsertSelectedConditionalBoundary;
            candidate.anchorByte = *insertByte;
            candidate.witness.evidence =
                IncludeAnchorEvidenceKind::SelectedConditionalBoundary;
            candidate.witness.hasAnchorByte = true;
            candidate.witness.anchorByte = *insertByte;
            if (auto rightArmRef = model_.FindArmRefAtPP(pos); rightArmRef &&
                rightArmRef->arm) {
              candidate.witness.hasCondArmId = true;
              candidate.witness.condArmId = rightArmRef->arm->id;
            }
            traceInsertCandidate(candidate, "selected-conditional-boundary");
            topTierCandidates.push_back(std::move(candidate));
          }
        }

        IncludeAnchorWitness childBoundaryWitness;
        if (const std::optional<uint64_t> insertByte =
                ComputeChildBoundaryInsertByte(p, file,
                                               &childBoundaryWitness)) {
          if (*insertByte <= fileLen && anchorMatchesCondArmCert(*insertByte)) {
            InsertAnchorCandidate candidate;
            candidate.path = AcceptedPathKind::IncludeInsertChildBoundary;
            candidate.anchorByte = *insertByte;
            candidate.witness = childBoundaryWitness;
            candidate.witness.hasAnchorByte = true;
            candidate.witness.anchorByte = *insertByte;
            traceInsertCandidate(candidate, "child-boundary");
            topTierCandidates.push_back(std::move(candidate));
          }
        }

        if (auto selected = selectBestInsertCandidate(topTierCandidates)) {
          commitInsertCandidate(*selected);
          continue;
        }
      }

      std::optional<uint64_t> rightAnchorPP;
      for (uint64_t pp = std::max(pos, ppLo); pp < ppHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
          rightAnchorPP = pp;
          break;
        }
      }

      if (rightAnchorPP) {
        const std::optional<uint64_t> rightStartByte =
            ByteStartForPPInFile(file, *rightAnchorPP,
                                 /* fallbackToEOF */ false, fileLen);
        IncludeAnchorWitness rightNeighborWitness;
        rightNeighborWitness.evidence =
            IncludeAnchorEvidenceKind::RightNeighborPP;
        rightNeighborWitness.hasNeighborPP = true;
        rightNeighborWitness.neighborPP = *rightAnchorPP;
        rightNeighborWitness.hasAnchorByte = rightStartByte.has_value();
        rightNeighborWitness.anchorByte =
            rightStartByte ? *rightStartByte : 0ULL;
        trace(
            "include/apply",
            "file={0} patch[{1}] INSERT: right-neighbor anchorPP={2} -> "
            "startByte={3} inventory={4}",
            file, idx, rightAnchorPP, rightStartByte,
            includeInventoryFor(AcceptedPathKind::IncludeInsertRightNeighborPP,
                                &rightNeighborWitness));
        if (!rightStartByte) {
          plan.requiresIncludeRealization = true;
          plan.realizationReason =
              llvm::formatv("INSERT: failed to map anchorPP={0} in file {1}",
                            *rightAnchorPP, file)
                  .str();
          return plan;
        }
        if (anchorMatchesCondArmCert(*rightStartByte)) {
          InsertAnchorCandidate candidate;
          candidate.path = AcceptedPathKind::IncludeInsertRightNeighborPP;
          candidate.anchorByte = *rightStartByte;
          candidate.witness = rightNeighborWitness;
          traceInsertCandidate(candidate, "right-neighbor");
          commitInsertCandidate(candidate);
          continue;
        }
      }

      std::optional<uint64_t> leftAnchorPP;
      if (pos > ppLo && ppHi > ppLo) {
        for (uint64_t pp = std::min(pos - 1, ppHi - 1);; --pp) {
          auto it = tokmapByPP.find(pp);
          if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
            leftAnchorPP = pp;
            break;
          }
          if (pp == ppLo)
            break;
        }
      }

      SmallVector<InsertAnchorCandidate, 2> fallbackCandidates;
      if (leftAnchorPP) {
        const std::optional<uint64_t> leftStartByte =
            ByteEndForPPInFile(file, *leftAnchorPP, false, fileLen);
        IncludeAnchorWitness leftNeighborWitness;
        leftNeighborWitness.evidence =
            IncludeAnchorEvidenceKind::LeftNeighborPP;
        leftNeighborWitness.hasNeighborPP = true;
        leftNeighborWitness.neighborPP = *leftAnchorPP;
        leftNeighborWitness.hasAnchorByte = leftStartByte.has_value();
        leftNeighborWitness.anchorByte = leftStartByte ? *leftStartByte : 0ULL;
        trace("include/apply",
              "file={0} patch[{1}] INSERT: left-neighbor anchorPP={2} -> "
              "startByte={3} inventory={4}",
              file, idx, leftAnchorPP, leftStartByte,
              includeInventoryFor(AcceptedPathKind::IncludeInsertLeftNeighborPP,
                                  &leftNeighborWitness));
        if (!leftStartByte) {
          plan.requiresIncludeRealization = true;
          plan.realizationReason =
              llvm::formatv("INSERT: failed to map left-neighbor anchorPP={0} "
                            "in file {1}",
                            *leftAnchorPP, file)
                  .str();
          return plan;
        }
        if (anchorMatchesCondArmCert(*leftStartByte)) {
          InsertAnchorCandidate candidate;
          candidate.path = AcceptedPathKind::IncludeInsertLeftNeighborPP;
          candidate.anchorByte = *leftStartByte;
          candidate.witness = leftNeighborWitness;
          traceInsertCandidate(candidate, "left-neighbor");
          fallbackCandidates.push_back(std::move(candidate));
        }
      } else if (decl) {
        const uint64_t declAnchorByte =
            std::clamp<uint64_t>(decl->headerE, 0ULL, fileLen);
        IncludeAnchorWitness declWitness;
        declWitness.evidence = IncludeAnchorEvidenceKind::DeclBoundary;
        declWitness.hasAnchorByte = true;
        declWitness.anchorByte = declAnchorByte;
        declWitness.hasDeclHeaderRange = true;
        declWitness.declHeaderB = decl->headerB;
        declWitness.declHeaderE = decl->headerE;
        trace("include/apply",
              "file={0} patch[{1}] INSERT: no neighbors; anchor at declEnd={2} "
              "inventory={3}",
              file, idx, declAnchorByte,
              includeInventoryFor(AcceptedPathKind::IncludeInsertDeclBoundary,
                                  &declWitness));
        if (anchorMatchesCondArmCert(declAnchorByte)) {
          InsertAnchorCandidate candidate;
          candidate.path = AcceptedPathKind::IncludeInsertDeclBoundary;
          candidate.anchorByte = declAnchorByte;
          candidate.witness = declWitness;
          traceInsertCandidate(candidate, "decl-boundary");
          fallbackCandidates.push_back(std::move(candidate));
        }
      }

      if (auto selected = selectBestInsertCandidate(fallbackCandidates)) {
        commitInsertCandidate(*selected);
        continue;
      }

      debug("include/apply.",
            "file={0} patch[{1}] INSERT: no admissible include-preserving "
            "anchor; realizing include",
            file, idx);
      plan.requiresIncludeRealization = true;
      plan.realizationReason =
          llvm::formatv("INSERT: cannot anchor include patch in file {0} "
                        "(no admissible neighbors/decl/child boundary)",
                        file)
              .str();
      return plan;
    } else {
      // DELETE / REPLACE: map non-empty A-range to byte range within this
      // header.
      uint64_t aLo = std::max(p.aStart, ppLo);
      uint64_t aHi = std::min(p.aEnd, ppHi);
      if (aHi <= aLo) {
        // Nothing of this patch lies in this header/declaration.
        debug("include/apply",
              "file={0} patch[{1}] DELETE/REPLACE: empty intersection; SKIP",
              file, idx);
        continue;
      }

      std::optional<uint64_t> firstPP, lastPP;
      for (uint64_t pp = aLo; pp < aHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
          if (!firstPP)
            firstPP = pp;
          lastPP = pp;
        }
      }

      if (!firstPP || !lastPP) {
        // No tokens from this patch actually map into this header file.
        debug("include/apply",
              "file={0} patch[{1}] DELETE/REPLACE: no mapped PP tokens in "
              "header; SKIP",
              file, idx);
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("DELETE/REPLACE: no mapped PP tokens for patch[{0}] "
                          "in header file {1}",
                          idx, file)
                .str();
        return plan;
      }

      startByte = ByteStartForPPInFile(file, *firstPP,
                                       /* fallbackToEOF */ false, fileLen);
      endByte =
          ByteEndForPPInFile(file, *lastPP, /* fallbackToEOF */ false, fileLen);
      mappedHeaderWitness.evidence =
          IncludeAnchorEvidenceKind::MappedHeaderTokens;
      mappedHeaderWitness.hasFirstPP = firstPP.has_value();
      mappedHeaderWitness.firstPP = firstPP ? *firstPP : 0ULL;
      mappedHeaderWitness.hasLastPP = lastPP.has_value();
      mappedHeaderWitness.lastPP = lastPP ? *lastPP : 0ULL;
      mappedHeaderWitness.hasByteRange =
          startByte.has_value() && endByte.has_value();
      mappedHeaderWitness.startByte = startByte ? *startByte : 0ULL;
      mappedHeaderWitness.endByte = endByte ? *endByte : 0ULL;
      trace("include/apply",
            "file={0} patch[{1}] DELETE/REPLACE: firstPP={2} lastPP={3} -> "
            "bytes=[{4},{5}) inventory={6}",
            file, idx, firstPP, lastPP, startByte, endByte,
            includeInventoryFor(
                AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens,
                &mappedHeaderWitness));
      if (!startByte || !endByte) {
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("DELETE/REPLACE: failed to map first/last PP tokens "
                          "to bytes in file {0}",
                          file)
                .str();
        return plan;
      }
    }

    // Clamp to the declaration’s header_span, if any, so we never cross decl
    // boundaries for DELETE/REPLACE.  For INSERTs we intentionally allow the
    // anchor to sit on the boundary between two declarations so that a new
    // declaration can be injected cleanly between them.
    if (!isInsert && decl) {
      startByte = std::clamp(*startByte, decl->headerB, decl->headerE);
      endByte = std::clamp(*endByte, decl->headerB, decl->headerE);
    }

    // Sanity clamp to file bounds.
    if (!startByte)
      continue;

    const uint64_t fLen = static_cast<uint64_t>(fileLen);
    startByte = std::clamp(*startByte, uint64_t(0), fLen);
    endByte = std::clamp(*endByte, *startByte, fLen);

    std::string replacement = isDelete ? "" : p.insertBytes;

    if (inTraceMode()) {
      StringRef opKind =
          isInsert ? "INSERT" : (isDelete ? "DELETE" : "REPLACE");

      // DEBUG: log the exact slice and replacement we are about to apply.
      const std::string originalSlice =
          headerText.substr(*startByte, *endByte - *startByte);
      std::string origDbg = originalSlice;
      std::string replDbg = replacement;
      constexpr int MAX_DBG = 120;
      if (origDbg.size() > MAX_DBG) {
        origDbg = origDbg.substr(0, MAX_DBG) + "…";
      }
      if (replDbg.size() > MAX_DBG) {
        replDbg = replDbg.substr(0, MAX_DBG) + "…";
      }

      std::string declInfo = "<none>";
      if (decl) {
        declInfo =
            formatv("kind={0} name={1} header=[{2},{3}) pp-span=[{4},{5})",
                    decl->kind, decl->name, decl->headerB, decl->headerE,
                    decl->span.begin, decl->span.end)
                .str();
      }

      trace("include/apply",
            "file={0} patch[{1}] kind={2} A=[{3},{4}) B=[{5},{6}) pp=[{7},{8}) "
            "bytes=[{9},{10}) decl={11} orig='{12}' repl='{13}'",
            file, idx, opKind, p.aStart, p.aEnd, p.bStart, p.bEnd, ppLo, ppHi,
            startByte, endByte, declInfo, stringutils::showWS(origDbg),
            stringutils::showWS(replDbg));
    }

    TextEdit edit = MakeTextEditWithResyncOrPending(
        headerText, *startByte, *endByte, replacement, file);
    AttachAcceptedResultCarrier(
        edit, BuildAcceptedIncludeCandidate(
                  AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens, p,
                  &mappedHeaderWitness));
    plan.edits.push_back(std::move(edit));
  }

  // Apply all edits inside this header, highest offset first so earlier edits
  // do not disturb the coordinates of later ones.
  sort(plan.edits, [](const TextEdit &lhs, const TextEdit &rhs) {
    if (lhs.start != rhs.start)
      return lhs.start > rhs.start;
    return lhs.end > rhs.end;
  });

  debug("include/apply", "file={0} computed {1} header TextEdits", file,
        plan.edits.size());

  for (const auto &e : plan.edits) {
    trace("include/apply",
          "file={0} header TextEdit bytes=[{1},{2}) replLen={3}", file, e.start,
          e.end, e.text.size());

    if (e.end < e.start || e.end > static_cast<uint64_t>(headerText.size())) {
      fatal("include/apply", "TextEdit out of bounds: bytes=[{0},{1}) size={2}",
            e.start, e.end, headerText.size());
    }
  }

  return plan;
}

std::optional<uint64_t> RefoldEngine::ComputeChildBoundaryInsertByte(
    const IncludePatch &p, StringRef file,
    IncludeAnchorWitness *witness) const {
  // Which include are we editing?
  const RefoldModel::IncludeItem *owner = p.include;
  if (!owner) {
    return std::nullopt;
  }

  // Structural boundary selection:
  //   * If the INSERT PP-gap is strictly inside a child include's PP cover,
  //     this is not a boundary insertion in the parent (it should have been a
  //     child-owned patch).
  //   * If the gap is exactly on a child cover boundary, we can anchor at that
  //     child's include-site byte span.
  //
  // Contract: boundary anchors must be unambiguous. If multiple children claim
  // the same boundary PP position, we must fail rather than guess.

  uint64_t pos = p.aStart; // PP position of the INSERT gap

  SmallVector<const RefoldModel::IncludeItem *, 4> beginMatches;
  SmallVector<const RefoldModel::IncludeItem *, 4> endMatches;

  for (const auto &child : model_.GetIncludes()) {
    // Only check direct children of the owner.
    if (!child.parent || *child.parent != owner->id)
      continue;

    // Paths must match the file currently being processed.
    if (!PathsEqual(child.sitePath, file))
      continue;

    const uint64_t cb = child.cover.begin;
    const uint64_t ce = child.cover.end;

    // If the gap is strictly inside, this is the wrong mechanism.
    if (cb < pos && pos < ce)
      return std::nullopt;

    if (cb == pos)
      beginMatches.push_back(&child);
    if (ce == pos)
      endMatches.push_back(&child);
  }

  auto dump = [](ArrayRef<const RefoldModel::IncludeItem *> v) -> std::string {
    std::string out;
    bool first = true;
    for (const auto *c : v) {
      if (!first)
        out += "; ";
      first = false;
      out +=
          formatv("child#{0} cover=[{1},{2}) site=[{3},{4}) target={5}", c->id,
                  c->cover.begin, c->cover.end, c->siteB, c->siteE, c->target)
              .str();
    }
    return out;
  };

  // Prefer anchoring before a child whose cover begins at this gap.
  if (!beginMatches.empty()) {
    if (beginMatches.size() != 1) {
      fatal("include/boundary",
            "ambiguous child include boundary: {0} children have cover.begin "
            "== {1} in file={2}; candidates: {3}",
            beginMatches.size(), pos, file, dump(beginMatches));
    }
    if (witness) {
      witness->evidence = IncludeAnchorEvidenceKind::ChildBoundary;
      witness->hasChildIncludeId = true;
      witness->childIncludeId = beginMatches[0]->id;
      witness->hasAnchorByte = true;
      witness->anchorByte = beginMatches[0]->siteB;
    }
    return beginMatches[0]->siteB;
  }

  if (!endMatches.empty()) {
    if (endMatches.size() != 1) {
      fatal("include/boundary",
            "ambiguous child include boundary: {0} children have cover.end == "
            "{1} in file={2}; candidates: {3}",
            endMatches.size(), pos, file, dump(endMatches));
    }
    if (witness) {
      witness->evidence = IncludeAnchorEvidenceKind::ChildBoundary;
      witness->hasChildIncludeId = true;
      witness->childIncludeId = endMatches[0]->id;
      witness->hasAnchorByte = true;
      witness->anchorByte = endMatches[0]->siteE;
    }
    return endMatches[0]->siteE;
  }

  return std::nullopt;
}

RefoldEngine::ResyncOutcome
RefoldEngine::ApplyResyncOrPend(StringRef originalFileText, uint64_t start,
                                uint64_t end, StringRef replacement,
                                StringRef fileSpellingForDirective) const {
  size_t origNl = stringutils::countNewlines(originalFileText, start, end);
  size_t replNl =
      stringutils::countNewlines(replacement, 0, replacement.size());
  if (origNl == replNl)
    return ResyncOutcome(replacement.str(), std::nullopt);

  // Drift detected: decide whether to inject a local `#line` or defer.
  size_t resumeLine = stringutils::lineAtOffset(originalFileText, end);
  trace("linedir/resync",
        "drift: span=[{0},{1}) origNl={2} replNl={3} resumeLine={4} file={5} "
        "replTail={6}",
        start, end, origNl, replNl, resumeLine, fileSpellingForDirective,
        stringutils::showWSWithClip(replacement, 100));

  // Attempt local injection first.
  std::string injected = lineDirs_.MaybeAppendResyncAfterReplacement(
      originalFileText, start, end, replacement, fileSpellingForDirective);

  // If the returned string changed, injection succeeded.
  if (injected != replacement) {
    trace("linedir/resync", "local inject succeeded: resumeLine={0} file={1}",
          resumeLine, fileSpellingForDirective);
    return ResyncOutcome(std::move(injected), std::nullopt);
  }

  trace("linedir/resync",
        "local inject failed -> PENDING: resumeLine={0} file={1}", resumeLine,
        fileSpellingForDirective);

  return ResyncOutcome{replacement.str(),
                       PendingResync{fileSpellingForDirective}};
}

void RefoldEngine::AttachAcceptedResultCarrier(
    TextEdit &edit, const AcceptedResultCandidate &candidate) const {
  edit.acceptedResults.push_back(
      std::make_shared<AcceptedResultCandidate>(candidate));
}

bool RefoldEngine::EmittedTextEditHasDischargedAcceptedResults(
    const TextEdit &edit, StringRef emissionPhase,
    StringRef emissionOwner) const {
  const StringRef owner = emissionOwner.empty() ? StringRef("<unknown>")
                                                : emissionOwner;
  ++lastTheoremAudit_.emittedNonTerminalEdits;

  auto carrierIsEmissionDischarged =
      [&](const AcceptedResultCandidate &carrier) -> bool {
    ++lastTheoremAudit_.emittedCarriers;

    if (carrier.kind == AcceptedResultCandidateKind::Unknown) {
      ++lastTheoremAudit_.emittedUnknownClassCarriers;
      NoteTheoremAuditViolation("emitted carrier had unknown candidate kind");
      return false;
    }
    if (carrier.kind == AcceptedResultCandidateKind::TerminalOutOfDomain) {
      ++lastTheoremAudit_.emittedOutOfDomainCarriers;
      NoteTheoremAuditViolation(
          "non-terminal emitted edit carried explicit out-of-domain result");
      return false;
    }
    if (carrier.proofSummary.acceptedClass == AcceptedProofClass::Unknown) {
      ++lastTheoremAudit_.emittedUnknownClassCarriers;
      NoteTheoremAuditViolation(
          "emitted carrier did not belong to a declared accepted proof class");
      return false;
    }
    ++lastTheoremAudit_.emittedDeclaredClassCarriers;

    // Step 3 is meant to eliminate transitional-gap states from emitted
    // behavior, not merely relabel them in the normalized proof model. Keep
    // any remaining non-first-class / transitional carrier fail-closed at the
    // byte-edit emission boundary even if some earlier stage forgot to stamp a
    // stronger local diagnosis onto the discharge record.
    if (carrier.proofSummary.inventory.support !=
            AcceptanceSupportKind::ExplicitProofBacked ||
        carrier.proofSummary.theoremDomain.kind ==
            TheoremDomainKind::TransitionalGap ||
        carrier.proofSummary.completeness.coverage ==
            CompletenessCoverageKind::TransitionalGap) {
      ++lastTheoremAudit_.emittedTransitionalTheoremCarriers;
      ++lastTheoremAudit_.emittedUndischargedCarriers;
      NoteTheoremAuditViolation(
          "emitted carrier remained transitional at the byte-edit boundary");
      return false;
    }

    if (carrier.proofSummary.theoremDomain.kind !=
            TheoremDomainKind::DeclaredInDomainClass ||
        !carrier.proofSummary.theoremDomain.inDeclaredDomain ||
        carrier.proofSummary.completeness.coverage !=
            CompletenessCoverageKind::DeclaredProofClass) {
      ++lastTheoremAudit_.emittedOutOfDomainCarriers;
      NoteTheoremAuditViolation("emitted carrier was not theorem-domain "
                                "in-domain at the byte-edit boundary");
      return false;
    }

    if (carrier.proofSummary.discharge.status ==
        ProofDischargeStatus::Discharged) {
      ++lastTheoremAudit_.emittedDischargedCarriers;
      return true;
    }

    ++lastTheoremAudit_.emittedUndischargedCarriers;
    NoteTheoremAuditViolation("emitted carrier failed local proof discharge at "
                              "the byte-edit boundary");
    return false;
  };

  if (edit.acceptedResults.empty()) {
    NoteTheoremAuditViolation("emitted edit reached the byte-edit boundary "
                              "without accepted-result carriers");
    RequestTerminalFallback(
        TerminalFallbackKind::UndischargedEmissionArtifact, emissionPhase,
        llvm::formatv("emitted edit bytes=[{0},{1}) in {2} has no normalized "
                      "accepted-result carriers",
                      edit.start, edit.end, owner)
            .str());
    return false;
  }

  for (size_t i = 0; i < edit.acceptedResults.size(); ++i) {
    const std::shared_ptr<const AcceptedResultCandidate> &carrier =
        edit.acceptedResults[i];
    if (!carrier) {
      NoteTheoremAuditViolation(
          "emitted edit carried a null accepted-result carrier");
      RequestTerminalFallback(
          TerminalFallbackKind::UndischargedEmissionArtifact, emissionPhase,
          llvm::formatv("emitted edit bytes=[{0},{1}) in {2} has null "
                        "accepted-result carrier #{3}",
                        edit.start, edit.end, owner, i)
              .str());
      return false;
    }

    if (!carrierIsEmissionDischarged(*carrier)) {
      RequestTerminalFallback(
          TerminalFallbackKind::UndischargedEmissionArtifact, emissionPhase,
          llvm::formatv("emitted edit bytes=[{0},{1}) in {2} carries "
                        "non-discharged result #{3}: {4}",
                        edit.start, edit.end, owner, i,
                        FormatAcceptedResultCandidate(*carrier))
              .str());
      return false;
    }
  }

  return true;
}

std::string RefoldEngine::ApplyTextEditsWithPendingResync(
    StringRef originalFileText, ArrayRef<TextEdit> edits,
    DenseSet<uint64_t> *appliedExpandedMacroRootIds,
    StringRef emissionOwner) const {
  if (edits.empty())
    return originalFileText.str();

  // Normalize edits by span.
  //
  // Historically we deduped by (start,end) using a map, which accidentally
  // dropped legitimate *multiple insertions* at the same byte offset.
  // This happens when the diff produces adjacent insert-only hunks (e.g. when
  // comments tokenize separately), and both map to the same insertion point.
  //
  // We now:
  //  * Preserve and concatenate multiple INSERT edits with identical
  //    zero-length spans ([x,x)) in their original order.
  //  * Merge duplicate non-zero edits only when they carry the same
  //    replacement payload; conflicting duplicate replacements request the
  //    explicit terminal fallback instead of selecting a writer by order.

  struct EditRef {
    const TextEdit *e;
    size_t idx;
  };

  SmallVector<EditRef, 32> ordered;
  ordered.reserve(edits.size());
  for (size_t i = 0; i < edits.size(); ++i)
    ordered.push_back(EditRef{&edits[i], i});

  sort(ordered, [](const EditRef &a, const EditRef &b) {
    if (a.e->start != b.e->start)
      return a.e->start < b.e->start;
    if (a.e->end != b.e->end)
      return a.e->end < b.e->end;
    return a.idx < b.idx;
  });

  SmallVector<TextEdit, 32> norm;
  norm.reserve(ordered.size());

  for (size_t i = 0; i < ordered.size();) {
    const uint64_t s = ordered[i].e->start;
    const uint64_t t = ordered[i].e->end;

    size_t j = i + 1;
    while (j < ordered.size() && ordered[j].e->start == s &&
           ordered[j].e->end == t)
      ++j;

    if (j - i == 1) {
      norm.push_back(*ordered[i].e);
      i = j;
      continue;
    }

    // Multiple edits share the same byte span.
    if (s == t) {
      // INSERT at identical position: concatenate in order.
      TextEdit merged;
      merged.start = s;
      merged.end = t;

      size_t totalLen = 0;
      for (size_t k = i; k < j; ++k)
        totalLen += ordered[k].e->text.size();
      merged.text.reserve(totalLen);

      for (size_t k = i; k < j; ++k) {
        merged.text.append(ordered[k].e->text);
        if (ordered[k].e->pending)
          merged.pending = ordered[k].e->pending;
        merged.acceptedResults.insert(merged.acceptedResults.end(),
                                      ordered[k].e->acceptedResults.begin(),
                                      ordered[k].e->acceptedResults.end());
      }

      trace("edits/apply",
            "merged {0} insertion edits at [{1},{2}) into len={3}", j - i, s, t,
            merged.text.size());

      norm.push_back(std::move(merged));
      i = j;
      continue;
    }

    // Non-zero span duplicates must be identical to compose safely.  Choosing
    // an arbitrary writer for the same replaced byte range is not a proof; if
    // two producers disagree about the replacement payload, fail closed and let
    // the caller select the explicit terminal fallback path.
    const TextEdit *first = ordered[i].e;
    bool payloadsAgree = true;
    for (size_t k = i + 1; k < j; ++k) {
      if (ordered[k].e->text != first->text) {
        payloadsAgree = false;
        break;
      }
    }

    if (!payloadsAgree) {
      auto tryMergeDirectDuplicateFragments = [&]() -> std::optional<TextEdit> {
        SmallVector<const TextEdit *, 8> fragments;
        fragments.reserve(j - i);

        // This merge is only valid for duplicate fragments that still have a
        // direct one-to-one TU hunk witness.  Anything synthesized, macro-owned,
        // include-owned, or missing hunk provenance must stay on the normal
        // composition path.
        for (size_t k = i; k < j; ++k) {
          const TextEdit &edit = *ordered[k].e;
          if (!edit.isDirectTUHunkEdit || !edit.directTUHunkIndex ||
              !edit.directTUHunkAStart || !edit.directTUHunkAEnd ||
              !edit.directTUHunkBStart || !edit.directTUHunkBEnd)
            return std::nullopt;
          fragments.push_back(&edit);
        }

        // Reconstruct the original hunk order before proving adjacency.  The
        // duplicate group is ordered by emitted byte span, which is not enough
        // to prove that the B-side replacement interval is contiguous.
        sort(fragments, [](const TextEdit *lhs, const TextEdit *rhs) {
          return *lhs->directTUHunkIndex < *rhs->directTUHunkIndex;
        });

        // Adjacent duplicate fragments may be collapsed only when they form one
        // uninterrupted run in both the A token hunks and B token hunks.  This
        // prevents merging unrelated fragments that merely overlap the same
        // emitted byte span.
        for (size_t k = 1; k < fragments.size(); ++k) {
          const TextEdit &prev = *fragments[k - 1];
          const TextEdit &cur = *fragments[k];

          if (*cur.directTUHunkIndex != *prev.directTUHunkIndex + 1)
            return std::nullopt;
          if (*cur.directTUHunkAStart != *prev.directTUHunkAEnd)
            return std::nullopt;
          if (*cur.directTUHunkBStart != *prev.directTUHunkBEnd)
            return std::nullopt;
        }

        // The merged replacement is exactly the contiguous B-token envelope
        // covered by the duplicate fragment run.
        const uint64_t bBegin = *fragments.front()->directTUHunkBStart;
        const uint64_t bEnd = *fragments.back()->directTUHunkBEnd;
        if (bBegin >= bEnd || bEnd > static_cast<uint64_t>(bToks_.size()))
          return std::nullopt;

        StringRef replacement =
            sliceExactTokenCoverage(bTokOff_, bToks_, bSource_, bBegin, bEnd);
        if (replacement.empty())
          return std::nullopt;

        // Apply normal resync handling to the whole duplicate byte span, rather
        // than preserving each fragment's already-conflicting local resync.
        ResyncOutcome ro = ApplyResyncOrPend(originalFileText, s, t,
                                             replacement, emissionOwner);
        TextEdit merged{s, t, std::move(ro.text), std::move(ro.pending),
                        std::nullopt, {}};

        // Preserve existing proof carriers from the fragments.  If the fragments
        // carry macro-root provenance, they must all agree on the same root.
        for (const TextEdit *fragment : fragments) {
          if (fragment->expandedMacroRootId) {
            if (merged.expandedMacroRootId &&
                *merged.expandedMacroRootId != *fragment->expandedMacroRootId)
              return std::nullopt;
            merged.expandedMacroRootId = fragment->expandedMacroRootId;
          }
          merged.acceptedResults.insert(merged.acceptedResults.end(),
                                        fragment->acceptedResults.begin(),
                                        fragment->acceptedResults.end());
        }

        // Add a carrier for the actual emitted merged surface so the final proof
        // gate can reason about this replacement as one conservative TU edit.
        AttachAcceptedResultCarrier(
            merged, BuildAcceptedTUTextEditCandidate(
                        AcceptedPathKind::TUByteSpanConservativeEdit, s, t,
                        replacement));

        trace("edits/apply",
              "merged {0} adjacent direct duplicate fragments at [{1},{2}) "
              "from Btok=[{3},{4}) into len={5}",
              fragments.size(), s, t, bBegin, bEnd, replacement.size());
        return merged;
      };

      // Split token-LCS frontiers can produce several direct TU hunk fragments
      // for the same original byte span.  If those fragments are adjacent in
      // both A-token and B-token space, they are not competing writers: they are
      // one replacement that was fragmented before byte emission.  Rebuild that
      // replacement from the closed B-token interval; otherwise keep the
      // existing fail-closed duplicate-edit policy.
      if (std::optional<TextEdit> mergedDirectDuplicate =
              tryMergeDirectDuplicateFragments()) {
        norm.push_back(std::move(*mergedDirectDuplicate));
        i = j;
        continue;
      }

      RequestTerminalFallback(
          TerminalFallbackKind::UndischargedEmissionArtifact, "edits/apply",
          llvm::formatv(
              "conflicting duplicate non-zero edits for span [{0},{1}) in {2}",
              s, t, emissionOwner)
              .str());
      return originalFileText.str();
    }

    TextEdit merged = *first;
    for (size_t k = i + 1; k < j; ++k) {
      const TextEdit &dup = *ordered[k].e;
      if (dup.pending)
        merged.pending = dup.pending;

      if (merged.expandedMacroRootId && dup.expandedMacroRootId &&
          *merged.expandedMacroRootId != *dup.expandedMacroRootId) {
        RequestTerminalFallback(
            TerminalFallbackKind::UndischargedEmissionArtifact, "edits/apply",
            llvm::formatv("duplicate non-zero edits for span [{0},{1}) in {2} "
                          "carry incompatible expanded macro roots {3} and {4}",
                          s, t, emissionOwner, *merged.expandedMacroRootId,
                          *dup.expandedMacroRootId)
                .str());
        return originalFileText.str();
      }
      if (!merged.expandedMacroRootId && dup.expandedMacroRootId)
        merged.expandedMacroRootId = dup.expandedMacroRootId;

      merged.acceptedResults.insert(merged.acceptedResults.end(),
                                    dup.acceptedResults.begin(),
                                    dup.acceptedResults.end());
    }

    trace("edits/apply",
          "merged {0} identical non-zero edits at [{1},{2}) into one edit",
          j - i, s, t);

    norm.push_back(std::move(merged));
    i = j;
  }

  auto sortNormalizedTextEdits = [](SmallVectorImpl<TextEdit> &items) {
    sort(items, [](const TextEdit &a, const TextEdit &b) {
      if (a.start != b.start)
        return a.start < b.start;
      return a.end < b.end;
    });
  };

  auto findDirectEditForHunk = [&](uint64_t hunkIndex)
      -> std::optional<size_t> {
    for (size_t idx = 0; idx < norm.size(); ++idx) {
      const TextEdit &edit = norm[idx];
      if (edit.directTUHunkIndex && *edit.directTUHunkIndex == hunkIndex)
        return idx;
    }
    return std::nullopt;
  };

  auto sourceTokenSpan = [&](uint64_t aTok)
      -> std::optional<std::pair<uint64_t, uint64_t>> {
    if (aTok >= static_cast<uint64_t>(aToks_.size()))
      return std::nullopt;
    return TUByteSpan(aTok, aTok + 1, emissionOwner);
  };

  struct ClosedTURealizationCandidate {
    uint64_t firstHunk = 0;
    uint64_t lastHunk = 0;
    uint64_t sourceBegin = 0;
    uint64_t sourceEnd = 0;
    uint64_t bBegin = 0;
    uint64_t bEnd = 0;
    SmallVector<size_t, 16> editIndices;
  };

  auto checkClosedTURealizationCandidate = [&](uint64_t firstHunk,
                                               uint64_t lastHunk,
                                               ClosedTURealizationCandidate
                                                   &candidate) -> bool {
    candidate = ClosedTURealizationCandidate{};
    candidate.firstHunk = firstHunk;
    candidate.lastHunk = lastHunk;

    for (uint64_t hunkIndex = firstHunk; hunkIndex <= lastHunk; ++hunkIndex) {
      std::optional<size_t> editIndex = findDirectEditForHunk(hunkIndex);
      if (!editIndex)
        return false;
      candidate.editIndices.push_back(*editIndex);
      if (hunkIndex == std::numeric_limits<uint64_t>::max())
        break;
    }

    candidate.sourceBegin = std::numeric_limits<uint64_t>::max();
    candidate.sourceEnd = 0;
    candidate.bBegin = std::numeric_limits<uint64_t>::max();
    candidate.bEnd = 0;

    std::optional<uint64_t> previousBEnd;
    for (size_t editIndex : candidate.editIndices) {
      const TextEdit &edit = norm[editIndex];
      if (!edit.isDirectTUHunkEdit || !edit.directTUHunkBStart ||
          !edit.directTUHunkBEnd)
        return false;
      if (previousBEnd && *edit.directTUHunkBStart < *previousBEnd)
        return false;
      previousBEnd = edit.directTUHunkBEnd;
      candidate.sourceBegin = std::min(candidate.sourceBegin, edit.start);
      candidate.sourceEnd = std::max(candidate.sourceEnd, edit.end);
      candidate.bBegin = std::min(candidate.bBegin, *edit.directTUHunkBStart);
      candidate.bEnd = std::max(candidate.bEnd, *edit.directTUHunkBEnd);
    }

    if (candidate.sourceBegin == std::numeric_limits<uint64_t>::max() ||
        candidate.sourceBegin >= candidate.sourceEnd)
      return false;
    if (candidate.bBegin == std::numeric_limits<uint64_t>::max() ||
        candidate.bBegin >= candidate.bEnd ||
        candidate.bEnd > static_cast<uint64_t>(bToks_.size()))
      return false;

    auto candidateContainsEdit = [&](size_t editIndex) {
      for (size_t candidateEditIndex : candidate.editIndices) {
        if (candidateEditIndex == editIndex)
          return true;
      }
      return false;
    };

    // The synthesized closure is allowed to replace only the selected hunk run.
    // Any independent edit that overlaps the closure, or any insertion strictly
    // inside it, would require another composition law and is therefore rejected.
    for (size_t editIndex = 0; editIndex < norm.size(); ++editIndex) {
      if (candidateContainsEdit(editIndex))
        continue;
      const TextEdit &edit = norm[editIndex];
      const bool nonZeroOverlap =
          edit.start < candidate.sourceEnd && candidate.sourceBegin < edit.end;
      const bool interiorInsertion =
          edit.start == edit.end && candidate.sourceBegin < edit.start &&
          edit.start < candidate.sourceEnd;
      if (nonZeroOverlap || interiorInsertion)
        return false;
    }

    // Prove that every A token physically consumed by the source interval has
    // no matched B occurrence outside the candidate B interval.  This is what
    // prevents a closure from stealing a stable token whose true B-side mate
    // remains outside the replacement.
    for (uint64_t aTok = 0; aTok < static_cast<uint64_t>(aToks_.size());
         ++aTok) {
      std::optional<std::pair<uint64_t, uint64_t>> span =
          sourceTokenSpan(aTok);
      if (!span)
        continue;

      const bool overlapsSource =
          span->first < candidate.sourceEnd && candidate.sourceBegin < span->second;
      if (!overlapsSource)
        continue;

      if (span->first < candidate.sourceBegin ||
          candidate.sourceEnd < span->second)
        return false;

      if (aTok < static_cast<uint64_t>(abTokMapA2B_.size())) {
        const int64_t mappedB = abTokMapA2B_[static_cast<size_t>(aTok)];
        if (mappedB >= 0 &&
            (static_cast<uint64_t>(mappedB) < candidate.bBegin ||
             static_cast<uint64_t>(mappedB) >= candidate.bEnd))
          return false;
      }
    }

    // Symmetrically, every matched B token emitted by the replacement must map
    // back to an A token wholly contained in the source interval.  Otherwise the
    // candidate would duplicate or move an outside stable token.
    for (uint64_t bTok = candidate.bBegin; bTok < candidate.bEnd; ++bTok) {
      if (bTok >= static_cast<uint64_t>(abTokMapB2A_.size()))
        return false;
      const int64_t mappedA = abTokMapB2A_[static_cast<size_t>(bTok)];
      if (mappedA < 0)
        continue;

      std::optional<std::pair<uint64_t, uint64_t>> span =
          sourceTokenSpan(static_cast<uint64_t>(mappedA));
      if (!span || span->first < candidate.sourceBegin ||
          candidate.sourceEnd < span->second)
        return false;
    }

    return true;
  };

  auto tryBuildClosedTURealization = [&](size_t overlapIndex,
                                         TextEdit &closure) -> bool {
    if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
      return false;

    size_t clusterBegin = overlapIndex == 0 ? 0 : overlapIndex - 1;
    uint64_t coverBegin = norm[clusterBegin].start;
    uint64_t coverEnd = norm[clusterBegin].end;
    while (clusterBegin > 0) {
      const TextEdit &prev = norm[clusterBegin - 1];
      if (prev.end <= coverBegin)
        break;
      --clusterBegin;
      coverBegin = std::min(coverBegin, prev.start);
      coverEnd = std::max(coverEnd, prev.end);
    }

    size_t clusterEnd = overlapIndex + 1;
    if (overlapIndex < norm.size()) {
      coverBegin = std::min(coverBegin, norm[overlapIndex].start);
      coverEnd = std::max(coverEnd, norm[overlapIndex].end);
    }
    while (clusterEnd < norm.size()) {
      const TextEdit &next = norm[clusterEnd];
      if (next.start >= coverEnd)
        break;
      coverBegin = std::min(coverBegin, next.start);
      coverEnd = std::max(coverEnd, next.end);
      ++clusterEnd;
    }

    std::optional<uint64_t> hunkMin;
    std::optional<uint64_t> hunkMax;
    for (size_t i = clusterBegin; i < clusterEnd; ++i) {
      const TextEdit &edit = norm[i];
      if (!edit.isDirectTUHunkEdit || !edit.directTUHunkIndex)
        return false;
      hunkMin = hunkMin ? std::min(*hunkMin, *edit.directTUHunkIndex)
                        : *edit.directTUHunkIndex;
      hunkMax = hunkMax ? std::max(*hunkMax, *edit.directTUHunkIndex)
                        : *edit.directTUHunkIndex;
    }
    if (!hunkMin || !hunkMax)
      return false;

    uint64_t directLo = *hunkMin;
    while (directLo > 0 && findDirectEditForHunk(directLo - 1))
      --directLo;

    uint64_t directHi = *hunkMax;
    while (directHi + 1 < static_cast<uint64_t>(abTokHunks_.size()) &&
           findDirectEditForHunk(directHi + 1))
      ++directHi;

    std::optional<ClosedTURealizationCandidate> best;
    const uint64_t minWidth = *hunkMax - *hunkMin + 1;
    const uint64_t maxWidth = directHi - directLo + 1;
    for (uint64_t width = minWidth; width <= maxWidth; ++width) {
      bool searchedAnyAtThisWidth = false;
      for (uint64_t first = directLo; first + width - 1 <= directHi; ++first) {
        const uint64_t last = first + width - 1;
        if (first > *hunkMin || last < *hunkMax)
          continue;
        searchedAnyAtThisWidth = true;

        ClosedTURealizationCandidate candidate;
        if (checkClosedTURealizationCandidate(first, last, candidate)) {
          best = std::move(candidate);
          break;
        }
      }
      if (best || !searchedAnyAtThisWidth)
        break;
    }
    if (!best)
      return false;

    StringRef replacement = sliceExactTokenCoverage(
        bTokOff_, bToks_, bSource_, best->bBegin, best->bEnd);
    ResyncOutcome ro = ApplyResyncOrPend(originalFileText, best->sourceBegin,
                                         best->sourceEnd, replacement,
                                         emissionOwner);
    closure = TextEdit{best->sourceBegin, best->sourceEnd, std::move(ro.text),
                       std::move(ro.pending), std::nullopt, {}};
    AttachAcceptedResultCarrier(
        closure, BuildAcceptedTUTextEditCandidate(
                     AcceptedPathKind::TUByteSpanConservativeEdit,
                     best->sourceBegin, best->sourceEnd, replacement));

    SmallVector<TextEdit, 32> resolved;
    resolved.reserve(norm.size() - best->editIndices.size() + 1);
    auto shouldRemove = [&](size_t editIndex) {
      for (size_t candidateEditIndex : best->editIndices) {
        if (candidateEditIndex == editIndex)
          return true;
      }
      return false;
    };
    for (size_t i = 0; i < norm.size(); ++i) {
      if (!shouldRemove(i))
        resolved.push_back(std::move(norm[i]));
    }
    resolved.push_back(std::move(closure));
    sortNormalizedTextEdits(resolved);
    norm = std::move(resolved);

    debug("edits/apply",
          "resolved uncomposable direct TU hunk run in {0}: hunks=[{1},{2}] "
          "source=[{3},{4}) Btok=[{5},{6}) replLen={7}",
          emissionOwner, best->firstHunk, best->lastHunk, best->sourceBegin,
          best->sourceEnd, best->bBegin, best->bEnd, replacement.size());
    return true;
  };

  // Some token-LCS tie choices can split one logical B-side TU realization
  // around stable punctuation tokens.  If the resulting direct TU hunk edits
  // overlap after lexical widening, first try to replace the minimal enclosing
  // direct-hunk run by one source/B-token-closed realization.  This is not a
  // merge heuristic: it is accepted only when the A→B and B→A token maps prove
  // that the source interval and replacement interval are mutually closed.
  for (;;) {
    uint64_t cursor = 0;
    bool changed = false;
    for (size_t editIndex = 0; editIndex < norm.size(); ++editIndex) {
      const TextEdit &edit = norm[editIndex];
      if (edit.end < edit.start || edit.end > originalFileText.size())
        break;
      if (edit.start < cursor) {
        TextEdit closure;
        if (tryBuildClosedTURealization(editIndex, closure)) {
          changed = true;
          break;
        }
        changed = false;
        editIndex = norm.size();
        break;
      }
      cursor = edit.end;
    }
    if (!changed)
      break;
  }

  // Step 2 makes the byte-edit emission boundary proof-gated. By the time an
  // edit reaches this function, every non-terminal artifact it composes must
  // already carry normalized accepted-result carriers that are fully
  // discharged for emitted source text. Do not emit any edit whose carriers
  // are missing or not yet discharged.
  for (const TextEdit &edit : norm) {
    if (!EmittedTextEditHasDischargedAcceptedResults(edit, "emit/nonterminal",
                                                     emissionOwner))
      return originalFileText.str();
  }

  const size_t n = originalFileText.size();

  // Validate the normalized edit set before appending any bytes or recording
  // side effects. The applicator has only one sound composition law here:
  // edits must be in bounds and non-overlapping in original-file byte space.
  // If that law is violated, do not guess whether an insertion should be
  // merged into, ordered around, or shadowed by a replacement. Fail closed to
  // the explicit terminal fallback instead.
  uint64_t validatedCursor = 0;
  for (size_t editIndex = 0; editIndex < norm.size(); ++editIndex) {
    const TextEdit &e = norm[editIndex];
    if (e.end < e.start || e.end > n) {
      RequestTerminalFallback(
          TerminalFallbackKind::UncomposableEmissionEditSet, "edits/apply",
          llvm::formatv(
              "bad normalized edit bounds in {0}: edit#{1}=[{2},{3}) "
              "fileLen={4} textLen={5}",
              emissionOwner, editIndex, e.start, e.end, n, e.text.size())
              .str());
      return originalFileText.str();
    }
    if (e.start < validatedCursor) {
      RequestTerminalFallback(
          TerminalFallbackKind::UncomposableEmissionEditSet, "edits/apply",
          llvm::formatv(
              "overlapping normalized edits in {0}: previousEnd={1} "
              "edit#{2}=[{3},{4}) textLen={5}",
              emissionOwner, validatedCursor, editIndex, e.start, e.end,
              e.text.size())
              .str());
      return originalFileText.str();
    }
    validatedCursor = e.end;
  }

  // Use SmallString for the output buffer to optimize small file edits.
  SmallString<0> out;
  out.reserve(originalFileText.size() + 128);
  std::optional<PendingResync> pending = std::nullopt;

  uint64_t cursor = 0;

  for (const auto &e : norm) {
    // Pass the SmallString to the appender.
    pending = AppendOriginalSliceWithPending(out, originalFileText, cursor,
                                             e.start, std::move(pending));

    if (appliedExpandedMacroRootIds && e.expandedMacroRootId)
      appliedExpandedMacroRootIds->insert(*e.expandedMacroRootId);

    out.append(e.text);

    if (e.pending) {
      pending = e.pending;
    }

    cursor = e.end;
  }

  pending = AppendOriginalSliceWithPending(out, originalFileText, cursor, n,
                                           std::move(pending));

  return std::string(out.str());
}

std::optional<RefoldEngine::PendingResync>
RefoldEngine::AppendOriginalSliceWithPending(
    SmallVectorImpl<char> &out, llvm::StringRef original, uint64_t from,
    uint64_t to, std::optional<RefoldEngine::PendingResync> pending) const {
  if (!pending || !lineDirs_.Enabled()) {
    auto slice = original.slice(from, to);
    out.append(slice.begin(), slice.end());
    return std::nullopt;
  }

  uint64_t i = from;

  auto canFlushBeforeUntouchedDirectiveLine = [&](size_t pos) -> bool {
    if (pos >= original.size())
      return false;

    size_t lineStart = pos;
    while (lineStart > 0 && original[lineStart - 1] != '\n')
      --lineStart;

    if (!stringutils::isIndentOnly(original, lineStart, pos))
      return false;

    size_t lineEnd = original.find('\n', pos);
    if (lineEnd == StringRef::npos)
      lineEnd = original.size();

    size_t firstNonWs = pos;
    while (firstNonWs < lineEnd && stringutils::isWs(original[firstNonWs]) &&
           original[firstNonWs] != '\n')
      ++firstNonWs;

    return firstNonWs < lineEnd && original[firstNonWs] == '#';
  };

  // Flush immediately when output is already at BOL and the next untouched
  // bytes are either:
  // (1) at a true BOL in the original file, or
  // (2) in the leading indentation of an untouched preprocessor-directive
  //     line. Directives may legally be preceded by horizontal whitespace, so
  //     flushing before that indentation keeps the directive line's logical
  //     location accurate without relaxing the ordinary code-line safety rule.
  if (stringutils::outAtBOL(StringRef(out.data(), out.size())) &&
      (stringutils::isBOL(original, static_cast<size_t>(from)) ||
       canFlushBeforeUntouchedDirectiveLine(static_cast<size_t>(from)))) {
    size_t line = stringutils::lineAtOffset(original, from);
    std::string directive =
        lineDirs_.FormatLineDirective(line, pending->fileSpellingForDir);

    // Create a view of the current buffer for the check
    StringRef currentOut(out.data(), out.size());

    if (LineDirectiveInserter::ShouldEmitLineDirective(
            currentOut, pending->fileSpellingForDir, line, directive)) {
      trace("line/pending",
            "flush@slice-begin file={0} line={1} (pending) outTail={2}",
            pending->fileSpellingForDir, line,
            stringutils::dbgOutTail(currentOut));
      out.append(directive.begin(), directive.end());
    } else {
      trace("line/pending",
            "SKIP flush@slice-begin (no-op) file={0} line={1} outTail={2}",
            pending->fileSpellingForDir, line,
            stringutils::dbgOutTail(currentOut));
    }

    auto slice =
        original.slice(static_cast<size_t>(from), static_cast<size_t>(to));
    out.append(slice.begin(), slice.end());
    return std::nullopt;
  }

  // Otherwise, scan forward for the first safe newline boundary (not
  // line-spliced).
  while (i < to) {
    size_t nl = original.find('\n', i);
    if (nl == llvm::StringRef::npos || nl >= static_cast<size_t>(to))
      break;

    auto slice = original.slice(static_cast<size_t>(i), nl + 1);
    out.append(slice.begin(), slice.end());

    i = nl + 1;

    if (!stringutils::isLineSplice(original, nl)) {
      size_t line = stringutils::lineAtOffset(original, static_cast<size_t>(i));
      std::string directive =
          lineDirs_.FormatLineDirective(line, pending->fileSpellingForDir);

      // Update our view of the output after appending the newline
      StringRef currentOut(out.data(), out.size());
      if (LineDirectiveInserter::ShouldEmitLineDirective(
              currentOut, pending->fileSpellingForDir, line, directive)) {
        trace("line/pending",
              "flush@safe-nl file={0} line={1} atOrig={2} outTail={3}",
              pending->fileSpellingForDir, line, i,
              stringutils::dbgOutTail(currentOut));
        out.append(directive.begin(), directive.end());
      } else {
        trace("line/pending",
              "SKIP flush@safe-nl (no-op) file={0} line={1} atOrig={2} "
              "outTail={3}",
              pending->fileSpellingForDir, line, i,
              stringutils::dbgOutTail(currentOut));
      }

      pending = std::nullopt;
      break;
    }
  }

  // Clean up remaining bytes
  if (i < to) {
    auto remaining =
        original.slice(static_cast<size_t>(i), static_cast<size_t>(to));
    out.append(remaining.begin(), remaining.end());
  }
  return pending;
}

// ==================== Low-level file & mapping utilities =====================

void RefoldEngine::CacheCanonicalPath(StringRef path) const {
  auto it = canonicalPathCache_.find(path);
  if (it != canonicalPathCache_.end())
    return;

  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(
      std::filesystem::path(path.str()), ec);
  if (ec) {
    fatal("path/canon", "failed to canonicalize '{0}': {1}", path,
          ec.message());
  }

  canonicalPathCache_.insert({path, canonical.string()});
}

bool RefoldEngine::PathsEqual(StringRef a, StringRef b) const {
  if (a.empty() || b.empty())
    return a == b;

  CacheCanonicalPath(a);
  CacheCanonicalPath(b);

  return canonicalPathCache_.find(a)->getValue() ==
         canonicalPathCache_.find(b)->getValue();
}

// ======================= Diagnostics & Debug Utilities =======================

void RefoldEngine::DebugIncludePatch(StringRef tag,
                                     const RefoldModel::IncludeItem &inc,
                                     const diffutils::Hunk &h) const {
  auto getByteOff = [](uint64_t tokenIdx,
                       ArrayRef<size_t> offsets) -> std::optional<uint64_t> {
    if (tokenIdx < offsets.size()) {
      return static_cast<uint64_t>(offsets[static_cast<size_t>(tokenIdx)]);
    }
    return std::nullopt;
  };

  const std::optional<uint64_t> a0 = getByteOff(h.aStart, aTokOff_);
  const std::optional<uint64_t> a1 = getByteOff(h.aEnd, aTokOff_);
  const std::optional<uint64_t> b0 = getByteOff(h.bStart, bTokOff_);
  const std::optional<uint64_t> b1 = getByteOff(h.bEnd, bTokOff_);

  StringRef aSlice = "";
  if (a0 && a1 && *a1 >= *a0 && *a1 <= aSource_.size()) {
    aSlice = aSource_.substr(static_cast<size_t>(*a0),
                             static_cast<size_t>(*a1 - *a0));
  }
  StringRef bSlice = "";
  if (b0 && b1 && *b1 >= *b0 && *b1 <= bSource_.size()) {
    bSlice = bSource_.substr(static_cast<size_t>(*b0),
                             static_cast<size_t>(*b1 - *b0));
  }

  trace("include/patch",
        "{0} inc #{1} hunk A[{2},{3})->B[{4},{5}) Abytes=[{6},{7}) "
        "Bbytes=[{8},{9}) Aslice='{10}' Bslice='{11}'",
        tag, inc.id, h.aStart, h.aEnd, h.bStart, h.bEnd, a0, a1, b0, b1,
        stringutils::showWSWithClip(aSlice, 120),
        stringutils::showWSWithClip(bSlice, 120));
}

std::string RefoldEngine::PPArgSpanToString(const RefoldModel::PPArgSpan &sp,
                                            bool isStringifyOcc) {
  std::string storage;
  llvm::raw_string_ostream os(storage);

  os << formatv("{kind='{0}', A=[{1},{2}), argIdx={3}", sp.kind, sp.begin,
                sp.end, sp.argIdx);

  if (isStringifyOcc) {
    os << ", occ=STRINGIFY";
  }

  if (sp.kind == PPArgSpanKind::Paste) {
    os << formatv(", byte=[{0},{1})", sp.byteBegin, sp.byteEnd);
  }

  os << '}';
  return os.str();
}

std::string
RefoldEngine::PPArgSpanListToString(ArrayRef<RefoldModel::PPArgSpan> spans,
                                    ArrayRef<char> isStringify) {
  std::string storage;
  llvm::raw_string_ostream os(storage);

  os << '[';
  for (size_t i = 0; i < spans.size(); ++i) {
    if (i > 0)
      os << ", ";

    bool isStr = (i < isStringify.size() && isStringify[i]);
    os << PPArgSpanToString(spans[i], isStr);
  }
  os << ']';

  return os.str();
}
