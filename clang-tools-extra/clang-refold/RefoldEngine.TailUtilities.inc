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

  // Match against the spelled callsite prefix, ignoring only leading trivia
  // before the macro name.
  while (i < n && stringutils::isWs(invSpanText[i]))
    i++;

  if (i >= n)
    return false;

  // The callsite must begin with an identifier spelling. This intentionally
  // avoids substring matching inside larger expressions or tokens.
  const char c0 = invSpanText[i];
  if (!stringutils::isIdentStart(c0))
    return false;

  size_t j = i + 1;
  while (j < n && stringutils::isIdentPart(invSpanText[j]))
    j++;

  // The leading identifier must be exactly the invocation's macro name.
  StringRef ident = invSpanText.slice(i, j);
  if (ident != m.name)
    return false;

  // Object-like macros have no required argument-list syntax, so the leading
  // identifier match is enough to identify the callsite prefix.
  if (m.subkind != "func")
    return true;

  // Function-like macros must be followed by an opening parenthesis, with
  // ordinary whitespace allowed between the macro name and '('.
  while (j < n && stringutils::isWs(invSpanText[j]))
    j++;

  return (j < n && invSpanText[j] == '(');
}

uint64_t RefoldEngine::GetRootMacroId(uint64_t macroId) const {
  const RefoldModel::MacroInvocation *cur = FindMacroInvocationById(macroId);
  if (!cur)
    return macroId;

  // Walk callerMacroId links to the outermost invocation in this
  // macro-expansion chain. If the model has a missing parent link, return the
  // highest resolved invocation rather than inventing a root.
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
  // Inline include realization is theorem-facing only when the include's
  // A-cover can be projected to a concrete B-token envelope by an accepted
  // witness: either the canonical A-cover mapping or a deterministic
  // consensus-rescued envelope. If no such envelope exists, do not synthesize a
  // weaker realization; request the explicit terminal fallback instead.
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

  // Record the precise realization witness that will justify slicing B for this
  // include: which include was realized, which A cover was replaced, and which
  // B-token envelope supplied the materialized text.
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

  // Build the normalized accepted-result carrier before returning the sliced
  // text, so callers that emit this realization can attach the same witness and
  // proof-discharge metadata to the final edit.
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
  // Include materialization is memoized by include id. A parent may reach the
  // same child through recursive expansion, so avoid rebuilding already
  // materialized text.
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

  // Start from the header's original source bytes. Some callers preseed
  // `includeExpansion` with raw header text; otherwise load it
  // deterministically from the resolved header path.
  std::string bytes;
  if (auto it = includeExpansion.find(includeId); it != includeExpansion.end())
    bytes = it->second;
  if (bytes.empty()) {
    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(headerPath));
    if (!bufOrErr) {
      fatal("include/mat", "failed to read header: {0} ({1})", headerPath,
            bufOrErr.getError().message());
    }

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

  // Accumulate all local edits in this header byte space, then apply them once
  // at the end. This lets include edits, macro patches, and child include
  // materializations participate in one overlap/resync pass.
  std::vector<TextEdit> edits;

  // 1) Macro patches owned by this include. Macro invocation ranges are already
  // recorded in the owner file's byte space, so they can be converted directly
  // into local header TextEdits.
  if (auto it = macroPatchesByOwner.find(includeId);
      it != macroPatchesByOwner.end()) {
    for (const auto &mp : it->second) {
      uint64_t mpEnd = stringutils::extendChainedCallEnd(
          StringRef(bytes), mp.invEnd, mp.replacement);
      if (mpEnd != mp.invEnd) {
        debug("macro/chain",
              "inc#{0} extend chained callsite [{1},{2}) -> [{1},{3})", inc->id,
              mp.invStart, mp.invEnd, mpEnd);
      }

      debug("include/mat", "inc#{0} macroPatch inv=[{1},{2}) replLen={3}",
            inc->id, mp.invStart, mpEnd, mp.replacement.size());

      // Apply local line-resync policy before staging the edit, and carry the
      // accepted macro proof metadata to the emitted TextEdit.
      ResyncOutcome ro = ApplyResyncOrPend(bytes, mp.invStart, mpEnd,
                                           mp.replacement, headerPath);
      TextEdit edit{mp.invStart,
                    mpEnd,
                    std::move(ro.text),
                    std::move(ro.pending),
                    MacroPatchRemainsExpanded(mp)
                        ? std::make_optional(GetRootMacroId(mp.macroId))
                        : std::nullopt,
                    {}};
      AttachAcceptedResultCarrier(edit, BuildAcceptedEmittedMacroCandidate(mp));
      edits.push_back(std::move(edit));
    }
  }

  // 2) Include-preserving edits owned by this include. If those edits cannot be
  // discharged as local anchored edits, materialize the whole include directly
  // from B using the include-realization proof path.
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
        TextEdit edit{te.start,           te.end,
                      std::move(ro.text), std::move(ro.pending),
                      std::nullopt,       {}};
        edit.acceptedResults = std::move(te.acceptedResults);
        edits.push_back(std::move(edit));
      }
    } else {
      debug("include/mat", "inc#{0} has no include patches", inc->id);
    }
  } else {
    debug("include/mat", "inc#{0} has no include patches", inc->id);
  }

  // Return true if this include subtree contains any work that requires
  // materialization: local include edits, local macro patches, or descendant
  // includes with work. Clean child subtrees are left as spelled directives.
  auto hasDescendantWork = [&](auto &&self, uint64_t id) -> bool {
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

  // 3) Materialize child includes only when their subtree has work. For each
  // dirty child, recursively build the child's fully materialized text and then
  // replace the child's include directive in this header.
  if (auto it = children.find(includeId); it != children.end()) {
    for (const auto *child : it->second) {
      const bool todo = hasDescendantWork(hasDescendantWork, child->id);
      debug("include/mat",
            "inc#{0} -> child#{1} target={2} resolved={3} site=[{4},{5}) "
            "expand={6}",
            inc->id, child->id, child->target, child->resolvedPath,
            child->siteB, child->siteE, todo ? "YES" : "NO");
      if (!todo) {
        // No descendant edits depend on this child, so preserve the original
        // include directive instead of expanding it unnecessarily.
        continue;
      }

      // Depth-first materialization ensures the child replacement is already
      // complete before it is wrapped and inserted into the parent header.
      MaterializeIncludeExpansion(child->id, perInclude, macroPatchesByOwner,
                                  children, includeExpansion,
                                  includeExpansionAcceptedResults,
                                  appliedExpandedMacroRootIds);

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
        // Wrap the materialized child body with #line directives so diagnostics
        // and line-sensitive builtins resume in the child and then return to
        // the parent header after the include directive.
        std::string childHeaderPath = resolveHeaderPath(*child);
        std::string wrapped = lineDirs_.WrapIncludeExpansion(
            childHeaderPath, headerPath,
            stringutils::lineAtOffset(bytes, child->siteE), childText);
        TextEdit edit{siteStart,    siteEnd,      std::move(wrapped),
                      std::nullopt, std::nullopt, {}};

        // Preserve the child's accepted-result carrier if the child was
        // materialized through a specific realization path; otherwise record
        // the generic materialized-expansion carrier for this child include.
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
                llvm::formatv("degenerate child replace site for parent inc#{0}"
                              " child#{1} site=[{2},{3})",
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

  // Apply all staged edits in this header. If application requests terminal
  // fallback, leave this include empty so callers do not consume a partial
  // materialization.
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
    // Treat malformed declaration spans as zero-length for tie-breaking rather
    // than allowing unsigned underflow.
    if (d->span.end <= d->span.begin)
      return 0;
    return d->span.end - d->span.begin;
  };

  for (const auto &d : inc.decls) {
    const uint64_t dLo = d.span.begin;
    const uint64_t dHi = d.span.end;
    const uint64_t curSpanLen = dHi - dLo;

    // For a pure insertion, attach the patch to the smallest declaration whose
    // closed declaration range contains the insertion point. The end boundary is
    // intentionally inclusive so insertions immediately after a declaration body
    // still belong to that declaration.
    if (aLo == aHi) {
      if (aLo >= dLo && aLo <= dHi) {
        if (!bestCover || curSpanLen < getSpanLen(bestCover))
          bestCover = &d;
      }
      continue;
    }

    // For non-empty patches, prefer a declaration that fully contains the
    // edited A interval. If no declaration covers the interval, fall back to the
    // smallest declaration that merely overlaps it.
    const bool covers = (aLo >= dLo && aHi <= dHi);
    const bool overlaps = (aLo < dHi && aHi > dLo);

    if (covers) {
      if (!bestCover || curSpanLen < getSpanLen(bestCover))
        bestCover = &d;
    } else if (overlaps) {
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

  struct HeaderMacroStateDirectivePiece {
    const RefoldModel::MacroDirective *directive = nullptr;
    uint64_t begin = 0;
    uint64_t end = 0;
    std::string name;
  };

  struct HeaderPreservedGapPiece {
    enum class Kind {
      MacroStateDirective,
      ZeroTokenConditionalGroup,
      ZeroTokenMacroInvocation,
      ZeroTokenChildInclude
    };

    Kind kind = Kind::MacroStateDirective;
    const RefoldModel::MacroDirective *directive = nullptr;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t id = 0;
  };

  // Recover the macro identifier from a recorded macro-state directive.  Keep
  // this parser strict: the preservation proof below is only for complete
  // `#define NAME ...` / `#undef NAME` directive lines recorded by the producer.
  auto parseHeaderMacroStateDirectiveName =
      [](StringRef text,
         StringRef expectedSubkind) -> std::optional<std::string> {
    // Accept leading indentation, but require the directive spelling itself to
    // start with a real preprocessor marker from the recorded directive text.
    StringRef s = text.ltrim();
    if (!s.consume_front("#"))
      return std::nullopt;
    s = s.ltrim();

    // `MacroDirective::subkind` stores the spelling with the leading '#'.
    // Drop it here and require a standalone directive keyword, not a prefix of
    // some longer identifier-like token.
    StringRef keyword = expectedSubkind.drop_front();
    if (!s.consume_front(keyword))
      return std::nullopt;
    if (!s.empty() && stringutils::isIdentPart(s.front()))
      return std::nullopt;

    // The first identifier after #define/#undef is the macro state key.  For a
    // function-like define, this stops before the opening paren, which is the
    // name spelling needed for replacement-payload liveness checks.
    s = s.ltrim();
    if (s.empty() || !stringutils::isIdentStart(s.front()))
      return std::nullopt;

    size_t end = 1;
    while (end < s.size() && stringutils::isIdentPart(s[end]))
      ++end;
    return s.take_front(end).str();
  };

  // Return true if `text` contains `name` as a real raw identifier token.  This
  // prevents after-replacement directive preservation from accidentally making
  // the B-derived replacement payload observe a macro-state transition that was
  // not visible while B was produced.
  auto rawIdentifierAppearsInHeaderReplacement = [&](StringRef name,
                                                     StringRef text) {
    if (name.empty() || text.empty())
      return false;

    // Use Clang's raw lexer rather than substring matching so comments,
    // literals, and identifier substrings do not falsely count as macro uses.
    const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
    std::string lexBuf = text.str();
    lexBuf.push_back('\0');

    const char *bufStart = lexBuf.data();
    const char *bufEnd = bufStart + text.size();
    Lexer lexer(baseLoc, lexLang_, bufStart, bufStart, bufEnd);
    lexer.SetCommentRetentionState(true);

    Token token;
    while (true) {
      lexer.LexFromRawLexer(token);
      if (token.is(tok::eof))
        return false;
      if (token.is(tok::comment))
        continue;
      if (!token.is(tok::raw_identifier) && !token.is(tok::identifier))
        continue;

      // Compare the original replacement slice for the token.  The lexer token
      // kind tells us this is an identifier; the slice check proves it is the
      // specific macro name whose state would be reintroduced.
      const size_t localBegin = tokenOffsetFromBase(token, baseLoc);
      const size_t localEnd = tokenEndOffsetFromBase(token, baseLoc);
      if (localEnd < localBegin || localEnd > text.size())
        continue;
      if (text.slice(localBegin, localEnd) == name)
        return true;
    }
  };

  // Recover the complete physical source line for a macro-state directive that
  // was spelled in this include expansion.  MacroDirective::siteB is anchored at
  // the macro name, so translate through the recorded directive text and require
  // an exact byte match in the header buffer before using the interval.
  auto headerMacroStateDirectiveInterval =
      [&](const RefoldModel::MacroDirective &directive)
          -> std::optional<HeaderMacroStateDirectivePiece> {
    // Only preserve directives physically spelled by this include expansion.
    // A same-path directive from a different include instance has a different
    // macro-state lifetime and cannot be used to tile this header gap.
    if (!PathsEqual(directive.sitePath, file))
      return std::nullopt;
    if (!directive.ownerIncludeId || *directive.ownerIncludeId != ie.include->id)
      return std::nullopt;

    // This proof class preserves macro state only.  Conditional directives,
    // includes, pragmas, and other PP structure need their own owners/proofs.
    if (directive.subkind != "#define" && directive.subkind != "#undef")
      return std::nullopt;

    std::optional<std::string> name = parseHeaderMacroStateDirectiveName(
        directive.text, directive.subkind);
    if (!name)
      return std::nullopt;

    // Re-scan the recorded directive text to find the byte offset of the macro
    // name inside that text.  The producer records siteB at the name spelling,
    // so this offset lets us recover the directive line's start in the header.
    StringRef text = directive.text;
    size_t pos = 0;
    stringutils::skipNonNewlineWs(text, pos);
    if (pos >= text.size() || text[pos] != '#')
      return std::nullopt;
    ++pos;
    stringutils::skipNonNewlineWs(text, pos);

    StringRef keyword = directive.subkind.drop_front();
    if (!text.substr(pos).starts_with(keyword))
      return std::nullopt;
    pos += keyword.size();
    if (pos < text.size() && stringutils::isIdentPart(text[pos]))
      return std::nullopt;
    stringutils::skipNonNewlineWs(text, pos);

    const size_t nameTextBegin = pos;
    if (pos >= text.size() || !stringutils::isIdentStart(text[pos]))
      return std::nullopt;
    ++pos;
    while (pos < text.size() && stringutils::isIdentPart(text[pos]))
      ++pos;

    if (directive.siteB < nameTextBegin)
      return std::nullopt;

    // Translate from the macro-name anchor back to the full directive interval
    // and byte-verify it.  This rejects stale metadata, partial lines, and any
    // directive text that cannot be proven to be exactly the header spelling.
    const uint64_t fileBegin = directive.siteB - nameTextBegin;
    const uint64_t fileEnd = fileBegin + text.size();
    if (fileBegin >= fileEnd || fileEnd > headerText.size())
      return std::nullopt;
    if (StringRef(headerText).slice(fileBegin, fileEnd) != text)
      return std::nullopt;

    HeaderMacroStateDirectivePiece piece;
    piece.directive = &directive;
    piece.begin = fileBegin;
    piece.end = fileEnd;
    piece.name = std::move(*name);
    return piece;
  };

  // Return true iff a recorded conditional group in this materialized header
  // can be preserved as a complete zero-token gap piece. This is the
  // header-owned counterpart of the TU zero-token conditional-gap proof: the
  // group bytes may be carried forward only when the producer recorded no
  // selected-arm PP material and no active source artifacts inside the group.
  auto headerConditionalGroupIsPreservableGap =
      [&](const RefoldModel::CondGroup &group, uint64_t gapBegin,
          uint64_t gapEnd) -> bool {
    if (!PathsEqual(group.file, file))
      return false;
    if (!group.parentIncludeId || *group.parentIncludeId != ie.include->id)
      return false;
    if (group.groupB >= group.groupE || group.groupE > headerText.size())
      return false;
    if (group.groupB < gapBegin || gapEnd < group.groupE)
      return false;
    if (!stringutils::beginsLineAfterWs(headerText, group.groupB))
      return false;

    for (const RefoldModel::CondArm &arm : group.arms)
      if (arm.selected && arm.span && arm.span->IsValid() &&
          arm.span->begin < arm.span->end)
        return false;

    auto insideGroup = [&](uint64_t b, uint64_t e) {
      return group.groupB <= b && b < e && e <= group.groupE;
    };

    // Do not hide recorded active source artifacts inside an opaque preserved
    // conditional group. Inactive-arm text is normally not recorded by the
    // producer; recorded includes/macros/directives/pragmas require their own
    // proof and therefore block this zero-token gap class.
    for (const auto &inc : model_.GetIncludes())
      if (PathsEqual(inc.sitePath, file) && inc.parent && *inc.parent == ie.include->id &&
          insideGroup(inc.siteB, inc.siteE))
        return false;
    for (const auto &macro : model_.GetMacroInvocations())
      if (macro.invFile && PathsEqual(*macro.invFile, file) && macro.invB &&
          macro.invE && insideGroup(*macro.invB, *macro.invE))
        return false;
    for (const auto &directive : model_.GetMacroDirectives())
      if (PathsEqual(directive.sitePath, file) && directive.ownerIncludeId &&
          *directive.ownerIncludeId == ie.include->id &&
          insideGroup(directive.siteB, directive.siteE))
        return false;
    for (const auto &pragma : model_.GetPragmas())
      if (PathsEqual(pragma.sitePath, file) &&
          insideGroup(pragma.siteB, pragma.siteE))
        return false;

    return true;
  };

  auto preservedGapPieceText = [&](const HeaderPreservedGapPiece &piece) {
    // Macro-state directives are preserved from the producer's recorded text so
    // spelling such as leading whitespace and line continuations matches the
    // directive record, not a best-effort slice reconstructed from ownership.
    if (piece.kind == HeaderPreservedGapPiece::Kind::MacroStateDirective &&
        piece.directive)
      return piece.directive->text.str();

    // Other preserved gap pieces are source intervals in the materialized
    // header buffer.  Require a valid in-buffer range before copying bytes into
    // the replacement payload.
    if (piece.end <= headerText.size() && piece.begin <= piece.end)
      return StringRef(headerText).slice(piece.begin, piece.end).str();
    return std::string();
  };

  // File-byte interval for the replacement list of the #define that created a
  // header macro invocation.  The interval starts after the macro name and
  // parameter list, because only the replacement list must be tiled by nested
  // zero-token invocations and trivia.
  struct HeaderMacroReplacementList {
    const RefoldModel::MacroDirective *directive = nullptr;
    size_t nameTextBegin = 0;
    size_t replacementTextBegin = 0;
    uint64_t fileBase = 0;
    uint64_t fileBegin = 0;
    uint64_t fileEnd = 0;
  };

  auto findHeaderMacroDefinitionDirective =
      [&](const RefoldModel::MacroInvocation &m)
          -> const RefoldModel::MacroDirective * {
    // The producer records the defining directive id for macro invocations.
    // Treat a missing/stale reference as unproved instead of guessing by name.
    if (!m.definitionDirectiveId)
      return nullptr;
    for (const auto &directive : model_.GetMacroDirectives())
      if (directive.id == *m.definitionDirectiveId)
        return &directive;
    return nullptr;
  };

  // Parse the defining #define enough to recover the replacement-list source
  // interval.  Header zero-token macro gaps use this to prove that a wrapper
  // invocation's replacement list is tiled only by nested zero-token calls and
  // trivia, rather than by unrecorded source-bearing replacement text.
  auto headerMacroDefinitionReplacementList =
      [&](const RefoldModel::MacroInvocation &m)
          -> std::optional<HeaderMacroReplacementList> {
    const RefoldModel::MacroDirective *definition =
        findHeaderMacroDefinitionDirective(m);
    if (!definition || definition->subkind != "#define")
      return std::nullopt;

    StringRef text = definition->text;
    size_t pos = 0;

    // Validate the directive spelling rather than assuming MacroDirective::text
    // is well-formed.  This keeps the zero-token proof fail-closed if the
    // producer ever records an unexpected directive form.
    stringutils::skipNonNewlineWs(text, pos);
    if (pos >= text.size() || text[pos] != '#')
      return std::nullopt;
    ++pos;
    stringutils::skipNonNewlineWs(text, pos);

    if (!text.substr(pos).starts_with("define"))
      return std::nullopt;
    pos += StringRef("define").size();
    if (pos < text.size() && stringutils::isIdentPart(text[pos]))
      return std::nullopt;
    stringutils::skipNonNewlineWs(text, pos);

    // siteB is anchored at the macro name, so remember the name's offset in
    // directive.text.  That lets us translate replacement-list offsets back to
    // file bytes and byte-verify nested invocation callsites.
    const size_t nameTextBegin = pos;
    if (!text.substr(pos).starts_with(m.name))
      return std::nullopt;
    pos += m.name.size();
    if (pos < text.size() && stringutils::isIdentPart(text[pos]))
      return std::nullopt;

    if (m.subkind == "func") {
      // Function-like macros may have parameters between the name and the
      // replacement list.  Skip the balanced parameter list so fileBegin lands
      // on the first byte that can contribute replacement-list source.
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

    if (definition->siteB < nameTextBegin)
      return std::nullopt;

    HeaderMacroReplacementList result;
    result.directive = definition;
    result.nameTextBegin = nameTextBegin;
    result.replacementTextBegin = pos;
    // Convert directive.text-local offsets into materialized header bytes.
    // siteE bounds the complete directive line recorded by the producer.
    result.fileBase = definition->siteB - nameTextBegin;
    result.fileBegin = result.fileBase + pos;
    result.fileEnd = definition->siteE;
    if (result.fileBegin > result.fileEnd ||
        result.fileEnd > result.fileBase + text.size())
      return std::nullopt;
    return result;
  };

  auto headerMacroInvocationHasMaterializedPPTokens =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    // Any materialized expansion, stringify, or paste span means this invocation
    // is not a source-neutral gap.  Such invocations must be represented by
    // normal macro realization machinery, not by copying their callsite through
    // a header gap.
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

  std::function<bool(const RefoldModel::MacroInvocation &,
                     DenseSet<uint64_t> &)>
      headerMacroInvocationIsSourceNeutralZeroTokenImpl;
  headerMacroInvocationIsSourceNeutralZeroTokenImpl =
      [&](const RefoldModel::MacroInvocation &m,
          DenseSet<uint64_t> &visiting) -> bool {
    // A zero-token invocation must be token-neutral at every recorded expansion
    // boundary.  Cycles are rejected even though real preprocessing would have
    // disabled recursive expansion, because the source-neutral proof needs a
    // finite acyclic tiling of replacement-list bytes.
    if (headerMacroInvocationHasMaterializedPPTokens(m))
      return false;
    if (!visiting.insert(m.id).second)
      return false;

    auto finish = [&](bool result) {
      visiting.erase(m.id);
      return result;
    };

    std::optional<HeaderMacroReplacementList> replacement =
        headerMacroDefinitionReplacementList(m);
    if (!replacement)
      return finish(false);

    struct ChildPiece {
      uint64_t begin = 0;
      uint64_t end = 0;
      const RefoldModel::MacroInvocation *child = nullptr;
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
        [&](uint64_t begin, uint64_t end, StringRef rangeFile) -> bool {
      if (begin > end)
        return false;

      SmallVector<ChildPiece, 8> children;
      for (const auto &candidate : model_.GetMacroInvocations()) {
        if (!candidate.callerMacroId || *candidate.callerMacroId != m.id)
          continue;

        // The range can be either the defining directive's replacement list or
        // the invocation argument text in this materialized header.  Only
        // nested invocations physically spelled in that same source range are
        // allowed to account for non-trivia bytes.
        if (!candidate.invFile || candidate.invFile->empty() ||
            !PathsEqual(*candidate.invFile, rangeFile))
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
        if (PathsEqual(rangeFile, replacement->directive->sitePath)) {
          if (cursor < replacement->fileBase || piece.begin < cursor ||
              piece.begin > replacement->fileBase +
                                replacement->directive->text.size())
            return false;
          const size_t gapBegin =
              static_cast<size_t>(cursor - replacement->fileBase);
          const size_t gapEnd =
              static_cast<size_t>(piece.begin - replacement->fileBase);
          gap = replacement->directive->text.slice(gapBegin, gapEnd);
        } else if (PathsEqual(rangeFile, file)) {
          if (piece.begin > headerText.size())
            return false;
          gap = StringRef(headerText).slice(cursor, piece.begin);
        } else {
          return false;
        }

        if (!isWsOrCompleteCommentTrivia(gap))
          return false;
        if (!headerMacroInvocationIsSourceNeutralZeroTokenImpl(*piece.child,
                                                              visiting))
          return false;
        cursor = piece.end;
      }

      StringRef suffix;
      if (PathsEqual(rangeFile, replacement->directive->sitePath)) {
        if (cursor < replacement->fileBase || end < cursor ||
            end > replacement->fileBase + replacement->directive->text.size())
          return false;
        const size_t suffixBegin =
            static_cast<size_t>(cursor - replacement->fileBase);
        const size_t suffixEnd =
            static_cast<size_t>(end - replacement->fileBase);
        suffix = replacement->directive->text.slice(suffixBegin, suffixEnd);
      } else if (PathsEqual(rangeFile, file)) {
        if (end > headerText.size())
          return false;
        suffix = StringRef(headerText).slice(cursor, end);
      } else {
        return false;
      }

      return isWsOrCompleteCommentTrivia(suffix);
    };

    StringRef replacementText =
        replacement->directive->text.substr(replacement->replacementTextBegin);

    // A literal empty replacement list is source-neutral.
    if (isWsOrCompleteCommentTrivia(replacementText))
      return finish(true);

    // First try the definition-site proof that existed for wrappers such as
    // `#define WRAP_EMPTY() EMPTY()`: every non-trivia byte in the replacement
    // list must be covered by a nested macro invocation that recursively proves
    // zero-token neutrality.  The formal-parameter proof below is an additional
    // admissible case, not a replacement for this one.
    if (sourceRangeIsTiledByNeutralNestedMacros(replacement->fileBegin,
                                                replacement->fileEnd,
                                                replacement->directive->sitePath))
      return finish(true);

    if (m.subkind == "func") {
      if (m.defParams.empty() || m.invArgRanges.size() < m.defParams.size())
        return finish(false);

      DenseMap<StringRef, unsigned> paramIndexByName;
      for (unsigned i = 0, e = m.defParams.size(); i != e; ++i) {
        // Variadic substitution is deliberately outside this proof.  It can
        // introduce separators and empty/non-empty distinctions that are not
        // represented by this simple per-formal range check.
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
          const size_t commentEnd = text.find("*/", pos + 2);
          if (commentEnd == StringRef::npos)
            return finish(false);
          pos = commentEnd + 2;
          continue;
        }
        if (tail.starts_with("//")) {
          const size_t commentEnd = text.find('\n', pos + 2);
          pos = commentEnd == StringRef::npos ? text.size() : commentEnd + 1;
          continue;
        }

        // The only source-bearing replacement-list tokens admitted for a
        // function-like zero-token wrapper are formal parameter names.  After
        // substitution, each used argument must independently prove to be a
        // zero-token source-neutral range.
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

    SmallVector<ChildPiece, 8> children;
    for (const auto &candidate : model_.GetMacroInvocations()) {
      // Nested macro invocations are the only non-trivia source allowed in the
      // replacement list.  Require them to be direct children of this expansion
      // and to live within the defining directive's replacement-list interval.
      if (!candidate.callerMacroId || *candidate.callerMacroId != m.id)
        continue;
      if (!candidate.invFile || candidate.invFile->empty() ||
          !candidate.invB || !candidate.invE || *candidate.invB >= *candidate.invE)
        return finish(false);
      if (!PathsEqual(*candidate.invFile, replacement->directive->sitePath))
        return finish(false);
      if (*candidate.invB < replacement->fileBegin ||
          replacement->fileEnd < *candidate.invE)
        return finish(false);
      children.push_back({*candidate.invB, *candidate.invE, &candidate});
    }

    if (children.empty())
      return finish(false);

    // Tile the replacement list from left to right.  Sorting gives deterministic
    // overlap detection and ensures every byte between child invocations is
    // checked as trivia/comment-only source.
    sortChildren(children);

    uint64_t cursor = replacement->fileBegin;
    for (const ChildPiece &piece : children) {
      // Overlapping children or source-bearing text between children defeats the
      // proof.  Each child must recursively prove that it also expands to no PP
      // material.
      if (piece.begin < cursor)
        return finish(false);
      const size_t gapBegin =
          static_cast<size_t>(cursor - replacement->fileBase);
      const size_t gapEnd =
          static_cast<size_t>(piece.begin - replacement->fileBase);
      if (!isWsOrCompleteCommentTrivia(
              replacement->directive->text.slice(gapBegin, gapEnd)))
        return finish(false);
      if (!headerMacroInvocationIsSourceNeutralZeroTokenImpl(*piece.child,
                                                            visiting))
        return finish(false);
      cursor = piece.end;
    }

    // After the last nested zero-token invocation, the remaining replacement
    // list suffix must also be trivia-only.  Otherwise the macro has unmodeled
    // source-bearing replacement text and cannot be copied as a neutral gap.
    const size_t suffixBegin =
        static_cast<size_t>(cursor - replacement->fileBase);
    StringRef suffix = replacement->directive->text.slice(
        suffixBegin, replacement->directive->text.size());
    return finish(isWsOrCompleteCommentTrivia(suffix));
  };

  auto headerMacroInvocationIsPreservableGap =
      [&](const RefoldModel::MacroInvocation &m, uint64_t gapBegin,
          uint64_t gapEnd) -> bool {
    // The invocation must be physically spelled in this materialized header gap
    // and owned by the include instance currently being rewritten.
    if (!m.invFile || m.invFile->empty() || !PathsEqual(*m.invFile, file))
      return false;
    if (!m.ownerIncludeId || *m.ownerIncludeId != ie.include->id)
      return false;
    if (!m.invB || !m.invE || *m.invB >= *m.invE)
      return false;
    if (*m.invB < gapBegin || gapEnd < *m.invE)
      return false;
    if (*m.invE > headerText.size())
      return false;
    // Byte-verify the recorded callsite text before preserving it.  This keeps
    // stale or ambiguous refold-map offsets from authorizing a source copy.
    if (m.invText && StringRef(headerText).slice(*m.invB, *m.invE) != *m.invText)
      return false;

    DenseSet<uint64_t> visiting;
    return headerMacroInvocationIsSourceNeutralZeroTokenImpl(m, visiting);
  };

  auto headerIncludeIsDescendantOf =
      [&](const RefoldModel::IncludeItem &candidate,
          const RefoldModel::IncludeItem &root) -> bool {
    // Walk producer include-parent links rather than inferring ancestry from
    // paths.  Multiple include instances of the same file may exist, and the
    // preservation proof is instance-specific.
    std::optional<uint64_t> cur = candidate.parent;
    while (cur) {
      if (*cur == root.id)
        return true;
      const RefoldModel::IncludeItem *parent = nullptr;
      for (const auto &inc : model_.GetIncludes())
        if (inc.id == *cur) {
          parent = &inc;
          break;
        }
      if (!parent)
        return false;
      cur = parent->parent;
    }
    return false;
  };

  auto macroDirectiveNameFromRecordedText =
      [&](const RefoldModel::MacroDirective &directive)
          -> std::optional<std::string> {
    // Only macro-state transitions matter for replacement-payload exposure.
    // Other directives in a zero-token include subtree are irrelevant here.
    if (directive.subkind != "#define" && directive.subkind != "#undef")
      return std::nullopt;
    return parseHeaderMacroStateDirectiveName(directive.text,
                                              directive.subkind);
  };

  auto headerZeroTokenChildIncludeIsPreservableGap =
      [&](const RefoldModel::IncludeItem &child, uint64_t gapBegin,
          uint64_t gapEnd, StringRef replacement) -> bool {
    // Preserve only complete direct child include directives spelled in the
    // current parent header gap.  Nested descendants are checked below as part
    // of proving that the entire child include subtree is token-neutral.
    if (!child.parent || *child.parent != ie.include->id)
      return false;
    if (!PathsEqual(child.sitePath, file))
      return false;
    if (child.siteB >= child.siteE || child.siteE > headerText.size())
      return false;
    if (child.siteB < gapBegin || gapEnd < child.siteE)
      return false;
    if (child.cover.IsValid())
      return false;
    if (child.text.empty() ||
        StringRef(headerText).slice(child.siteB, child.siteE) != child.text)
      return false;

    // A zero-token include subtree may contain nested include directives, but
    // none of those descendants may contribute materialized PP tokens.
    for (const auto &inc : model_.GetIncludes()) {
      if (&inc == &child)
        continue;
      if (!headerIncludeIsDescendantOf(inc, child))
        continue;
      if (inc.cover.IsValid())
        return false;
    }

    for (const auto &macro : model_.GetMacroInvocations()) {
      // Macro calls inside the include subtree are allowed only when they are
      // themselves zero-token.  Any expansion, paste, or stringify output means
      // the include is not a neutral gap owner.
      if (!macro.ownerIncludeId)
        continue;
      bool ownedByChild = *macro.ownerIncludeId == child.id;
      if (!ownedByChild)
        for (const auto &inc : model_.GetIncludes())
          if (inc.id == *macro.ownerIncludeId &&
              headerIncludeIsDescendantOf(inc, child)) {
            ownedByChild = true;
            break;
          }
      if (ownedByChild && headerMacroInvocationHasMaterializedPPTokens(macro))
        return false;
    }

    for (const auto &group : model_.GetConds()) {
      // Conditional groups inside the include subtree are acceptable only if no
      // selected arm produced PP material.  Otherwise preserving the include as
      // a zero-token gap would silently discard live conditional output.
      if (!group.parentIncludeId)
        continue;
      bool ownedByChild = *group.parentIncludeId == child.id;
      if (!ownedByChild)
        for (const auto &inc : model_.GetIncludes())
          if (inc.id == *group.parentIncludeId &&
              headerIncludeIsDescendantOf(inc, child)) {
            ownedByChild = true;
            break;
          }
      if (!ownedByChild)
        continue;
      for (const RefoldModel::CondArm &arm : group.arms)
        if (arm.span && arm.span->IsValid() && arm.span->begin < arm.span->end)
          return false;
    }

    for (const auto &directive : model_.GetMacroDirectives()) {
      // Macro-state transitions in a preserved zero-token include become visible
      // after the replacement payload is emitted.  Reject the preservation if
      // the B-derived replacement text mentions that macro name, because then
      // moving the transition could change how the replacement preprocesses.
      if (!directive.ownerIncludeId)
        continue;
      bool ownedByChild = *directive.ownerIncludeId == child.id;
      if (!ownedByChild)
        for (const auto &inc : model_.GetIncludes())
          if (inc.id == *directive.ownerIncludeId &&
              headerIncludeIsDescendantOf(inc, child)) {
            ownedByChild = true;
            break;
          }
      if (!ownedByChild)
        continue;
      std::optional<std::string> name =
          macroDirectiveNameFromRecordedText(directive);
      if (name && rawIdentifierAppearsInHeaderReplacement(*name, replacement))
        return false;
    }

    return true;
  };


  auto ppSpanInsideHeaderMaterial = [&](const RefoldModel::PPSpan &span,
                                        uint64_t materialBeginA,
                                        uint64_t materialEndA) {
    if (!span.IsValid() || span.begin >= span.end)
      return true;
    return materialBeginA <= span.begin && span.end <= materialEndA;
  };

  auto headerArmIsSameOrNestedUnder = [&](const RefoldModel::ArmRef &candidate,
                                          uint64_t ancestorArmId) {
    if (!candidate.group || !candidate.arm)
      return false;
    if (candidate.arm->id == ancestorArmId)
      return true;

    // FindArmRefAtPP() reports the innermost selected arm that owns a token.
    // When proving that an outer header conditional group is wholly consumed,
    // tokens in nested selected arms still count as effective material of the
    // outer arm. Walk parent-arm links until either the queried arm is found or
    // the chain leaves the nested conditional tree.
    const RefoldModel::CondGroup *group = candidate.group;
    while (group && group->parentArmId) {
      if (*group->parentArmId == ancestorArmId)
        return true;
      std::optional<RefoldModel::ArmRef> parent =
          model_.GetArmRefById(*group->parentArmId);
      if (!parent || !parent->group)
        break;
      group = parent->group;
    }
    return false;
  };

  auto headerSelectedArmEffectiveMaterialInside =
      [&](const RefoldModel::CondArm &arm, uint64_t materialBeginA,
          uint64_t materialEndA) {
    bool sawEffectiveMaterial = false;
    for (uint64_t pp = 0; pp < aToks_.size(); ++pp) {
      std::optional<RefoldModel::ArmRef> owner = model_.FindArmRefAtPP(pp);
      if (!owner || !headerArmIsSameOrNestedUnder(*owner, arm.id))
        continue;

      sawEffectiveMaterial = true;
      if (pp < materialBeginA || pp >= materialEndA)
        return false;
    }

    // Prefer effective PP ownership when the map can answer it, because the
    // direct arm span may omit include- or macro-produced tokens controlled by
    // this arm. Fall back to the direct span only for older/sparse maps.
    if (sawEffectiveMaterial)
      return true;
    return arm.span && arm.span->IsValid() && arm.span->begin < arm.span->end &&
           ppSpanInsideHeaderMaterial(*arm.span, materialBeginA, materialEndA);
  };

  auto headerConditionalGroupIsConsumedSourceEnvelope =
      [&](const RefoldModel::CondGroup &group, uint64_t materialBeginA,
          uint64_t materialEndA) {
    if (!PathsEqual(group.file, file))
      return false;
    if (!group.parentIncludeId || *group.parentIncludeId != ie.include->id)
      return false;
    if (group.groupB >= group.groupE || group.groupE > headerText.size())
      return false;

    // This is the source-bearing counterpart of the zero-token conditional gap
    // proof.  The complete directive group may become a source-envelope piece
    // only when the selected arm's effective PP material is wholly consumed by
    // the A-side replacement material.  Otherwise copying/removing the group
    // would move surviving tokens across conditional-control structure.
    for (const RefoldModel::CondArm &arm : group.arms) {
      if (!arm.selected)
        continue;
      if (headerSelectedArmEffectiveMaterialInside(arm, materialBeginA,
                                                  materialEndA))
        return true;
    }
    return false;
  };

  auto headerMacroInvocationIsConsumedSourceEnvelope =
      [&](const RefoldModel::MacroInvocation &m, uint64_t materialBeginA,
          uint64_t materialEndA) {
    if (!m.invFile || m.invFile->empty() || !PathsEqual(*m.invFile, file))
      return false;
    if (!m.ownerIncludeId || *m.ownerIncludeId != ie.include->id)
      return false;
    if (!m.invB || !m.invE || *m.invB >= *m.invE ||
        *m.invE > headerText.size())
      return false;
    if (m.invText && StringRef(headerText).slice(*m.invB, *m.invE) != *m.invText)
      return false;

    bool sawMaterializedSpan = false;
    auto checkSpan = [&](const RefoldModel::PPSpan &span) {
      if (!span.IsValid() || span.begin >= span.end)
        return true;
      sawMaterializedSpan = true;
      return ppSpanInsideHeaderMaterial(span, materialBeginA, materialEndA);
    };

    // A source-bearing macro callsite is consumable only when every PP range
    // the producer attributes to the invocation is covered by the replacement
    // material. This prevents deleting a callsite whose expansion still has
    // surviving output elsewhere in B.
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

  auto headerMacroInvocationOverlapsMaterial =
      [&](const RefoldModel::MacroInvocation &m, uint64_t materialBeginA,
          uint64_t materialEndA) {
    if (!m.invFile || m.invFile->empty() || !PathsEqual(*m.invFile, file))
      return false;
    if (!m.ownerIncludeId || *m.ownerIncludeId != ie.include->id)
      return false;
    if (!m.cover.IsValid() || m.cover.begin >= m.cover.end)
      return false;
    return materialBeginA < m.cover.end && m.cover.begin < materialEndA;
  };

  auto headerConditionalGroupSelectedMaterialOverlaps =
      [&](const RefoldModel::CondGroup &group, uint64_t materialBeginA,
          uint64_t materialEndA) {
    if (!PathsEqual(group.file, file))
      return false;
    if (!group.parentIncludeId || *group.parentIncludeId != ie.include->id)
      return false;
    for (const RefoldModel::CondArm &arm : group.arms) {
      if (!arm.selected || !arm.span || !arm.span->IsValid() ||
          arm.span->begin >= arm.span->end)
        continue;
      if (materialBeginA < arm.span->end && arm.span->begin < materialEndA)
        return true;
    }
    return false;
  };

  auto headerLineDirectiveStartsAtPrefix = [&](uint64_t pos) -> bool {
    if (pos >= headerText.size())
      return true;
    if (stringutils::isBOL(StringRef(headerText), static_cast<size_t>(pos)))
      return true;

    size_t lineStart = static_cast<size_t>(pos);
    while (lineStart > 0 && headerText[lineStart - 1] != '\n')
      --lineStart;
    return stringutils::isIndentOnly(StringRef(headerText), lineStart,
                                     static_cast<size_t>(pos));
  };

  auto canStartHeaderLineDirectiveWithOptionalLeadingNewline =
      [&](uint64_t pos) -> bool {
    if (headerLineDirectiveStartsAtPrefix(pos))
      return true;
    return pos == 0 || headerText[pos - 1] != '\\';
  };

  auto replacementHeaderSuffixBoundaryAllowsDirectiveLine =
      [&](StringRef replacement, uint64_t sourceEnd) {
    if (sourceEnd > headerText.size())
      return false;

    // Do not insert a directive after a line splice or trailing backslash: the
    // `#` would be joined to the previous logical source line instead of
    // starting a standalone preprocessing directive.
    if (!replacement.empty()) {
      if (replacement.back() == '\n') {
        if (stringutils::isLineSplice(replacement, replacement.size() - 1))
          return false;
      } else if (replacement.back() == '\\') {
        return false;
      }
    }

    // Whitespace on either side is already a lexical separator.  Otherwise,
    // fall back to token-boundary checking so widening the edit cannot glue two
    // neighboring tokens into a different token spelling.
    if (replacement.empty() || stringutils::isWs(replacement.back()))
      return true;
    if (sourceEnd >= headerText.size() || stringutils::isWs(headerText[sourceEnd]))
      return true;

    std::optional<LexBoundaryToken> leftTok =
        lastLexToken(replacement, lexLang_);
    std::optional<LexBoundaryToken> rightTok =
        firstLexToken(StringRef(headerText).drop_front(sourceEnd), lexLang_);
    if (!leftTok || !rightTok)
      return true;

    return !needsLexicalSeparator(*leftTok, *rightTok, lexLang_);
  };


  auto tokenRangesHaveSameSpelling = [&](uint64_t aBegin, uint64_t aEnd,
                                           uint64_t bBegin, uint64_t bEnd) {
    if (aEnd < aBegin || bEnd < bBegin ||
        (aEnd - aBegin) != (bEnd - bBegin))
      return false;
    if (aEnd > static_cast<uint64_t>(aToks_.size()) ||
        bEnd > static_cast<uint64_t>(bToks_.size()))
      return false;
    for (uint64_t i = 0; i < aEnd - aBegin; ++i)
      if (aToks_[static_cast<size_t>(aBegin + i)].spelling !=
          bToks_[static_cast<size_t>(bBegin + i)].spelling)
        return false;
    return true;
  };

  auto includeOwnsDirective = [&](const RefoldModel::MacroDirective &directive,
                                  const RefoldModel::IncludeItem &root) {
    if (!directive.ownerIncludeId)
      return false;
    if (*directive.ownerIncludeId == root.id)
      return true;
    for (const auto &inc : model_.GetIncludes())
      if (inc.id == *directive.ownerIncludeId)
        return headerIncludeIsDescendantOf(inc, root);
    return false;
  };

  auto recordedMacroDirectiveMatchesOwnerFile =
      [&](const RefoldModel::MacroDirective &directive) {
    const RefoldModel::IncludeItem *owner = nullptr;
    if (!directive.ownerIncludeId)
      return false;
    for (const auto &inc : model_.GetIncludes())
      if (inc.id == *directive.ownerIncludeId) {
        owner = &inc;
        break;
      }
    if (!owner)
      return false;

    std::optional<std::string> name =
        macroDirectiveNameFromRecordedText(directive);
    if (!name)
      return false;

    StringRef text = directive.text;
    size_t pos = 0;
    stringutils::skipNonNewlineWs(text, pos);
    if (pos >= text.size() || text[pos] != '#')
      return false;
    ++pos;
    stringutils::skipNonNewlineWs(text, pos);

    StringRef keyword = directive.subkind.drop_front();
    if (!text.substr(pos).starts_with(keyword))
      return false;
    pos += keyword.size();
    if (pos < text.size() && stringutils::isIdentPart(text[pos]))
      return false;
    stringutils::skipNonNewlineWs(text, pos);

    const size_t nameTextBegin = pos;
    if (!text.substr(pos).starts_with(*name))
      return false;
    if (directive.siteB < nameTextBegin)
      return false;

    const uint64_t fileBegin = directive.siteB - nameTextBegin;
    const uint64_t fileEnd = fileBegin + text.size();
    const std::string ownerPath = resolveHeaderPath(*owner);
    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(ownerPath));
    if (!bufOrErr)
      return false;
    const MemoryBuffer &mb = **bufOrErr;
    StringRef ownerBytes(mb.getBufferStart(), mb.getBufferSize());
    if (fileBegin >= fileEnd || fileEnd > ownerBytes.size())
      return false;
    return ownerBytes.slice(fileBegin, fileEnd) == text;
  };

  auto collectMacroStatePreservationsFromIncludeSubtree =
      [&](const RefoldModel::IncludeItem &root, StringRef replacement,
          SmallVectorImpl<HeaderPreservedGapPiece> &out) {
    SmallVector<HeaderPreservedGapPiece, 4> pieces;
    for (const auto &directive : model_.GetMacroDirectives()) {
      if (directive.subkind != "#define" && directive.subkind != "#undef")
        continue;
      if (!includeOwnsDirective(directive, root))
        continue;

      std::optional<std::string> name =
          macroDirectiveNameFromRecordedText(directive);
      if (!name)
        return false;
      if (rawIdentifierAppearsInHeaderReplacement(*name, replacement))
        return false;
      if (!recordedMacroDirectiveMatchesOwnerFile(directive))
        return false;

      HeaderPreservedGapPiece piece;
      piece.kind = HeaderPreservedGapPiece::Kind::MacroStateDirective;
      piece.directive = &directive;
      piece.id = directive.id;
      pieces.push_back(std::move(piece));
    }

    llvm::sort(pieces, [](const HeaderPreservedGapPiece &lhs,
                          const HeaderPreservedGapPiece &rhs) {
      return lhs.id < rhs.id;
    });
    out.append(pieces.begin(), pieces.end());
    return true;
  };


  auto tryBuildConsumedHeaderConditionalCoalescedPatch =
      [&](size_t idx, IncludePatch &coalesced, size_t &skipThrough) {
    if (idx + 1 >= ie.patches.size())
      return false;

    const IncludePatch &first = ie.patches[idx];
    if (first.aStart >= first.aEnd)
      return false;

    // Some LCS shapes preserve a token from inside a selected conditional arm
    // (for example the '=' in `int middle = 99;`) while adjacent hunks delete
    // the rest of that arm.  Per-patch materialization cannot prove the whole
    // #if/#endif envelope because no individual patch covers the selected arm.
    // Merge the shortest adjacent patch run that does cover a complete selected
    // header conditional group, then let the normal header source-envelope
    // proof consume the group as one source piece.
    for (size_t endIdx = idx + 1; endIdx < ie.patches.size(); ++endIdx) {
      const IncludePatch &last = ie.patches[endIdx];
      if (last.include != first.include)
        break;
      if (last.ownerHasCondArmCert != first.ownerHasCondArmCert)
        break;
      if (last.ownerHasCondArmCert &&
          last.ownerCondArmIdCert != first.ownerCondArmIdCert)
        break;
      if (last.aStart >= last.aEnd || last.aEnd <= first.aStart)
        break;
      if (last.bEnd < first.bStart)
        break;

      const uint64_t materialBeginA = first.aStart;
      const uint64_t materialEndA = last.aEnd;
      bool consumesConditionalGroup = false;
      bool overlapsConditionalGroup = false;
      for (const auto &group : model_.GetConds()) {
        if (!headerConditionalGroupSelectedMaterialOverlaps(
                group, materialBeginA, materialEndA))
          continue;
        overlapsConditionalGroup = true;
        if (headerConditionalGroupIsConsumedSourceEnvelope(
                group, materialBeginA, materialEndA)) {
          consumesConditionalGroup = true;
          break;
        }
      }

      if (overlapsConditionalGroup && !consumesConditionalGroup)
        continue;
      if (!consumesConditionalGroup)
        continue;

      coalesced = first;
      coalesced.aEnd = materialEndA;
      coalesced.bEnd = last.bEnd;
      coalesced.insertBytes = sliceExactTokenCoverage(
                                  bTokOff_, bToks_, bSource_, coalesced.bStart,
                                  coalesced.bEnd)
                                  .str();
      skipThrough = endIdx;
      trace("include/apply",
            "file={0} coalesced patch[{1}..{2}] across consumed header "
            "conditional group: A=[{3},{4}) B=[{5},{6})",
            file, idx, skipThrough, coalesced.aStart, coalesced.aEnd,
            coalesced.bStart, coalesced.bEnd);
      return true;
    }

    return false;
  };

  auto tryBuildMaterialChildStateCoalescedPatch =
      [&](size_t idx, IncludePatch &coalesced, size_t &skipThrough) {
    if (idx + 1 >= ie.patches.size())
      return false;

    const IncludePatch &first = ie.patches[idx];
    const IncludePatch &second = ie.patches[idx + 1];
    if (first.include != second.include)
      return false;
    if (first.ownerHasCondArmCert != second.ownerHasCondArmCert)
      return false;
    if (first.ownerHasCondArmCert &&
        first.ownerCondArmIdCert != second.ownerCondArmIdCert)
      return false;
    if (first.aStart >= first.aEnd || second.aStart >= second.aEnd)
      return false;
    if (second.aEnd <= first.aStart || second.bEnd < first.bStart)
      return false;

    for (const auto &child : model_.GetIncludes()) {
      if (!child.parent || *child.parent != ie.include->id ||
          !PathsEqual(child.sitePath, file) || !child.cover.IsValid())
        continue;
      if (child.cover.end <= child.cover.begin)
        continue;

      if (!(first.aStart < child.cover.begin &&
            child.cover.begin < first.aEnd &&
            first.aEnd <= second.aStart &&
            second.aStart < child.cover.end &&
            child.cover.end < second.aEnd))
        continue;

      uint64_t prefix = 0;
      while (first.aStart + prefix < child.cover.begin &&
             first.bStart + prefix < second.bEnd &&
             tokenRangesHaveSameSpelling(first.aStart + prefix,
                                         first.aStart + prefix + 1,
                                         first.bStart + prefix,
                                         first.bStart + prefix + 1)) {
        ++prefix;
      }

      if (first.aStart + prefix >= child.cover.begin ||
          first.bStart + prefix > second.bEnd)
        continue;

      const uint64_t newAStart = first.aStart + prefix;
      const uint64_t newBStart = first.bStart + prefix;
      std::string newInsert = sliceExactTokenCoverage(
                                  bTokOff_, bToks_, bSource_, newBStart,
                                  second.bEnd)
                                  .str();

      SmallVector<HeaderPreservedGapPiece, 4> statePieces;
      if (!collectMacroStatePreservationsFromIncludeSubtree(
              child, StringRef(newInsert), statePieces) ||
          statePieces.empty())
        continue;

      coalesced = first;
      coalesced.aStart = newAStart;
      coalesced.aEnd = second.aEnd;
      coalesced.bStart = newBStart;
      coalesced.bEnd = second.bEnd;
      coalesced.insertBytes = std::move(newInsert);
      skipThrough = idx + 1;
      trace("include/apply",
            "file={0} coalesced patch[{1}..{2}] across material child "
            "include #{3}: A=[{4},{5}) B=[{6},{7})",
            file, idx, skipThrough, child.id, coalesced.aStart,
            coalesced.aEnd, coalesced.bStart, coalesced.bEnd);
      return true;
    }

    return false;
  };

  DenseSet<size_t> skippedIncludePatchIndices;

  for (size_t idx = 0; idx < ie.patches.size(); ++idx) {
    if (skippedIncludePatchIndices.count(idx))
      continue;

    IncludePatch coalescedPatch = ie.patches[idx];
    size_t skipThrough = idx;
    const IncludePatch *patchPtr = &ie.patches[idx];
    if (tryBuildConsumedHeaderConditionalCoalescedPatch(idx, coalescedPatch,
                                                       skipThrough) ||
        tryBuildMaterialChildStateCoalescedPatch(idx, coalescedPatch,
                                                 skipThrough)) {
      patchPtr = &coalescedPatch;
      for (size_t skipIdx = idx + 1; skipIdx <= skipThrough; ++skipIdx)
        skippedIncludePatchIndices.insert(skipIdx);
    }

    const IncludePatch &p = *patchPtr;
    trace("include/patch", "computeIncludeTextEdits: patch={0}", p);

    // Record the local obligations discharged by each accepted
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
    // previous one. For DELETE / REPLACE we restrict to the owning decl.
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
    SmallVector<HeaderPreservedGapPiece, 4> headerGapPreservations;
    std::optional<SourceLineDirectiveGapResume> headerSourceLineDirectiveResume;
    bool usedFullHeaderEnvelope = false;

    const auto &tokmapByPP = model_.GetTokmapByPP();

    uint64_t materialAStart = p.aStart;
    uint64_t materialAEnd = p.aEnd;
    uint64_t materialBStart = p.bStart;
    uint64_t materialBEnd = p.bEnd;
    std::string materialInsertBytes = p.insertBytes;

    // If a replacement hunk starts by re-emitting the exact tokens of a
    // complete child include, keep that include as the survivor and remove the
    // matching prefix from the materialized replacement. This is the header
    // analogue of preferring the selected-arm include owner in mixed
    // TU/include closures: identical duplicate tokens should not force us to
    // replace a preserved include with raw B bytes.
    if (isReplace) {
      bool strippedPrefix = true;
      while (strippedPrefix && materialAStart < materialAEnd &&
             materialBStart < materialBEnd) {
        strippedPrefix = false;
        for (const auto &child : model_.GetIncludes()) {
          if (!child.parent || *child.parent != ie.include->id ||
              !PathsEqual(child.sitePath, file) || !child.cover.IsValid())
            continue;
          if (child.cover.begin != materialAStart ||
              child.cover.end > materialAEnd)
            continue;

          const uint64_t childTokenCount = child.cover.end - child.cover.begin;
          if (materialBStart + childTokenCount > materialBEnd)
            continue;
          if (!tokenRangesHaveSameSpelling(child.cover.begin, child.cover.end,
                                           materialBStart,
                                           materialBStart + childTokenCount))
            continue;

          trace("include/apply",
                "file={0} patch[{1}] preserving child include prefix inc#{2} "
                "A=[{3},{4}) B=[{5},{6})",
                file, idx, child.id, child.cover.begin, child.cover.end,
                materialBStart, materialBStart + childTokenCount);
          materialAStart = child.cover.end;
          materialBStart += childTokenCount;
          materialInsertBytes =
              sliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                      materialBStart, materialBEnd)
                  .str();
          strippedPrefix = true;
          break;
        }
      }
    }

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

      // Select the best theorem-facing insertion anchor by wrapping each local
      // anchor candidate in the accepted-result carrier and letting the shared
      // proof-discharge/lattice selector choose among them.
      auto selectBestInsertCandidate =
          [&](ArrayRef<InsertAnchorCandidate> candidates)
          -> std::optional<InsertAnchorCandidate> {
        if (candidates.empty())
          return std::nullopt;

        SmallVector<AcceptedResultCandidate, 4> acceptedCandidates;
        acceptedCandidates.reserve(candidates.size());
        for (const InsertAnchorCandidate &candidate : candidates) {
          acceptedCandidates.push_back(BuildAcceptedIncludeCandidate(
              candidate.path, p, &candidate.witness));
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

      // Materialize the selected insertion anchor as a header-local TextEdit,
      // applying boundary padding and line-resync handling before attaching the
      // accepted include-candidate carrier.
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
        // First try the strongest insertion anchors: boundaries already tied to
        // the selected conditional arm or to a child include boundary. These
        // anchors preserve include structure without relying on neighboring PP
        // tokens.
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
            if (auto rightArmRef = model_.FindArmRefAtPP(pos);
                rightArmRef && rightArmRef->arm) {
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

      // If no structural boundary anchor won, look for the nearest mapped PP
      // token to the right in the same file and anchor before it.
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

      // Right-neighbor anchoring is preferred because it naturally inserts
      // before the next stable token. If unavailable, collect weaker fallback
      // anchors: the nearest mapped left neighbor, or the containing
      // declaration boundary when no neighbor exists.
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
        // With no mapped PP neighbor, fall back to the declaration boundary
        // that owns this patch. This keeps the insertion local to the header
        // declaration instead of widening immediately to include realization.
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

      // No include-preserving insertion anchor discharged. Switch to the
      // explicit include-realization path instead of manufacturing a weaker
      // insertion witness.
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
      // DELETE / REPLACE: intersect the patch's non-empty A-token range with
      // the PP-token range owned by this header/declaration. Only the
      // intersection can be materialized as a local header edit.
      uint64_t aLo = std::max(materialAStart, ppLo);
      uint64_t aHi = std::min(materialAEnd, ppHi);
      if (aHi <= aLo) {
        // Nothing of this patch lies in this header/declaration.
        debug("include/apply",
              "file={0} patch[{1}] DELETE/REPLACE: empty intersection; SKIP",
              file, idx);
        continue;
      }

      // Find the first and last PP tokens in the intersected patch range that
      // actually map back to this header file. These tokens define the concrete
      // source byte range to replace/delete in the header.
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
        // The token intersection exists in PP space, but none of those tokens
        // have a source mapping into this header. Fall back to include
        // realization rather than manufacturing a header byte range.
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

      // Convert the mapped PP-token endpoints to a closed source byte envelope
      // in the header. The witness records both the PP-token evidence and the
      // resolved byte range so proof discharge can validate this include edit.
      startByte = ByteStartForPPInFile(file, *firstPP,
                                       /*fallbackToEOF=*/false, fileLen);
      endByte =
          ByteEndForPPInFile(file, *lastPP, /*fallbackToEOF=*/false, fileLen);
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

      // If either endpoint cannot be projected into header bytes, this patch
      // cannot be represented as an include-preserving mapped-header edit.
      if (!startByte || !endByte) {
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("DELETE/REPLACE: failed to map first/last PP tokens "
                          "to bytes in file {0}",
                          file)
                .str();
        return plan;
      }

      // A single PP hunk can legitimately cross neighboring header-owned
      // source pieces when the bytes between the material tokens are complete
      // zero-token preprocessor structure. The ordinary mapped-token range is
      // intentionally local, but leaving the rest of the hunk in the header can
      // preserve stale source (`int`, a comma, or an include directive). Build
      // a full source envelope from header tokens and complete child-include
      // sites, then prove every inter-piece gap before overriding the local
      // range.
      const bool declClippedNonEmptyPatch =
          decl && (materialAStart < ppLo || ppHi < materialAEnd);
      struct HeaderSourceEnvelopePiece {
        uint64_t begin = 0;
        uint64_t end = 0;
        uint64_t ppBegin = 0;
        uint64_t ppEnd = 0;
        uint64_t id = 0;
        StringRef kind;
      };

      SmallVector<HeaderSourceEnvelopePiece, 8> hunkSourcePieces;
      bool hasCompleteChildIncludePiece = false;
      bool hasPartialChildIncludeOverlap = false;
      const uint64_t fullLo = std::max(materialAStart, coverBegin);
      const uint64_t fullHi = std::min(materialAEnd, coverEnd);

      for (uint64_t pp = fullLo; pp < fullHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it == tokmapByPP.end() || !PathsEqual(it->second.file, file))
          continue;

        // Refuse fallback-to-EOF anchors here: widening across header source
        // pieces needs precise token byte bounds on both sides of each gap.
        std::optional<uint64_t> tokBegin = ByteStartForPPInFile(
            file, pp, /*fallbackToEOF=*/false, fileLen);
        std::optional<uint64_t> tokEnd = ByteEndForPPInFile(
            file, pp, /*fallbackToEOF=*/false, fileLen);
        if (!tokBegin || !tokEnd || *tokBegin >= *tokEnd) {
          hunkSourcePieces.clear();
          break;
        }
        hunkSourcePieces.push_back(
            {*tokBegin, *tokEnd, pp, pp + 1, pp, "token"});
      }

      for (const auto &child : model_.GetIncludes()) {
        if (!child.parent || *child.parent != ie.include->id || !PathsEqual(child.sitePath, file) ||
            !child.cover.IsValid())
          continue;
        if (child.cover.end <= fullLo || fullHi <= child.cover.begin)
          continue;

        if (fullLo <= child.cover.begin && child.cover.end <= fullHi &&
            child.siteB < child.siteE && child.siteE <= headerText.size()) {
          hasCompleteChildIncludePiece = true;
          hunkSourcePieces.push_back({child.siteB, child.siteE,
                                      child.cover.begin, child.cover.end,
                                      child.id, "child-include"});
          continue;
        }

        // A partial child-include overlap would require editing inside the
        // child include and its parent directive at the same time. Leave that
        // to include realization rather than inventing a parent-local range.
        hasPartialChildIncludeOverlap = true;
      }


      bool hasCompleteMacroInvocationPiece = false;
      bool hasCompleteConditionalGroupPiece = false;
      bool hasPartialMacroInvocationOverlap = false;
      bool hasPartialConditionalGroupOverlap = false;

      for (const auto &macro : model_.GetMacroInvocations()) {
        if (!headerMacroInvocationOverlapsMaterial(macro, fullLo, fullHi))
          continue;
        if (headerMacroInvocationIsConsumedSourceEnvelope(macro, fullLo,
                                                          fullHi)) {
          hasCompleteMacroInvocationPiece = true;
          hunkSourcePieces.push_back({*macro.invB, *macro.invE,
                                      macro.cover.begin, macro.cover.end,
                                      macro.id, "macro-invocation"});
          continue;
        }

        // A partial source-bearing macro overlap means the parent header edit
        // would delete only part of the callsite's produced material.  That is
        // not a source-envelope proof; require a stronger/coalesced candidate.
        hasPartialMacroInvocationOverlap = true;
      }

      for (const auto &group : model_.GetConds()) {
        if (!headerConditionalGroupSelectedMaterialOverlaps(group, fullLo,
                                                            fullHi))
          continue;
        if (headerConditionalGroupIsConsumedSourceEnvelope(group, fullLo,
                                                           fullHi)) {
          hasCompleteConditionalGroupPiece = true;
          hunkSourcePieces.push_back({group.groupB, group.groupE, fullLo,
                                      fullHi, group.id, "conditional-group"});
          continue;
        }

        // The hunk touches selected conditional output but does not consume the
        // selected arm.  Do not widen across the directive wrapper unless a
        // coalesced patch can prove the whole group is replaced.
        hasPartialConditionalGroupOverlap = true;
      }

      const bool shouldTryFullHeaderEnvelope =
          declClippedNonEmptyPatch || hasCompleteChildIncludePiece ||
          hasCompleteMacroInvocationPiece || hasCompleteConditionalGroupPiece;
      if (shouldTryFullHeaderEnvelope && !hasPartialChildIncludeOverlap &&
          !hasPartialMacroInvocationOverlap &&
          !hasPartialConditionalGroupOverlap) {
        std::optional<OwnerProofSourceEnvelope> sourceEnvelope;
        if (hunkSourcePieces.size() >= 2 &&
            normalizeOwnerProofPieces(
                hunkSourcePieces,
                [](const HeaderSourceEnvelopePiece &lhs,
                   const HeaderSourceEnvelopePiece &rhs) {
                  return lhs.kind < rhs.kind;
                },
                [](const HeaderSourceEnvelopePiece &outer,
                   const HeaderSourceEnvelopePiece &piece) {
                  if (outer.kind == "child-include" && piece.kind == "token") {
                    // Tokens produced by a child include are owned by the child
                    // site. Once the complete child include is a source-envelope
                    // piece, any mapped child token inside that site is not a
                    // second parent piece.
                    return true;
                  }
                  if (outer.kind == "macro-invocation" &&
                      piece.kind == "token") {
                    // Macro-expanded tokens often map to the macro callsite
                    // spelling. Once the complete source-bearing invocation is
                    // proved consumed, those mapped token intervals are internal
                    // evidence, not separate parent-header pieces.
                    return true;
                  }
                  if (outer.kind == "conditional-group" &&
                      (piece.kind == "token" ||
                       piece.kind == "macro-invocation" ||
                       piece.kind == "child-include")) {
                    // A consumed conditional group owns the selected-arm source
                    // inside its directive wrapper. Nested token/macro/include
                    // pieces inside the group are covered by the group-level
                    // source-envelope proof.
                    return true;
                  }
                  return false;
                }))
          sourceEnvelope = {ownerProofPieceBegin(hunkSourcePieces.front()),
                            ownerProofPieceEnd(hunkSourcePieces.back())};

        SmallVector<HeaderPreservedGapPiece, 4> preservedPieces;
        if (sourceEnvelope) {
          for (const HeaderSourceEnvelopePiece &piece : hunkSourcePieces) {
            if (piece.kind != "child-include")
              continue;
            const RefoldModel::IncludeItem *child =
                model_.GetIncludeById(piece.id);
            if (!child)
              continue;
            if (!collectMacroStatePreservationsFromIncludeSubtree(
                    *child, StringRef(materialInsertBytes), preservedPieces)) {
              sourceEnvelope.reset();
              break;
            }
          }
        }
        if (sourceEnvelope) {
          SourceLineDirectiveLogicalLineRewriter
              headerSourceLineDirectiveLineRewriter =
                  [&](StringRef logicalLine, ArrayRef<uint64_t> sourceOffsets,
                      SourceLineDirectiveBuiltinMacroResolver
                          builtinMacroResolver)
              -> std::optional<SourceLineDirectiveLogicalLineRewrite> {
            return rewriteSourceLineDirectiveLogicalLineMacros(
                model_, file, logicalLine, sourceOffsets,
                [&](StringRef lhs, StringRef rhs) {
                  return PathsEqual(lhs, rhs);
                },
                ie.include->id, std::move(builtinMacroResolver));
          };

          if (!proveOwnerSourceEnvelopeGaps(
                  hunkSourcePieces, [&](uint64_t gapBegin, uint64_t gapEnd) {
                if (std::optional<SourceLineDirectiveGapResume> lineResume =
                        computeSourceLineDirectiveGapResume(
                            StringRef(headerText), gapBegin, gapEnd,
                            sourceEnvelope->end, file,
                            headerSourceLineDirectiveLineRewriter, nullptr,
                            model_.GetSourcePath())) {
                  // Header full-envelope widening uses the same owner-piece gap
                  // proof as TU/include closure.  A source-spelled
                  // line-control gap is not disposable trivia, but it is
                  // preservable by carrying its net line state forward to the
                  // untouched header suffix.
                  headerSourceLineDirectiveResume = std::move(lineResume);
                  return true;
                }

                SmallVector<HeaderPreservedGapPiece, 4> gapPieces;
                for (const auto &directive : model_.GetMacroDirectives()) {
                  std::optional<HeaderMacroStateDirectivePiece> directivePiece =
                      headerMacroStateDirectiveInterval(directive);
                  if (!directivePiece || directivePiece->begin < gapBegin ||
                      gapEnd < directivePiece->end)
                    continue;
                  if (rawIdentifierAppearsInHeaderReplacement(
                          directivePiece->name,
                          StringRef(materialInsertBytes)))
                    return false;

                  HeaderPreservedGapPiece piece;
                  piece.kind =
                      HeaderPreservedGapPiece::Kind::MacroStateDirective;
                  piece.directive = directivePiece->directive;
                  piece.begin = directivePiece->begin;
                  piece.end = directivePiece->end;
                  piece.id = directivePiece->directive
                                 ? directivePiece->directive->id
                                 : 0;
                  gapPieces.push_back(std::move(piece));
                }

                for (const auto &group : model_.GetConds()) {
                  if (!headerConditionalGroupIsPreservableGap(
                          group, gapBegin, gapEnd))
                    continue;
                  HeaderPreservedGapPiece piece;
                  piece.kind = HeaderPreservedGapPiece::Kind::
                      ZeroTokenConditionalGroup;
                  piece.begin = group.groupB;
                  piece.end = group.groupE;
                  piece.id = group.id;
                  gapPieces.push_back(std::move(piece));
                }

                for (const auto &macro : model_.GetMacroInvocations()) {
                  if (!headerMacroInvocationIsPreservableGap(
                          macro, gapBegin, gapEnd))
                    continue;
                  HeaderPreservedGapPiece piece;
                  piece.kind = HeaderPreservedGapPiece::Kind::
                      ZeroTokenMacroInvocation;
                  piece.begin = *macro.invB;
                  piece.end = *macro.invE;
                  piece.id = macro.id;
                  gapPieces.push_back(std::move(piece));
                }

                for (const auto &child : model_.GetIncludes()) {
                  if (!headerZeroTokenChildIncludeIsPreservableGap(
                          child, gapBegin, gapEnd,
                          StringRef(materialInsertBytes)))
                    continue;
                  HeaderPreservedGapPiece piece;
                  piece.kind =
                      HeaderPreservedGapPiece::Kind::ZeroTokenChildInclude;
                  piece.begin = child.siteB;
                  piece.end = child.siteE;
                  piece.id = child.id;
                  gapPieces.push_back(std::move(piece));
                }

                if (!proveOwnerProofGap(
                        gapPieces, gapBegin, gapEnd,
                        [](const HeaderPreservedGapPiece &lhs,
                           const HeaderPreservedGapPiece &rhs) {
                          return static_cast<unsigned>(lhs.kind) <
                                 static_cast<unsigned>(rhs.kind);
                        },
                        [](const HeaderPreservedGapPiece &outer,
                           const HeaderPreservedGapPiece &piece) {
                          if (outer.kind == piece.kind &&
                              (outer.kind == HeaderPreservedGapPiece::Kind::
                                                 ZeroTokenConditionalGroup ||
                               outer.kind == HeaderPreservedGapPiece::Kind::
                                                 ZeroTokenMacroInvocation)) {
                            // Nested zero-token conditional groups and nested
                            // zero-token macro invocations are consumed by
                            // their outer proof piece. Once the outer source
                            // interval is proved neutral, keeping the inner
                            // record as a separate tiling piece would only
                            // double-count the same bytes.
                            return true;
                          }
                          return false;
                        },
                        [&](uint64_t begin, uint64_t end) {
                          return isWsOrCompleteCommentTrivia(
                              StringRef(headerText).slice(begin, end));
                        },
                        [](const HeaderPreservedGapPiece &) {}))
                  return false;

                preservedPieces.append(gapPieces.begin(), gapPieces.end());
                return true;
              }))
            sourceEnvelope.reset();
        }

        if (sourceEnvelope) {
          const bool hasHeaderSuffixAfterSourceLineDirective =
              headerSourceLineDirectiveResume &&
              sourceEnvelope->end < headerText.size();
          const bool canRestoreSourceLineState =
              !headerSourceLineDirectiveResume ||
              (!hasHeaderSuffixAfterSourceLineDirective ||
               (lineDirs_.Enabled() &&
                headerLineDirectiveStartsAtPrefix(sourceEnvelope->end) &&
                (!isDelete ||
                 canStartHeaderLineDirectiveWithOptionalLeadingNewline(
                     sourceEnvelope->begin))));

          if (sourceEnvelope->begin <= *startByte &&
              *endByte <= sourceEnvelope->end && canRestoreSourceLineState &&
              replacementHeaderSuffixBoundaryAllowsDirectiveLine(
                  StringRef(materialInsertBytes), sourceEnvelope->end)) {
            startByte = sourceEnvelope->begin;
            endByte = sourceEnvelope->end;
            headerGapPreservations = std::move(preservedPieces);
            usedFullHeaderEnvelope = true;
            mappedHeaderWitness.startByte = sourceEnvelope->begin;
            mappedHeaderWitness.endByte = sourceEnvelope->end;
            mappedHeaderWitness.firstPP = hunkSourcePieces.front().ppBegin;
            mappedHeaderWitness.lastPP = hunkSourcePieces.back().ppEnd - 1;
            trace("include/apply",
                  "file={0} patch[{1}] DELETE/REPLACE: widened across "
                  "header source gap bytes=[{2},{3}) preserved={4} pieces={5}",
                  file, idx, sourceEnvelope->begin, sourceEnvelope->end,
                  headerGapPreservations.size(), hunkSourcePieces.size());
          }
        }
      }
    }

    // For DELETE/REPLACE, keep the resolved byte range inside the declaration
    // that owns the patch. INSERTs are different: their anchor may legitimately
    // sit on the boundary between declarations, so do not clamp insertion
    // anchors into one side of that boundary.
    if (!isInsert && decl && !usedFullHeaderEnvelope) {
      startByte = std::clamp(*startByte, decl->headerB, decl->headerE);
      endByte = std::clamp(*endByte, decl->headerB, decl->headerE);
    }

    // If no byte anchor/range was resolved, this patch cannot produce a local
    // header edit.
    if (!startByte)
      continue;

    // Clamp the final edit range to the physical header buffer. `endByte` is
    // clamped relative to `startByte` so malformed or boundary-adjusted ranges
    // collapse to a safe empty edit instead of inverting.
    const uint64_t fLen = static_cast<uint64_t>(fileLen);
    startByte = std::clamp(*startByte, uint64_t(0), fLen);
    endByte = std::clamp(*endByte, *startByte, fLen);

    // DELETE emits an empty replacement; INSERT and REPLACE both use the
    // B-derived patch payload carried by the IncludePatch.
    std::string replacement = isDelete ? "" : materialInsertBytes;

    if (!headerGapPreservations.empty()) {
      // Emit preserved gap pieces after the B-derived payload. That keeps
      // replacement tokens in the same preprocessing environment as B, while
      // the untouched header suffix sees the restored zero-token structure.
      if (!replacement.empty() && replacement.back() != '\n')
        replacement.push_back('\n');
      for (const HeaderPreservedGapPiece &piece : headerGapPreservations) {
        std::string pieceText = preservedGapPieceText(piece);
        if (pieceText.empty())
          continue;
        replacement += pieceText;
        if (replacement.empty() || replacement.back() != '\n')
          replacement.push_back('\n');
      }
    }

    const bool emitsHeaderSourceLineDirectiveResume =
        lineDirs_.Enabled() && usedFullHeaderEnvelope &&
        headerSourceLineDirectiveResume && *endByte < fileLen;
    if (emitsHeaderSourceLineDirectiveResume) {
      // A full-envelope delete has no B-derived payload, but the consumed
      // source interval may still have carried a line-control directive whose
      // net state must be restored before the untouched header suffix. Emit a
      // resync-only replacement in that case; non-empty replacements get the
      // same directive on a fresh physical line after their edited payload.
      if (replacement.empty()) {
        if (!headerLineDirectiveStartsAtPrefix(*startByte))
          replacement.push_back('\n');
      } else if (replacement.back() != '\n') {
        replacement.push_back('\n');
      }
      replacement +=
          formatSourceLineDirectiveGapResume(*headerSourceLineDirectiveResume);
      trace("include/apply",
            "file={0} patch[{1}] emitted source #line resume line={2} "
            "resumeFile={3}",
            file, idx, headerSourceLineDirectiveResume->lineAtResume,
            headerSourceLineDirectiveResume->fileSpelling);
    }

    if (inTraceMode()) {
      StringRef opKind =
          isInsert ? "INSERT" : (isDelete ? "DELETE" : "REPLACE");

      // Log the concrete header slice and replacement payload at the byte range
      // that will be staged. This is intentionally after all clamping so the
      // trace reflects the actual emitted edit.
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

      // Include declaration context when available; it explains why the final
      // byte range may have been clamped narrower than the raw mapped PP-token
      // envelope.
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
            startByte, endByte, declInfo, stringutils::showWs(origDbg),
            stringutils::showWs(replDbg));
    }

    // Stage the include-preserving mapped-header edit and attach the witness
    // that records which PP tokens and header bytes justified the edit range.
    TextEdit edit = emitsHeaderSourceLineDirectiveResume
                        ? TextEdit{*startByte, *endByte, std::move(replacement),
                                   std::nullopt, std::nullopt, {}}
                        : MakeTextEditWithResyncOrPending(
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
  // Child-boundary anchoring only applies to patches owned by a concrete parent
  // include. Without that owner, there is no child include set to inspect.
  const RefoldModel::IncludeItem *owner = p.include;
  if (!owner)
    return std::nullopt;

  // The INSERT hunk's A-side start is the PP-gap where the new text should be
  // anchored. This helper only recognizes gaps that coincide exactly with a
  // direct child include's cover boundary.
  uint64_t pos = p.aStart;

  SmallVector<const RefoldModel::IncludeItem *, 4> beginMatches;
  SmallVector<const RefoldModel::IncludeItem *, 4> endMatches;

  for (const auto &child : model_.GetIncludes()) {
    // Only direct children of this include can supply a boundary in the current
    // parent header.
    if (!child.parent || *child.parent != owner->id)
      continue;

    // The child include directive must be spelled in the header file currently
    // being materialized.
    if (!PathsEqual(child.sitePath, file))
      continue;

    const uint64_t cb = child.cover.begin;
    const uint64_t ce = child.cover.end;

    // A gap strictly inside the child's PP cover is not a parent boundary. That
    // edit should be represented as child-owned work, not as an insertion at
    // the parent include site.
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

  // Prefer anchoring before a child include whose PP cover begins at the gap.
  // The returned byte is the beginning of the child's spelled include
  // directive.
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

  // Otherwise, anchor after the unique child include whose PP cover ends at the
  // gap. The returned byte is the end of the child's spelled include directive.
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
  // If the replacement preserves the original newline count, no line-state
  // correction is needed.
  size_t origNl = stringutils::countNewlines(originalFileText, start, end);
  size_t replNl =
      stringutils::countNewlines(replacement, 0, replacement.size());
  if (origNl == replNl)
    return ResyncOutcome(replacement.str(), std::nullopt);

  // Newline drift means the following untouched source would resume at a
  // different logical line unless we emit a #line directive. Prefer a local
  // directive immediately after the replacement when that can be done safely.
  size_t resumeLine = stringutils::lineAtOffset(originalFileText, end);
  trace("linedir/resync",
        "drift: span=[{0},{1}) origNl={2} replNl={3} resumeLine={4} file={5} "
        "replTail={6}",
        start, end, origNl, replNl, resumeLine, fileSpellingForDirective,
        stringutils::showWsWithClip(replacement, 100));

  std::string injected = lineDirs_.MaybeAppendResyncAfterReplacement(
      originalFileText, start, end, replacement, fileSpellingForDirective);

  // A changed result means the directive was inserted directly into this
  // replacement, so no deferred resync state needs to be carried forward.
  if (injected != replacement) {
    trace("linedir/resync", "local inject succeeded: resumeLine={0} file={1}",
          resumeLine, fileSpellingForDirective);
    return ResyncOutcome(std::move(injected), std::nullopt);
  }

  // Local injection was not safe, usually because the replacement rejoins
  // untouched bytes mid-line. Carry a pending resync so the next copied
  // original slice can emit the directive at a valid boundary.
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
  const StringRef owner =
      emissionOwner.empty() ? StringRef("<unknown>") : emissionOwner;
  ++lastTheoremAudit_.emittedNonTerminalEdits;

  auto carrierIsEmissionDischarged =
      [&](const AcceptedResultCandidate &carrier) -> bool {
    ++lastTheoremAudit_.emittedCarriers;

    // Emitted byte edits must be backed by ordinary in-domain accepted results.
    // Unknown carriers and explicit terminal/out-of-domain carriers are never
    // valid on a non-terminal edit.
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

    // Transitional-gap states must be eliminated before emission. This check
    // fail-closes any carrier that is not explicitly proof-backed or still
    // reports a transitional theorem/completeness domain, even if earlier
    // selection code allowed it to reach the edit boundary.
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

    // The carrier must describe a declared in-domain proof class, not merely a
    // syntactically classified candidate. This is the theorem-domain gate for
    // emitted non-terminal edits.
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

    // Finally require the local validator for this accepted-result class to
    // have discharged all obligations.
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

  // Every emitted non-terminal edit must carry at least one selected
  // accepted-result witness. An unannotated edit is an emission artifact, not a
  // theorem-facing refolding result.
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

  // Validate every carrier attached to the edit. Multiple carriers are allowed
  // when one concrete edit composes several accepted results, but each carrier
  // must independently discharge.
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

    // Non-zero span duplicates must be identical to compose safely. Choosing
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
        // direct one-to-one TU hunk witness. Anything synthesized, macro-owned,
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

        // Reconstruct the original hunk order before proving adjacency. The
        // duplicate group is ordered by emitted byte span, which is not enough
        // to prove that the B-side replacement interval is contiguous.
        sort(fragments, [](const TextEdit *lhs, const TextEdit *rhs) {
          return *lhs->directTUHunkIndex < *rhs->directTUHunkIndex;
        });

        // Adjacent duplicate fragments may be collapsed only when they form one
        // uninterrupted run in both the A token hunks and B token hunks. This
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

        // Preserve existing proof carriers from the fragments. If the fragments
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
      // for the same original byte span. If those fragments are adjacent in
      // both A-token and B-token space, they are not competing writers: they are
      // one replacement that was fragmented before byte emission. Rebuild that
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

      // Identical non-zero edits collapse to one physical replacement, but any
      // deferred line-resync state produced by the later duplicate must still be
      // preserved on the merged edit.
      if (dup.pending)
        merged.pending = dup.pending;

      // If duplicate edits represent an expanded macro replacement, they must
      // agree on the same expanded root. Different roots for the same concrete
      // byte span would make the emitted edit's proof carrier ambiguous.
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

      // A duplicate may carry the expanded-root marker even when the first edit
      // did not. Preserve that marker so counter stabilization and audit still
      // see the emitted replacement as macro-expanded.
      if (!merged.expandedMacroRootId && dup.expandedMacroRootId)
        merged.expandedMacroRootId = dup.expandedMacroRootId;

      // Keep all accepted-result carriers from the equivalent duplicates. The
      // text replacement is shared, but each proof witness still explains one
      // path that contributed to the emitted edit.
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

  // Validate that a contiguous run of direct TU-hunk edits can be replaced by
  // one closed TU realization. The candidate is accepted only when every hunk
  // in the run has a direct edit, the source/B envelopes are monotone and
  // non-empty, no independent edit intrudes into the source interval, and the
  // A↔B token map proves that all stable tokens crossing either envelope stay
  // inside the corresponding opposite envelope.
  auto checkClosedTURealizationCandidate =
      [&](uint64_t firstHunk, uint64_t lastHunk,
          ClosedTURealizationCandidate &candidate) -> bool {
    candidate = ClosedTURealizationCandidate{};
    candidate.firstHunk = firstHunk;
    candidate.lastHunk = lastHunk;

    // The closure is defined over a contiguous hunk run, so every hunk in the
    // run must already have a direct TU edit that can be absorbed.
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

    // Build the source byte envelope and B-token envelope for the selected
    // direct edits. The B intervals must appear in order; otherwise a single
    // replacement would not preserve the token-order proof for the hunk run.
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

    // The synthesized replacement must cover a real source byte interval and a
    // real B-token interval.
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

    // The closure may compose only the selected hunk-run edits. Any other edit
    // that overlaps the source envelope, including a zero-width insertion
    // inside it, would need a separate composition law and is rejected here.
    for (size_t editIndex = 0; editIndex < norm.size(); ++editIndex) {
      if (candidateContainsEdit(editIndex))
        continue;
      const TextEdit &edit = norm[editIndex];
      const bool nonZeroOverlap =
          edit.start < candidate.sourceEnd && candidate.sourceBegin < edit.end;
      const bool interiorInsertion = edit.start == edit.end &&
                                     candidate.sourceBegin < edit.start &&
                                     edit.start < candidate.sourceEnd;
      if (nonZeroOverlap || interiorInsertion)
        return false;
    }

    // A-side closure: every A token whose source bytes overlap the candidate
    // must be wholly contained by the candidate source interval. If that token
    // has a stable B mate, that B mate must also lie inside the candidate
    // replacement envelope.
    for (uint64_t aTok = 0; aTok < static_cast<uint64_t>(aToks_.size());
         ++aTok) {
      std::optional<std::pair<uint64_t, uint64_t>> span = sourceTokenSpan(aTok);
      if (!span)
        continue;

      const bool overlapsSource = span->first < candidate.sourceEnd &&
                                  candidate.sourceBegin < span->second;
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

    // B-side closure: every stable B token emitted by the replacement must map
    // back to an A token wholly contained by the source interval. This prevents
    // the synthesized edit from duplicating or moving stable outside tokens.
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

  // Try to replace an uncomposable cluster of overlapping direct TU-hunk edits
  // with one conservative closed TU realization. The synthesized edit is
  // accepted only if a contiguous direct-hunk run can be proven closed in both
  // source-byte space and B-token space by the A↔B token maps.
  auto tryBuildClosedTURealization = [&](size_t overlapIndex,
                                         TextEdit &closure) -> bool {
    // Closed realization depends on both token-map directions: A tokens inside
    // the source interval must map into the B envelope, and stable B tokens in
    // the B envelope must map back into the source interval.
    if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
      return false;

    // Start from the overlap point and collect the full source-overlap cluster
    // around it. The cluster determines the minimum set of direct edits that
    // must be absorbed by the synthesized closure.
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

    // Every edit in the overlap cluster must be a direct TU-hunk edit. Mixed
    // edit classes would require a separate composition law, so fail closed.
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

    // Expand to the maximal contiguous run of direct TU hunks available around
    // the cluster. The search below starts with the minimal cluster-covering
    // run and widens only as needed to find a closed realization.
    uint64_t directLo = *hunkMin;
    while (directLo > 0 && findDirectEditForHunk(directLo - 1))
      --directLo;

    uint64_t directHi = *hunkMax;
    while (directHi + 1 < static_cast<uint64_t>(abTokHunks_.size()) &&
           findDirectEditForHunk(directHi + 1))
      ++directHi;

    // Prefer the smallest closed hunk run that contains the overlap cluster.
    // This keeps the synthesized TU edit as local as possible while still
    // allowing widening when closure requires adjacent direct hunks.
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

    // Materialize exactly the proven B-token envelope, then apply the same
    // line-resync machinery used by ordinary emitted edits.
    StringRef replacement = sliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                                    best->bBegin, best->bEnd);
    ResyncOutcome ro =
        ApplyResyncOrPend(originalFileText, best->sourceBegin, best->sourceEnd,
                          replacement, emissionOwner);
    closure =
        TextEdit{best->sourceBegin,     best->sourceEnd, std::move(ro.text),
                 std::move(ro.pending), std::nullopt,    {}};
    AttachAcceptedResultCarrier(
        closure, BuildAcceptedTUTextEditCandidate(
                     AcceptedPathKind::TUByteSpanConservativeEdit,
                     best->sourceBegin, best->sourceEnd, replacement));

    // Replace the absorbed direct edits with the single closed realization and
    // re-sort so downstream application sees a normal non-overlapping edit set.
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
  // around stable punctuation tokens. If the resulting direct TU hunk edits
  // overlap after lexical widening, first try to replace the minimal enclosing
  // direct-hunk run by one source/B-token-closed realization. This is not a
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

  // The byte-edit emission boundary is proof-gated. By the time an edit
  // reaches this function, every non-terminal artifact it composes must
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

  // Assemble the final file by copying original slices between the normalized
  // edits and splicing each replacement at its staged byte range.
  SmallString<0> out;
  out.reserve(originalFileText.size() + 128);
  std::optional<PendingResync> pending = std::nullopt;

  uint64_t cursor = 0;

  for (const auto &e : norm) {
    // Copy the untouched source before this edit. If a previous replacement
    // could not safely emit its #line resync locally, this copy step is also the
    // next opportunity to flush that pending resync at a safe boundary.
    pending = AppendOriginalSliceWithPending(out, originalFileText, cursor,
                                             e.start, std::move(pending));

    // Record expanded macro roots that actually made it into the emitted text.
    // Counter stabilization uses this to force later __COUNTER__-sensitive
    // macro occurrences when needed.
    if (appliedExpandedMacroRootIds && e.expandedMacroRootId)
      appliedExpandedMacroRootIds->insert(*e.expandedMacroRootId);

    out.append(e.text);

    // Carry any resync that the edit itself could not emit locally. It will be
    // discharged by the next copied original slice, or by the final tail copy.
    if (e.pending) {
      pending = e.pending;
    }

    cursor = e.end;
  }

  // Copy the final untouched tail and flush any pending line resync that
  // survived the last replacement.
  pending = AppendOriginalSliceWithPending(out, originalFileText, cursor, n,
                                           std::move(pending));

  return std::string(out.str());
}

std::optional<RefoldEngine::PendingResync>
RefoldEngine::AppendOriginalSliceWithPending(
    SmallVectorImpl<char> &out, llvm::StringRef original, uint64_t from,
    uint64_t to, std::optional<RefoldEngine::PendingResync> pending) const {
  // Fast path: if there is no pending #line correction, or line directives are
  // disabled, this helper is just a straight copy of the untouched source
  // slice.
  if (!pending || !lineDirs_.Enabled()) {
    auto slice = original.slice(from, to);
    out.append(slice.begin(), slice.end());
    return std::nullopt;
  }

  uint64_t i = from;

  auto canFlushBeforeUntouchedDirectiveLine = [&](size_t pos) -> bool {
    if (pos >= original.size())
      return false;

    // Find the physical start of the current line and require that `pos` is
    // still inside that line's indentation prefix.
    size_t lineStart = pos;
    while (lineStart > 0 && original[lineStart - 1] != '\n')
      --lineStart;

    if (!stringutils::isIndentOnly(original, lineStart, pos))
      return false;

    size_t lineEnd = original.find('\n', pos);
    if (lineEnd == StringRef::npos)
      lineEnd = original.size();

    // A preprocessor directive may be preceded by horizontal whitespace. If the
    // first non-whitespace character is '#', flushing before the indentation is
    // still a directive-line-safe resync point.
    size_t firstNonWs = pos;
    while (firstNonWs < lineEnd && stringutils::isWs(original[firstNonWs]) &&
           original[firstNonWs] != '\n')
      ++firstNonWs;

    return firstNonWs < lineEnd && original[firstNonWs] == '#';
  };

  // Prefer to flush the pending resync before copying this slice when the
  // emitted output is already at BOL and the untouched input also resumes at a
  // safe line boundary. This handles both true BOL and the indentation prefix
  // of an untouched preprocessor directive line.
  if (stringutils::outAtBOL(StringRef(out.data(), out.size())) &&
      (stringutils::isBOL(original, static_cast<size_t>(from)) ||
       canFlushBeforeUntouchedDirectiveLine(static_cast<size_t>(from)))) {
    size_t line = stringutils::lineAtOffset(original, from);
    std::string directive =
        lineDirs_.FormatLineDirective(line, pending->fileSpellingForDir);

    StringRef currentOut(out.data(), out.size());

    // Avoid emitting a redundant directive when the current output state
    // already represents the requested file/line location.
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

  // If the slice does not begin at a safe flush point, copy forward until the
  // first real newline boundary. Line-spliced newlines are not safe because
  // they do not end the logical source line.
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

  // Copy any bytes after the flush point, or the whole slice tail if no safe
  // flush point was found. In the latter case, return the still-pending resync
  // so a later original slice can discharge it.
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

  // Convert the token hunk endpoints to byte offsets when the offset tables
  // have entries for those token boundaries.
  const std::optional<uint64_t> a0 = getByteOff(h.aStart, aTokOff_);
  const std::optional<uint64_t> a1 = getByteOff(h.aEnd, aTokOff_);
  const std::optional<uint64_t> b0 = getByteOff(h.bStart, bTokOff_);
  const std::optional<uint64_t> b1 = getByteOff(h.bEnd, bTokOff_);

  // Slice previews are diagnostic-only. Leave them empty if the projected byte
  // range is missing or invalid rather than risking an out-of-range access.
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
        stringutils::showWsWithClip(aSlice, 120),
        stringutils::showWsWithClip(bSlice, 120));
}

std::string RefoldEngine::PPArgSpanToString(const RefoldModel::PPArgSpan &sp,
                                            bool isStringifyOcc) {
  std::string storage;
  llvm::raw_string_ostream os(storage);

  // Print the common PP-argument span fields first so all span kinds share a
  // stable diagnostic shape.
  os << formatv("{kind='{0}', A=[{1},{2}), argIdx={3}", sp.kind, sp.begin,
                sp.end, sp.argIdx);

  if (isStringifyOcc) {
    os << ", occ=STRINGIFY";
  }

  // Paste spans may describe a byte subrange inside one pasted PP token, so
  // include that finer-grained range only for paste metadata.
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

    // `isStringify` is parallel metadata when available; missing entries mean
    // the corresponding span is not marked as a stringify occurrence.
    bool isStr = (i < isStringify.size() && isStringify[i]);
    os << PPArgSpanToString(spans[i], isStr);
  }
  os << ']';

  return os.str();
}
