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

std::optional<std::string> RefoldEngine::TryExpansionClosureFallback() const {
  if (!terminalFallbackRequested_)
    return std::nullopt;

  // This intermediate stage remains proof-driven: discover a declared witness
  // kind first, and only later steps are allowed to turn that witness into an
  // emitted fallback source.
  if (auto witness = BuildMacroOwnerExpansionClosureWitness()) {
    debug("fallback", "expansion-closure witness discovered: {0}",
          FormatExpansionClosureWitness(*witness));
  } else {
    debug("fallback",
          "expansion-closure stage: no proved single-owner macro fallback "
          "witness available");
  }

  // Step 2 only proves the first concrete fallback witness. Source synthesis
  // still arrives in a later step, so preserve the existing fail-closed
  // behavior here until the witness is connected to an emitted replacement.
  return std::nullopt;
}
