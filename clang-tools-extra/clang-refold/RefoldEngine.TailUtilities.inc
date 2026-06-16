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

const RefoldModel::MacroInvocation *
RefoldEngine::LineStateObservableMacroSite(
    const RefoldModel::MacroInvocation &macro) const {
  const RefoldModel::MacroInvocation *site = &macro;
  const unsigned maxDepth =
      static_cast<unsigned>(model_.GetMacroInvocations().size());
  unsigned depth = 0;
  while (site->callerMacroId && depth++ < maxDepth) {
    const RefoldModel::MacroInvocation *caller =
        FindMacroInvocationById(*site->callerMacroId);
    if (!caller)
      break;
    site = caller;
  }
  return site;
}

std::optional<std::string> RefoldEngine::BuildInlineIncludeRealizationFromB(
    const RefoldModel::IncludeItem &inc, StringRef reason,
    AcceptedResultCandidate *acceptedCandidate) const {
  // Inline include realization is theorem-facing only when the include's
  // A-cover can be projected to a concrete B-token envelope by an accepted
  // witness: either the canonical A-cover mapping or the boundary-stable
  // consensus proof.  If no such envelope exists, do not synthesize a weaker
  // realization; request the explicit terminal fallback instead.
  IncludeRealizationEvidenceKind evidenceKind =
      IncludeRealizationEvidenceKind::Unknown;
  auto bEnvOpt = ResolveIncludeRealizationBTokenEnvelope(
      inc.cover.begin, inc.cover.end, &evidenceKind);
  if (!bEnvOpt) {
    RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::IncludeRealizationBEnvelopeMapped,
        TerminalFallbackFailureReason::UnmappableIncludeBEnvelope),
        "include/mat",
        llvm::formatv("include realization from B failed to resolve a "
                      "canonical-or-consensus-resolvable B envelope for "
                      "A cover [{0},{1}) for inc#{2}; reason={3}",
                      inc.cover.begin, inc.cover.end, inc.id, reason)
            .str());
    return std::nullopt;
  }

  // Build the normalized accepted-result carrier before returning the sliced
  // text.  The producer-proven B envelope is passed directly into the shared
  // owner-realization proof path instead of being copied into a parallel
  // include-specific closure witness.
  AcceptedResultCandidate realizationCandidate =
      BuildAcceptedIncludeRealizationCandidate(
          AcceptedPathKind::IncludeRealizationInlineFromB, inc, evidenceKind,
          *bEnvOpt);
  if (acceptedCandidate)
    *acceptedCandidate = realizationCandidate;

  return SliceBSource(bEnvOpt->first, bEnvOpt->second).str();
}

// ============ Include processing (normalize, materialize, apply) =============

/// Return true iff a physical source line is lexically a line-control
/// directive.  This intentionally recognizes only the directive class, not the
/// expanded operands: for entry-wrapper minimization it is enough to know that
/// the emitted child text will overwrite any synthetic entry #line before a
/// later observer can consume it.
static bool refoldLineSpellingIsLineControlDirective(StringRef line) {
  const size_t to = line.size();
  size_t p = 0;
  while (p < to && stringutils::isWs(line[p]) && line[p] != '\n')
    ++p;
  if (p >= to || line[p] != '#')
    return false;
  ++p;
  while (p < to && stringutils::isWs(line[p]) && line[p] != '\n')
    ++p;

  if (p + 4 <= to && line.substr(p, 4) == "line" &&
      (p + 4 == to || stringutils::isWs(line[p + 4])))
    return true;

  // Clang/GCC numeric line-control spelling: # 123 "file".
  return p < to && std::isdigit(static_cast<unsigned char>(line[p]));
}

/// Return the original-source byte offset of the directive-introducing '#'
/// for a physical source line.  The conditional model is expressed in original
/// byte coordinates, so the activity query below must use the real source byte
/// rather than the trimmed line-local position.
static bool refoldIsHorizontalWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r';
}

static std::optional<uint64_t>
refoldFindDirectiveHashOffset(StringRef src, uint64_t lineBegin,
                              uint64_t lineEnd) {
  uint64_t p = lineBegin;
  while (p < lineEnd && p < src.size()) {
    if (refoldIsHorizontalWhitespace(src[p])) {
      ++p;
      continue;
    }

    if (src[p] == '\\') {
      if (p + 1 < lineEnd && src[p + 1] == '\n') {
        p += 2;
        continue;
      }
      if (p + 2 < lineEnd && src[p + 1] == '\r' && src[p + 2] == '\n') {
        p += 3;
        continue;
      }
    }

    if (p + 1 < lineEnd && src[p] == '/' && src[p + 1] == '*') {
      p += 2;
      bool closed = false;
      while (p + 1 < lineEnd) {
        if (src[p] == '\\') {
          if (p + 1 < lineEnd && src[p + 1] == '\n') {
            p += 2;
            continue;
          }
          if (p + 2 < lineEnd && src[p + 1] == '\r' &&
              src[p + 2] == '\n') {
            p += 3;
            continue;
          }
        }
        if (src[p] == '*' && src[p + 1] == '/') {
          p += 2;
          closed = true;
          break;
        }
        ++p;
      }
      if (!closed)
        return std::nullopt;
      continue;
    }

    if (p + 1 < lineEnd && src[p] == '/' && src[p + 1] == '/')
      return std::nullopt;

    if (src[p] == '#')
      return p;

    return std::nullopt;
  }
  return std::nullopt;
}

bool RefoldEngine::SourcePrefixHasProducerActiveLineControl(
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    uint64_t offset) const {
  auto bufOrErr =
      MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(ownerFile));
  if (!bufOrErr) {
    // This query is used only to suppress an otherwise demanded synthetic
    // include-entry #line.  If the owner source cannot be read, fail closed and
    // keep the wrapper rather than inferring that the real line-control stream
    // will kill it.
    return false;
  }

  const MemoryBuffer &mb = **bufOrErr;
  StringRef src(mb.getBufferStart(), mb.getBufferSize());
  const uint64_t limit = std::min<uint64_t>(offset, src.size());

  auto directiveIsProducerActive = [&](uint64_t hashOffset) -> bool {
    for (const RefoldModel::CondGroup &group : model_.GetConds()) {
      if (!PathsEqual(group.file, ownerFile) ||
          group.parentIncludeId != ownerIncludeId)
        continue;
      if (!group.ContainsByte(hashOffset))
        continue;

      bool sawSelectedArm = false;
      bool selectedArmContainsDirective = false;
      for (const RefoldModel::CondArm &arm : group.arms) {
        if (!arm.selected)
          continue;
        sawSelectedArm = true;
        if (arm.ContainsByte(hashOffset)) {
          selectedArmContainsDirective = true;
          break;
        }
      }

      if (selectedArmContainsDirective)
        continue;

      // If a selected material-producing arm exists but does not contain the
      // directive, the directive is proven inactive.  If no selected arm
      // exists, the active branch may have produced only directive effects,
      // which is unproven from CondArm::selected alone; keep the synthetic
      // wrapper in that case as well.
      (void)sawSelectedArm;
      return false;
    }
    return true;
  };

  uint64_t lineBegin = 0;
  while (lineBegin < limit) {
    uint64_t lineEnd = lineBegin;
    while (lineEnd < src.size() && src[lineEnd] != '\n')
      ++lineEnd;

    const uint64_t clampedEnd = std::min<uint64_t>(lineEnd, limit);
    if (std::optional<uint64_t> hash =
            refoldFindDirectiveHashOffset(src, lineBegin, clampedEnd)) {
      if (refoldLineSpellingIsLineControlDirective(
              src.slice(*hash, clampedEnd)) &&
          directiveIsProducerActive(*hash)) {
        return true;
      }
    }

    if (lineEnd >= limit || lineEnd >= src.size())
      break;
    lineBegin = lineEnd + 1;
  }

  return false;
}

RefoldEngine::LineStateObserverDemand
RefoldEngine::IncludeSubtreeLineStateObserverDemand(uint64_t includeId) const {
  LineStateObserverDemand demand;

  auto isDescendantOrSelf = [&](uint64_t owner) -> bool {
    uint64_t cur = owner;
    while (true) {
      if (cur == includeId)
        return true;
      const RefoldModel::IncludeItem *inc = model_.GetIncludeById(cur);
      if (!inc || !inc->parent)
        return false;
      cur = *inc->parent;
    }
  };

  for (const auto &macro : model_.GetMacroInvocations()) {
    const bool observesLine = macro.name == "__LINE__";
    const bool observesFile = macro.name == "__FILE__";
    const bool observesFileName = macro.name == "__FILE_NAME__";
    if (!observesLine && !observesFile && !observesFileName)
      continue;

    // A builtin may be recorded at a macro definition site while the observable
    // use lives at an outer caller in this include subtree. Follow the same
    // caller chain used by the owner-suffix demand query so child-entry
    // wrappers are selected by the site that will remain in emitted source.
    const RefoldModel::MacroInvocation *site = &macro;
    unsigned depth = 0;
    const unsigned maxDepth =
        static_cast<unsigned>(model_.GetMacroInvocations().size());
    while (site->callerMacroId && depth++ < maxDepth) {
      const RefoldModel::MacroInvocation *caller =
          FindMacroInvocationById(*site->callerMacroId);
      if (!caller)
        break;
      site = caller;
    }

    std::optional<uint64_t> owner = site->ownerIncludeId;
    if (!owner)
      owner = macro.ownerIncludeId;
    if (!owner || !isDescendantOrSelf(*owner))
      continue;

    if (!LineStateBuiltinInvocationIsPreservedObserver(macro)) {
      continue;
    }

    if (site->invFile && site->invB &&
        SourcePrefixHasProducerActiveLineControl(*site->invFile, owner,
                                                 *site->invB)) {
      continue;
    }

    demand.needsLine |= observesLine;
    demand.needsFile |= observesFile;
    demand.needsFileName |= observesFileName;
    demand.hasModelBackedLineStateDemand |=
        LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(macro);
    if (demand.needsLine && demand.needsFile && demand.needsFileName &&
        demand.hasModelBackedLineStateDemand)
      break;
  }

  return demand;
}

bool RefoldEngine::IncludeSubtreeHasLineStateSensitiveBuiltin(
    uint64_t includeId) const {
  return IncludeSubtreeLineStateObserverDemand(includeId).Any();
}

bool RefoldEngine::IncludeEntryLineDirectiveDischargesLayoutBarrier(
    const RefoldModel::IncludeItem &child,
    StringRef parentOwnerFileForDemand) const {
  auto bufOrErr =
      MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(parentOwnerFileForDemand));
  if (!bufOrErr) {
    // Failing closed here preserves the old wrapper behavior. The minimization
    // proof is source-backed; if the parent owner cannot be read, do not infer
    // that a layout barrier is unneeded.
    return true;
  }

  const MemoryBuffer &mb = **bufOrErr;
  StringRef parent(mb.getBufferStart(), mb.getBufferSize());
  const uint64_t includeSite =
      std::min<uint64_t>(child.siteB, static_cast<uint64_t>(parent.size()));
  if (includeSite == 0)
    return false;

  const auto &tokmapByPP = model_.GetTokmapByPP();

  auto ownerLineHasMappedToken = [&](uint64_t lineBegin,
                                     uint64_t lineEnd) -> bool {
    for (const auto &it : tokmapByPP) {
      const RefoldModel::TokMapEntry &tok = it.second;
      if (!PathsEqual(tok.file, parentOwnerFileForDemand))
        continue;
      if (tok.b >= lineBegin && tok.b < lineEnd)
        return true;
    }
    return false;
  };

  auto lineHasActiveIncludeSite = [&](uint64_t lineBegin,
                                      uint64_t lineEnd) -> bool {
    for (const RefoldModel::IncludeItem &inc : model_.GetIncludes()) {
      if (!PathsEqual(inc.sitePath, parentOwnerFileForDemand))
        continue;
      if (inc.siteB >= lineBegin && inc.siteB < lineEnd)
        return true;
    }
    return false;
  };

  auto lineCanEmitPreprocessedOutput = [&](StringRef line, uint64_t lineBegin,
                                           uint64_t lineEnd) -> bool {
    if (ownerLineHasMappedToken(lineBegin, lineEnd))
      return true;
    if (lineHasActiveIncludeSite(lineBegin, lineEnd))
      return true;

    // Pragmas are not part of the ordinary token map, but preserved active
    // pragmas are visible in `-E -P` output and therefore reset the physical
    // blank-line run. Other preprocessing directives are treated as zero-token
    // layout material for this proof; keeping a wrapper in those cases is
    // conservative and avoids inferring output from directive spelling alone.
    return stringutils::lineStartsWithDirectiveKeyword(line, "pragma");
  };

  bool sawOutputLine = false;
  bool sawZeroTokenLineBeforeFirstOutput = false;
  bool sawZeroTokenLineAfterLastOutput = false;

  uint64_t lineBegin = 0;
  while (lineBegin < includeSite) {
    uint64_t lineEnd = lineBegin;
    while (lineEnd < parent.size() && parent[lineEnd] != '\n')
      ++lineEnd;
    if (lineEnd >= includeSite)
      break; // Ignore indentation/trivia on the current include line.

    StringRef line = parent.slice(lineBegin, lineEnd);
    const bool canEmit =
        lineCanEmitPreprocessedOutput(line, lineBegin, lineEnd);
    if (canEmit) {
      sawOutputLine = true;
      sawZeroTokenLineAfterLastOutput = false;
    } else {
      if (sawOutputLine)
        sawZeroTokenLineAfterLastOutput = true;
      else
        sawZeroTokenLineBeforeFirstOutput = true;
    }

    lineBegin = lineEnd + 1;
  }


  const bool needsBarrier =
      (!sawOutputLine && sawZeroTokenLineBeforeFirstOutput) ||
      (sawOutputLine && sawZeroTokenLineAfterLastOutput);

  if (needsBarrier && child.resolvedPath) {
    std::optional<uint64_t> firstChildOutputByte;
    const auto &tokmapByPP = model_.GetTokmapByPP();
    for (const RefoldModel::PPArgSpan::PPSpan &span : child.spans) {
      if (!span.IsValid())
        continue;
      for (uint64_t pp = span.begin; pp < span.end; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it == tokmapByPP.end())
          continue;
        const RefoldModel::TokMapEntry &tok = it->second;
        if (!PathsEqual(tok.file, *child.resolvedPath))
          continue;
        if (!firstChildOutputByte || tok.b < *firstChildOutputByte)
          firstChildOutputByte = tok.b;
      }
    }

    if (firstChildOutputByte &&
        SourcePrefixHasProducerActiveLineControl(*child.resolvedPath, child.id,
                                                 *firstChildOutputByte)) {
      return false;
    }
  }

  return needsBarrier;
}


/// Return true iff \p text is only horizontal whitespace or newlines, and it
/// contains at least one physical newline.  This is used to prove that a source
/// line containing a line-state observer has been merged left in the edited
/// preprocessed stream: the original source boundary was a pure line separator,
/// but the B-side boundary no longer contains a newline.
static bool isPureWhitespaceContainingNewline(StringRef text) {
  bool sawNewline = false;
  for (char c : text) {
    if (c == '\n' || c == '\r') {
      sawNewline = true;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\f' || c == '\v')
      continue;
    return false;
  }
  return sawNewline;
}

/// Return the start offset of the physical line containing \p offset.
static uint64_t physicalLineBegin(StringRef bytes, uint64_t offset) {
  offset = std::min<uint64_t>(offset, bytes.size());
  while (offset > 0 && bytes[static_cast<size_t>(offset - 1)] != '\n' &&
         bytes[static_cast<size_t>(offset - 1)] != '\r')
    --offset;
  return offset;
}

/// Return the end offset, not including the line separator, of the physical
/// line containing \p offset.
static uint64_t physicalLineEndNoSeparator(StringRef bytes, uint64_t offset) {
  offset = std::min<uint64_t>(offset, bytes.size());
  while (offset < bytes.size() && bytes[static_cast<size_t>(offset)] != '\n' &&
         bytes[static_cast<size_t>(offset)] != '\r')
    ++offset;
  return offset;
}

/// Return the source-byte construct that must be considered when a line-state
/// builtin is observed through \p site.
///
/// Direct `__LINE__` uses observe the physical line containing the builtin.
/// When the builtin is reached through a user macro call, however, the visible
/// source construct is the callsite, not the builtin token in the macro body or
/// argument.  A function-like callsite may span multiple physical lines (for
/// example `ID(\n  __LINE__\n)`), so line-layout proofs must cover the first
/// source line containing the visible invocation through the line containing
/// the end of that invocation.  The returned end is line-end exclusive and does
/// not include the trailing newline separator.
static std::pair<uint64_t, uint64_t>
lineObserverConstructSpan(StringRef bytes,
                          const RefoldModel::MacroInvocation &site) {
  const uint64_t begin = physicalLineBegin(bytes, site.invB.value_or(0));
  const uint64_t endProbe = site.invE.value_or(site.invB.value_or(0));
  const uint64_t end = physicalLineEndNoSeparator(bytes, endProbe);
  return {begin, end};
}

static bool lineObserverCoverIsTokenPreserved(
    const RefoldModel::MacroInvocation &macro, ArrayRef<int64_t> abTokMapA2B,
    ArrayRef<int64_t> abTokMapB2A, ArrayRef<PPTok> aToks,
    ArrayRef<PPTok> bToks) {
  int64_t previousB = -1;
  for (uint64_t aTok = macro.cover.begin; aTok < macro.cover.end; ++aTok) {
    if (aTok >= static_cast<uint64_t>(abTokMapA2B.size()) ||
        aTok >= static_cast<uint64_t>(aToks.size()))
      return false;

    const int64_t bTok = abTokMapA2B[static_cast<size_t>(aTok)];
    if (bTok < 0 || bTok >= static_cast<int64_t>(bToks.size()) ||
        static_cast<uint64_t>(bTok) >= abTokMapB2A.size() ||
        abTokMapB2A[static_cast<size_t>(bTok)] !=
            static_cast<int64_t>(aTok) ||
        aToks[static_cast<size_t>(aTok)].spelling !=
            bToks[static_cast<size_t>(bTok)].spelling ||
        (previousB >= 0 && bTok != previousB + 1))
      return false;

    previousB = bTok;
  }
  return true;
}

struct LineObserverConstructTokens {
  uint64_t firstATok = 0;
  uint64_t lastATok = 0;
  size_t firstBTok = 0;
  size_t lastBTok = 0;
  uint64_t firstBBegin = 0;
  uint64_t bEnd = 0;
};

template <typename PathsEqualFn>
static std::optional<LineObserverConstructTokens>
lineObserverConstructTokens(
    StringRef ownerFile, uint64_t constructBegin, uint64_t constructEnd,
    const RefoldModel::MacroInvocation &macro, PathsEqualFn pathsEqual,
    const DenseMap<uint64_t, RefoldModel::TokMapEntry> &tokmapByPP,
    ArrayRef<int64_t> abTokMapA2B, ArrayRef<PPTok> aToks,
    ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff,
    bool requirePreviousATok) {
  std::optional<uint64_t> firstATok;
  std::optional<uint64_t> lastATok;
  for (uint64_t aTok = 0; aTok < static_cast<uint64_t>(aToks.size());
       ++aTok) {
    auto it = tokmapByPP.find(aTok);
    if (it == tokmapByPP.end())
      continue;
    const RefoldModel::TokMapEntry &entry = it->second;
    if (!pathsEqual(entry.file, ownerFile))
      continue;
    if (entry.b < constructBegin || entry.e > constructEnd)
      continue;
    if (!firstATok || aTok < *firstATok)
      firstATok = aTok;
    if (!lastATok || aTok > *lastATok)
      lastATok = aTok;
  }

  if (!firstATok || !lastATok)
    return std::nullopt;
  if (requirePreviousATok && *firstATok == 0)
    return std::nullopt;
  if (macro.cover.begin < *firstATok || macro.cover.end - 1 > *lastATok)
    return std::nullopt;
  if (*firstATok >= static_cast<uint64_t>(abTokMapA2B.size()) ||
      *lastATok >= static_cast<uint64_t>(abTokMapA2B.size()))
    return std::nullopt;

  const int64_t firstBTokI = abTokMapA2B[static_cast<size_t>(*firstATok)];
  const int64_t lastBTokI = abTokMapA2B[static_cast<size_t>(*lastATok)];
  if (firstBTokI < 0 || lastBTokI < firstBTokI ||
      firstBTokI >= static_cast<int64_t>(bTokOff.size()) ||
      lastBTokI >= static_cast<int64_t>(bTokOff.size()) ||
      lastBTokI >= static_cast<int64_t>(bToks.size()))
    return std::nullopt;

  const size_t firstBTok = static_cast<size_t>(firstBTokI);
  const size_t lastBTok = static_cast<size_t>(lastBTokI);
  return LineObserverConstructTokens{
      *firstATok, *lastATok, firstBTok, lastBTok,
      static_cast<uint64_t>(bTokOff[firstBTok]),
      static_cast<uint64_t>(bTokOff[lastBTok] +
                            bToks[lastBTok].spelling.size())};
}

struct LineObserverCollapsedGap {
  uint64_t leftATok = 0;
  uint64_t rightATok = 0;
  const RefoldModel::TokMapEntry *leftEntry = nullptr;
  size_t leftBTok = 0;
  uint64_t leftBEnd = 0;
};

template <typename PathsEqualFn>
static std::optional<LineObserverCollapsedGap>
findLineObserverCollapsedPreGap(
    StringRef ownerBytes, StringRef ownerFile, StringRef bSource,
    uint64_t searchBegin, uint64_t observerLineBegin,
    uint64_t firstObserverATok, PathsEqualFn pathsEqual,
    const DenseMap<uint64_t, RefoldModel::TokMapEntry> &tokmapByPP,
    ArrayRef<int64_t> abTokMapA2B, ArrayRef<int64_t> abTokMapB2A,
    ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff) {
  for (uint64_t aTok = 1; aTok < firstObserverATok; ++aTok) {
    const uint64_t leftATok = aTok - 1;
    auto leftIt = tokmapByPP.find(leftATok);
    auto rightIt = tokmapByPP.find(aTok);
    if (leftIt == tokmapByPP.end() || rightIt == tokmapByPP.end())
      continue;

    const RefoldModel::TokMapEntry &leftEntry = leftIt->second;
    const RefoldModel::TokMapEntry &rightEntry = rightIt->second;
    if (!pathsEqual(leftEntry.file, ownerFile) ||
        !pathsEqual(rightEntry.file, ownerFile))
      continue;
    if (leftEntry.e < searchBegin || rightEntry.b > observerLineBegin)
      continue;
    if (leftEntry.e > rightEntry.b)
      continue;
    if (leftATok >= static_cast<uint64_t>(abTokMapA2B.size()) ||
        aTok >= static_cast<uint64_t>(abTokMapA2B.size()))
      continue;

    const int64_t leftBTokI = abTokMapA2B[static_cast<size_t>(leftATok)];
    const int64_t rightBTokI = abTokMapA2B[static_cast<size_t>(aTok)];
    if (leftBTokI < 0 || rightBTokI < 0 ||
        static_cast<uint64_t>(leftBTokI) >= abTokMapB2A.size() ||
        static_cast<uint64_t>(rightBTokI) >= abTokMapB2A.size() ||
        abTokMapB2A[static_cast<size_t>(leftBTokI)] !=
            static_cast<int64_t>(leftATok) ||
        abTokMapB2A[static_cast<size_t>(rightBTokI)] !=
            static_cast<int64_t>(aTok))
      continue;
    if (static_cast<size_t>(leftBTokI) >= bTokOff.size() ||
        static_cast<size_t>(rightBTokI) >= bTokOff.size() ||
        static_cast<size_t>(leftBTokI) >= bToks.size())
      continue;

    const uint64_t leftBEnd = static_cast<uint64_t>(
        bTokOff[static_cast<size_t>(leftBTokI)] +
        bToks[static_cast<size_t>(leftBTokI)].spelling.size());
    const uint64_t rightBBegin =
        static_cast<uint64_t>(bTokOff[static_cast<size_t>(rightBTokI)]);
    if (rightBBegin < leftBEnd || rightBBegin > bSource.size())
      continue;

    StringRef sourceGap = ownerBytes.slice(leftEntry.e, rightEntry.b);
    StringRef bGap = bSource.slice(leftBEnd, rightBBegin);
    if (!isPureWhitespaceContainingNewline(sourceGap))
      continue;
    if (bGap.contains('\n') || bGap.contains('\r'))
      continue;

    return LineObserverCollapsedGap{leftATok, aTok, &leftEntry,
                                    static_cast<size_t>(leftBTokI), leftBEnd};
  }

  return std::nullopt;
}

/// Proven right-extension of a line-observer materialization envelope.
///
/// The own-line merge theorem starts from a closed owner-local interval ending
/// at the physical line that contains the observable line-state construct.  If
/// B also removed the newline after that construct line, stopping there leaves
/// a suffix token stranded on its original source line even though B placed it
/// on the same physical line.  This helper extends the interval across each
/// immediately following owner-local source line whose separating newline was
/// removed in B.  Every extended line must be token-mapped, token-contiguous in
/// B, and contain only whitespace outside the mapped tokens, so the resulting
/// byte edit is a deterministic closed B-line envelope rather than a heuristic
/// layout grab.
struct LineObserverRightClosure {
  uint64_t sourceEnd = 0;
  uint64_t bEnd = 0;
  uint64_t lastATok = 0;
  size_t lastBTok = 0;

  // Source-spelled text for any right-extension lines appended after the
  // initial observer construct.  Prefix-collapse repairs preserve the observer
  // source spelling, so they cannot materialize these right-side lines from B
  // without replacing `__LINE__` by its numeric expansion. Instead, each
  // extension records the B inter-token gap that removed the physical newline,
  // followed by the original source spelling of the next owner-local line.
  // Own-line materialization ignores this field because it emits the whole
  // closed envelope from B.
  std::string sourceSpelledExtension;
};

template <typename PathsEqualFn>
static std::optional<LineObserverRightClosure>
extendLineObserverRightClosure(
    StringRef ownerBytes, StringRef ownerFile, StringRef bSource,
    PathsEqualFn pathsEqual,
    const DenseMap<uint64_t, RefoldModel::TokMapEntry> &tokmapByPP,
    ArrayRef<int64_t> abTokMapA2B, ArrayRef<int64_t> abTokMapB2A,
    ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff, uint64_t sourceEnd,
    uint64_t bEnd, uint64_t lastATok, size_t lastBTok) {
  LineObserverRightClosure closure{sourceEnd, bEnd, lastATok, lastBTok, {}};

  auto mappedBTok = [&](uint64_t aTok) -> std::optional<size_t> {
    if (aTok >= static_cast<uint64_t>(abTokMapA2B.size()))
      return std::nullopt;
    const int64_t bTokI = abTokMapA2B[static_cast<size_t>(aTok)];
    if (bTokI < 0 || static_cast<uint64_t>(bTokI) >= abTokMapB2A.size() ||
        abTokMapB2A[static_cast<size_t>(bTokI)] !=
            static_cast<int64_t>(aTok))
      return std::nullopt;
    const size_t bTok = static_cast<size_t>(bTokI);
    if (bTok >= bTokOff.size() || bTok >= bToks.size())
      return std::nullopt;
    return bTok;
  };

  auto bTokenEnd = [&](size_t bTok) -> uint64_t {
    return static_cast<uint64_t>(bTokOff[bTok] + bToks[bTok].spelling.size());
  };

  while (true) {
    const uint64_t nextATok = closure.lastATok + 1;
    auto nextIt = tokmapByPP.find(nextATok);
    if (nextIt == tokmapByPP.end())
      return closure;
    const RefoldModel::TokMapEntry &nextEntry = nextIt->second;
    if (!pathsEqual(nextEntry.file, ownerFile))
      return closure;
    if (nextEntry.b < closure.sourceEnd)
      return std::nullopt;

    StringRef sourceGap = ownerBytes.slice(closure.sourceEnd, nextEntry.b);
    if (!isPureWhitespaceContainingNewline(sourceGap))
      return closure;

    std::optional<size_t> nextBTok = mappedBTok(nextATok);
    if (!nextBTok)
      return std::nullopt;
    const uint64_t nextBBegin = static_cast<uint64_t>(bTokOff[*nextBTok]);
    if (nextBBegin < closure.bEnd || nextBBegin > bSource.size())
      return std::nullopt;
    StringRef bGap = bSource.slice(closure.bEnd, nextBBegin);
    if (bGap.contains('\n') || bGap.contains('\r'))
      return closure;

    const uint64_t lineBegin = physicalLineBegin(ownerBytes, nextEntry.b);
    const uint64_t lineEnd =
        physicalLineEndNoSeparator(ownerBytes, nextEntry.b);
    if (lineBegin < closure.sourceEnd || nextEntry.e > lineEnd)
      return std::nullopt;

    uint64_t cursor = nextEntry.e;
    uint64_t lineLastATok = nextATok;
    size_t lineLastBTok = *nextBTok;
    uint64_t lineBEnd = bTokenEnd(lineLastBTok);

    for (uint64_t aTok = nextATok + 1;; ++aTok) {
      auto it = tokmapByPP.find(aTok);
      if (it == tokmapByPP.end())
        break;
      const RefoldModel::TokMapEntry &entry = it->second;
      if (!pathsEqual(entry.file, ownerFile) || entry.b >= lineEnd)
        break;
      if (entry.b < cursor || entry.e > lineEnd)
        return std::nullopt;
      if (!ownerBytes.slice(cursor, entry.b).trim().empty())
        return std::nullopt;

      std::optional<size_t> bTok = mappedBTok(aTok);
      if (!bTok || *bTok != lineLastBTok + 1)
        return std::nullopt;
      const uint64_t bBegin = static_cast<uint64_t>(bTokOff[*bTok]);
      if (bBegin < lineBEnd || bBegin > bSource.size())
        return std::nullopt;
      StringRef intraLineBGap = bSource.slice(lineBEnd, bBegin);
      if (intraLineBGap.contains('\n') || intraLineBGap.contains('\r'))
        return std::nullopt;

      cursor = entry.e;
      lineLastATok = aTok;
      lineLastBTok = *bTok;
      lineBEnd = bTokenEnd(lineLastBTok);
    }

    if (!ownerBytes.slice(cursor, lineEnd).trim().empty())
      return std::nullopt;

    closure.sourceSpelledExtension += bGap.str();
    closure.sourceSpelledExtension +=
        ownerBytes.slice(nextEntry.b, lineEnd).str();
    closure.sourceEnd = lineEnd;
    closure.bEnd = lineBEnd;
    closure.lastATok = lineLastATok;
    closure.lastBTok = lineLastBTok;
  }
}


struct LineObserverOwnLineMerge {
  uint64_t previousATok = 0;
  const RefoldModel::TokMapEntry *prevEntry = nullptr;
  size_t prevBTok = 0;
  uint64_t prevBEnd = 0;
  uint64_t firstBBegin = 0;
  uint64_t initialBEnd = 0;
  LineObserverRightClosure closure;
};

/// Resolve the shared owner-polymorphic proof for the case where B removes the
/// physical newline immediately before a preserved `__LINE__` observer.  TU and
/// header materialization lower the resulting envelope differently, but the
/// closure obligation is identical: previous owner token, source newline gap,
/// B non-newline gap, preserved observer envelope, and any right-extension
/// lines must all be token-map closed.
template <typename PathsEqualFn>
static std::optional<LineObserverOwnLineMerge> resolveLineObserverOwnLineMerge(
    StringRef ownerBytes, StringRef ownerFile, StringRef bSource,
    PathsEqualFn pathsEqual,
    const DenseMap<uint64_t, RefoldModel::TokMapEntry> &tokmapByPP,
    ArrayRef<int64_t> abTokMapA2B, ArrayRef<int64_t> abTokMapB2A,
    ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff,
    uint64_t sourceLineBegin, uint64_t sourceLineEnd,
    const LineObserverConstructTokens &observerTokens) {
  if (observerTokens.firstATok == 0)
    return std::nullopt;

  const uint64_t previousATok = observerTokens.firstATok - 1;
  auto prevIt = tokmapByPP.find(previousATok);
  if (prevIt == tokmapByPP.end())
    return std::nullopt;
  const RefoldModel::TokMapEntry &prevEntry = prevIt->second;
  if (!pathsEqual(prevEntry.file, ownerFile))
    return std::nullopt;
  if (prevEntry.e > sourceLineBegin)
    return std::nullopt;

  StringRef originalSeparator = ownerBytes.slice(prevEntry.e, sourceLineBegin);
  if (!isPureWhitespaceContainingNewline(originalSeparator))
    return std::nullopt;

  if (previousATok >= static_cast<uint64_t>(abTokMapA2B.size()))
    return std::nullopt;
  const int64_t prevBTokI = abTokMapA2B[static_cast<size_t>(previousATok)];
  if (prevBTokI < 0)
    return std::nullopt;
  const size_t prevBTok = static_cast<size_t>(prevBTokI);
  if (prevBTok >= bTokOff.size() || prevBTok >= bToks.size())
    return std::nullopt;

  const uint64_t prevBEnd = static_cast<uint64_t>(
      bTokOff[prevBTok] + bToks[prevBTok].spelling.size());
  const uint64_t firstBBegin = observerTokens.firstBBegin;
  if (firstBBegin < prevBEnd || firstBBegin > bSource.size())
    return std::nullopt;
  StringRef bSeparator = bSource.slice(prevBEnd, firstBBegin);
  if (bSeparator.contains('\n') || bSeparator.contains('\r'))
    return std::nullopt;

  const uint64_t initialBEnd = observerTokens.bEnd;
  if (initialBEnd < prevBEnd || initialBEnd > bSource.size())
    return std::nullopt;

  std::optional<LineObserverRightClosure> closure =
      extendLineObserverRightClosure(
          ownerBytes, ownerFile, bSource, pathsEqual, tokmapByPP, abTokMapA2B,
          abTokMapB2A, bToks, bTokOff, sourceLineEnd, initialBEnd,
          observerTokens.lastATok, observerTokens.lastBTok);
  if (!closure)
    return std::nullopt;

  return LineObserverOwnLineMerge{previousATok, &prevEntry, prevBTok, prevBEnd,
                                  firstBBegin, initialBEnd, std::move(*closure)};
}


/// Source-spelled prefix segment for a collapsed pre-observer B line.
///
/// Prefix-collapse repairs preserve the line-state observer source spelling and
/// emit a canonical #line immediately before it.  The prefix before that repair
/// should therefore be represented as "B layout plus original source
/// constructs" rather than blindly materializing B bytes.  This keeps
/// still-valid macro invocations such as `B_STMT` source-spelled while proving
/// the same B physical-line collapse.
struct LineObserverSourcePrefix {
  std::string text;
  uint64_t sourceEnd = 0;
  uint64_t lastATok = 0;
  size_t lastBTok = 0;
};

/// Return true iff every token in [firstATok,lastATok] maps bijectively to a
/// contiguous B-token range.  The helper reports that B range so callers can
/// use B's inter-construct whitespace while preserving the source spelling of
/// the construct itself.
static bool lineObserverMappedContiguousBRange(
    uint64_t firstATok, uint64_t lastATok, ArrayRef<int64_t> abTokMapA2B,
    ArrayRef<int64_t> abTokMapB2A, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, size_t &firstBTok, size_t &lastBTok) {
  if (firstATok > lastATok || lastATok >= abTokMapA2B.size())
    return false;

  std::optional<size_t> firstB;
  std::optional<size_t> prevB;
  for (uint64_t aTok = firstATok; aTok <= lastATok; ++aTok) {
    const int64_t bTokI = abTokMapA2B[static_cast<size_t>(aTok)];
    if (bTokI < 0 || static_cast<uint64_t>(bTokI) >= abTokMapB2A.size() ||
        abTokMapB2A[static_cast<size_t>(bTokI)] !=
            static_cast<int64_t>(aTok))
      return false;
    const size_t bTok = static_cast<size_t>(bTokI);
    if (bTok >= bTokOff.size() || bTok >= bToks.size())
      return false;
    if (prevB && bTok != *prevB + 1)
      return false;
    if (!firstB)
      firstB = bTok;
    prevB = bTok;
  }

  if (!firstB || !prevB)
    return false;
  firstBTok = *firstB;
  lastBTok = *prevB;
  return true;
}

template <typename PathsEqualFn>
static const RefoldModel::MacroInvocation *
lineObserverSourceMacroConstructAt(
    ArrayRef<RefoldModel::MacroInvocation> macroInvocations, uint64_t aTok,
    uint64_t limitATok, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId, PathsEqualFn pathsEqual) {
  const RefoldModel::MacroInvocation *best = nullptr;
  for (const RefoldModel::MacroInvocation &macro : macroInvocations) {
    if (!macro.cover.IsValid() || macro.name == "__LINE__")
      continue;
    if (macro.ownerIncludeId != ownerIncludeId)
      continue;
    if (!macro.invFile || !macro.invB || !macro.invE)
      continue;
    if (!pathsEqual(*macro.invFile, ownerFile))
      continue;
    if (macro.cover.begin != aTok || macro.cover.end > limitATok)
      continue;
    // Prefer the widest source-spelled construct beginning at this token.  This
    // preserves an outer macro call when nested macro calls share the same
    // first produced token, and it is deterministic because macro ids are
    // stable.
    if (!best || macro.cover.end > best->cover.end ||
        (macro.cover.end == best->cover.end && macro.id < best->id)) {
      best = &macro;
    }
  }
  return best;
}

template <typename PathsEqualFn>
static std::optional<LineObserverSourcePrefix>
buildLineObserverSourceSpelledPrefix(
    StringRef ownerBytes, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId, StringRef bSource,
    PathsEqualFn pathsEqual,
    const DenseMap<uint64_t, RefoldModel::TokMapEntry> &tokmapByPP,
    ArrayRef<RefoldModel::MacroInvocation> macroInvocations,
    ArrayRef<int64_t> abTokMapA2B, ArrayRef<int64_t> abTokMapB2A,
    ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff, uint64_t sourceBegin,
    uint64_t bBegin, uint64_t firstATok, uint64_t limitATok,
    uint64_t sourceLimit, uint64_t bLimit) {
  LineObserverSourcePrefix result;
  uint64_t cursor = sourceBegin;
  uint64_t bCursor = bBegin;
  uint64_t aTok = firstATok;

  while (aTok < limitATok) {
    uint64_t constructFirstATok = aTok;
    uint64_t constructLastATok = aTok;
    uint64_t constructBegin = 0;
    uint64_t constructEnd = 0;

    if (const RefoldModel::MacroInvocation *macro =
            lineObserverSourceMacroConstructAt(macroInvocations, aTok,
                                               limitATok, ownerFile,
                                               ownerIncludeId, pathsEqual)) {
      constructFirstATok = macro->cover.begin;
      constructLastATok = macro->cover.end - 1;
      constructBegin = *macro->invB;
      constructEnd = *macro->invE;
    } else {
      auto it = tokmapByPP.find(aTok);
      if (it == tokmapByPP.end())
        return std::nullopt;
      const RefoldModel::TokMapEntry &entry = it->second;
      if (!pathsEqual(entry.file, ownerFile))
        return std::nullopt;
      constructBegin = entry.b;
      constructEnd = entry.e;
    }

    if (constructBegin < cursor || constructEnd > sourceLimit)
      return std::nullopt;
    if (!ownerBytes.slice(cursor, constructBegin).trim().empty())
      return std::nullopt;

    size_t constructFirstBTok = 0;
    size_t constructLastBTok = 0;
    if (!lineObserverMappedContiguousBRange(
            constructFirstATok, constructLastATok, abTokMapA2B, abTokMapB2A,
            bToks, bTokOff, constructFirstBTok, constructLastBTok))
      return std::nullopt;

    const uint64_t constructBBegin =
        static_cast<uint64_t>(bTokOff[constructFirstBTok]);
    if (constructBBegin < bCursor || constructBBegin > bSource.size())
      return std::nullopt;
    StringRef bGap = bSource.slice(bCursor, constructBBegin);
    if (bGap.contains('\n') || bGap.contains('\r'))
      return std::nullopt;

    result.text += bGap.str();
    result.text += ownerBytes.slice(constructBegin, constructEnd).str();
    result.sourceEnd = constructEnd;
    result.lastATok = constructLastATok;
    result.lastBTok = constructLastBTok;

    cursor = constructEnd;
    bCursor = static_cast<uint64_t>(
        bTokOff[constructLastBTok] + bToks[constructLastBTok].spelling.size());
    aTok = constructLastATok + 1;
  }

  if (!ownerBytes.slice(cursor, sourceLimit).trim().empty())
    return std::nullopt;
  if (bLimit < bCursor || bLimit > bSource.size())
    return std::nullopt;
  result.text += bSource.slice(bCursor, bLimit).str();
  return result;
}

bool RefoldEngine::SameLineControlPhysicalFile(StringRef lhs,
                                                   StringRef rhs) const {
  if (lhs == rhs)
    return true;

  auto isPseudoFile = [](StringRef path) {
    return path.starts_with("<") && path.ends_with(">");
  };
  if (isPseudoFile(lhs) || isPseudoFile(rhs))
    return false;

  return lineDirs_.ToAbsolutePath(lhs) == lineDirs_.ToAbsolutePath(rhs);
}

std::optional<uint64_t> RefoldEngine::LatestProducerLineControlEndBefore(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  std::optional<uint64_t> result;
  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls()) {
    if (!event.active || !event.producerProven || !event.siteE)
      continue;
    if (event.ownerIncludeId != ownerIncludeId)
      continue;
    if (!SameLineControlPhysicalFile(event.physicalFile, ownerFile))
      continue;
    if (*event.siteE > offset)
      continue;
    result = result ? std::max(*result, *event.siteE) : *event.siteE;
  }
  return result;
}

std::optional<LineDirectiveLocation>
RefoldEngine::ProducerBackedLineControlLocationAt(
    StringRef ownerBytes, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId, uint64_t eventEndLimit,
    uint64_t locationOffset) const {
  const RefoldModel::LineControlEvent *best = nullptr;
  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls()) {
    if (!event.active || !event.producerProven || !event.siteE)
      continue;
    if (event.ownerIncludeId != ownerIncludeId)
      continue;
    if (!SameLineControlPhysicalFile(event.physicalFile, ownerFile))
      continue;
    if (*event.siteE > eventEndLimit)
      continue;
    if (!best || *event.siteE > *best->siteE ||
        (*event.siteE == *best->siteE && event.id > best->id))
      best = &event;
  }

  if (!best)
    return std::nullopt;

  const size_t delta = stringutils::countNonSplicedNewlines(
      ownerBytes, *best->siteE, locationOffset);
  return LineDirectiveLocation(best->logicalFileAfter,
                               best->logicalLineAfter + delta, true);
}

LineDirectiveLocation RefoldEngine::LogicalLocationAtOwnerOffset(
    StringRef ownerBytes, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId, uint64_t offset) const {
  LineDirectiveLocation loc = LineDirectiveInserter::LogicalLocationAtOffset(
      ownerBytes, static_cast<size_t>(offset), ownerFile, model_, ownerFile,
      ownerIncludeId);
  if (loc.producerProven)
    return loc;
  if (std::optional<LineDirectiveLocation> producerLoc =
          ProducerBackedLineControlLocationAt(ownerBytes, ownerFile,
                                              ownerIncludeId, offset, offset))
    return *producerLoc;
  return loc;
}

bool RefoldEngine::AppendLineObserverLayoutRealizationEdits(
    StringRef tuPath, StringRef tuBytes, std::vector<TextEdit> &tuEdits) {
  if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
    return true;

  auto editOverlapsExisting = [&](uint64_t begin, uint64_t end) {
    for (const TextEdit &edit : tuEdits) {
      if (begin < edit.end && edit.start < end)
        return true;
    }
    return false;
  };

  const auto &tokmapByPP = model_.GetTokmapByPP();

  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__LINE__")
      continue;
    const RefoldModel::MacroInvocation *site =
        LineStateObservableMacroSite(macro);
    if (!site || site->ownerIncludeId)
      continue;
    if (!site->invFile || !site->invB || !site->invE)
      continue;
    if (!PathsEqual(*site->invFile, tuPath))
      continue;
    if (!macro.cover.IsValid())
      continue;

    // Only handle the fully token-preserved case.  If the builtin expansion is
    // already token-changed, ordinary macro materialization owns it.
    if (!lineObserverCoverIsTokenPreserved(macro, abTokMapA2B_, abTokMapB2A_,
                                           aToks_, bToks_))
      continue;

    const auto [sourceLineBegin, sourceLineEnd] =
        lineObserverConstructSpan(tuBytes, *site);
    if (sourceLineBegin >= sourceLineEnd)
      continue;

    std::optional<LineObserverConstructTokens> observerTokens =
        lineObserverConstructTokens(
            tuPath, sourceLineBegin, sourceLineEnd, macro,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, abTokMapA2B_, aToks_, bToks_, bTokOff_,
            /*requirePreviousATok=*/true);
    if (!observerTokens)
      continue;

    std::optional<LineObserverOwnLineMerge> ownLineMerge =
        resolveLineObserverOwnLineMerge(
            tuBytes, tuPath, bSource_,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, abTokMapA2B_, abTokMapB2A_, bToks_, bTokOff_,
            sourceLineBegin, sourceLineEnd, *observerTokens);
    if (!ownLineMerge)
      continue;

    const RefoldModel::TokMapEntry &prevEntry = *ownLineMerge->prevEntry;
    const uint64_t prevBEnd = ownLineMerge->prevBEnd;
    const uint64_t initialBEnd = ownLineMerge->initialBEnd;
    const LineObserverRightClosure &ownLineClosure = ownLineMerge->closure;

    const uint64_t editBegin = prevEntry.e;
    const uint64_t editEnd = ownLineClosure.sourceEnd;
    if (editBegin >= editEnd)
      continue;
    if (editOverlapsExisting(editBegin, editEnd)) {
      continue;
    }

    // Materialize only the left/observer part of the B physical-line envelope
    // from B.  Any proven right-extension line is appended from the original
    // owner spelling with B's inter-token gap.  This preserves source-spelled
    // constructs such as object-like macro invocations (`SEMI`) that merely
    // moved onto the same B line; expanding them to their PP token spelling
    // would validate, but would be a less faithful refolding.
    std::string replacement = bSource_.slice(prevBEnd, initialBEnd).str();
    replacement += ownLineClosure.sourceSpelledExtension;
    if (replacement.empty())
      continue;

    TextEdit edit{editBegin, editEnd, replacement, std::nullopt, std::nullopt,
                  {}, {}, {}};
    StampTextEditMaterializedBByteRange(edit, prevBEnd, initialBEnd);
    AttachAcceptedResultCarrier(
        edit, BuildAcceptedTUTextEditCandidate(
                  AcceptedPathKind::TUByteSpanConservativeEdit, editBegin,
                  editEnd, replacement));
    tuEdits.push_back(std::move(edit));

  }

  // A preserved source-spelled __LINE__ observer can remain source-spelled when
  // B preserves the observer line itself, but only if the copied prefix between
  // the dominating #line state and that observer preserves the same physical
  // line count.  A token-identical B edit can still merge two earlier ordinary
  // source lines, leaving the observer on a fresh B line but one logical line
  // earlier if we simply copy the original source.  In that case, realize the
  // collapsed prefix bytes from B and emit a canonical repair immediately
  // before the preserved observer.
  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__LINE__")
      continue;
    const RefoldModel::MacroInvocation *site =
        LineStateObservableMacroSite(macro);
    if (!site || site->ownerIncludeId)
      continue;
    if (!site->invFile || !site->invB || !site->invE)
      continue;
    if (!PathsEqual(*site->invFile, tuPath))
      continue;
    if (!macro.cover.IsValid())
      continue;

    if (!lineObserverCoverIsTokenPreserved(macro, abTokMapA2B_, abTokMapB2A_,
                                           aToks_, bToks_))
      continue;

    const auto [observerLineBegin, observerConstructEnd] =
        lineObserverConstructSpan(tuBytes, *site);
    if (observerLineBegin == 0 || observerLineBegin >= observerConstructEnd)
      continue;

    std::optional<LineObserverConstructTokens> observerTokens =
        lineObserverConstructTokens(
            tuPath, observerLineBegin, observerConstructEnd, macro,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, abTokMapA2B_, aToks_, bToks_, bTokOff_,
            /*requirePreviousATok=*/true);
    if (!observerTokens)
      continue;

    const uint64_t firstObserverBBegin = observerTokens->firstBBegin;
    const uint64_t observerBEnd = observerTokens->bEnd;

    const std::optional<uint64_t> latestLineControlEnd =
        LatestProducerLineControlEndBefore(std::nullopt, tuPath,
                                           observerLineBegin);
    const uint64_t searchBegin = latestLineControlEnd.value_or(0);

    std::optional<LineObserverCollapsedGap> collapsedGap =
        findLineObserverCollapsedPreGap(
            tuBytes, tuPath, bSource_, searchBegin, observerLineBegin,
            observerTokens->firstATok,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, abTokMapA2B_, abTokMapB2A_, bToks_, bTokOff_);
    if (!collapsedGap)
      continue;

    if (firstObserverBBegin < collapsedGap->leftBEnd ||
        firstObserverBBegin > bSource_.size() || observerBEnd > bSource_.size())
      continue;

    std::optional<LineObserverSourcePrefix> sourcePrefix =
        buildLineObserverSourceSpelledPrefix(
            tuBytes, tuPath, std::nullopt, bSource_,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, model_.GetMacroInvocations(), abTokMapA2B_,
            abTokMapB2A_, bToks_, bTokOff_, collapsedGap->leftEntry->e,
            collapsedGap->leftBEnd, collapsedGap->rightATok,
            observerTokens->firstATok, observerLineBegin, firstObserverBBegin);
    if (!sourcePrefix)
      continue;

    std::optional<LineObserverRightClosure> closure =
        extendLineObserverRightClosure(
            tuBytes, tuPath, bSource_,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, abTokMapA2B_, abTokMapB2A_, bToks_, bTokOff_,
            observerConstructEnd, observerBEnd,
            observerTokens->lastATok, observerTokens->lastBTok);
    if (!closure)
      continue;

    const uint64_t editBegin = collapsedGap->leftEntry->e;
    const uint64_t editEnd = closure->sourceEnd;
    if (editBegin >= editEnd)
      continue;
    if (editOverlapsExisting(editBegin, editEnd)) {
      continue;
    }

    std::string replacement = sourcePrefix->text;
    if (replacement.empty())
      continue;
    if (!stringutils::outAtBOL(replacement))
      continue;

    LineDirectiveLocation loc = LogicalLocationAtOwnerOffset(
        tuBytes, tuPath, std::nullopt, observerLineBegin);
    if (!loc.producerProven)
      continue;
    replacement += lineDirs_.FormatLineDirective(loc.lineNo, loc.fileSpelling);
    replacement += tuBytes.slice(observerLineBegin, observerConstructEnd).str();
    replacement += closure->sourceSpelledExtension;

    TextEdit edit{editBegin, editEnd, replacement, std::nullopt,
                  std::nullopt, {}, {}, {}};
    StampTextEditMaterializedBByteRange(edit, collapsedGap->leftBEnd,
                                        firstObserverBBegin);
    AttachAcceptedResultCarrier(
        edit, BuildAcceptedTUTextEditCandidate(
                  AcceptedPathKind::TUByteSpanConservativeEdit, editBegin,
                  editEnd, replacement));
    tuEdits.push_back(std::move(edit));
  }

  return true;
}

bool RefoldEngine::AppendIncludeLineObserverLayoutRealizationEdits(
    DenseMap<uint64_t, IncludeEdits> &perInclude) {
  if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
    return true;

  const auto &tokmapByPP = model_.GetTokmapByPP();

  auto patchOverlapsExisting = [&](uint64_t includeId, uint64_t begin,
                                   uint64_t end) {
    auto it = perInclude.find(includeId);
    if (it == perInclude.end())
      return false;
    for (const IncludePatch &patch : it->second.patches) {
      if (begin < patch.aEnd && patch.aStart < end)
        return true;
    }
    return false;
  };

  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__LINE__")
      continue;
    const RefoldModel::MacroInvocation *site =
        LineStateObservableMacroSite(macro);
    if (!site || !site->ownerIncludeId)
      continue;
    if (!site->invFile || !site->invB || !site->invE)
      continue;
    if (!macro.cover.IsValid())
      continue;

    const RefoldModel::IncludeItem *include =
        model_.GetIncludeById(*site->ownerIncludeId);
    if (!include)
      continue;

    const std::string ownerFile = resolveHeaderPath(*include);
    if (!PathsEqual(*site->invFile, ownerFile))
      continue;

    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(ownerFile));
    if (!bufOrErr) {
      continue;
    }
    const MemoryBuffer &mb = **bufOrErr;
    StringRef headerBytes = mb.getBuffer();
    if (*site->invB >= headerBytes.size())
      continue;

    // Only handle the fully token-preserved case. If the builtin expansion is
    // already token-changed, ordinary macro/include materialization owns it.
    if (!lineObserverCoverIsTokenPreserved(macro, abTokMapA2B_, abTokMapB2A_,
                                           aToks_, bToks_))
      continue;

    const auto [sourceLineBegin, sourceLineEnd] =
        lineObserverConstructSpan(headerBytes, *site);
    if (sourceLineBegin >= sourceLineEnd)
      continue;

    std::optional<LineObserverConstructTokens> observerTokens =
        lineObserverConstructTokens(
            ownerFile, sourceLineBegin, sourceLineEnd, macro,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, abTokMapA2B_, aToks_, bToks_, bTokOff_,
            /*requirePreviousATok=*/true);
    if (!observerTokens)
      continue;

    std::optional<LineObserverOwnLineMerge> ownLineMerge =
        resolveLineObserverOwnLineMerge(
            headerBytes, ownerFile, bSource_,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, abTokMapA2B_, abTokMapB2A_, bToks_, bTokOff_,
            sourceLineBegin, sourceLineEnd, *observerTokens);
    if (!ownLineMerge)
      continue;

    const RefoldModel::TokMapEntry &prevEntry = *ownLineMerge->prevEntry;
    const uint64_t previousATok = ownLineMerge->previousATok;
    const size_t prevBTok = ownLineMerge->prevBTok;
    const uint64_t prevBEnd = ownLineMerge->prevBEnd;
    const uint64_t initialBEnd = ownLineMerge->initialBEnd;
    const LineObserverRightClosure &ownLineClosure = ownLineMerge->closure;

    // Include patches operate in PP-token ranges. Use the previous token as the
    // left edge so the materialized replacement owns the removed inter-line
    // separator without requiring a half-token byte edit in the header planner.
    const uint64_t patchAStart = previousATok;
    const uint64_t patchAEnd = ownLineClosure.lastATok + 1;
    const uint64_t patchBStart = static_cast<uint64_t>(prevBTok);
    const uint64_t patchBEnd =
        static_cast<uint64_t>(ownLineClosure.lastBTok + 1);
    if (patchOverlapsExisting(include->id, patchAStart, patchAEnd)) {
      continue;
    }

    // As in the TU theorem, the left/observer portion is a B materialization,
    // but any right-extension lines are preserved from the original header
    // spelling.  This lets an adjacent macro call stay source-spelled while
    // still closing the full B physical line around the observer.
    std::string replacement = bSource_.slice(prevBEnd, initialBEnd).str();
    replacement += ownLineClosure.sourceSpelledExtension;

    IncludePatch patch{include, std::move(replacement), patchAStart, patchAEnd,
                       patchBStart, patchBEnd};
    patch.hasDirectHeaderByteRange = true;
    patch.directHeaderByteBegin = prevEntry.e;
    patch.directHeaderByteEnd = ownLineClosure.sourceEnd;

    auto [it, _] = perInclude.try_emplace(include->id, include);
    it->second.Add(std::move(patch));

  }

  // Header-owned analogue of the TU pre-observer layout theorem.  When B
  // collapses an earlier ordinary source line into the prefix before a
  // preserved `__LINE__` observer, and the observer itself remains at BOL, the
  // header must materialize that collapsed prefix and then re-establish the
  // producer-proven logical state immediately before the observer.  This is
  // distinct from the own-line-merge theorem above: here a legal directive line
  // exists, so preserving the source-spelled observer is the stronger
  // refolding.
  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__LINE__")
      continue;
    const RefoldModel::MacroInvocation *site =
        LineStateObservableMacroSite(macro);
    if (!site || !site->ownerIncludeId)
      continue;
    if (!site->invFile || !site->invB || !site->invE)
      continue;
    if (!macro.cover.IsValid())
      continue;

    const RefoldModel::IncludeItem *include =
        model_.GetIncludeById(*site->ownerIncludeId);
    if (!include)
      continue;

    const std::string ownerFile = resolveHeaderPath(*include);
    if (!PathsEqual(*site->invFile, ownerFile))
      continue;

    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(ownerFile));
    if (!bufOrErr) {
      continue;
    }
    const MemoryBuffer &mb = **bufOrErr;
    StringRef headerBytes = mb.getBuffer();
    if (*site->invB >= headerBytes.size())
      continue;

    if (!lineObserverCoverIsTokenPreserved(macro, abTokMapA2B_, abTokMapB2A_,
                                           aToks_, bToks_))
      continue;

    const auto [observerLineBegin, observerConstructEnd] =
        lineObserverConstructSpan(headerBytes, *site);
    if (observerLineBegin >= observerConstructEnd)
      continue;

    std::optional<LineObserverConstructTokens> observerTokens =
        lineObserverConstructTokens(
            ownerFile, observerLineBegin, observerConstructEnd, macro,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, abTokMapA2B_, aToks_, bToks_, bTokOff_,
            /*requirePreviousATok=*/true);
    if (!observerTokens)
      continue;

    const uint64_t firstObserverBBegin = observerTokens->firstBBegin;


    const std::optional<uint64_t> latestLineControlEnd =
        LatestProducerLineControlEndBefore(site->ownerIncludeId, ownerFile,
                                           observerLineBegin);
    const uint64_t searchBegin = latestLineControlEnd.value_or(0);

    const uint64_t observerBEnd = observerTokens->bEnd;

    std::optional<LineObserverCollapsedGap> collapsedGap =
        findLineObserverCollapsedPreGap(
            headerBytes, ownerFile, bSource_, searchBegin, observerLineBegin,
            observerTokens->firstATok,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, abTokMapA2B_, abTokMapB2A_, bToks_, bTokOff_);
    if (!collapsedGap)
      continue;

    const RefoldModel::TokMapEntry &leftEntry = *collapsedGap->leftEntry;
    if (firstObserverBBegin < collapsedGap->leftBEnd ||
        firstObserverBBegin > bSource_.size())
      continue;

    if (observerBEnd > bSource_.size())
      continue;

    std::optional<LineObserverSourcePrefix> sourcePrefix =
        buildLineObserverSourceSpelledPrefix(
            headerBytes, ownerFile, site->ownerIncludeId, bSource_,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, model_.GetMacroInvocations(), abTokMapA2B_,
            abTokMapB2A_, bToks_, bTokOff_, leftEntry.e,
            collapsedGap->leftBEnd, collapsedGap->rightATok,
            observerTokens->firstATok, observerLineBegin, firstObserverBBegin);
    if (!sourcePrefix)
      continue;

    std::optional<LineObserverRightClosure> closure =
        extendLineObserverRightClosure(
            headerBytes, ownerFile, bSource_,
            [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
            tokmapByPP, abTokMapA2B_, abTokMapB2A_, bToks_, bTokOff_,
            observerConstructEnd, observerBEnd, observerTokens->lastATok,
            observerTokens->lastBTok);
    if (!closure)
      continue;

    std::string replacement = sourcePrefix->text;
    if (replacement.empty())
      continue;
    if (!stringutils::outAtBOL(replacement))
      continue;

    LineDirectiveLocation loc = LogicalLocationAtOwnerOffset(
        headerBytes, ownerFile, site->ownerIncludeId, observerLineBegin);
    if (!loc.producerProven)
      continue;
    replacement += lineDirs_.FormatLineDirective(loc.lineNo, loc.fileSpelling);
    replacement +=
        headerBytes.slice(observerLineBegin, observerConstructEnd).str();
    replacement += closure->sourceSpelledExtension;

    const uint64_t patchAStart = collapsedGap->rightATok;
    const uint64_t patchAEnd = closure->lastATok + 1;
    const uint64_t patchBStart = static_cast<uint64_t>(collapsedGap->leftBTok);
    const uint64_t patchBEnd = static_cast<uint64_t>(closure->lastBTok + 1);
    if (patchOverlapsExisting(include->id, patchAStart, patchAEnd)) {
      continue;
    }

    IncludePatch patch{include, std::move(replacement), patchAStart, patchAEnd,
                       patchBStart, patchBEnd};
    patch.hasDirectHeaderByteRange = true;
    patch.directHeaderByteBegin = leftEntry.e;
    patch.directHeaderByteEnd = closure->sourceEnd;

    auto [it, _] = perInclude.try_emplace(include->id, include);
    it->second.Add(std::move(patch));
  }

  return true;
}

bool RefoldEngine::LineStateBuiltinInvocationIsPreservedObserver(
    const RefoldModel::MacroInvocation &macro) const {
  // The demand side of synthetic #line insertion is intentionally about
  // *preserved observers*, not merely about source spellings that existed in
  // the original owner. A later accepted macro-realization edit may already
  // materialize a changed __LINE__ value as an ordinary literal. In that case
  // inserting a #line before the materialized literal is gratuitous and can
  // make the refolding less faithful to the B-side edit.
  //
  // The producer/LCS token map gives us a deterministic proof for the only case
  // that should demand resync here: the builtin's realized A-side token surface
  // survived token-identically into B. If any produced token is unmapped, maps
  // back to a different A token, or has different spelling in B, the builtin is
  // not considered a preserved observer for line-resync demand; the ordinary
  // macro realization machinery is then responsible for materializing it.
  if (!macro.cover.IsValid())
    return true;

  if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
    return true;

  int64_t previousB = -1;
  for (uint64_t aTok = macro.cover.begin; aTok < macro.cover.end; ++aTok) {
    if (aTok >= static_cast<uint64_t>(abTokMapA2B_.size()))
      return false;

    const int64_t bTok = abTokMapA2B_[static_cast<size_t>(aTok)];
    if (bTok < 0)
      return false;
    if (bTok >= static_cast<int64_t>(bToks_.size()) ||
        aTok >= static_cast<uint64_t>(aToks_.size()))
      return false;
    if (static_cast<uint64_t>(bTok) >= abTokMapB2A_.size())
      return false;
    if (abTokMapB2A_[static_cast<size_t>(bTok)] !=
        static_cast<int64_t>(aTok))
      return false;

    if (previousB >= 0 && bTok != previousB + 1)
      return false;
    previousB = bTok;

    if (aToks_[static_cast<size_t>(aTok)].spelling !=
        bToks_[static_cast<size_t>(bTok)].spelling)
      return false;
  }

  return true;
}

bool RefoldEngine::LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(
    const RefoldModel::MacroInvocation &macro) const {
  // Direct lexical builtin tokens that survive into ordinary emitted source are
  // visible without extra model facts.  If a predefined builtin is
  // reached through a caller macro, the final source normally contains the
  // caller spelling rather than the builtin token; record that the demand needs
  // model-backed line-state evidence instead of lexical discovery alone.
  if (macro.callerMacroId)
    return true;

  if (!macro.invFile || !macro.invB)
    return true;

  // Missing cover/map evidence is already treated as preserved by
  // LineStateBuiltinInvocationIsPreservedObserver() for emission safety.  It
  // also requires model-backed line-state demand rather than direct lexical
  // discovery.
  if (!macro.cover.IsValid())
    return true;
  if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
    return true;

  return false;
}

RefoldEngine::LineStateObserverDemand
RefoldEngine::OwnerSuffixLineStateObserverDemand(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  LineStateObserverDemand demand;

  for (const auto &macro : model_.GetMacroInvocations()) {
    const bool observesLine = macro.name == "__LINE__";
    const bool observesFile = macro.name == "__FILE__";
    const bool observesFileName = macro.name == "__FILE_NAME__";
    if (!observesLine && !observesFile && !observesFileName)
      continue;
    if (macro.ownerIncludeId != ownerIncludeId)
      continue;

    // The producer records nested predefined builtins at their definition-site
    // spelling when they are expanded from another macro replacement list. For
    // example, RF_MARK(x) may contain __LINE__ in common.h even though the
    // observable line-state use is the RF_MARK(...) invocation in the current
    // header.  Line-resync decisions therefore have to project a recorded
    // builtin use back to its outermost caller before comparing byte offsets in
    // the owner file.  This is still a deterministic map-backed proof: every
    // step follows caller_macro_id edges emitted by the producer.
    const RefoldModel::MacroInvocation *site = &macro;
    unsigned depth = 0;
    const unsigned maxDepth =
        static_cast<unsigned>(model_.GetMacroInvocations().size());
    while (site->callerMacroId && depth++ < maxDepth) {
      const RefoldModel::MacroInvocation *caller =
          FindMacroInvocationById(*site->callerMacroId);
      if (!caller)
        break;
      site = caller;
    }

    if (!site->invFile || !site->invB)
      continue;
    if (!PathsEqual(*site->invFile, ownerFile))
      continue;
    if (*site->invB < offset)
      continue;

    if (!LineStateBuiltinInvocationIsPreservedObserver(macro)) {
      continue;
    }

    demand.needsLine |= observesLine;
    demand.needsFile |= observesFile;
    demand.needsFileName |= observesFileName;
    demand.hasModelBackedLineStateDemand |=
        LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(macro);
    if (demand.needsLine && demand.needsFile && demand.needsFileName &&
        demand.hasModelBackedLineStateDemand)
      break;
  }

  return demand;
}

std::optional<RefoldEngine::LineStateObserverSite>
RefoldEngine::FirstOwnerSuffixLineStateObserverSite(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  std::optional<LineStateObserverSite> best;

  auto considerSite = [&](const RefoldModel::MacroInvocation &builtin,
                          const RefoldModel::MacroInvocation &site) {
    const bool observesLine = builtin.name == "__LINE__";
    const bool observesFile = builtin.name == "__FILE__";
    const bool observesFileName = builtin.name == "__FILE_NAME__";
    if (!observesLine && !observesFile && !observesFileName)
      return;
    if (site.ownerIncludeId != ownerIncludeId)
      return;
    if (!site.invFile || !site.invB)
      return;
    if (!PathsEqual(*site.invFile, ownerFile))
      return;
    if (*site.invB < offset)
      return;
    if (IsInvocationInsideDefineDirective(site))
      return;
    if (!LineStateBuiltinInvocationIsPreservedObserver(builtin))
      return;

    if (!best || *site.invB < best->offset) {
      best = LineStateObserverSite{};
      best->offset = *site.invB;
    }
    best->demand.needsLine |= observesLine;
    best->demand.needsFile |= observesFile;
    best->demand.needsFileName |= observesFileName;
    best->demand.hasModelBackedLineStateDemand |=
        LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(builtin);
  };

  for (const auto &macro : model_.GetMacroInvocations()) {
    const bool observesLine = macro.name == "__LINE__";
    const bool observesFile = macro.name == "__FILE__";
    const bool observesFileName = macro.name == "__FILE_NAME__";
    if (!observesLine && !observesFile && !observesFileName)
      continue;

    const RefoldModel::MacroInvocation *site = &macro;
    unsigned depth = 0;
    const unsigned maxDepth =
        static_cast<unsigned>(model_.GetMacroInvocations().size());
    while (site && depth++ <= maxDepth) {
      considerSite(macro, *site);
      if (!site->callerMacroId)
        break;
      site = FindMacroInvocationById(*site->callerMacroId);
    }
  }

  return best;
}

bool RefoldEngine::OwnerSuffixHasLineStateSensitiveBuiltin(
    std::optional<uint64_t> ownerIncludeId, StringRef ownerFile,
    uint64_t offset) const {
  return OwnerSuffixLineStateObserverDemand(ownerIncludeId, ownerFile, offset)
      .Any();
}


/// Return true iff a physical source line spells one of the line-control
/// directive forms that can affect the logical file/line state.  This helper is
/// intentionally syntactic and conservative; it is used only to identify the
/// concrete previous directive that caused no-op suppression in the already
/// emitted final stream.
static bool isPhysicalLineControlDirectiveLine(StringRef line) {
  line = line.trim();
  if (!line.starts_with("#"))
    return false;

  line = line.drop_front().ltrim();
  if (line.starts_with("line")) {
    line = line.drop_front(4);
    return line.empty() || line.front() == '/' || stringutils::isWs(line.front());
  }

  // Clang/GCC line-marker spelling: `# 42 "file" ...`.
  return !line.empty() && line.front() >= '0' && line.front() <= '9';
}

/// Find the last concrete line-control directive line before an insertion point
/// in an already-emitted output prefix.  The returned range is in final-output
/// coordinates and includes the trailing newline when present.
static std::optional<std::pair<uint64_t, uint64_t>>
findLastLineControlDirectiveRangeBefore(StringRef out,
                                        uint64_t insertionOffset) {
  if (insertionOffset > out.size())
    insertionOffset = out.size();

  const uint64_t lookbackLimit =
      insertionOffset > 16 * 1024 ? insertionOffset - 16 * 1024 : 0;
  uint64_t cursor = insertionOffset;
  while (cursor > lookbackLimit) {
    uint64_t contentEnd = cursor;
    uint64_t rangeEnd = cursor;
    if (contentEnd > lookbackLimit && out[contentEnd - 1] == '\n') {
      --contentEnd;
      rangeEnd = cursor;
    }

    uint64_t lineBegin = contentEnd;
    while (lineBegin > lookbackLimit && out[lineBegin - 1] != '\n')
      --lineBegin;

    StringRef line(out.data() + lineBegin,
                   static_cast<size_t>(contentEnd - lineBegin));
    if (isPhysicalLineControlDirectiveLine(line))
      return std::make_pair(lineBegin, rangeEnd);

    if (lineBegin == 0)
      break;
    cursor = lineBegin - 1;
  }

  return std::nullopt;
}

bool RefoldEngine::LineResyncShouldDeferToConditionalJoin(
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    uint64_t resumeOffset) const {
  std::optional<LineStateObserverSite> firstObserver =
      FirstOwnerSuffixLineStateObserverSite(ownerIncludeId, ownerFile,
                                           resumeOffset);
  if (!firstObserver || !firstObserver->demand.Any())
    return false;

  for (const RefoldModel::CondGroup *group :
       model_.GetCondGroups(ownerFile, ownerIncludeId)) {
    if (!group || group->file != ownerFile ||
        group->parentIncludeId != ownerIncludeId)
      continue;

    // The edit resumes inside a selected arm, but the first preserved
    // line-state observer is reached only after the conditional island has
    // rejoined.  A local arm repair is then only configuration-local; the
    // canonical domination point is the join repair before that observer.
    if (group->ContainsByte(resumeOffset) &&
        firstObserver->offset >= group->groupE)
      return true;
  }

  return false;
}

/// Build one explicit final-line-control pruning candidate for a synthetic
/// directive emitted into an intermediate text buffer.
///
/// The offsets are intentionally relative to the buffer that owns the directive
/// right now.  The edit applicator shifts them exactly once when the buffer is
/// spliced into its parent output.  This keeps candidate generation
/// deterministic despite later edit normalization, include nesting, and
/// pending-resync flushes.
static FinalLineControlPruneCandidate makeSyntheticLineControlPruneCandidate(
    uint64_t begin, uint64_t end, FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> owner,
    FinalLineControlObligation obligation) {
  return MakeFinalLineControlPruneCandidate(
      begin, end, origin, std::move(owner),
      /*producerProven=*/true, obligation);
}

/// Append source candidates to destination after shifting their offsets by
/// delta.
static void appendShiftedLineControlPruneCandidates(
    std::vector<FinalLineControlPruneCandidate> &dst,
    ArrayRef<FinalLineControlPruneCandidate> src, uint64_t delta) {
  dst.reserve(dst.size() + src.size());
  for (FinalLineControlPruneCandidate candidate : src) {
    candidate.finalBegin += delta;
    candidate.finalEnd += delta;
    dst.push_back(std::move(candidate));
  }
}

/// Append final-to-source mappings after shifting their final-output offsets by
/// delta.  Source byte ranges are not changed; only the enclosing final buffer
/// coordinate changes as materialized source text is nested into a parent.
static void appendShiftedLineControlSourceMappings(
    std::vector<FinalLineControlSourceMapping> &dst,
    ArrayRef<FinalLineControlSourceMapping> src, uint64_t delta) {
  dst.reserve(dst.size() + src.size());
  for (FinalLineControlSourceMapping mapping : src) {
    mapping.finalBegin += delta;
    mapping.finalEnd += delta;
    dst.push_back(std::move(mapping));
  }
}

static void deduplicateLineControlSourceMappings(
    std::vector<FinalLineControlSourceMapping> &mappings) {
  CanonicalizeFinalLineControlSourceMappings(mappings);
}

/// Build a pruning candidate for a directive that was inserted into a
/// replacement buffer by LineDirectiveInserter.  The proof is exact: the new
/// text must be the old text plus one complete directive at a single stable
/// byte offset.
static std::optional<FinalLineControlPruneCandidate>
makeInsertedSyntheticLineControlPruneCandidate(
    StringRef before, StringRef after, StringRef directive,
    FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> owner,
    FinalLineControlObligation obligation) {
  if (directive.empty())
    return std::nullopt;
  if (after.size() != before.size() + directive.size())
    return std::nullopt;

  size_t prefix = 0;
  while (prefix < before.size() && before[prefix] == after[prefix])
    ++prefix;

  if (prefix + directive.size() > after.size())
    return std::nullopt;
  if (after.substr(prefix, directive.size()) != directive)
    return std::nullopt;

  StringRef afterTail = after.substr(prefix + directive.size());
  StringRef beforeTail = before.substr(prefix);
  if (afterTail != beforeTail)
    return std::nullopt;

  return makeSyntheticLineControlPruneCandidate(
      static_cast<uint64_t>(prefix),
      static_cast<uint64_t>(prefix + directive.size()), origin,
      std::move(owner), obligation);
}

/// Remove duplicate candidate records after composition/duplicate-edit merging.
///
/// Candidate equality is byte-range based because the final pruner matches the
/// concrete directive bytes.  Preserve compact obligation/removal proofs only
/// when duplicate generation sites agree exactly; conflicting metadata is
/// discarded fail-closed while leaving the operational candidate eligible under
/// the existing authoritative final model.
static void deduplicateLineControlPruneCandidates(
    std::vector<FinalLineControlPruneCandidate> &candidates) {
  llvm::sort(candidates, [](const FinalLineControlPruneCandidate &lhs,
                            const FinalLineControlPruneCandidate &rhs) {
    if (lhs.finalBegin != rhs.finalBegin)
      return lhs.finalBegin < rhs.finalBegin;
    if (lhs.finalEnd != rhs.finalEnd)
      return lhs.finalEnd < rhs.finalEnd;
    return static_cast<unsigned>(lhs.origin) <
           static_cast<unsigned>(rhs.origin);
  });

  std::vector<FinalLineControlPruneCandidate> deduped;
  deduped.reserve(candidates.size());
  for (FinalLineControlPruneCandidate candidate : candidates) {
    if (!deduped.empty() && deduped.back().finalBegin == candidate.finalBegin &&
        deduped.back().finalEnd == candidate.finalEnd &&
        deduped.back().origin == candidate.origin) {
      FinalLineControlPruneCandidate &merged = deduped.back();
      if (!SameFinalLineControlOwner(merged.physicalOwner,
                                     candidate.physicalOwner))
        merged.physicalOwner = std::nullopt;
      if (!SameFinalLineControlObligationProof(merged.obligationProof,
                                               candidate.obligationProof))
        merged.obligationProof = std::nullopt;
      if (!SameFinalLineControlRemovalProof(merged.removalProof,
                                            candidate.removalProof))
        merged.removalProof = std::nullopt;
      merged.producerProven = merged.producerProven || candidate.producerProven;
      continue;
    }

    deduped.push_back(std::move(candidate));
  }

  candidates = std::move(deduped);
}

RefoldEngine::LineControlWrappedText
RefoldEngine::WrapIncludeExpansionForMaterialization(
    const RefoldModel::IncludeItem &child, StringRef parentFileSpelling,
    StringRef parentOwnerFileForDemand,
    std::optional<uint64_t> parentOwnerIncludeId, uint64_t parentResumeOffset,
    size_t childEntryLineNo, size_t parentResumeLineNo, StringRef childBody,
    ArrayRef<FinalLineControlPruneCandidate> childBodyLineControlCandidates,
    ArrayRef<FinalLineControlSourceMapping> childBodyLineControlSourceMappings,
    bool allowUnobservableLineDirectiveSuppression) const {
  LineControlWrappedText wrapped;

  auto appendChildBody = [&]() {
    const uint64_t bodyBegin = static_cast<uint64_t>(wrapped.text.size());
    wrapped.text += childBody.str();
    appendShiftedLineControlPruneCandidates(
        wrapped.lineControlPruneCandidates, childBodyLineControlCandidates,
        bodyBegin);
    appendShiftedLineControlSourceMappings(
        wrapped.lineControlSourceMappings, childBodyLineControlSourceMappings,
        bodyBegin);
  };

  if (!lineDirs_.Enabled()) {
    appendChildBody();
    return wrapped;
  }

  const std::string childFileSpelling = resolveHeaderPath(child);

  const LineStateObserverDemand childDemand =
      IncludeSubtreeLineStateObserverDemand(child.id);
  const LineStateObserverDemand parentDemand =
      OwnerSuffixLineStateObserverDemand(parentOwnerIncludeId,
                                         parentOwnerFileForDemand,
                                         parentResumeOffset);
  const bool childNeedsLineState = childDemand.Any();
  const bool parentNeedsLineState = parentDemand.Any();
  const bool childNeedsLayoutBarrier =
      !childBody.empty() &&
      IncludeEntryLineDirectiveDischargesLayoutBarrier(
          child, parentOwnerFileForDemand);

  auto forEachLineControlDemandComponent =
      [&](const LineStateObserverDemand &demand, bool includeLayoutLineState,
          auto &&fn) {
        bool emittedAny = false;
        if (demand.needsLine) {
          fn(OwnerStateComponent::LineNumber);
          emittedAny = true;
        }
        if (demand.needsFile) {
          fn(OwnerStateComponent::FileState);
          emittedAny = true;
        }
        if (demand.needsFileName) {
          fn(OwnerStateComponent::FileName);
          emittedAny = true;
        }

        // Include-entry wrappers are sometimes required by a -E -P layout
        // barrier even before a preserved location builtin is known.  Keep that
        // obligation on the line-control gateway surface as a line-state
        // transition; the gateway will discharge it as suffix-unobserved if the
        // persistent observer graph confirms that no suffix owner observes it.
        if (!emittedAny && includeLayoutLineState)
          fn(OwnerStateComponent::LineNumber);
      };

  auto checkLineControlRepair =
      [&](const OwnerStateBoundary &boundary, OwnerStateComponent component,
          StateMutationKind mutation, StringRef stage, StringRef detail,
          bool requireKnownObserver) {
        return CheckStateTransitionAcrossEditBoundary(
            boundary, component, mutation,
            BuildStateTransitionWitness(SuffixStabilityWitnessKind::StateRepair,
                                        component, boundary, detail),
            stage, detail, requireKnownObserver);
      };

  auto parentReturnShouldDeferToConditionalJoin = [&]() -> bool {
    // If the include is materialized inside one arm of a preserved conditional
    // group and the first parent observer is after the group rejoins, an
    // arm-local return #line is insufficient for other build configurations.
    // The repair must dominate the join instead.  Do not evaluate the #if
    // expression here; this is a purely structural dominance proof over the
    // recorded conditional island and the first preserved observer site.
    return LineResyncShouldDeferToConditionalJoin(
        parentOwnerFileForDemand, parentOwnerIncludeId, parentResumeOffset);
  };

  auto appendSyntheticDirective =
      [&](std::string directive, FinalLineDirective::Origin origin,
          std::optional<FinalLineControlOwnerKey> owner,
          bool removableCandidate, FinalLineControlObligation obligation) {
        const uint64_t begin = static_cast<uint64_t>(wrapped.text.size());
        wrapped.text += directive;
        const uint64_t end = static_cast<uint64_t>(wrapped.text.size());
        if (removableCandidate) {
          wrapped.lineControlPruneCandidates.push_back(
              makeSyntheticLineControlPruneCandidate(
                  begin, end, origin, std::move(owner), obligation));
        }
      };

  auto appendChildEntryIfDemanded = [&]() {
    if (childNeedsLineState || childNeedsLayoutBarrier) {
      const OwnerStateBoundary childEntryBoundary =
          OwnerStateBoundary::FromSource(OwnerSourceRange::From(
              childFileSpelling, 0, 0, std::optional<uint64_t>(child.id)));
      forEachLineControlDemandComponent(
          childDemand, childNeedsLayoutBarrier,
          [&](OwnerStateComponent component) {
            const std::string detail =
                llvm::formatv("synthetic include-entry #line for inc#{0} "
                              "component={1} child='{2}'",
                              child.id, component, childFileSpelling)
                    .str();
            (void)checkLineControlRepair(
                childEntryBoundary, component, StateMutationKind::Replayed,
                "linedir/include-entry", detail,
                /*requireKnownObserver=*/childNeedsLineState);
          });
      appendSyntheticDirective(
          lineDirs_.FormatLineDirective(childEntryLineNo, childFileSpelling),
          FinalLineDirective::Origin::SyntheticIncludeEntry,
          FinalLineControlOwnerKey(childFileSpelling, child.id),
          /*removableCandidate=*/true,
          childNeedsLineState
              ? FinalLineControlObligation::SourceStateRepair
              : FinalLineControlObligation::LayoutBoundaryRepair);
      return;
    }
  };

  auto appendParentReturnIfDemanded = [&]() {
    if (parentNeedsLineState) {
      if (parentReturnShouldDeferToConditionalJoin()) {
        return;
      }
      const OwnerStateBoundary parentReturnBoundary =
          OwnerStateBoundary::FromSource(OwnerSourceRange::From(
              parentOwnerFileForDemand, parentResumeOffset, parentResumeOffset,
              parentOwnerIncludeId));
      forEachLineControlDemandComponent(
          parentDemand, /*includeLayoutLineState=*/false,
          [&](OwnerStateComponent component) {
            const std::string detail =
                llvm::formatv("synthetic include-return #line for inc#{0} "
                              "component={1} parent='{2}' byte={3}",
                              child.id, component, parentOwnerFileForDemand,
                              parentResumeOffset)
                    .str();
            (void)checkLineControlRepair(parentReturnBoundary, component,
                                         StateMutationKind::Replayed,
                                         "linedir/include-return", detail,
                                         /*requireKnownObserver=*/true);
          });
      appendSyntheticDirective(
          lineDirs_.FormatLineDirective(parentResumeLineNo, parentFileSpelling),
          FinalLineDirective::Origin::SyntheticIncludeReturn,
          FinalLineControlOwnerKey(parentOwnerFileForDemand.str(),
                                   parentOwnerIncludeId),
          /*removableCandidate=*/true,
          FinalLineControlObligation::IncludeReturnRepair);
      return;
    }
  };

  if (!allowUnobservableLineDirectiveSuppression) {
    // Ordinary include materialization and sideband-driven materialization use
    // the same proof obligation for synthetic line-control.  The local emission
    // policy remains conservative, but every synthetic wrapper emitted here is
    // registered as an explicit removable candidate; final observer/layout
    // liveness plus executable clang -E -P validation decide whether it
    // survives.
    wrapped.text.reserve(childBody.size() + 128);
    appendChildEntryIfDemanded();
    appendChildBody();
    if (!childBody.empty() && childBody.back() != '\n')
      wrapped.text += '\n';
    appendParentReturnIfDemanded();
    deduplicateLineControlPruneCandidates(wrapped.lineControlPruneCandidates);
    deduplicateLineControlSourceMappings(wrapped.lineControlSourceMappings);
    return wrapped;
  }

  if (childBody.empty() && !childNeedsLineState && !childNeedsLayoutBarrier &&
      !parentNeedsLineState) {
    appendChildBody();
    return wrapped;
  }

  wrapped.text.reserve(childBody.size() + 128);
  appendChildEntryIfDemanded();
  appendChildBody();

  if (!wrapped.text.empty() && wrapped.text.back() != '\n')
    wrapped.text += '\n';
  appendParentReturnIfDemanded();

  deduplicateLineControlPruneCandidates(wrapped.lineControlPruneCandidates);
  deduplicateLineControlSourceMappings(wrapped.lineControlSourceMappings);
  return wrapped;

}

void RefoldEngine::MaterializeIncludeExpansion(
    uint64_t includeId, const DenseMap<uint64_t, IncludeEdits> &perInclude,
    const DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
        &macroPatchesByOwner,
    const DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>>
        &children,
    DenseMap<uint64_t, std::string> &includeExpansion,
    DenseMap<uint64_t, std::vector<FinalLineControlPruneCandidate>>
        &includeExpansionLineControlPruneCandidates,
    DenseMap<uint64_t, std::vector<FinalLineControlSourceMapping>>
        &includeExpansionLineControlSourceMappings,
    DenseMap<uint64_t, size_t> &includeExpansionStartLineNos,
    DenseMap<uint64_t, AcceptedResultCandidate>
        &includeExpansionAcceptedResults,
    DenseSet<uint64_t> *appliedExpandedMacroRootIds) const {
  // Include materialization is memoized by include id. A parent may reach the
  // same child through recursive expansion, so avoid rebuilding already
  // materialized text.
  if (includeExpansion.count(includeId)) {
    // Preseeded or previously materialized include bodies that do not carry a
    // more precise source-line witness still enter at the header start.
    if (!includeExpansionStartLineNos.count(includeId))
      includeExpansionStartLineNos[includeId] = 1;
    return;
  }

  const RefoldModel::IncludeItem *inc = model_.GetIncludeById(includeId);
  if (!inc)
    REFOLD_LOG_FATAL("include/mat", "unknown includeId {0}", includeId);


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
      REFOLD_LOG_FATAL("include/mat", "failed to read header: {0} ({1})", headerPath,
            bufOrErr.getError().message());
    }

    const MemoryBuffer &mb = **bufOrErr;
    bytes.assign(mb.getBufferStart(), mb.getBufferEnd());
  }

  auto editsIt = perInclude.find(includeId);
  // Accumulate all local edits in this header byte space, then apply them once
  // at the end. This lets include edits, macro patches, and child include
  // materializations participate in one overlap/resync pass.
  std::vector<TextEdit> edits;

  auto materializedHeaderHasReplayContextSensitiveConditionalControl = [&]() {
    // A materialized header is replayed from the including surface, not from
    // the header's original file.  Conditional operators such as
    // __has_include perform quoted-header lookup relative to the file that
    // contains the directive; moving that directive out of its original header
    // can therefore flip the selected preprocessor branch even when ordinary
    // macro state is otherwise closed.  When such conditional control is part
    // of a materialized include, use the producer-proven B-token envelope for
    // the include realization instead of copying the directive text verbatim.
    for (const RefoldModel::CondGroup *group :
         model_.GetCondGroups(headerPath, includeId)) {
      if (!group)
        continue;
      for (const RefoldModel::CondArm &arm : group->arms) {
        if (!arm.cond)
          continue;
        if (arm.cond->contains("__has_include"))
          return true;
      }
    }
    return false;
  };

  auto materializedHeaderHasIncludeLevelObserver = [&]() {
    // __INCLUDE_LEVEL__ observes include-stack depth, not line/file state.
    // Once an include owner is materialized, the source-spelled header bytes are
    // replayed from the flattened parent/TU surface instead of being entered by
    // an actual #include edge.  Leaving a preserved __INCLUDE_LEVEL__ spelling
    // in that materialized text would therefore expand at the replay depth
    // rather than at the producer-observed depth.  Treat any observable
    // __INCLUDE_LEVEL__ in this owner as a request for producer-proven B-surface
    // realization, which literalizes the recorded expansion value and avoids
    // fabricating an include-stack transition that no longer exists.
    for (const RefoldModel::MacroInvocation &macro :
         model_.GetMacroInvocations()) {
      if (macro.name != "__INCLUDE_LEVEL__")
        continue;

      const RefoldModel::MacroInvocation *site =
          LineStateObservableMacroSite(macro);
      std::optional<uint64_t> owner =
          site && site->ownerIncludeId ? site->ownerIncludeId
                                       : macro.ownerIncludeId;
      if (!owner || *owner != includeId)
        continue;

      // The owner id is the semantic proof; the optional invocation file is a
      // hardening check against malformed producer records that accidentally
      // attach a builtin from another source buffer to this include instance.
      if (site && site->invFile && !PathsEqual(*site->invFile, headerPath))
        continue;
      if (macro.invFile && !PathsEqual(*macro.invFile, headerPath) &&
          (!site || !site->invFile || !PathsEqual(*site->invFile, headerPath)))
        continue;

      return true;
    }
    return false;
  };

  auto realizeMaterializedHeaderFromB = [&](StringRef reason) -> bool {
    AcceptedResultCandidate realizationCandidate;
    if (auto realized =
            BuildInlineIncludeRealizationFromB(*inc, reason,
                                               &realizationCandidate)) {
      includeExpansion[includeId] = std::move(*realized);
      includeExpansionStartLineNos[includeId] = 1;
      includeExpansionAcceptedResults[includeId] =
          std::move(realizationCandidate);
      REFOLD_LOG_TRACE("include/mat", "realize include #{0} from B: {1}",
                       includeId, reason);
      return true;
    }

    includeExpansion[includeId] = std::string();
    return false;
  };

  if (materializedHeaderHasReplayContextSensitiveConditionalControl()) {
    (void)realizeMaterializedHeaderFromB(
        "replay-context-sensitive conditional control in materialized header");
    return;
  }

  if (materializedHeaderHasIncludeLevelObserver()) {
    (void)realizeMaterializedHeaderFromB(
        "__INCLUDE_LEVEL__ observer in materialized header");
    return;
  }

  auto includeInsertionPatchCarriesSidebandReplay =
      [&](const SidebandPragmaEdit &sideband) -> bool {
    if (editsIt == perInclude.end())
      return false;
    if (!sideband.SourceIsZeroWidthInsertion())
      return false;

    const auto sidebandB = sideband.MaterializedBByteRange();
    for (const IncludePatch &patch : editsIt->second.patches) {
      if (patch.aStart != patch.aEnd || patch.bStart >= patch.bEnd)
        continue;
      if (patch.bStart >= bTokOff_.size() || patch.bEnd > bTokOff_.size())
        continue;

      StringRef patchEnvelope =
          sliceTokenEnvelope(bTokOff_, bSource_, patch.bStart, patch.bEnd);
      const uint64_t patchB = bTokOff_[static_cast<size_t>(patch.bStart)];
      const uint64_t patchE = patchB + patchEnvelope.size();
      if (patchB <= sidebandB.first && sidebandB.second <= patchE)
        return true;
    }
    return false;
  };

  // 0) Sideband pragma edits owned by this include.  These pragmas were printed
  // verbatim in the raw `.i` replay surface but deliberately removed from the
  // normal A/B token streams before diffing.  When such a pragma lives in a
  // header, deleting/replacing it is an include-owned source edit, not a TU
  // edit.  Stage it here so it composes with the same owner-polymorphic header
  // materialization path used for ordinary include and macro edits.
  for (const SidebandPragmaEdit &sideband : sidebandPragmaEdits_) {
    if (!sideband.TargetsInclude(includeId))
      continue;

    const auto sourceRange = sideband.SourceByteRange();
    if (!ValidateSidebandPragmaEditProof(sideband, "pragma/sideband/include")) {
      includeExpansion[includeId] = std::string();
      return;
    }

    if (!sideband.SourceIsWithinOwnerBytes(static_cast<uint64_t>(bytes.size()))) {
      RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
          TerminalFallbackObligationKind::PragmaBoundaryKnown,
          TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary),
          "pragma/sideband",
          llvm::formatv("header-owned sideband pragma edit has invalid range "
                        "inc#{0} path='{1}' site=[{2},{3}) headerSize={4}",
                        includeId, headerPath, sourceRange.first,
                        sourceRange.second, bytes.size())
              .str());
      includeExpansion[includeId] = std::string();
      return;
    }

    if (includeInsertionPatchCarriesSidebandReplay(sideband)) {
      // A header-prefix/suffix ordinary insertion patch uses the full B token
      // envelope.  If that envelope already contains this sideband replay
      // range, staging a second header-owned sideband edit would duplicate the
      // pragma in the materialized header.  The sideband proof is still
      // necessary for owner selection and token-stream normalization, but the
      // include patch is the unique replay owner for the visible bytes.
      continue;
    }

    auto hasCopiedHeaderSuffixAfterSidebandEdit = [&]() -> bool {
      // A header-local resync can only protect bytes that remain in this same
      // materialized header after the sideband edit.  If the sideband edit is
      // the last visible header-owned replay text, the parent-resume wrapper is
      // the next meaningful line-state transition and an extra local #line
      // would be gratuitous.
      for (uint64_t p = sourceRange.second; p < bytes.size(); ++p)
        if (!stringutils::isWs(bytes[p]))
          return true;
      return false;
    };

    // A sideband pragma line is not part of the normal PP token stream, but it
    // can still be visible header-owned replay text.  The include enter/exit
    // wrapper handles the owner transition for that visible replay.
    // Header-local resync is narrower: emit it only when sideband-owned
    // trailing B whitespace would otherwise shift copied header suffix bytes,
    // or when the suffix has a source-location observer such as
    // __LINE__/__FILE__.
    const bool mustPreserveHeaderLineState =
        (sideband.ForcesIncludeLineDirectiveWrappers() &&
         hasCopiedHeaderSuffixAfterSidebandEdit() &&
         sideband.OwnsTrailingReplayBlankLine()) ||
        OwnerSuffixHasLineStateSensitiveBuiltin(includeId, headerPath,
                                                sourceRange.second);
    ResyncOutcome ro =
        mustPreserveHeaderLineState
            ? ApplyResyncOrPend(bytes, sourceRange.first, sourceRange.second,
                                sideband.ReplacementText(), headerPath,
                                includeId)
            : ResyncOutcome(sideband.ReplacementText().str(), std::nullopt);
    TextEdit edit{sourceRange.first,
                  sourceRange.second,
                  std::move(ro.text),
                  std::move(ro.pending),
                  std::nullopt,
                  {},
                  {},
                  {}};
    edit.lineControlPruneCandidates = std::move(ro.lineControlPruneCandidates);
    AttachAcceptedResultCarrier(
        edit, BuildAcceptedIncludeRealizationCandidate(
                  AcceptedPathKind::IncludeMaterializedExpansion, *inc));
    edits.push_back(std::move(edit));
  }

  // Replay-context stability, include-owned macro-state repair.
  //
  // A macro patch inside a materialized header is emitted as B-surface source
  // text in the header file's byte space.  That replay is only sound if the
  // text sees the same macro environment that produced B.  When the original
  // header contains an active #define before the patched invocation, the B text
  // might accidentally expand under that preserved A-side definition.  The
  // finite invariant enforced below is:
  //
  //   * identify active header-owned macro-state directives that the B text
  //     would observe at the current replay site;
  //   * carry only whole directive lines, never partial directive spellings;
  //   * move those directive lines after the completed replacement interval
  //     only across source proven not to observe them; and
  //   * fail closed if the directive cannot be parsed, located, moved, or kept
  //     on a real directive line.
  //
  // This makes replay stability a property of owner materialization itself,
  // rather than a special case of the test that motivated it.
  using MaterializedHeaderMacroStateDirectivePiece =
      MacroStateDirectiveLineInterval;

  auto materializedHeaderMacroStateBoundary = [&](uint64_t begin,
                                                  uint64_t end) {
    return OwnerStateBoundary::FromSource(OwnerSourceRange::From(
        headerPath, begin, end, std::optional<uint64_t>(includeId)));
  };

  auto checkMaterializedHeaderMacroState =
      [&](const OwnerStateBoundary &boundary, StateMutationKind mutation,
          SuffixStabilityWitness witness, StringRef stage, StringRef detail,
          bool requireKnownObserver) {
        return CheckStateTransitionAcrossEditBoundary(
            boundary, OwnerStateComponent::MacroState, mutation,
            std::move(witness), stage, detail, requireKnownObserver);
      };

  auto failMaterializedHeaderMacroState =
      [&](const OwnerStateBoundary &boundary, StateMutationKind mutation,
          StringRef detail) {
        return checkMaterializedHeaderMacroState(
            boundary, mutation,
            BuildStateTransitionWitness(
                SuffixStabilityWitnessKind::TerminalStateFailure,
                OwnerStateComponent::MacroState, boundary, detail),
            "include/materialized-macro-state", detail,
            /*requireKnownObserver=*/true);
      };

  auto widenMaterializedHeaderMacroState =
      [&](const OwnerStateBoundary &boundary, StateMutationKind mutation,
          StringRef detail) {
        return checkMaterializedHeaderMacroState(
            boundary, mutation,
            BuildStateTransitionWitness(
                SuffixStabilityWitnessKind::ClosureWidening,
                OwnerStateComponent::MacroState, boundary, detail),
            "include/materialized-macro-state", detail,
            /*requireKnownObserver=*/false);
      };

  auto materializedHeaderReplacementObservesMacroStateDirective =
      [&](const RefoldModel::MacroDirective &directive, StringRef replacement,
          bool unparseableObserves) {
        return ReplacementObservesMacroStateDirective(directive, replacement,
                                                      unparseableObserves);
      };

  // Convert a recorded macro directive back to its exact interval in the
  // materialized header buffer for this include owner. The shared helper owns
  // the macro-name-anchor reconstruction and exact-byte validation.
  auto materializedHeaderMacroStateDirectiveInterval =
      [&](const RefoldModel::MacroDirective &directive) {
        return RecoverMacroStateDirectiveLineInterval(
            directive, headerPath, StringRef(bytes),
            std::optional<uint64_t>(includeId));
      };

  auto materializedHeaderSourceChunkObservesDefinitionWhenCrossed =
      [&](const RefoldModel::MacroDirective &definition, StringRef macroName,
          StringRef chunk, StringRef following) {
        return SourceChunkObservesMacroStateDirectiveWhenCrossed(
            definition, macroName, chunk, following);
      };

  // Reconstruct the active definition for `macroName` immediately before a
  // header byte offset using only directives owned by this materialized include.
  // The last matching #define/#undef before the offset wins, with directive id
  // as a deterministic tie-breaker for equal byte endpoints.
  auto activeMaterializedHeaderDefinitionAtByte =
      [&](const RefoldModel::MacroDirective &definition, StringRef macroName,
          uint64_t offset) {
        const RefoldModel::MacroDirective *active = nullptr;
        uint64_t activeEnd = 0;
        for (const auto &candidate : model_.GetMacroDirectives()) {
          std::optional<MaterializedHeaderMacroStateDirectivePiece> piece =
              materializedHeaderMacroStateDirectiveInterval(candidate);
          if (!piece || piece->end > offset)
            continue;
          if (StringRef(piece->name) != macroName)
            continue;
          if (!active || piece->end > activeEnd ||
              (piece->end == activeEnd && candidate.id > active->id)) {
            active = &candidate;
            activeEnd = piece->end;
          }
        }
        return active == &definition && definition.subkind == "#define";
      };

  // A macro-state repair interval must not overlap an edit already staged
  // for this materialized header.  Overlap would require composing two proof
  // artifacts, which belongs to the global edit-composition layer rather than
  // this local replay-stability repair.
  auto materializedHeaderRangeOverlapsStagedEdit = [&](uint64_t begin,
                                                       uint64_t end) {
    for (const TextEdit &edit : edits)
      if (begin < edit.end && end > edit.start)
        return true;
    return false;
  };

  // Extend a macro patch to the end of its physical line before appending
  // carried directive lines.  This guarantees that moved #define lines remain
  // real preprocessing directives, not tokens pasted onto the invocation line.
  auto materializedHeaderLineEndAfter = [&](uint64_t offset) {
    uint64_t p = std::min<uint64_t>(offset, bytes.size());
    while (p < bytes.size() && bytes[static_cast<size_t>(p)] != '\n')
      ++p;
    if (p < bytes.size())
      ++p;
    return p;
  };

  // Replacement interval produced by replay stabilization.  The interval may
  // be wider than the original macro invocation because it can consume earlier
  // #define lines and the invocation's physical-line suffix, then re-emit the
  // carried definitions after the replacement payload.
  struct StabilizedMaterializedHeaderMacroPatch {
    uint64_t start = 0;
    uint64_t end = 0;
    std::string replacement;
  };

  // Stabilize an include-owned macro patch against the macro state at its
  // final replay position.
  //
  // Return value protocol:
  //   * std::nullopt: no active header definition is observed by the B text, so
  //     the original patch can be staged unchanged;
  //   * non-empty StabilizedMaterializedHeaderMacroPatch: stage the widened,
  //     replay-stable repair interval returned here; and
  //   * empty {0,0,""}: terminal fallback was requested and materialization of
  //     this include must stop.
  //
  // This keeps candidate selection deterministic: the function either proves a
  // concrete source rewrite that restores B's macro context, proves that no
  // rewrite is needed, or records the missing proof obligation.
  auto stabilizeMaterializedHeaderMacroPatchReplay =
      [&](const MacroPatch &mp, uint64_t mpEnd)
          -> std::optional<StabilizedMaterializedHeaderMacroPatch> {
    if (mp.invStart > mp.invEnd || mp.invEnd > bytes.size() ||
        mpEnd < mp.invEnd || mpEnd > bytes.size())
      return std::nullopt;

    // Collect active #define lines that the B replacement would observe if
    // emitted at the original invocation site.  Only movable whole directives
    // become candidates; if an observed definition exists but cannot be moved,
    // the proof fails closed below.
    SmallVector<MaterializedHeaderMacroStateDirectivePiece, 4> candidates;
    bool observedUncarriedDefinition = false;
    for (const auto &directive : model_.GetMacroDirectives()) {
      if (directive.subkind != "#define")
        continue;
      std::optional<MaterializedHeaderMacroStateDirectivePiece> piece =
          materializedHeaderMacroStateDirectiveInterval(directive);
      if (!piece || piece->end > mp.invStart)
        continue;
      if (!activeMaterializedHeaderDefinitionAtByte(directive, piece->name,
                                                    mp.invStart))
        continue;
      if (!materializedHeaderReplacementObservesMacroStateDirective(
              directive, StringRef(mp.replacement),
              /*unparseableObserves=*/false))
        continue;

      observedUncarriedDefinition = true;
      if (materializedHeaderRangeOverlapsStagedEdit(piece->begin, piece->end))
        continue;
      candidates.push_back(std::move(*piece));
    }

    if (candidates.empty()) {
      if (!observedUncarriedDefinition)
        return std::nullopt;
      (void)failMaterializedHeaderMacroState(
          materializedHeaderMacroStateBoundary(mp.invStart, mpEnd),
          StateMutationKind::Materialized,
          llvm::formatv("include-owned macro patch in {0} observes active "
                        "header macro state before [{1},{2}) but no movable "
                        "definition line was proven",
                        headerPath, mp.invStart, mpEnd)
              .str());
      return StabilizedMaterializedHeaderMacroPatch{};
    }

    // Emit carried directives in source order so the relative macro-state
    // transition order among multiple carried definitions is preserved.
    llvm::sort(candidates,
               [](const MaterializedHeaderMacroStateDirectivePiece &lhs,
                  const MaterializedHeaderMacroStateDirectivePiece &rhs) {
                 if (lhs.begin != rhs.begin)
                   return lhs.begin < rhs.begin;
                 return lhs.directive->id < rhs.directive->id;
               });

    // Widen from the first carried directive through the completed invocation
    // line.  The suffix after the invocation is consumed and replayed before
    // the carried directives, which keeps the directives line-started after
    // repair.
    const uint64_t newStart = candidates.front().begin;
    const uint64_t newEnd = materializedHeaderLineEndAfter(mpEnd);
    if (newStart > mp.invStart || newEnd < mpEnd || newEnd > bytes.size() ||
        materializedHeaderRangeOverlapsStagedEdit(newStart, newEnd)) {
      (void)failMaterializedHeaderMacroState(
          materializedHeaderMacroStateBoundary(newStart, newEnd),
          StateMutationKind::WidenedIntoClosure,
          llvm::formatv("include-owned macro patch in {0} cannot widen "
                        "macro-state repair interval [{1},{2})",
                        headerPath, newStart, newEnd)
              .str());
      return StabilizedMaterializedHeaderMacroPatch{};
    }

    // Helper used while rebuilding the replacement to distinguish directive
    // bytes intentionally moved to the end from ordinary source bytes that must
    // remain in their original relative order.
    auto isCarriedDirectiveInterval = [&](uint64_t begin, uint64_t end) {
      for (const auto &candidate : candidates)
        if (candidate.begin == begin && candidate.end == end)
          return true;
      return false;
    };

    // Build the source text that a carried directive crosses, excluding the
    // carried directive lines themselves.  That text is the proof obligation
    // for non-observation: if it observes the definition being delayed,
    // movement is not semantics-preserving.
    auto appendCrossedSourceExcludingCarried = [&](std::string &out,
                                                   uint64_t begin,
                                                   uint64_t end) {
      uint64_t cursor = begin;
      for (const auto &candidate : candidates) {
        if (candidate.end <= cursor || candidate.begin >= end)
          continue;
        if (cursor < candidate.begin)
          out.append(bytes.begin() + cursor, bytes.begin() + candidate.begin);
        cursor = std::max(cursor, candidate.end);
      }
      if (cursor < end)
        out.append(bytes.begin() + cursor, bytes.begin() + end);
    };

    // The replacement payload will be emitted before each carried definition,
    // and the original line suffix after the macro invocation must also remain
    // before those definitions so the directives still start on real directive
    // lines.  Therefore every byte crossed by each carried definition must be
    // proven non-observing for that definition.  Otherwise the macro-state move
    // would change how preserved header source preprocesses.
    for (const auto &candidate : candidates) {
      std::string crossed;
      appendCrossedSourceExcludingCarried(crossed, candidate.end, mp.invStart);
      appendCrossedSourceExcludingCarried(crossed, mpEnd, newEnd);
      StringRef following = StringRef(bytes).slice(mp.invStart, mpEnd);
      if (materializedHeaderSourceChunkObservesDefinitionWhenCrossed(
              *candidate.directive, candidate.name, StringRef(crossed),
              following)) {
        (void)failMaterializedHeaderMacroState(
            materializedHeaderMacroStateBoundary(candidate.begin,
                                                 candidate.end),
            StateMutationKind::MovedLater,
            llvm::formatv("include-owned macro patch in {0} cannot carry "
                          "#{1} for '{2}' across observing header source",
                          headerPath, candidate.directive->id, candidate.name)
                .str());
        return StabilizedMaterializedHeaderMacroPatch{};
      }
    }

    // Rebuild the widened interval in the only order that preserves B's
    // replay context and the surviving suffix context:
    //
    //   1. original source before each carried definition,
    //   2. original source between the last carried definition and callsite,
    //   3. the B replacement text,
    //   4. the original physical-line suffix after the callsite, and
    //   5. the carried #define lines.
    //
    // Thus the B replacement is evaluated before the carried definitions, while
    // later surviving source sees those definitions restored.
    std::string replacement;
    replacement.reserve((mp.invStart - newStart) + mp.replacement.size() +
                        (newEnd - mpEnd) + candidates.size() * 32);
    uint64_t cursor = newStart;
    for (const auto &candidate : candidates) {
      if (!isCarriedDirectiveInterval(candidate.begin, candidate.end) ||
          cursor > candidate.begin) {
        (void)failMaterializedHeaderMacroState(
            materializedHeaderMacroStateBoundary(newStart, newEnd),
            StateMutationKind::MovedLater,
            llvm::formatv("include-owned macro patch in {0} found invalid "
                          "carried directive ordering",
                          headerPath)
                .str());
        return StabilizedMaterializedHeaderMacroPatch{};
      }
      replacement.append(bytes.begin() + cursor,
                         bytes.begin() + candidate.begin);
      cursor = candidate.end;
    }
    replacement.append(bytes.begin() + cursor, bytes.begin() + mp.invStart);
    replacement += mp.replacement;
    replacement.append(bytes.begin() + mpEnd, bytes.begin() + newEnd);
    if (!replacement.empty() && replacement.back() != '\n')
      replacement.push_back('\n');
    for (const auto &candidate : candidates) {
      replacement.append(candidate.directive->text.begin(),
                         candidate.directive->text.end());
      if (replacement.empty() || replacement.back() != '\n')
        replacement.push_back('\n');
    }

    for (const auto &candidate : candidates) {
      (void)widenMaterializedHeaderMacroState(
          materializedHeaderMacroStateBoundary(candidate.begin, candidate.end),
          StateMutationKind::MovedLater,
          llvm::formatv(
              "include-owned macro patch in {0} carries #define #{1} for "
              "'{2}' after replay interval [{3},{4})",
              headerPath, candidate.directive->id, candidate.name, newStart,
              newEnd)
              .str());
    }

    StabilizedMaterializedHeaderMacroPatch stabilized;
    stabilized.start = newStart;
    stabilized.end = newEnd;
    stabilized.replacement = std::move(replacement);
    return stabilized;
  };

  // 1) Macro patches owned by this include. Macro invocation ranges are already
  // recorded in the owner file's byte space, so they can be converted directly
  // into local header TextEdits.  Before staging the TextEdit, prove replay
  // context stability for the B-surface text: if the replacement would observe
  // an active header-owned #define that B did not have active, move that
  // definition after the completed replacement line.  Macro-state movement is
  // admitted only when every crossed source byte is proven non-observing;
  // otherwise materialization fails closed instead of emitting a refolding that
  // preprocesses under the wrong macro environment.
  if (auto it = macroPatchesByOwner.find(includeId);
      it != macroPatchesByOwner.end()) {
    for (const auto &mp : it->second) {
      uint64_t mpEnd = stringutils::extendChainedCallEnd(
          StringRef(bytes), mp.invEnd, mp.replacement);
      uint64_t editStart = mp.invStart;
      uint64_t editEnd = mpEnd;
      std::string editReplacement = mp.replacement;
      if (std::optional<StabilizedMaterializedHeaderMacroPatch> stabilized =
              stabilizeMaterializedHeaderMacroPatchReplay(mp, mpEnd)) {
        // Empty {0,0,""} is the local sentinel used by the stabilizer after
        // it has requested terminal fallback.  Stop materializing this include
        // so no partially proven header edit is emitted.
        if (stabilized->replacement.empty() && stabilized->start == 0 &&
            stabilized->end == 0) {
          includeExpansion[includeId] = std::string();
          return;
        }
        editStart = stabilized->start;
        editEnd = stabilized->end;
        editReplacement = std::move(stabilized->replacement);
      }

      // Apply local line-resync policy before staging the edit, and carry the
      // accepted macro proof metadata to the emitted TextEdit.
      ResyncOutcome ro = ApplyResyncOrPend(
          bytes, editStart, editEnd, editReplacement, headerPath, includeId);
      TextEdit edit{editStart,
                    editEnd,
                    std::move(ro.text),
                    std::move(ro.pending),
                    MacroPatchRemainsExpanded(mp)
                        ? std::make_optional(GetRootMacroId(mp.macroId))
                        : std::nullopt,
                    {},
                    {},
                    {}};
      edit.lineControlPruneCandidates =
          std::move(ro.lineControlPruneCandidates);
      AttachAcceptedResultCarrier(edit, BuildAcceptedEmittedMacroCandidate(mp));
      edits.push_back(std::move(edit));
    }
  }

  // 2) Include-preserving edits owned by this include. If those edits cannot be
  // discharged as local anchored edits, materialize the whole include directly
  // from B using the include-realization proof path.
  if (auto it = perInclude.find(includeId); it != perInclude.end()) {
    if (!it->second.patches.empty()) {

      IncludeTextEditPlan plan = ComputeIncludeTextEdits(it->second, bytes);
      if (plan.requiresIncludeRealization) {
        AcceptedResultCandidate realizationCandidate;
        if (auto realized = BuildInlineIncludeRealizationFromB(
                *inc, plan.realizationReason, &realizationCandidate)) {
          includeExpansion[includeId] = std::move(*realized);
          includeExpansionStartLineNos[includeId] = 1;
          includeExpansionAcceptedResults[includeId] =
              std::move(realizationCandidate);
          return;
        }
        includeExpansion[includeId] = std::string();
        return;
      }

      for (auto &te : plan.edits) {
        // ComputeIncludeTextEdits() already applies the owner-local padding and
        // line-resync proof for each header edit.  Re-running resync here would
        // treat a deliberately inserted `#line` as fresh newline drift and can
        // duplicate the same local resume before the copied suffix.
        edits.push_back(std::move(te));
      }
    }
  }

  // Return true if this include subtree contains any work that requires
  // materialization: local include edits, local macro patches, or descendant
  // includes with work. Clean child subtrees are left as spelled directives.
  auto includeHasSidebandPragmaWork = [&](uint64_t id) {
    return llvm::any_of(sidebandPragmaEdits_, [&](const SidebandPragmaEdit &e) {
      return e.TargetsInclude(id);
    });
  };

  auto includeHasLineDirectiveForcingSidebandWork = [&](uint64_t id) {
    return llvm::any_of(sidebandPragmaEdits_, [&](const SidebandPragmaEdit &e) {
      return e.TargetsInclude(id) && e.ForcesIncludeLineDirectiveWrappers();
    });
  };

  auto includeHasOrdinaryReplayTokens = [&](uint64_t id) {
    if (const auto *item = model_.GetIncludeById(id))
      return item->cover.end > item->cover.begin;
    return false;
  };

  // Return true when the materialized include subtree needs replay provenance
  // only from sideband pragma edits.  This is intentionally separate from the
  // #line-wrapper classification below: visible sideband replacement text may
  // force ordinary enter/exit #line wrappers while the include still has a
  // zero-token normal PP cover, so its edit-map witness must come from the
  // sideband B-byte envelope rather than from a nonexistent token envelope.
  auto includeUsesOnlySidebandReplayEnvelope =
      [&](auto &&self, uint64_t id) -> bool {
    bool sawSideband = includeHasSidebandPragmaWork(id);
    if (includeHasOrdinaryReplayTokens(id))
      return false;
    if (auto it = perInclude.find(id);
        it != perInclude.end() && !it->second.patches.empty())
      return false;
    if (auto it = macroPatchesByOwner.find(std::optional<uint64_t>(id));
        it != macroPatchesByOwner.end() && !it->second.empty())
      return false;
    if (auto it = children.find(id); it != children.end()) {
      for (const auto *child : it->second) {
        if (!self(self, child->id))
          return false;
        sawSideband = true;
      }
    }
    return sawSideband;
  };

  // A sideband-only include materialization is the one class where omitting
  // otherwise-unobservable #line wrappers is intentional.  The invariant is not
  // "materialized because of a sideband edit"; it is stricter: the materialized
  // owner must have no ordinary PP-token replay of its own.  Once we emit
  // normal header tokens while replacing an include directive, --with-lines
  // must preserve the historical enter/exit file transition even if no current
  // token observes that state.  Ordinary include patches, macro patches, B-only
  // header sideband insertions, and non-empty include token covers all re-enter
  // that policy.
  enum class MaterializationWorkClass { None, SidebandPragmaOnly, Ordinary };
  auto classifyMaterializationWork =
      [&](auto &&self, uint64_t id) -> MaterializationWorkClass {
    bool sawSideband = includeHasSidebandPragmaWork(id);
    if (includeHasLineDirectiveForcingSidebandWork(id))
      return MaterializationWorkClass::Ordinary;
    if (sawSideband && includeHasOrdinaryReplayTokens(id))
      return MaterializationWorkClass::Ordinary;
    if (auto it = perInclude.find(id);
        it != perInclude.end() && !it->second.patches.empty())
      return MaterializationWorkClass::Ordinary;
    if (auto it = macroPatchesByOwner.find(std::optional<uint64_t>(id));
        it != macroPatchesByOwner.end() && !it->second.empty())
      return MaterializationWorkClass::Ordinary;
    if (auto it = children.find(id); it != children.end()) {
      for (const auto *child : it->second) {
        switch (self(self, child->id)) {
        case MaterializationWorkClass::Ordinary:
          return MaterializationWorkClass::Ordinary;
        case MaterializationWorkClass::SidebandPragmaOnly:
          sawSideband = true;
          break;
        case MaterializationWorkClass::None:
          break;
        }
      }
    }
    return sawSideband ? MaterializationWorkClass::SidebandPragmaOnly
                       : MaterializationWorkClass::None;
  };

  enum class CleanChildIncludeReplayAction { None, RewriteOperand, Materialize };

  struct CleanChildIncludeReplayPlan {
    CleanChildIncludeReplayAction action = CleanChildIncludeReplayAction::None;
    std::string rewrittenOperand;
    std::string reason;
  };

  auto absolutePathInPPCwd = [&](StringRef path) -> std::filesystem::path {
    std::filesystem::path fsPath(path.str());
    if (fsPath.is_absolute())
      return fsPath;
    return std::filesystem::path(model_.GetPPCwd().str()) / fsPath;
  };

  auto canonicalizeForLookupProof = [&](StringRef path)
      -> std::optional<std::filesystem::path> {
    std::error_code ec;
    std::filesystem::path canonical =
        std::filesystem::weakly_canonical(absolutePathInPPCwd(path), ec);
    if (ec)
      return std::nullopt;
    return canonical;
  };

  auto pathExists = [&](const std::filesystem::path &path) -> bool {
    const std::string spelling = path.generic_string();
    return llvm::sys::fs::exists(StringRef(spelling));
  };

  auto sourceDirectoryForReplaySurface = [&]()
      -> std::optional<std::filesystem::path> {
    std::optional<std::filesystem::path> source =
        canonicalizeForLookupProof(model_.GetSourcePath());
    if (!source)
      return std::nullopt;
    source->remove_filename();
    return source;
  };

  auto appendSearchDirIfSafe = [&](SmallVectorImpl<std::filesystem::path> &dirs,
                                   StringRef path) {
    if (path.empty())
      return;
    dirs.push_back(absolutePathInPPCwd(path));
  };

  auto recordedQuotedIncludeSearchDirs = [&]() {
    SmallVector<std::filesystem::path, 16> quoteDirs;
    SmallVector<std::filesystem::path, 32> includeDirs;
    ArrayRef<std::string> argv = model_.GetPPArgv();

    for (size_t i = 0; i < argv.size(); ++i) {
      StringRef arg(argv[i]);
      auto consumeJoinedOrSeparate = [&](StringRef joinedPrefix,
                                         SmallVectorImpl<std::filesystem::path>
                                             &out) -> bool {
        if (arg == joinedPrefix) {
          if (i + 1 < argv.size())
            appendSearchDirIfSafe(out, argv[++i]);
          return true;
        }
        if (arg.starts_with(joinedPrefix) && arg.size() > joinedPrefix.size()) {
          appendSearchDirIfSafe(out, arg.drop_front(joinedPrefix.size()));
          return true;
        }
        return false;
      };

      if (consumeJoinedOrSeparate("-iquote", quoteDirs))
        continue;
      if (consumeJoinedOrSeparate("-I", includeDirs))
        continue;
      if ((arg == "-isystem" || arg == "-idirafter") && i + 1 < argv.size()) {
        appendSearchDirIfSafe(includeDirs, argv[++i]);
        continue;
      }
      const StringRef isystemPrefix("-isystem");
      const StringRef idirafterPrefix("-idirafter");
      if (arg.starts_with(isystemPrefix) && arg.size() > isystemPrefix.size()) {
        appendSearchDirIfSafe(includeDirs, arg.drop_front(isystemPrefix.size()));
        continue;
      }
      if (arg.starts_with(idirafterPrefix) &&
          arg.size() > idirafterPrefix.size()) {
        appendSearchDirIfSafe(includeDirs, arg.drop_front(idirafterPrefix.size()));
        continue;
      }
    }

    quoteDirs.append(includeDirs.begin(), includeDirs.end());
    return quoteDirs;
  };

  auto resolveQuotedIncludeFromReplaySurface = [&](StringRef operand)
      -> std::optional<std::filesystem::path> {
    std::optional<std::filesystem::path> sourceDir =
        sourceDirectoryForReplaySurface();
    if (!sourceDir)
      return std::nullopt;

    std::filesystem::path direct = *sourceDir / operand.str();
    if (pathExists(direct))
      return direct;

    for (const std::filesystem::path &dir : recordedQuotedIncludeSearchDirs()) {
      std::filesystem::path candidate = dir / operand.str();
      if (pathExists(candidate))
        return candidate;
    }

    return std::nullopt;
  };

  auto replaySurfaceQuotedIncludePreservesResolvedPath =
      [&](const RefoldModel::IncludeItem &child) -> bool {
    if (child.subkind != "#include" || child.angled || !child.resolvedPath)
      return false;

    std::optional<std::string> operand = quotedIncludeReplayLookupOperand(child);
    if (!operand)
      return false;

    std::optional<std::filesystem::path> replayResolved =
        resolveQuotedIncludeFromReplaySurface(*operand);
    if (!replayResolved)
      return false;

    const std::string replaySpelling = replayResolved->generic_string();
    return PathsEqual(replaySpelling, *child.resolvedPath);
  };

  auto synthesizeQuotedIncludeOperandForReplaySurface =
      [&](const RefoldModel::IncludeItem &child) -> std::optional<std::string> {
    if (!child.resolvedPath)
      return std::nullopt;

    std::optional<std::filesystem::path> sourceDir =
        sourceDirectoryForReplaySurface();
    std::optional<std::filesystem::path> resolved =
        canonicalizeForLookupProof(*child.resolvedPath);
    if (!sourceDir || !resolved)
      return std::nullopt;

    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(*resolved,
                                                               *sourceDir, ec);
    if (ec || relative.empty())
      return std::nullopt;

    const std::string operand = relative.generic_string();
    if (!safeRewrittenQuotedIncludeOperand(operand))
      return std::nullopt;

    SmallString<256> replayPath(sourceDir->generic_string());
    llvm::sys::path::append(replayPath, operand);
    StringRef replayPathRef(replayPath.data(), replayPath.size());
    if (!llvm::sys::fs::exists(replayPathRef) ||
        !PathsEqual(replayPathRef, *child.resolvedPath))
      return std::nullopt;

    return operand;
  };

  auto includeSubtreeHasIncludeLevelSensitiveBuiltin =
      [&](uint64_t includeId) -> bool {
    auto isDescendantOrSelf = [&](uint64_t owner) -> bool {
      uint64_t cur = owner;
      while (true) {
        if (cur == includeId)
          return true;
        const RefoldModel::IncludeItem *inc = model_.GetIncludeById(cur);
        if (!inc || !inc->parent)
          return false;
        cur = *inc->parent;
      }
    };

    for (const auto &macro : model_.GetMacroInvocations()) {
      if (macro.name != "__INCLUDE_LEVEL__")
        continue;

      // As with line/file builtins, a predefined builtin may be recorded at a
      // definition-site spelling while the preserved observable use is an
      // outer caller.  Attribute the observer to the source site that would
      // remain spelled in the replayed child subtree.
      const RefoldModel::MacroInvocation *site = &macro;
      unsigned depth = 0;
      const unsigned maxDepth =
          static_cast<unsigned>(model_.GetMacroInvocations().size());
      while (site->callerMacroId && depth++ < maxDepth) {
        const RefoldModel::MacroInvocation *caller =
            FindMacroInvocationById(*site->callerMacroId);
        if (!caller)
          break;
        site = caller;
      }

      std::optional<uint64_t> owner = site->ownerIncludeId;
      if (!owner)
        owner = macro.ownerIncludeId;
      if (!owner || !isDescendantOrSelf(*owner))
        continue;

      if (!LineStateBuiltinInvocationIsPreservedObserver(macro))
        continue;

      return true;
    }

    return false;
  };

  auto planCleanChildIncludeReplayFromMaterializedParent =
      [&](const RefoldModel::IncludeItem &child) -> CleanChildIncludeReplayPlan {
    CleanChildIncludeReplayPlan plan;

    // A clean child include may stay spelled as an include directive only when
    // replaying that exact directive from the materialized parent surface resolves
    // to the same physical file the producer recorded and the subtree does not
    // observe include-entry depth.  Materializing a parent header moves the child
    // include one level outward; even a lookup-stable directive would make a
    // preserved __INCLUDE_LEVEL__ expansion observe the replay stack rather than
    // the producer stack.
    if (!child.parent || !child.resolvedPath)
      return plan;

    if (includeSubtreeHasIncludeLevelSensitiveBuiltin(child.id)) {
      plan.action = CleanChildIncludeReplayAction::Materialize;
      plan.reason = "child subtree observes __INCLUDE_LEVEL__ across "
                    "materialized-parent include relocation";
      return plan;
    }

    if (child.subkind == "#include_next" && child.angled) {
      // An angled include-next has no quoted operand to replay from the
      // materialized surface, and copying it into the primary source restarts
      // the include search from the beginning.  Materialize the clean child
      // owner rather than preserving a directive whose search-stack semantics
      // are no longer reproducible.
      plan.action = CleanChildIncludeReplayAction::Materialize;
      plan.reason = "angled #include_next cannot be replayed from a "
                    "materialized parent surface without the original "
                    "include-next search stack";
      return plan;
    }

    if (child.subkind == "#include") {
      if (child.angled || replaySurfaceQuotedIncludePreservesResolvedPath(child))
        return plan;
    }

    bool mayRewriteUnstableOperand = false;
    std::string reason;

    if (child.subkind == "#include_next") {
      // `#include_next` is defined by the producer's include-search stack.  Once
      // the directive is copied into a materialized parent surface, that stack is
      // gone.  A deterministic ordinary include naming the recorded resolved file
      // is therefore the only include-preserving repair, provided the child does
      // not itself observe line/file state through preserved builtins.
      mayRewriteUnstableOperand = true;
      reason = "#include_next cannot be replayed from a materialized parent "
               "surface without the original include-next search stack";
    } else {
      std::optional<std::string> childReplayOperand =
          quotedIncludeReplayLookupOperand(child);
      if (!childReplayOperand)
        return plan;

      std::optional<std::filesystem::path> replayResolved =
          resolveQuotedIncludeFromReplaySurface(*childReplayOperand);
      if (replayResolved) {
        const std::string replaySpelling = replayResolved->generic_string();
        if (PathsEqual(replaySpelling, *child.resolvedPath))
          return plan;

        // A concrete replay-surface hit for the original quoted operand is a
        // real shadowing conflict.  Rewriting around that shadow would invent a
        // third lookup event rather than preserving the producer-observed event,
        // so the owner-closed repair is to materialize the child edge.
        reason = "quoted operand resolves to a different file at replay surface";
      } else {
        // The original quoted operand has no replay-surface lookup target, so
        // there is no concrete TU-surface shadow to bypass.  Whether the operand
        // was a parent-directory spelling (`../leaf.h`) or a simple
        // header-relative spelling (`leaf.h`), materializing the parent has
        // removed the original including-header directory from quoted lookup.
        // A deterministic TU-surface operand naming the producer-recorded file
        // is therefore an owner-closed include-preserving repair.  The line/file
        // observer guard below still rejects this rewrite when the child subtree
        // observes replay location.
        mayRewriteUnstableOperand = true;
        reason = includeOperandHasParentComponent(*childReplayOperand)
                     ? "parent-directory quoted operand has no direct replay "
                       "target"
                     : "quoted operand has no replay-surface lookup target";
      }
    }

    if (mayRewriteUnstableOperand &&
        IncludeSubtreeHasLineStateSensitiveBuiltin(child.id)) {
      // Rewriting the include operand can change the physical/presumed file and
      // line state observed inside the child.  Without a more specific proof for
      // those observers, materialize the child text instead.
      mayRewriteUnstableOperand = false;
      if (!reason.empty())
        reason += "; ";
      reason += "child subtree observes line/file state";
    }

    if (mayRewriteUnstableOperand) {
      if (std::optional<std::string> rewritten =
              synthesizeQuotedIncludeOperandForReplaySurface(child)) {
        plan.action = CleanChildIncludeReplayAction::RewriteOperand;
        plan.rewrittenOperand = std::move(*rewritten);
        plan.reason = std::move(reason);
        return plan;
      }
    }

    plan.action = CleanChildIncludeReplayAction::Materialize;
    plan.reason = std::move(reason);
    return plan;
  };

  auto extendIncludeDirectiveEnd = [&](const RefoldModel::IncludeItem &item,
                                       StringRef ownerBytes,
                                       uint64_t siteStart) -> uint64_t {
    uint64_t siteEnd = std::clamp<uint64_t>(item.siteE, siteStart,
                                            ownerBytes.size());
    if (siteStart >= static_cast<uint64_t>(ownerBytes.size()))
      return siteEnd;

    size_t cursor = static_cast<size_t>(siteStart);
    while (true) {
      size_t nl = ownerBytes.find('\n', cursor);
      if (nl == StringRef::npos)
        return static_cast<uint64_t>(ownerBytes.size());
      cursor = nl + 1;
      if (!stringutils::isLineSplice(ownerBytes, nl))
        return std::max<uint64_t>(siteEnd, cursor);
    }
  };

  auto rewriteIncludeNextDirectiveAsOrdinaryInclude =
      [](std::string &replacement) -> bool {
    StringRef text(replacement);
    std::optional<std::pair<size_t, size_t>> keyword =
        findIncludeDirectiveKeywordRange(text);
    if (!keyword)
      return false;

    StringRef spelling = text.slice(keyword->first, keyword->second);
    if (spelling != "include_next")
      return false;

    replacement.replace(keyword->first, keyword->second - keyword->first,
                        "include");
    return true;
  };

  auto makeCleanChildIncludeOperandRewriteEdit =
      [&](const RefoldModel::IncludeItem &child, const std::string &operand,
          StringRef ownerBytes) -> std::optional<TextEdit> {
    const size_t n = ownerBytes.size();
    const uint64_t siteStart = std::clamp<uint64_t>(child.siteB, 0ULL, n);
    const uint64_t siteEnd = extendIncludeDirectiveEnd(child, ownerBytes,
                                                       siteStart);
    if (siteStart >= siteEnd)
      return std::nullopt;
    if (!child.cover.IsValid() || child.cover.begin >= child.cover.end)
      return std::nullopt;

    StringRef directiveBytes = ownerBytes.slice(siteStart, siteEnd);
    std::optional<IncludeDirectiveHeaderOperandRange> targetRange =
        findIncludeDirectiveHeaderOperandRange(directiveBytes, child.target);
    if (!targetRange)
      return std::nullopt;

    std::string replacement = directiveBytes.str();
    replacement.replace(targetRange->Begin,
                        targetRange->End - targetRange->Begin,
                        (Twine("\"") + operand + "\"").str());
    if (child.subkind == "#include_next" &&
        !rewriteIncludeNextDirectiveAsOrdinaryInclude(replacement))
      return std::nullopt;

    // Rewriting a clean child include operand is a parent-surface source edit,
    // not child expansion.  The proof witness is the child's own mapped token
    // envelope: the rewritten directive is accepted only because it is proven to
    // resolve to the same physical file and therefore to reproduce the same
    // child PP-token range.  Stamping a normal include-preserving carrier keeps
    // strict theorem/audit mode from treating this deterministic closure repair
    // as an unproven emission artifact.
    IncludePatch patch{&child,
                       replacement,
                       child.cover.begin,
                       child.cover.end,
                       child.cover.begin,
                       child.cover.end};
    patch.hasDirectHeaderByteRange = true;
    patch.directHeaderByteBegin = siteStart;
    patch.directHeaderByteEnd = siteEnd;

    IncludeAnchorWitness witness;
    witness.evidence = IncludeAnchorEvidenceKind::MappedHeaderTokens;
    witness.hasByteRange = true;
    witness.startByte = siteStart;
    witness.endByte = siteEnd;
    witness.hasFirstPP = true;
    witness.firstPP = child.cover.begin;
    witness.hasLastPP = true;
    witness.lastPP = child.cover.end - 1;

    TextEdit edit{siteStart, siteEnd, replacement, std::nullopt, std::nullopt,
                  {}, {}, {}};
    AttachAcceptedResultCarrier(
        edit, BuildAcceptedIncludeCandidate(
                  AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens,
                  patch, &witness));
    return edit;
  };

  // This predicate answers only whether a descendant already carries real
  // materialization work: token/sideband include patches or macro patches.
  // It must not recursively apply the clean-child replay-context test.  That
  // test is meaningful only for the immediate child directive that would be
  // copied into the currently materialized surface.  If a clean child remains
  // an include directive, its own children are still replayed from the child's
  // original header context, not from the outer materialized surface.
  auto hasDescendantWork = [&](auto &&self, uint64_t id) -> bool {
    bool selfWork = includeHasSidebandPragmaWork(id);
    if (auto it = perInclude.find(id); it != perInclude.end())
      selfWork = selfWork || !it->second.patches.empty();
    if (!selfWork) {
      if (auto it = macroPatchesByOwner.find(id);
          it != macroPatchesByOwner.end())
        selfWork = !it->second.empty();
    }

    if (selfWork) {
      return true;
    }

    if (auto it = children.find(id); it != children.end()) {
      for (const auto *child : it->second) {
        if (self(self, child->id)) {
          return true;
        }
      }
    }

    return false;
  };

  // 3) Materialize child includes only when their subtree has work. For each
  // dirty child, recursively build the child's fully materialized text and then
  // replace the child's include directive in this header.
  if (auto it = children.find(includeId); it != children.end()) {
    for (const auto *child : it->second) {
      const bool todo = hasDescendantWork(hasDescendantWork, child->id);
      if (!todo) {
        CleanChildIncludeReplayPlan replayPlan =
            planCleanChildIncludeReplayFromMaterializedParent(*child);
        if (replayPlan.action == CleanChildIncludeReplayAction::RewriteOperand) {
          if (std::optional<TextEdit> rewrite =
                  makeCleanChildIncludeOperandRewriteEdit(
                      *child, replayPlan.rewrittenOperand, bytes)) {
            REFOLD_LOG_TRACE(
                "include/lookup",
                "rewrite clean child include #{0} for materialized-parent "
                "quote lookup: target={1} rewritten=\"{2}\" reason={3}",
                child->id, child->target, replayPlan.rewrittenOperand,
                replayPlan.reason);
            edits.push_back(std::move(*rewrite));
          } else {
            // The lookup proof showed that preserving the original operand is
            // unsafe, but the source spelling was not rewriteable as a bounded
            // parent-surface edit.  Fall back to owner-closed child
            // materialization rather than silently preserving an unstable
            // directive.
            REFOLD_LOG_TRACE(
                "include/lookup",
                "failed to rewrite clean child include #{0}; materializing "
                "child edge instead",
                child->id);
            replayPlan.action = CleanChildIncludeReplayAction::Materialize;
          }
        }

        if (replayPlan.action != CleanChildIncludeReplayAction::Materialize) {
          // No descendant edits depend on this child.  Either the original
          // directive was lookup-stable and can stay as-is, or we staged a
          // local operand rewrite above.
          continue;
        }
      }

      // Depth-first materialization ensures the child replacement is already
      // complete before it is wrapped and inserted into the parent header.
      MaterializeIncludeExpansion(child->id, perInclude, macroPatchesByOwner,
                                  children, includeExpansion,
                                  includeExpansionLineControlPruneCandidates,
                                  includeExpansionLineControlSourceMappings,
                                  includeExpansionStartLineNos,
                                  includeExpansionAcceptedResults,
                                  appliedExpandedMacroRootIds);

      const auto &childText = includeExpansion[child->id];
      const size_t n = bytes.size();
      const uint64_t siteStart = std::clamp<uint64_t>(child->siteB, 0ULL, n);
      uint64_t siteEnd = extendIncludeDirectiveEnd(*child, bytes, siteStart);

      if (siteStart < siteEnd) {
        const bool sidebandOnly = classifyMaterializationWork(
                                      classifyMaterializationWork, child->id) ==
                                  MaterializationWorkClass::SidebandPragmaOnly;
        const size_t childEntryLineNo =
            includeExpansionStartLineNos.lookup(child->id);
        LineDirectiveLocation parentResume =
            LineDirectiveInserter::LogicalLocationAtOffset(
                bytes, siteEnd, headerPath, model_, headerPath, includeId);
        ArrayRef<FinalLineControlPruneCandidate> childLineCandidates;
        if (auto childCandidatesIt =
                includeExpansionLineControlPruneCandidates.find(child->id);
            childCandidatesIt !=
            includeExpansionLineControlPruneCandidates.end())
          childLineCandidates = childCandidatesIt->second;
        ArrayRef<FinalLineControlSourceMapping> childLineSourceMappings;
        if (auto childMappingsIt =
                includeExpansionLineControlSourceMappings.find(child->id);
            childMappingsIt != includeExpansionLineControlSourceMappings.end())
          childLineSourceMappings = childMappingsIt->second;
        LineControlWrappedText wrapped = WrapIncludeExpansionForMaterialization(
            *child, parentResume.fileSpelling, headerPath, includeId,
            siteEnd, childEntryLineNo ? childEntryLineNo : 1,
            parentResume.lineNo, childText, childLineCandidates,
            childLineSourceMappings, sidebandOnly);
        TextEdit edit{siteStart,
                      siteEnd,
                      std::move(wrapped.text),
                      std::nullopt,
                      std::nullopt,
                      {},
                      {},
                      {}};
        edit.lineControlPruneCandidates =
            std::move(wrapped.lineControlPruneCandidates);
        edit.lineControlSourceMappings =
            std::move(wrapped.lineControlSourceMappings);
        if (sidebandOnly ||
            includeUsesOnlySidebandReplayEnvelope(
                includeUsesOnlySidebandReplayEnvelope, child->id)) {
          if (auto sidebandBRange =
                  SidebandPragmaMaterializedBByteRangeForInclude(child->id))
            StampTextEditMaterializedBByteRange(edit, sidebandBRange->first,
                                                sidebandBRange->second);
        }

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
        AcceptedResultCandidate realizationCandidate;
        if (auto realized = BuildInlineIncludeRealizationFromB(
                *inc,
                llvm::formatv("degenerate child replace site for parent inc#{0}"
                              " child#{1} site=[{2},{3})",
                              inc->id, child->id, siteStart, siteEnd)
                    .str(),
                &realizationCandidate)) {
          includeExpansion[includeId] = std::move(*realized);
          includeExpansionStartLineNos[includeId] = 1;
          includeExpansionAcceptedResults[includeId] =
              std::move(realizationCandidate);
        } else {
          includeExpansion[includeId] = std::string();
        }
        return;
      }
    }
  }

  auto computeFirstEmittedHeaderLine = [&]() -> size_t {
    if (edits.empty())
      return 1;

    SmallVector<const TextEdit *, 8> ordered;
    ordered.reserve(edits.size());
    for (const TextEdit &edit : edits)
      ordered.push_back(&edit);
    llvm::sort(ordered, [](const TextEdit *lhs, const TextEdit *rhs) {
      if (lhs->start != rhs->start)
        return lhs->start < rhs->start;
      return lhs->end < rhs->end;
    });

    // Track the first physical line that will actually be emitted from the
    // materialized header.  A leading sideband deletion consumes the directive
    // line before wrapping, so the include-enter #line must name the first
    // surviving source line, not blindly line 1 of the header.  A leading
    // replacement still occupies the replaced directive's logical line.
    uint64_t cursor = 0;
    for (const TextEdit *edit : ordered) {
      if (cursor < edit->start)
        return stringutils::lineAtOffset(bytes, cursor);
      if (cursor > edit->start)
        continue;
      if (!edit->text.empty())
        return stringutils::lineAtOffset(bytes, edit->start);
      cursor = std::max(cursor, edit->end);
    }

    if (cursor < bytes.size())
      return stringutils::lineAtOffset(bytes, cursor);
    return 1;
  };

  const size_t firstEmittedHeaderLine = computeFirstEmittedHeaderLine();

  // Apply all staged edits in this header. If application requests terminal
  // fallback, leave this include empty so callers do not consume a partial
  // materialization.
  std::vector<FinalLineControlPruneCandidate> appliedLineControlCandidates;
  std::vector<FinalLineControlSourceMapping> appliedLineControlSourceMappings;
  std::string applied = ApplyTextEditsWithPendingResync(
      bytes, edits, appliedExpandedMacroRootIds, headerPath, includeId,
      /*materializedEditMappings=*/nullptr, &appliedLineControlCandidates,
      &appliedLineControlSourceMappings);
  if (HasTerminalFallbackRequest()) {
    includeExpansion[includeId] = std::string();
    includeExpansionLineControlPruneCandidates[includeId].clear();
    includeExpansionLineControlSourceMappings[includeId].clear();
    return;
  }

  includeExpansion[includeId] = std::move(applied);
  deduplicateLineControlPruneCandidates(appliedLineControlCandidates);
  deduplicateLineControlSourceMappings(appliedLineControlSourceMappings);
  includeExpansionLineControlPruneCandidates[includeId] =
      std::move(appliedLineControlCandidates);
  includeExpansionLineControlSourceMappings[includeId] =
      std::move(appliedLineControlSourceMappings);
  includeExpansionStartLineNos[includeId] = firstEmittedHeaderLine;
  includeExpansionAcceptedResults[includeId] =
      BuildAcceptedIncludeRealizationCandidate(
          AcceptedPathKind::IncludeMaterializedExpansion, *inc);
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
    // closed declaration range contains the insertion point. The end boundary
    // is intentionally inclusive so insertions immediately after a declaration
    // body still belong to that declaration.
    if (aLo == aHi) {
      if (aLo >= dLo && aLo <= dHi) {
        if (!bestCover || curSpanLen < getSpanLen(bestCover))
          bestCover = &d;
      }
      continue;
    }

    // For non-empty patches, prefer a declaration that fully contains the
    // edited A interval. If no declaration covers the interval, fall back to
    // the smallest declaration that merely overlaps it.
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

  // PP cover for this include inside the header; if not present, these will
  // already have been derived from spans when building the model.
  const uint64_t coverBegin =
      std::max(static_cast<uint64_t>(0), ie.include->cover.begin);
  const uint64_t coverEnd = std::max(coverBegin, ie.include->cover.end);

  // Single list of edits; we will apply them highest-offset-first so indices
  // remain stable as we mutate the StringBuilder.
  IncludeTextEditPlan plan;

  using HeaderMacroStateDirectivePiece = MacroStateDirectiveLineInterval;

  struct HeaderPreservedGapPiece {
    enum class Kind {
      MacroStateDirective,
      ZeroTokenConditionalGroup,
      ZeroTokenMacroInvocation,
      ZeroTokenChildInclude,
      BalancedPragmaStateIsland
    };

    Kind kind = Kind::MacroStateDirective;
    const RefoldModel::MacroDirective *directive = nullptr;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t id = 0;
  };

  auto headerReplacementObservesMacroStateDirective =
      [&](const RefoldModel::MacroDirective &directive, StringRef replacement,
          bool unparseableObserves) {
        return ReplacementObservesMacroStateDirective(directive, replacement,
                                                      unparseableObserves);
      };

  // Recover the complete physical source line for a macro-state directive that
  // was spelled in this include expansion. The shared helper owns the
  // macro-name-anchor reconstruction and exact-byte validation.
  auto headerMacroStateDirectiveInterval =
      [&](const RefoldModel::MacroDirective &directive) {
        return RecoverMacroStateDirectiveLineInterval(
            directive, file, StringRef(headerText),
            std::optional<uint64_t>(ie.include->id));
      };

  std::function<bool(const RefoldModel::MacroInvocation &)>
      headerMacroInvocationIsSourceNeutralZeroToken;
  std::function<bool(const RefoldModel::IncludeItem &)>
      headerIncludeIsSourceNeutralZeroToken;

  // Return true iff a recorded conditional group in this materialized header
  // can be preserved as a complete zero-token gap piece. This is the
  // header-owned counterpart of the TU zero-token conditional-gap proof: the
  // group bytes may be carried forward only when the producer recorded no
  // selected-arm PP material and every selected-arm body artifact either has no
  // source-state effect or discharges its own neutral proof. Macro invocations
  // in conditional-control lines are allowed because the complete group is
  // preserved verbatim; arm-body macro invocations must prove zero-token
  // neutrality through the ordinary header macro-gap proof.
  auto headerConditionalGroupIsPreservableGap =
      [&](const RefoldModel::CondGroup &group, uint64_t gapBegin,
          uint64_t gapEnd) -> bool {
    auto groupBelongs = [&](const RefoldModel::CondGroup &candidate) {
      return PathsEqual(candidate.file, file) && candidate.parentIncludeId &&
             *candidate.parentIncludeId == ie.include->id;
    };
    auto includeBelongs = [&](const RefoldModel::IncludeItem &inc) {
      return PathsEqual(inc.sitePath, file) && inc.parent &&
             *inc.parent == ie.include->id;
    };
    auto includeIsNeutral = [&](const RefoldModel::IncludeItem &inc) {
      return headerIncludeIsSourceNeutralZeroToken &&
             headerIncludeIsSourceNeutralZeroToken(inc);
    };
    auto directiveBelongs = [&](const RefoldModel::MacroDirective &directive) {
      return PathsEqual(directive.sitePath, file) && directive.ownerIncludeId &&
             *directive.ownerIncludeId == ie.include->id;
    };
    auto pragmaBelongs = [&](const RefoldModel::PragmaDirective &pragma) {
      return PathsEqual(pragma.sitePath, file);
    };
    auto macroBelongs = [&](const RefoldModel::MacroInvocation &macro) {
      return macro.invFile && PathsEqual(*macro.invFile, file);
    };
    auto macroIsNeutral = [&](const RefoldModel::MacroInvocation &macro) {
      return headerMacroInvocationIsSourceNeutralZeroToken &&
             headerMacroInvocationIsSourceNeutralZeroToken(macro);
    };

    NeutralConditionalIslandPolicy policy{
        headerText,
        /*requireGroupBeginAtLineStart=*/true,
        NeutralConditionalArmSpanMode::SelectedArmsOnly,
        groupBelongs,
        includeBelongs,
        includeIsNeutral,
        directiveBelongs,
        pragmaBelongs,
        macroBelongs,
        macroIsNeutral};
    return conditionalGroupIsNeutralIsland(model_, group, gapBegin, gapEnd,
                                           policy);
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

  using HeaderMacroReplacementList = MacroDefinitionReplacementListInterval;

  auto headerMacroDefinitionReplacementList =
      [&](const RefoldModel::MacroInvocation &m) {
        return RecoverMacroDefinitionReplacementListInterval(m);
      };

  auto headerMacroInvocationHasMaterializedPPTokens =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    // Any materialized expansion, stringify, or paste span means this
    // invocation is not a source-neutral gap.  Such invocations must be
    // represented by normal macro realization machinery, not by copying their
    // callsite through a header gap.
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

    enum class HeaderZeroTokenRangeSurface {
      DefinitionReplacementList,
      InvocationArgument
    };

    auto sourceRangeIsTiledByNeutralNestedMacros =
        [&](uint64_t begin, uint64_t end, StringRef rangeFile,
            HeaderZeroTokenRangeSurface surface) -> bool {
      if (begin > end)
        return false;

      // The byte coordinates recorded for a nested zero-token proof may refer
      // either to the defining directive's replacement-list spelling or to the
      // materialized invocation argument spelling in the current header.  Those
      // two surfaces can be in the same physical file, so path equality alone
      // is not enough to decide which buffer should be sliced.  Keep the origin
      // explicit at each callsite: replacement-list tiling uses the producer's
      // recorded directive text, while formal-argument tiling uses the actual
      // materialized header bytes at the invocation callsite.
      auto sliceTrivia = [&](uint64_t sliceBegin, uint64_t sliceEnd)
          -> std::optional<StringRef> {
        if (sliceEnd < sliceBegin)
          return std::nullopt;

        switch (surface) {
        case HeaderZeroTokenRangeSurface::DefinitionReplacementList: {
          if (!PathsEqual(rangeFile, replacement->directive->sitePath))
            return std::nullopt;
          const uint64_t textBegin = replacement->fileBase;
          const uint64_t textEnd =
              replacement->fileBase + replacement->directive->text.size();
          if (sliceBegin < textBegin || sliceEnd > textEnd)
            return std::nullopt;
          return replacement->directive->text.slice(
              static_cast<size_t>(sliceBegin - replacement->fileBase),
              static_cast<size_t>(sliceEnd - replacement->fileBase));
        }
        case HeaderZeroTokenRangeSurface::InvocationArgument:
          if (!PathsEqual(rangeFile, file) || sliceEnd > headerText.size())
            return std::nullopt;
          return StringRef(headerText).slice(sliceBegin, sliceEnd);
        }

        return std::nullopt;
      };

      SmallVector<ChildPiece, 8> children;
      for (const auto &candidate : model_.GetMacroInvocations()) {
        if (!candidate.callerMacroId || *candidate.callerMacroId != m.id)
          continue;

        // Only nested invocations physically spelled on the same source surface
        // may account for non-trivia bytes in the checked interval.
        if (!candidate.invFile || candidate.invFile->empty() ||
            !PathsEqual(*candidate.invFile, rangeFile))
          continue;
        if (!candidate.invB || !candidate.invE ||
            *candidate.invB >= *candidate.invE)
          continue;
        if (*candidate.invB < begin || end < *candidate.invE)
          continue;
        if (!sliceTrivia(*candidate.invB, *candidate.invE))
          continue;

        children.push_back({*candidate.invB, *candidate.invE, &candidate});
      }

      sortChildren(children);

      uint64_t cursor = begin;
      for (const ChildPiece &piece : children) {
        if (piece.begin < cursor)
          return false;

        std::optional<StringRef> gap = sliceTrivia(cursor, piece.begin);
        if (!gap || !isWsOrCompleteCommentTrivia(*gap))
          return false;
        if (!headerMacroInvocationIsSourceNeutralZeroTokenImpl(*piece.child,
                                                              visiting))
          return false;
        cursor = piece.end;
      }

      std::optional<StringRef> suffix = sliceTrivia(cursor, end);
      return suffix && isWsOrCompleteCommentTrivia(*suffix);
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
    if (sourceRangeIsTiledByNeutralNestedMacros(
            replacement->fileBegin, replacement->fileEnd,
            replacement->directive->sitePath,
            HeaderZeroTokenRangeSurface::DefinitionReplacementList))
      return finish(true);

    if (m.subkind == "func") {
      if (m.defParams.empty() || m.invArgRanges.size() < m.defParams.size())
        return finish(false);

      DenseMap<StringRef, unsigned> paramIndexByName;
      std::optional<unsigned> variadicParamIndex;
      for (unsigned i = 0, e = m.defParams.size(); i != e; ++i) {
        // Variadic formals are safe in the same narrow forwarding proof as
        // ordinary formals.  The replacement list may only mention the formal
        // identifier, and the corresponding invocation argument range must be
        // tiled entirely by recorded nested zero-token macro invocations plus
        // trivia below.  `__VA_OPT__` is admitted only in the erased case,
        // where that same variadic range proves token-empty before the payload
        // is ignored.
        paramIndexByName.try_emplace(m.defParams[i].name, i);
        if (m.defParams[i].variadic) {
          if (variadicParamIndex)
            return finish(false);
          variadicParamIndex = i;
        }
      }

      auto argumentRangeIsNeutral = [&](unsigned argIdx) -> bool {
        const auto &range = m.invArgRanges[argIdx];
        if (!range.first || !range.second || *range.first > *range.second)
          return false;
        if (!m.invFile || m.invFile->empty())
          return false;
        return sourceRangeIsTiledByNeutralNestedMacros(
            *range.first, *range.second, *m.invFile,
            HeaderZeroTokenRangeSurface::InvocationArgument);
      };

      std::optional<bool> variadicTailIsNeutral;
      auto proveVariadicTailNeutral = [&]() -> bool {
        if (variadicTailIsNeutral)
          return *variadicTailIsNeutral;
        if (!variadicParamIndex ||
            *variadicParamIndex >= m.invArgRanges.size()) {
          variadicTailIsNeutral = false;
          return false;
        }
        variadicTailIsNeutral = argumentRangeIsNeutral(*variadicParamIndex);
        return *variadicTailIsNeutral;
      };

      auto parseVaOptPayloadEnd =
          [&](size_t openParen) -> std::optional<size_t> {
        StringRef text = replacement->directive->text;
        if (openParen >= text.size() || text[openParen] != '(')
          return std::nullopt;

        unsigned depth = 1;
        for (size_t pos = openParen + 1; pos < text.size();) {
          const char ch = text[pos];
          if (ch == '\n' || ch == '\r')
            return std::nullopt;

          if (ch == '"' || ch == '\'') {
            const char quote = ch;
            ++pos;
            bool closed = false;
            while (pos < text.size()) {
              const char litCh = text[pos++];
              if (litCh == '\n' || litCh == '\r')
                return std::nullopt;
              if (litCh == '\\') {
                if (pos >= text.size())
                  return std::nullopt;
                ++pos;
                continue;
              }
              if (litCh == quote) {
                closed = true;
                break;
              }
            }
            if (!closed)
              return std::nullopt;
            continue;
          }

          if (pos + 1 < text.size() && ch == '/') {
            if (text[pos + 1] == '*') {
              pos += 2;
              bool closed = false;
              while (pos + 1 < text.size()) {
                if (text[pos] == '*' && text[pos + 1] == '/') {
                  pos += 2;
                  closed = true;
                  break;
                }
                ++pos;
              }
              if (!closed)
                return std::nullopt;
              continue;
            }
            if (text[pos + 1] == '/')
              return std::nullopt;
          }

          if (ch == '(') {
            ++depth;
            ++pos;
            continue;
          }
          if (ch == ')') {
            --depth;
            if (depth == 0)
              return pos + 1;
            ++pos;
            continue;
          }
          ++pos;
        }
        return std::nullopt;
      };

      SmallVector<unsigned, 4> usedParams;
      StringRef text = replacement->directive->text;

      auto replacementFragmentIsTiledByNeutralNestedMacros =
          [&](size_t begin, size_t end) -> bool {
        if (begin > end || end > text.size())
          return false;
        return sourceRangeIsTiledByNeutralNestedMacros(
            replacement->fileBase + static_cast<uint64_t>(begin),
            replacement->fileBase + static_cast<uint64_t>(end),
            replacement->directive->sitePath,
            HeaderZeroTokenRangeSurface::DefinitionReplacementList);
      };

      auto skipReplacementTriviaUntil = [&](size_t &pos,
                                            size_t limit) -> bool {
        if (limit > text.size())
          return false;
        while (pos < limit) {
          if (stringutils::isWs(text[pos])) {
            ++pos;
            continue;
          }

          StringRef tail = text.substr(pos, limit - pos);
          if (tail.starts_with("/*")) {
            const size_t commentEnd = text.find("*/", pos + 2);
            if (commentEnd == StringRef::npos || commentEnd + 2 > limit)
              return false;
            pos = commentEnd + 2;
            continue;
          }
          if (tail.starts_with("//")) {
            const size_t commentEnd = text.find('\n', pos + 2);
            if (commentEnd != StringRef::npos && commentEnd < limit)
              return false;
            pos = limit;
            continue;
          }
          return true;
        }
        return true;
      };

      auto neutralNestedChildAt = [&](size_t pos,
                                      size_t limit) -> std::optional<size_t> {
        if (limit > text.size())
          return std::nullopt;

        const uint64_t filePos =
            replacement->fileBase + static_cast<uint64_t>(pos);
        const uint64_t fileLimit =
            replacement->fileBase + static_cast<uint64_t>(limit);

        const RefoldModel::MacroInvocation *matched = nullptr;
        for (const auto &candidate : model_.GetMacroInvocations()) {
          if (!candidate.callerMacroId || *candidate.callerMacroId != m.id)
            continue;
          if (!candidate.invFile || candidate.invFile->empty() ||
              !PathsEqual(*candidate.invFile, replacement->directive->sitePath))
            continue;
          if (!candidate.invB || !candidate.invE ||
              *candidate.invB >= *candidate.invE)
            continue;
          if (*candidate.invB != filePos || *candidate.invE > fileLimit)
            continue;

          // Two direct children cannot soundly claim the same source spelling
          // in this local replacement-list proof.  Reject duplicate ownership
          // rather than choosing one heuristically.
          if (matched)
            return std::nullopt;
          matched = &candidate;
        }

        if (!matched)
          return std::nullopt;
        if (!headerMacroInvocationIsSourceNeutralZeroTokenImpl(*matched,
                                                               visiting))
          return std::nullopt;
        return static_cast<size_t>(*matched->invE - replacement->fileBase);
      };

      std::function<bool(size_t, size_t)> replacementFragmentIsNeutral;
      replacementFragmentIsNeutral = [&](size_t begin, size_t end) -> bool {
        if (begin > end || end > text.size())
          return false;

        // A fragment made entirely of nested recorded zero-token macro
        // invocations plus trivia is neutral regardless of whether it is the
        // whole replacement list or the selected payload of __VA_OPT__.
        if (replacementFragmentIsTiledByNeutralNestedMacros(begin, end))
          return true;

        size_t pos = begin;
        while (pos < end) {
          if (!skipReplacementTriviaUntil(pos, end))
            return false;
          if (pos >= end)
            break;

          if (std::optional<size_t> childEnd = neutralNestedChildAt(pos, end)) {
            pos = *childEnd;
            continue;
          }

          if (!stringutils::isIdentStart(text[pos]))
            return false;
          const size_t identBegin = pos++;
          while (pos < end && stringutils::isIdentPart(text[pos]))
            ++pos;
          StringRef ident = text.slice(identBegin, pos);

          if (ident == "__VA_OPT__") {
            if (!skipReplacementTriviaUntil(pos, end))
              return false;
            std::optional<size_t> afterVaOpt = parseVaOptPayloadEnd(pos);
            if (!afterVaOpt || *afterVaOpt > end)
              return false;

            const size_t payloadBegin = pos + 1;
            const size_t payloadEnd = *afterVaOpt - 1;

            // __VA_OPT__ is neutral in either of two proof-bounded cases:
            //   * the variadic tail is proved token-empty, so the payload is
            //     erased and need not be inspected; or
            //   * the payload itself is proved zero-token, so exposing it also
            //     contributes no PP tokens.
            // This branch-independent rule covers both erased forwarding forms
            // and non-erased payloads such as __VA_OPT__(EMPTY), without
            // evaluating arbitrary payload semantics.
            if (!proveVariadicTailNeutral() &&
                !replacementFragmentIsNeutral(payloadBegin, payloadEnd))
              return false;

            pos = *afterVaOpt;
            continue;
          }

          // Outside __VA_OPT__, the only non-trivia replacement-list
          // identifiers admitted by the function-like wrapper proof are formal
          // parameter names.  Their invocation argument ranges are checked once
          // after the fragment walk completes.
          auto it = paramIndexByName.find(ident);
          if (it == paramIndexByName.end())
            return false;

          // Token pasting can still be source-neutral in the placemarker
          // domain.  The shared helper admits only paste chains whose operands
          // are formals with token-empty invocation arguments; any paste that
          // could synthesize real PP-token material remains outside this proof.
          if (consumeNeutralPlacemarkerPasteChain(
                  text, pos, end, it->second, paramIndexByName,
                  skipReplacementTriviaUntil, argumentRangeIsNeutral,
                  usedParams))
            continue;

          usedParams.push_back(it->second);
        }

        return true;
      };

      if (!replacementFragmentIsNeutral(replacement->replacementTextBegin,
                                        text.size()))
        return finish(false);

      llvm::sort(usedParams);
      usedParams.erase(std::unique(usedParams.begin(), usedParams.end()),
                       usedParams.end());

      for (unsigned argIdx : usedParams)
        if (!argumentRangeIsNeutral(argIdx))
          return finish(false);

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
      if (!candidate.invFile || candidate.invFile->empty() || !candidate.invB ||
          !candidate.invE || *candidate.invB >= *candidate.invE)
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

    // Tile the replacement list from left to right.  Sorting gives
    // deterministic overlap detection and ensures every byte between child
    // invocations is checked as trivia/comment-only source.
    sortChildren(children);

    uint64_t cursor = replacement->fileBegin;
    for (const ChildPiece &piece : children) {
      // Overlapping children or source-bearing text between children defeats
      // the proof.  Each child must recursively prove that it also expands to
      // no PP material.
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

  headerMacroInvocationIsSourceNeutralZeroToken =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    DenseSet<uint64_t> visiting;
    return headerMacroInvocationIsSourceNeutralZeroTokenImpl(m, visiting);
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
    if (m.invText &&
        StringRef(headerText).slice(*m.invB, *m.invE) != *m.invText)
      return false;

    return headerMacroInvocationIsSourceNeutralZeroToken(m);
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
      // Macro-state transitions in a preserved zero-token include become
      // visible after the replacement payload is emitted.  Reject the
      // preservation if the B-derived replacement text mentions that macro
      // name, because then moving the transition could change how the
      // replacement preprocesses.
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
      if (headerReplacementObservesMacroStateDirective(
              directive, replacement, /*unparseableObserves=*/false))
        return false;
    }

    return true;
  };

  headerIncludeIsSourceNeutralZeroToken =
      [&](const RefoldModel::IncludeItem &child) -> bool {
    // Header conditional islands are preserved verbatim, so an include inside
    // a selected arm is admissible when the ordinary header include-gap proof
    // can prove the complete child include subtree contributes no PP material.
    // Use the full materialized header as the gap surface here: the containing
    // conditional proof has already checked that the include site lies inside
    // the preserved group.
    return headerZeroTokenChildIncludeIsPreservableGap(
        child, 0, headerText.size(), StringRef());
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
        *m.invE > headerText.size()) {
      return false;
    }
    if (m.invText &&
        StringRef(headerText).slice(*m.invB, *m.invE) != *m.invText) {
      return false;
    }

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
    if (sourceEnd >= headerText.size() ||
        stringutils::isWs(headerText[sourceEnd]))
      return true;

    std::optional<LexBoundaryToken> leftTok =
        lastLexToken(replacement, lexLang_);
    std::optional<LexBoundaryToken> rightTok =
        firstLexToken(StringRef(headerText).drop_front(sourceEnd), lexLang_);
    if (!leftTok || !rightTok)
      return true;

    return !needsLexicalSeparator(*leftTok, *rightTok, lexLang_);
  };

  auto headerSourceChunkObservesDefinitionWhenCrossed =
      [&](const RefoldModel::MacroDirective &definition, StringRef macroName,
          StringRef chunk, StringRef following) {
        return SourceChunkObservesMacroStateDirectiveWhenCrossed(
            definition, macroName, chunk, following);
      };

  auto activeHeaderDefinitionAtByte =
      [&](const RefoldModel::MacroDirective &definition, StringRef macroName,
          uint64_t offset) {
        // The carry proof applies only to the definition that is active at the
        // replacement start. Re-emitting a shadowed definition after the B
        // payload would synthesize a macro state that never existed at this
        // point in the original header.
        const RefoldModel::MacroDirective *active = nullptr;
        uint64_t activeEnd = 0;
        for (const auto &candidate : model_.GetMacroDirectives()) {
          std::optional<HeaderMacroStateDirectivePiece> piece =
              headerMacroStateDirectiveInterval(candidate);
          if (!piece || piece->end > offset)
            continue;
          if (StringRef(piece->name) != macroName)
            continue;
          if (!active || piece->end > activeEnd ||
              (piece->end == activeEnd && candidate.id > active->id)) {
            active = &candidate;
            activeEnd = piece->end;
          }
        }
        return active == &definition && definition.subkind == "#define";
      };

  auto headerRangeOverlapsStagedEdit = [&](uint64_t begin, uint64_t end) {
    // Do not carry directive text that is already being rewritten by another
    // staged header edit. A directive can be moved only when its original bytes
    // are still intact and owned by this single materialization proof.
    for (const TextEdit &edit : plan.edits)
      if (begin < edit.end && end > edit.start)
        return true;
    return false;
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

    const std::string ownerPath = resolveHeaderPath(*owner);
    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(ownerPath));
    if (!bufOrErr)
      return false;
    const MemoryBuffer &mb = **bufOrErr;
    StringRef ownerBytes(mb.getBufferStart(), mb.getBufferSize());
    return RecoverMacroStateDirectiveLineInterval(
               directive, ownerPath, ownerBytes, directive.ownerIncludeId)
        .has_value();
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

      if (headerReplacementObservesMacroStateDirective(
              directive, replacement, /*unparseableObserves=*/true))
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
    if (first.hasDirectHeaderByteRange)
      return false;
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
    if (first.hasDirectHeaderByteRange || second.hasDirectHeaderByteRange)
      return false;
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

    const bool isInsert = (p.aStart == p.aEnd) && (p.bStart < p.bEnd);
    const bool isDelete = (p.aStart < p.aEnd) && (p.bStart == p.bEnd);
    const bool isReplace = (p.aStart < p.aEnd) && (p.bStart < p.bEnd);


    if (!isInsert && !isDelete && !isReplace) {
      // Ignore empty or malformed patches defensively.
      continue;
    }

    if (p.hasDirectHeaderByteRange) {
      if (p.directHeaderByteBegin > p.directHeaderByteEnd ||
          p.directHeaderByteEnd > fileLen) {
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("direct include layout patch[{0}] has invalid "
                          "header byte range [{1},{2}) for file {3}",
                          idx, p.directHeaderByteBegin, p.directHeaderByteEnd,
                          file)
                .str();
        return plan;
      }

      IncludeAnchorWitness directWitness;
      directWitness.evidence = IncludeAnchorEvidenceKind::MappedHeaderTokens;
      directWitness.hasByteRange = true;
      directWitness.startByte = p.directHeaderByteBegin;
      directWitness.endByte = p.directHeaderByteEnd;
      directWitness.hasFirstPP = p.aStart < p.aEnd;
      directWitness.firstPP = p.aStart;
      directWitness.hasLastPP = p.aStart < p.aEnd;
      directWitness.lastPP = p.aStart < p.aEnd ? p.aEnd - 1 : p.aStart;


      TextEdit edit{p.directHeaderByteBegin, p.directHeaderByteEnd,
                    p.insertBytes, std::nullopt, std::nullopt, {}, {}, {}};
      AttachAcceptedResultCarrier(
          edit, BuildAcceptedIncludeCandidate(
                    AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens, p,
                    &directWitness));
      plan.edits.push_back(std::move(edit));
      continue;
    }

    // Decide which logical header decl "owns" this patch, if any.
    const auto *decl = FindHeaderDeclForPatch(*ie.include, p);

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
    uint64_t materialInsertPos = p.aStart;
    std::string materialInsertBytes = p.insertBytes;

    // Return true when an owner-local byte gap contains source-authored
    // line-control state.  This is intentionally a syntactic gap predicate: the
    // full logical-state evaluator lives in LineDirectiveInserter, but here we
    // only need to know whether an LCS boundary-token tie would cross a gap
    // that can change the logical location of the copied suffix.
    auto sourceIntervalContainsLineControlDirective =
        [&](uint64_t begin, uint64_t end) -> bool {
      if (begin >= end || end > headerText.size())
        return false;

      uint64_t lineBegin = begin;
      while (lineBegin < end) {
        std::string logicalLine;
        uint64_t afterLine = lineBegin;
        if (!collectLineSpliceLogicalLine(headerText, lineBegin, end, logicalLine,
                                      afterLine))
          return false;
        if (afterLine <= lineBegin)
          return false;

        StringRef rest = StringRef(logicalLine).ltrim(" \t\v\f");
        if (rest.consume_front("#")) {
          rest = rest.ltrim(" \t\v\f");
          if (rest.starts_with("line") &&
              (rest.size() == StringRef("line").size() ||
               !stringutils::isIdentPart(rest[StringRef("line").size()])))
            return true;
          if (!rest.empty() && '0' <= rest.front() && rest.front() <= '9')
            return true;
        }

        lineBegin = afterLine;
      }

      return false;
    };

    if (isInsert) {
      // Local repair for a duplicated boundary-token LCS tie.
      //
      // Shape:
      //   A:  <owner token T>  <source-only gap>  <next owner token>
      //   B:  T <inserted payload ending with another T> <next owner token>
      //
      // A token-level LCS may match the original source token T to the later B
      // copy after the insertion island.  For ordinary gaps this is usually just
      // another valid spelling of the same token edit.  Across a source-only
      // line-control gap, however, that tie changes where the inserted payload
      // sits relative to the logical-location state that the copied suffix will
      // observe.  The source token must remain before the gap; the inserted
      // payload must be emitted after the gap, followed by normal #line resync.
      //
      // This normalization is deliberately local to include-owned insertions and
      // only fires when the existing A->B token map proves the exact duplicate
      // boundary-token shape.  It does not mutate the global LCS, owner choice,
      // or any sideband/macro proof.
      auto normalizeDuplicatedBoundaryTokenAcrossLineControlGap = [&]() {
        if (materialAStart >= static_cast<uint64_t>(aToks_.size()))
          return;
        if (materialBStart >= materialBEnd ||
            materialBEnd >= static_cast<uint64_t>(bToks_.size()))
          return;
        if (abTokMapA2B_.empty() ||
            materialAStart >= static_cast<uint64_t>(abTokMapA2B_.size()))
          return;
        // The original A boundary token is currently matched to the B token
        // immediately after the insertion island.  Without this witness, there
        // is no duplicated-boundary-token tie to normalize.
        if (abTokMapA2B_[static_cast<size_t>(materialAStart)] !=
            static_cast<int64_t>(materialBEnd))
          return;
        if (aToks_[static_cast<size_t>(materialAStart)].spelling !=
                bToks_[static_cast<size_t>(materialBStart)].spelling ||
            aToks_[static_cast<size_t>(materialAStart)].spelling !=
                bToks_[static_cast<size_t>(materialBEnd)].spelling)
          return;

        std::optional<uint64_t> nextOwnerPP;
        for (uint64_t pp = materialAStart + 1; pp < ppHi; ++pp) {
          auto it = tokmapByPP.find(pp);
          if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
            nextOwnerPP = pp;
            break;
          }
        }
        if (!nextOwnerPP)
          return;

        // Examine the owner-local source bytes between the retained source
        // boundary token and the next copied owner token.  Only a source
        // line-control directive makes this token tie semantically observable
        // for placement; otherwise the normal token-envelope insertion is fine.
        std::optional<uint64_t> leftEnd =
            ByteEndForPPInFile(file, materialAStart);
        std::optional<uint64_t> rightBegin =
            ByteStartForPPInFile(file, *nextOwnerPP);
        if (!leftEnd || !rightBegin || *rightBegin < *leftEnd)
          return;
        if (!sourceIntervalContainsLineControlDirective(*leftEnd, *rightBegin))
          return;

        const uint64_t normalizedBStart = materialBStart + 1;
        const uint64_t normalizedBEnd = materialBEnd + 1;
        if (normalizedBStart >= normalizedBEnd ||
            normalizedBEnd > static_cast<uint64_t>(bTokOff_.size()))
          return;

        // Drop the duplicated leading B token from the island and include the
        // later B token that closed the island.  The insertion point advances
        // to the next owner token so the source line-control gap remains in
        // place before the inserted payload.
        materialBStart = normalizedBStart;
        materialBEnd = normalizedBEnd;
        materialInsertPos = *nextOwnerPP;
        // Use the B token *envelope*, not exact token coverage.  The envelope
        // preserves the whitespace/newline between the inserted payload and the
        // copied suffix; losing that newline would concatenate C tokens even
        // though the token interval proof is otherwise correct.
        materialInsertBytes =
            sliceTokenEnvelope(bTokOff_, bSource_, materialBStart, materialBEnd)
                .str();
      };

      normalizeDuplicatedBoundaryTokenAcrossLineControlGap();

      std::optional<std::pair<uint64_t, uint64_t>> bBytes =
          BTokenRangeToByteRange(materialBStart, materialBEnd);
      materialInsertBytes = StripSeparatelyOwnedSidebandReplay(
          materialInsertBytes,
          bBytes ? std::optional<uint64_t>(bBytes->first) : std::nullopt,
          bBytes ? std::optional<uint64_t>(bBytes->second) : std::nullopt);
    }

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
      // INSERT: interpret the normalized A-position as "before the next token"
      // in this header.  Most insertions use the original hunk A gap.  The
      // duplicate-boundary-token normalization above may advance this position
      // across a source-only line-control gap while keeping the original token
      // in place.
      const uint64_t pos = materialInsertPos;
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

      struct SelectedInsertAnchorCandidate {
        InsertAnchorCandidate anchor;
        AcceptedResultCandidate accepted;
      };

      // Select the best theorem-facing insertion anchor by wrapping each local
      // anchor candidate in the accepted-result carrier and letting the shared
      // proof-discharge/lattice selector choose among them.  Return the
      // accepted carrier together with the path-local anchor, so the commit
      // path does not have to rediscover or rebuild the proof that won.
      auto selectBestInsertCandidate =
          [&](ArrayRef<InsertAnchorCandidate> candidates)
          -> std::optional<SelectedInsertAnchorCandidate> {
        if (candidates.empty())
          return std::nullopt;

        SmallVector<AcceptedResultCandidate, 4> acceptedCandidates;
        acceptedCandidates.reserve(candidates.size());
        for (const InsertAnchorCandidate &candidate : candidates) {
          acceptedCandidates.push_back(BuildAcceptedIncludeCandidate(
              candidate.path, p, &candidate.witness));
        }

        const std::optional<SelectedAcceptedResultCandidate> selected =
            SelectPreferredAcceptedResultCandidate(acceptedCandidates);
        if (!selected) {
          return std::nullopt;
        }

        SelectedInsertAnchorCandidate result;
        result.anchor = candidates[selected->index];
        result.accepted = selected->candidate;
        return result;
      };

      // Materialize the selected insertion anchor as a header-local TextEdit,
      // applying boundary padding and line-resync handling before attaching the
      // exact accepted include-candidate carrier that won selection.
      auto commitInsertCandidate = [&](const SelectedInsertAnchorCandidate
                                           &selected) {
        const InsertAnchorCandidate &candidate = selected.anchor;
        std::string text = PadAtBoundaries(
            headerText, static_cast<size_t>(candidate.anchorByte),
            static_cast<size_t>(candidate.anchorByte), materialInsertBytes,
            /* allowLeft */ true, /* allowRight */ true);

        auto hasCopiedHeaderSuffix = [&]() -> bool {
          for (uint64_t byte = candidate.anchorByte; byte < fileLen; ++byte)
            if (!stringutils::isWs(headerText[static_cast<size_t>(byte)]))
              return true;
          return false;
        };

        auto childIncludeAtBoundaryAnchor =
            [&]() -> const RefoldModel::IncludeItem * {
          for (const auto &child : model_.GetIncludes()) {
            if (!child.parent || *child.parent != ie.include->id ||
                !PathsEqual(child.sitePath, file))
              continue;
            if (child.siteB == candidate.anchorByte ||
                child.siteE == candidate.anchorByte)
              return &child;
          }
          return nullptr;
        };

        auto includeHasVisibleSidebandWork = [&](uint64_t includeId) -> bool {
          return llvm::any_of(sidebandPragmaEdits_,
                              [&](const SidebandPragmaEdit &sideband) {
            return sideband.TargetsInclude(includeId) &&
                   sideband.EmitsVisibleReplayText();
          });
        };

        auto suffixBeginsWithPreprocessorDirective = [&]() -> bool {
          size_t pos = static_cast<size_t>(candidate.anchorByte);
          if (pos >= headerText.size())
            return false;

          // Local line resync is still required before a preserved directive
          // line.  Unlike ordinary suffix tokens, directives such as #include,
          // #define, #undef, and #line observe their presumed source location
          // through preprocessing state and diagnostics.  Horizontal
          // indentation before the '#' belongs to the directive line and is a
          // valid flush point for the pending #line.
          size_t lineEnd = headerText.find('\n', pos);
          if (lineEnd == StringRef::npos)
            lineEnd = headerText.size();
          while (pos < lineEnd &&
                 (headerText[pos] == ' ' || headerText[pos] == '\t'))
            ++pos;
          return pos < lineEnd && headerText[pos] == '#';
        };

        auto replayContainsPreprocessorDirectiveLine =
            [](StringRef replay) -> bool {
          size_t lineBegin = 0;
          while (lineBegin < replay.size()) {
            size_t lineEnd = replay.find('\n', lineBegin);
            if (lineEnd == StringRef::npos)
              lineEnd = replay.size();

            size_t pos = lineBegin;
            while (pos < lineEnd &&
                   (replay[pos] == ' ' || replay[pos] == '\t'))
              ++pos;
            if (pos < lineEnd && replay[pos] == '#')
              return true;

            if (lineEnd == replay.size())
              break;
            lineBegin = lineEnd + 1;
          }
          return false;
        };

        const bool copiedHeaderSuffix = hasCopiedHeaderSuffix();
        const RefoldModel::IncludeItem *boundaryChild =
            childIncludeAtBoundaryAnchor();
        const bool boundaryChildWillBeMaterialized =
            boundaryChild && includeHasVisibleSidebandWork(boundaryChild->id);
        const bool suffixObservesLineState =
            OwnerSuffixHasLineStateSensitiveBuiltin(ie.include->id, file,
                                                    candidate.anchorByte);
        const bool suffixStartsDirective =
            suffixBeginsWithPreprocessorDirective();
        const bool replayCarriesDirectiveLine =
            replayContainsPreprocessorDirectiveLine(text);

        // A pure insertion at physical header EOF has no copied header suffix
        // whose logical location must be restored inside this owner.  For a
        // child-include boundary, suppress the local parent resync only when
        // the child is itself being materialized by a proved sideband replay;
        // the child wrapper then performs the transition immediately.  If the
        // child #include remains preserved, or if the next untouched suffix
        // line is any other preprocessing directive, restore the parent line
        // state before that directive.
        //
        // Two additional map-backed observations also require local resync:
        //  * a copied suffix macro invocation whose expansion contains
        //    __LINE__/__FILE__ through caller_macro_id evidence; and
        //  * an insertion payload that itself carries a preprocessing directive
        //    line, such as a sideband #pragma co-owned by the ordinary
        //    insertion island.  In both cases, the following copied header
        //    bytes must resume in the original owner file/line state.
        const bool needsLocalResync =
            copiedHeaderSuffix &&
            (suffixObservesLineState || suffixStartsDirective ||
             replayCarriesDirectiveLine) &&
            !boundaryChildWillBeMaterialized;
        TextEdit edit = needsLocalResync ? MakeTextEditWithResyncOrPending(
                                               headerText, candidate.anchorByte,
                                               candidate.anchorByte, text, file,
                                               ie.include->id)
                                         : TextEdit{candidate.anchorByte,
                                                    candidate.anchorByte,
                                                    std::move(text),
                                                    std::nullopt,
                                                    std::nullopt,
                                                    {},
                                                    {},
                                                    {}};
        AttachAcceptedResultCarrier(edit, selected.accepted);
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
        // the selected conditional arm or to a child include boundary.  The
        // child-boundary case is a declared include-preserving proof, not an
        // unclassified fallback branch: the witness names the direct child
        // include whose spelled directive supplies the anchor. These anchors
        // preserve include structure without relying on neighboring PP tokens.
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
            ByteStartForPPInFile(file, *rightAnchorPP);
        IncludeAnchorWitness rightNeighborWitness;
        rightNeighborWitness.evidence =
            IncludeAnchorEvidenceKind::RightNeighborPP;
        rightNeighborWitness.hasNeighborPP = true;
        rightNeighborWitness.neighborPP = *rightAnchorPP;
        rightNeighborWitness.hasAnchorByte = rightStartByte.has_value();
        rightNeighborWitness.anchorByte =
            rightStartByte ? *rightStartByte : 0ULL;
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
          SmallVector<InsertAnchorCandidate, 1> rightCandidates;
          rightCandidates.push_back(std::move(candidate));
          if (auto selected = selectBestInsertCandidate(rightCandidates)) {
            commitInsertCandidate(*selected);
            continue;
          }
        }
      }

      // Right-neighbor anchoring is preferred because it naturally inserts
      // before the next stable token. If unavailable, collect secondary
      // include-preserving anchors: the nearest mapped left neighbor, or the
      // containing declaration boundary when no neighbor exists. These are not
      // source-mapping fallbacks; each candidate still needs a concrete witness
      // before it can be selected.
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

      SmallVector<InsertAnchorCandidate, 2> secondaryCandidates;
      if (leftAnchorPP) {
        const std::optional<uint64_t> leftStartByte =
            ByteEndForPPInFile(file, *leftAnchorPP);
        const bool insertionAtIncludeEOF = pos >= ppHi;
        // Insertion at the include cover end is an explicit left-neighbor EOF
        // anchor, not an unmapped PP->EOF source projection.  The proof still
        // requires `leftAnchorPP` to map into this file; only after that mapped
        // neighbor establishes owner-local suffix placement do we use the
        // physical file length as the concrete byte anchor for the zero-width
        // EOF insertion.
        const std::optional<uint64_t> leftAnchorByte =
            leftStartByte ? std::optional<uint64_t>(
                                insertionAtIncludeEOF ? fileLen
                                                      : *leftStartByte)
                          : std::nullopt;
        IncludeAnchorWitness leftNeighborWitness;
        leftNeighborWitness.evidence =
            IncludeAnchorEvidenceKind::LeftNeighborPP;
        leftNeighborWitness.hasNeighborPP = true;
        leftNeighborWitness.neighborPP = *leftAnchorPP;
        leftNeighborWitness.hasAnchorByte = leftAnchorByte.has_value();
        leftNeighborWitness.anchorByte =
            leftAnchorByte ? *leftAnchorByte : 0ULL;
        if (!leftAnchorByte) {
          plan.requiresIncludeRealization = true;
          plan.realizationReason =
              llvm::formatv("INSERT: failed to map left-neighbor anchorPP={0} "
                            "in file {1}",
                            *leftAnchorPP, file)
                  .str();
          return plan;
        }
        if (anchorMatchesCondArmCert(*leftAnchorByte)) {
          InsertAnchorCandidate candidate;
          candidate.path = AcceptedPathKind::IncludeInsertLeftNeighborPP;
          candidate.anchorByte = *leftAnchorByte;
          candidate.witness = leftNeighborWitness;
          secondaryCandidates.push_back(std::move(candidate));
        }
      } else if (decl) {
        // With no mapped PP neighbor, use the declaration boundary that owns
        // this patch. This keeps the insertion local to the header declaration
        // only when the declaration witness discharges; otherwise the caller
        // widens to include realization.
        const uint64_t declAnchorByte =
            std::clamp<uint64_t>(decl->headerE, 0ULL, fileLen);
        IncludeAnchorWitness declWitness;
        declWitness.evidence = IncludeAnchorEvidenceKind::DeclBoundary;
        declWitness.hasAnchorByte = true;
        declWitness.anchorByte = declAnchorByte;
        declWitness.hasDeclHeaderRange = true;
        declWitness.declHeaderB = decl->headerB;
        declWitness.declHeaderE = decl->headerE;
        if (anchorMatchesCondArmCert(declAnchorByte)) {
          InsertAnchorCandidate candidate;
          candidate.path = AcceptedPathKind::IncludeInsertDeclBoundary;
          candidate.anchorByte = declAnchorByte;
          candidate.witness = declWitness;
          secondaryCandidates.push_back(std::move(candidate));
        }
      }

      if (auto selected = selectBestInsertCandidate(secondaryCandidates)) {
        commitInsertCandidate(*selected);
        continue;
      }

      // No include-preserving insertion anchor discharged. Switch to the
      // explicit include-realization path instead of manufacturing a weaker
      // insertion witness.
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
      startByte = ByteStartForPPInFile(file, *firstPP);
      endByte =
          ByteEndForPPInFile(file, *lastPP);
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

        // Require exact token byte bounds here: widening across header source
        // pieces needs precise endpoints on both sides of each gap.
        std::optional<uint64_t> tokBegin = ByteStartForPPInFile(file, pp);
        std::optional<uint64_t> tokEnd = ByteEndForPPInFile(file, pp);
        if (!tokBegin || !tokEnd || *tokBegin >= *tokEnd) {
          hunkSourcePieces.clear();
          break;
        }
        hunkSourcePieces.push_back(
            {*tokBegin, *tokEnd, pp, pp + 1, pp, "token"});
      }

      for (const auto &child : model_.GetIncludes()) {
        if (!child.parent || *child.parent != ie.include->id ||
            !PathsEqual(child.sitePath, file) || !child.cover.IsValid())
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
      const bool fullHeaderEnvelopeBlockedByPartialOverlap =
          shouldTryFullHeaderEnvelope &&
          (hasPartialChildIncludeOverlap || hasPartialMacroInvocationOverlap ||
           hasPartialConditionalGroupOverlap);
      const bool mustUseFullHeaderEnvelope =
          hasCompleteChildIncludePiece || hasCompleteMacroInvocationPiece ||
          hasCompleteConditionalGroupPiece ||
          fullHeaderEnvelopeBlockedByPartialOverlap;
      if (shouldTryFullHeaderEnvelope &&
          !fullHeaderEnvelopeBlockedByPartialOverlap) {
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
                    // site. Once the complete child include is a
                    // source-envelope piece, any mapped child token inside that
                    // site is not a second parent piece.
                    return true;
                  }
                  if (outer.kind == "macro-invocation" &&
                      piece.kind == "token") {
                    // Macro-expanded tokens often map to the macro callsite
                    // spelling. Once the complete source-bearing invocation is
                    // proved consumed, those mapped token intervals are
                    // internal evidence, not separate parent-header pieces.
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
                lexLang_, ie.include->id, std::move(builtinMacroResolver));
          };

          if (!proveOwnerSourceEnvelopeGaps(hunkSourcePieces, [&](uint64_t
                                                                      gapBegin,
                                                                  uint64_t
                                                                      gapEnd) {
                if (std::optional<SourceLineDirectiveGapResume> lineResume =
                        computeSourceLineDirectiveGapResume(
                            StringRef(headerText), gapBegin, gapEnd,
                            sourceEnvelope->end, file,
                            headerSourceLineDirectiveLineRewriter, nullptr,
                            model_.GetSourcePath(),
                            !sourceSuffixMayObservePresumedFileSpelling(
                                model_, file, sourceEnvelope->end,
                                [&](StringRef lhs, StringRef rhs) {
                                  return PathsEqual(lhs, rhs);
                                },
                                StringRef(headerText)))) {
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
                  if (headerReplacementObservesMacroStateDirective(
                          *directivePiece->directive,
                          StringRef(materialInsertBytes),
                          /*unparseableObserves=*/true))
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
                  if (!headerConditionalGroupIsPreservableGap(group, gapBegin,
                                                              gapEnd))
                    continue;
                  HeaderPreservedGapPiece piece;
                  piece.kind =
                      HeaderPreservedGapPiece::Kind::ZeroTokenConditionalGroup;
                  piece.begin = group.groupB;
                  piece.end = group.groupE;
                  piece.id = group.id;
                  gapPieces.push_back(std::move(piece));
                }

                // Pragma/state proof for materialized header owners.
                // A balanced diagnostic pragma island is a source-state atom:
                // the island may be preserved across a header source gap only
                // when push/pop depth returns to zero and every byte crossed by
                // the island is trivia.  This mirrors the TU owner proof while
                // keeping non-diagnostic or unbalanced pragmas fail-closed.
                SmallVector<BalancedDiagnosticPragmaStateIsland, 4>
                    pragmaIslands;
                collectBalancedDiagnosticPragmaStateIslands(
                    model_, StringRef(headerText), gapBegin, gapEnd,
                    [&](const RefoldModel::PragmaDirective &pragma) {
                      return PathsEqual(pragma.sitePath, file);
                    },
                    pragmaIslands, lexLang_);
                for (const BalancedDiagnosticPragmaStateIsland &island :
                     pragmaIslands) {
                  HeaderPreservedGapPiece piece;
                  piece.kind =
                      HeaderPreservedGapPiece::Kind::BalancedPragmaStateIsland;
                  piece.begin = island.begin;
                  piece.end = island.end;
                  piece.id = island.id;
                  gapPieces.push_back(std::move(piece));
                }

                for (const auto &macro : model_.GetMacroInvocations()) {
                  if (!headerMacroInvocationIsPreservableGap(macro, gapBegin,
                                                             gapEnd))
                    continue;
                  HeaderPreservedGapPiece piece;
                  piece.kind =
                      HeaderPreservedGapPiece::Kind::ZeroTokenMacroInvocation;
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
                          if (outer.kind == HeaderPreservedGapPiece::Kind::
                                                ZeroTokenMacroInvocation &&
                              piece.kind == HeaderPreservedGapPiece::Kind::
                                                ZeroTokenMacroInvocation) {
                            // Nested zero-token macro invocations are already
                            // part of the outer invocation's source-neutral
                            // proof. Preserve only the outer callsite text so
                            // the refolded header does not duplicate the same
                            // gap structure.
                            return true;
                          }

                          if (outer.kind == HeaderPreservedGapPiece::Kind::
                                                ZeroTokenConditionalGroup) {
                            // A preserved complete conditional group owns every
                            // artifact inside its source island after the
                            // shared conditional-island proof has discharged
                            // those artifacts. This includes nested conditional
                            // records, zero-token macro calls, zero-token or
                            // state-preserving child includes, direct
                            // macro-state directives in selected arms, and
                            // balanced pragma-state islands.
                            //
                            // Without this ownership rule, the gap tiler would
                            // see both the outer #if/#endif island and its
                            // already-proved child include/directive/pragma
                            // pieces as overlapping top-level proof pieces,
                            // reject the header envelope, and fall back to raw
                            // B.
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
          }
        }
      }

      // If the replacement material intersects complete child source structure
      // owned by this header, a plain mapped-token edit is not a valid proof:
      // it can replace only the directly mapped parent tokens and leave stale
      // neighboring macro/include syntax behind.  Either the edit widened to
      // the proven source envelope above, or this include must be realized from
      // B.
      if (fullHeaderEnvelopeBlockedByPartialOverlap) {
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("DELETE/REPLACE: patch[{0}] in header file {1} "
                          "partially overlaps source-envelope structure",
                          idx, file)
                .str();
        return plan;
      }
      if (mustUseFullHeaderEnvelope && !usedFullHeaderEnvelope) {
        plan.requiresIncludeRealization = true;
        plan.realizationReason =
            llvm::formatv("DELETE/REPLACE: patch[{0}] in header file {1} "
                          "requires a full source-envelope proof",
                          idx, file)
                .str();
        return plan;
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

    if (isReplace && startByte && endByte && *startByte <= *endByte &&
        !replacement.empty() &&
        replacementHeaderSuffixBoundaryAllowsDirectiveLine(
            StringRef(replacement), *endByte)) {
      struct HeaderMacroStateCarryCandidate {
        const RefoldModel::MacroDirective *directive = nullptr;
        HeaderMacroStateDirectivePiece piece;
      };

      // Collect active header-owned #define directives that currently precede
      // this header-local replacement and that the B-derived replacement would
      // observe if they stayed there.  Those are exactly the definitions whose
      // source-order position must be reconsidered to make the replacement see
      // B's macro state.
      SmallVector<HeaderMacroStateCarryCandidate, 4> carryCandidates;
      for (const auto &directive : model_.GetMacroDirectives()) {
        if (directive.subkind != "#define")
          continue;

        std::optional<HeaderMacroStateDirectivePiece> piece =
            headerMacroStateDirectiveInterval(directive);
        if (!piece || piece->end > *startByte)
          continue;
        if (headerRangeOverlapsStagedEdit(piece->begin, piece->end))
          continue;
        if (!activeHeaderDefinitionAtByte(directive, piece->name, *startByte))
          continue;
        if (!headerReplacementObservesMacroStateDirective(
                directive, StringRef(replacement),
                /*unparseableObserves=*/false))
          continue;

        // Header materialization has the same placement-sensitive macro-state
        // invariant as TU gap carry.  If an active #define remains before a
        // header-local replacement, the replacement is preprocessed under the
        // old macro state rather than B's macro state.  We may carry that
        // definition after the replacement only when the crossed original
        // header bytes do not themselves observe the definition; otherwise
        // those bytes are part of the old zero-token proof and must keep the
        // original source order.
        if (headerSourceChunkObservesDefinitionWhenCrossed(
                directive, piece->name,
                StringRef(headerText).slice(piece->end, *startByte),
                StringRef(headerText).slice(*startByte, *endByte)))
          continue;

        carryCandidates.push_back(
            HeaderMacroStateCarryCandidate{&directive, *piece});
      }

      if (!carryCandidates.empty()) {
        // Apply carries in source order so widening the replacement is
        // deterministic and so each directive is removed from its original
        // position exactly once before being re-emitted after the payload.
        llvm::sort(carryCandidates,
                   [](const HeaderMacroStateCarryCandidate &lhs,
                      const HeaderMacroStateCarryCandidate &rhs) {
                     if (lhs.piece.begin != rhs.piece.begin)
                       return lhs.piece.begin < rhs.piece.begin;
                     return lhs.directive->id < rhs.directive->id;
                   });

        bool admissibleCarry = true;
        const uint64_t newStart = carryCandidates.front().piece.begin;
        uint64_t cursor = newStart;
        for (const HeaderMacroStateCarryCandidate &candidate :
             carryCandidates) {
          if (cursor > candidate.piece.begin) {
            admissibleCarry = false;
            break;
          }
          StringRef crossed =
              StringRef(headerText).slice(cursor, candidate.piece.begin);
          StringRef following =
              StringRef(headerText).slice(candidate.piece.begin, *startByte);
          // Once an earlier definition is carried, every byte crossed before
          // the next carried directive must remain non-observing for that prior
          // definition.  This keeps multi-directive carries from silently
          // moving a macro past source that depended on the old ordering.
          for (const HeaderMacroStateCarryCandidate &prior : carryCandidates) {
            if (prior.piece.end <= cursor)
              continue;
            if (prior.piece.begin >= candidate.piece.begin)
              break;
            if (headerSourceChunkObservesDefinitionWhenCrossed(
                    *prior.directive, prior.piece.name, crossed, following)) {
              admissibleCarry = false;
              break;
            }
          }
          if (!admissibleCarry)
            break;
          cursor = candidate.piece.end;
        }

        if (admissibleCarry) {
          uint64_t replacementTailEnd = *endByte;
          if (replacementTailEnd < headerText.size() &&
              !stringutils::isBOL(StringRef(headerText),
                                  static_cast<size_t>(replacementTailEnd))) {
            size_t nl = StringRef(headerText).find('\n', replacementTailEnd);
            replacementTailEnd = nl == StringRef::npos
                                     ? static_cast<uint64_t>(headerText.size())
                                     : static_cast<uint64_t>(nl + 1);
          }

          // A carried directive must be re-emitted at a physical line boundary.
          // If the replacement stops in the middle of an original source line,
          // appending the directive immediately would split the surviving line
          // (`USE(3) #define ... ;`).  Carry the non-observing tail of that
          // physical line into the widened edit before re-emitting directives.
          // This also consumes a single original newline when the replacement
          // already ends at the previous line's last token, avoiding a spurious
          // blank line before the untouched suffix.
          if (replacementTailEnd > *endByte) {
            StringRef tail =
                StringRef(headerText).slice(*endByte, replacementTailEnd);
            StringRef following =
                StringRef(headerText).drop_front(replacementTailEnd);
            for (const HeaderMacroStateCarryCandidate &candidate :
                 carryCandidates) {
              if (headerSourceChunkObservesDefinitionWhenCrossed(
                      *candidate.directive, candidate.piece.name, tail,
                      following)) {
                admissibleCarry = false;
                break;
              }
            }
          }

          std::string carriedReplacement;
          carriedReplacement.reserve((*startByte - newStart) +
                                     replacement.size() +
                                     (replacementTailEnd - *endByte) + 64);

          // Rebuild the widened header edit as:
          //   preserved bytes before carried directives,
          //   B-derived replacement payload,
          //   non-observing physical line tail after the payload,
          //   carried #define directive lines.
          // The directive bytes are intentionally omitted from their original
          // locations and appended after the completed payload line so the
          // payload is not macro-expanded by definitions that B did not have
          // active there, while the surviving suffix still sees the restored
          // macro state.
          cursor = newStart;
          for (const HeaderMacroStateCarryCandidate &candidate :
               carryCandidates) {
            if (cursor > candidate.piece.begin) {
              admissibleCarry = false;
              break;
            }
            carriedReplacement.append(headerText.begin() + cursor,
                                      headerText.begin() +
                                          candidate.piece.begin);
            cursor = candidate.piece.end;
          }

          if (admissibleCarry) {
            carriedReplacement.append(headerText.begin() + cursor,
                                      headerText.begin() + *startByte);
            carriedReplacement += replacement;
            if (replacementTailEnd > *endByte) {
              carriedReplacement.append(headerText.begin() + *endByte,
                                        headerText.begin() +
                                            replacementTailEnd);
            } else if (!carriedReplacement.empty() &&
                       carriedReplacement.back() != '\n') {
              carriedReplacement.push_back('\n');
            }
            for (const HeaderMacroStateCarryCandidate &candidate :
                 carryCandidates) {
              carriedReplacement.append(candidate.directive->text.begin(),
                                        candidate.directive->text.end());
              if (carriedReplacement.empty() ||
                  carriedReplacement.back() != '\n')
                carriedReplacement.push_back('\n');
            }

            *startByte = newStart;
            *endByte = replacementTailEnd;
            replacement = std::move(carriedReplacement);
            mappedHeaderWitness.hasByteRange = true;
            mappedHeaderWitness.startByte = *startByte;
            mappedHeaderWitness.endByte = *endByte;
          }
        }
      }
    }

    if (!headerGapPreservations.empty()) {
      // Emit preserved gap pieces after the B-derived payload. That keeps
      // replacement tokens in the same preprocessing environment as B, while
      // the untouched header suffix sees the restored zero-token structure.
      //
      // Balanced diagnostic pragma islands are the one side-effect gap class
      // that may already be present in the B-derived replay bytes.  Sideband
      // normalization removes their directive tokens from the structural diff,
      // but the raw replacement slice still contains the emitted `#pragma`
      // lines.  When that replay surface canonically carries the same balanced
      // island, suppress the source-gap copy so the pragma state is represented
      // exactly once.
      bool insertedSeparator = false;
      for (const HeaderPreservedGapPiece &piece : headerGapPreservations) {
        std::string pieceText = preservedGapPieceText(piece);
        if (pieceText.empty())
          continue;

        if (piece.kind ==
                HeaderPreservedGapPiece::Kind::BalancedPragmaStateIsland &&
            balancedDiagnosticPragmaStateIslandIsCarriedByReplacement(
                StringRef(pieceText), StringRef(replacement), lexLang_)) {
          continue;
        }

        if (!insertedSeparator && !replacement.empty() &&
            replacement.back() != '\n') {
          replacement.push_back('\n');
          insertedSeparator = true;
        }
        replacement += pieceText;
        if (replacement.empty() || replacement.back() != '\n')
          replacement.push_back('\n');
      }
    }

    const bool emitsHeaderSourceLineDirectiveResume =
        lineDirs_.Enabled() && usedFullHeaderEnvelope &&
        headerSourceLineDirectiveResume && *endByte < fileLen;
    std::vector<FinalLineControlPruneCandidate> sourceLineResumeCandidates;
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
      const uint64_t resumeBegin = static_cast<uint64_t>(replacement.size());
      replacement +=
          formatSourceLineDirectiveGapResume(*headerSourceLineDirectiveResume);
      const uint64_t resumeEnd = static_cast<uint64_t>(replacement.size());
      sourceLineResumeCandidates.push_back(
          makeSyntheticLineControlPruneCandidate(
              resumeBegin, resumeEnd,
              FinalLineDirective::Origin::SyntheticSourceLineResume,
              FinalLineControlOwnerKey(file, ie.include->id),
              FinalLineControlObligation::HeaderResumeRepair));

      const OwnerStateBoundary sourceLineResumeBoundary =
          OwnerStateBoundary::FromSource(OwnerSourceRange::From(
              file, *endByte, *endByte,
              std::optional<uint64_t>(ie.include->id)));
      auto checkHeaderSourceLineResume = [&](OwnerStateComponent component) {
        const std::string detail =
            llvm::formatv("synthetic header source #line resume for include "
                          "#{0} patch[{1}] component={2} sourceEnd={3}",
                          ie.include->id, idx, component, *endByte)
                .str();
        (void)CheckStateTransitionAcrossEditBoundary(
            sourceLineResumeBoundary, component, StateMutationKind::Replayed,
            BuildStateTransitionWitness(SuffixStabilityWitnessKind::StateRepair,
                                        component, sourceLineResumeBoundary,
                                        detail),
            "include/source-line-resume", detail,
            /*requireKnownObserver=*/false);
      };
      checkHeaderSourceLineResume(OwnerStateComponent::LineNumber);
      checkHeaderSourceLineResume(OwnerStateComponent::FileState);
      checkHeaderSourceLineResume(OwnerStateComponent::FileName);

    }

    // Stage the include-preserving mapped-header edit and attach the witness
    // that records which PP tokens and header bytes justified the edit range.
    TextEdit edit = emitsHeaderSourceLineDirectiveResume
                        ? TextEdit{*startByte,
                                   *endByte,
                                   std::move(replacement),
                                   std::nullopt,
                                   std::nullopt,
                                   {},
                                   {},
                                   {}}
                        : MakeTextEditWithResyncOrPending(
                              headerText, *startByte, *endByte, replacement,
                              file, ie.include->id);
    if (emitsHeaderSourceLineDirectiveResume)
      edit.lineControlPruneCandidates = std::move(sourceLineResumeCandidates);
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

  for (const auto &e : plan.edits) {

    if (e.end < e.start || e.end > static_cast<uint64_t>(headerText.size())) {
      REFOLD_LOG_FATAL("include/apply", "TextEdit out of bounds: bytes=[{0},{1}) size={2}",
            e.start, e.end, headerText.size());
    }
  }

  return plan;
}

std::optional<uint64_t> RefoldEngine::ComputeChildBoundaryInsertByte(
    const IncludePatch &p, StringRef file,
    IncludeAnchorWitness *witness) const {
  // This is the declared IncludeInsertionByChildBoundary proof. It is
  // intentionally narrow: a parent-owned pure insertion may anchor at a
  // preserved direct child include only when the PP gap equals exactly one
  // child cover boundary. Anything inside a child cover belongs to the child
  // owner, and any ambiguous sibling boundary fails closed instead of guessing.
  //
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
      REFOLD_LOG_FATAL("include/boundary",
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
      REFOLD_LOG_FATAL("include/boundary",
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
                                StringRef fileSpellingForDirective,
                                std::optional<uint64_t> ownerIncludeId) const {
  // If the replacement preserves the original newline count, no line-state
  // correction is needed.
  size_t origNl = stringutils::countNewlines(originalFileText, start, end);
  size_t replNl =
      stringutils::countNewlines(replacement, 0, replacement.size());
  if (origNl == replNl)
    return ResyncOutcome(replacement.str(), std::nullopt);

  // Newline drift is only observable when a preserved suffix builtin depends
  // on the logical line component. Do not synthesize #line directives merely
  // because physical newline counts changed: materialized __LINE__ values do
  // not observe the stream, and preserved __FILE__/__FILE_NAME__ only observe
  // the file component, which newline drift alone does not change.
  LineStateObserverDemand demand = OwnerSuffixLineStateObserverDemand(
      ownerIncludeId, fileSpellingForDirective, end);
  const OwnerStateBoundary suffixBoundary =
      OwnerStateBoundary::FromSource(OwnerSourceRange::From(
          fileSpellingForDirective, end, end, ownerIncludeId));
  const bool resyncPruneEligible = demand.PrunableByCompactFinalLineControl();

  auto checkLineControlStateWithWitness =
      [&](OwnerStateComponent component, StateMutationKind mutation,
          SuffixStabilityWitness witness, StringRef detail,
          bool requireKnownObserver) {
        return CheckStateTransitionAcrossEditBoundary(
            suffixBoundary, component, mutation, std::move(witness),
            "linedir/resync", detail, requireKnownObserver);
      };

  auto checkLineControlStateRepaired =
      [&](OwnerStateComponent component, StateMutationKind mutation,
          StringRef detail, bool requireKnownObserver = false) {
        return checkLineControlStateWithWitness(
            component, mutation,
            BuildStateTransitionWitness(SuffixStabilityWitnessKind::StateRepair,
                                        component, suffixBoundary, detail),
            detail, requireKnownObserver);
      };

  auto checkLineControlStateTerminal =
      [&](OwnerStateComponent component, StateMutationKind mutation,
          StringRef detail, bool requireKnownObserver = true) {
        return checkLineControlStateWithWitness(
            component, mutation,
            BuildStateTransitionWitness(
                SuffixStabilityWitnessKind::TerminalStateFailure, component,
                suffixBoundary, detail),
            detail, requireKnownObserver);
      };

  if (!demand.needsLine) {
    return ResyncOutcome(replacement.str(), std::nullopt);
  }

  // A preserved suffix __LINE__ would otherwise resume at a different logical
  // line. Use the source-authored line-control state at `end`, not merely the
  // physical line in this owner file: `#line`, `# line`, and `# <number>` can
  // make a copied suffix observe a virtual file/line.
  LineDirectiveLocation resumeLoc =
      LineDirectiveInserter::LogicalLocationAtOffset(
          originalFileText, end, fileSpellingForDirective, model_,
          fileSpellingForDirective, ownerIncludeId);

  if (!resumeLoc.producerProven) {
    if (resumeLoc.unprovenLineControlDirectiveOffset &&
        *resumeLoc.unprovenLineControlDirectiveOffset >= start &&
        *resumeLoc.unprovenLineControlDirectiveOffset < end) {
      (void)checkLineControlStateTerminal(
          OwnerStateComponent::LineNumber, StateMutationKind::Consumed,
          llvm::formatv(
              "newline-drift replacement consumes unmodeled source #line "
              "directive at byte {0} in owner '{1}'",
              *resumeLoc.unprovenLineControlDirectiveOffset,
              fileSpellingForDirective)
              .str(),
          /*requireKnownObserver=*/true);
      return ResyncOutcome(replacement.str(), std::nullopt);
    }

    if (std::optional<LineDirectiveLocation> producerLoc =
            ProducerBackedLineControlLocationAt(originalFileText,
                                                fileSpellingForDirective,
                                                ownerIncludeId, start, end)) {
      resumeLoc = std::move(*producerLoc);
    } else {
      return ResyncOutcome(replacement.str(), std::nullopt);
    }
  }

  const bool deferToConditionalJoin = LineResyncShouldDeferToConditionalJoin(
      fileSpellingForDirective, ownerIncludeId, end);
  if (deferToConditionalJoin) {
    (void)checkLineControlStateRepaired(
        OwnerStateComponent::LineNumber, StateMutationKind::MovedLater,
        llvm::formatv("deferred synthetic #line repair for newline drift at "
                      "owner byte {0}",
                      end)
            .str(),
        /*requireKnownObserver=*/false);
    return ResyncOutcome{replacement.str(),
                         PendingResync{fileSpellingForDirective, ownerIncludeId,
                                       resyncPruneEligible,
                                       resumeLoc.fileSpelling, resumeLoc.lineNo,
                                       end,
                                       /*deferToJoin=*/true}};
  }

  std::string injected = lineDirs_.MaybeAppendResyncAfterReplacement(
      originalFileText, start, end, replacement, resumeLoc);
  const std::string resyncDirective =
      lineDirs_.FormatLineDirective(resumeLoc.lineNo, resumeLoc.fileSpelling);

  // A changed result means the directive was inserted directly into this
  // replacement, so no deferred resync state needs to be carried forward.
  if (injected != replacement) {
    (void)checkLineControlStateRepaired(
        OwnerStateComponent::LineNumber, StateMutationKind::Replayed,
        llvm::formatv("local synthetic #line repair for newline drift at "
                      "owner byte {0}",
                      end)
            .str(),
        /*requireKnownObserver=*/false);

    std::vector<FinalLineControlPruneCandidate> candidates;
    if (resyncPruneEligible) {
      if (std::optional<FinalLineControlPruneCandidate> candidate =
              makeInsertedSyntheticLineControlPruneCandidate(
                  replacement, injected, resyncDirective,
                  FinalLineDirective::Origin::SyntheticNewlineResync,
                  FinalLineControlOwnerKey(fileSpellingForDirective.str(),
                                           ownerIncludeId),
                  FinalLineControlObligation::CosmeticSyntheticResync)) {
        candidates.push_back(std::move(*candidate));
      }
    }

    return ResyncOutcome(std::move(injected), std::nullopt,
                         std::move(candidates));
  }

  // Local injection was not safe, usually because the replacement rejoins
  // untouched bytes mid-line. Carry a pending resync so the next copied
  // original slice can emit the directive at a valid boundary.
  (void)checkLineControlStateRepaired(
      OwnerStateComponent::LineNumber, StateMutationKind::MovedLater,
      llvm::formatv("pending synthetic #line repair for newline drift at "
                    "owner byte {0}",
                    end)
          .str(),
      /*requireKnownObserver=*/false);

  return ResyncOutcome{replacement.str(),
                       PendingResync{fileSpellingForDirective, ownerIncludeId,
                                     resyncPruneEligible,
                                     resumeLoc.fileSpelling, resumeLoc.lineNo,
                                     end}};
}

void RefoldEngine::AttachAcceptedResultCarrier(
    TextEdit &edit, const AcceptedResultCandidate &candidate) const {
  AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "AttachAcceptedResultCarrier");
  edit.acceptedResults.push_back(
      std::make_shared<AcceptedResultCandidate>(candidate));
}

bool RefoldEngine::AuditAcceptedEditProofs(ArrayRef<TextEdit> edits,
                                          StringRef emissionStage,
                                          StringRef emissionOwner) const {
  // Centralize the final accepted-proof audit at the last byte-edit boundary.
  // Earlier builders may still stage candidates path-by-path, but once the
  // normalized edit set is known the applicator must see a theorem carrier for
  // every emitted edit and a composition law for every multi-carrier edit.  The
  // state-transition audit is layered onto the same boundary:
  // any edit path that already routed state through the gateway must have a
  // typed, component-named suffix-stability witness or a named terminal failure
  // before non-terminal bytes can be emitted.
  if (!AuditStateTransitionGatewayProofs(emissionStage, emissionOwner))
    return false;

  for (const TextEdit &edit : edits) {
    if (!EmittedTextEditHasDischargedAcceptedResults(edit, emissionStage,
                                                     emissionOwner))
      return false;
  }
  return true;
}

bool RefoldEngine::EmittedTextEditHasDischargedAcceptedResults(
    const TextEdit &edit, StringRef emissionStage,
    StringRef emissionOwner) const {
  const StringRef owner =
      emissionOwner.empty() ? StringRef("<unknown>") : emissionOwner;
  ++lastTheoremAudit_.emittedNonTerminalEdits;

  auto carrierIsEmissionDischarged =
      [&](const AcceptedResultCandidate &carrier) -> bool {
    ++lastTheoremAudit_.emittedCarriers;
    AuditAcceptedResultCandidateForLegacyAuthority(
        carrier, "EmittedTextEditHasDischargedAcceptedResults");

    // Non-terminal byte edits must never carry terminal/out-of-domain results.
    // After these structural exclusions, the single normalizer is the only
    // authority for deciding whether the candidate's construction path and
    // proof-family metadata discharge one final theorem proof class.
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

    const std::optional<TheoremProofClass> theoremProof =
        NormalizeAcceptedProof(carrier);
    if (!theoremProof) {
      if (carrier.proofSummary.theoremClass == TheoremProofClass::Unknown) {
        ++lastTheoremAudit_.emittedUnknownClassCarriers;
        NoteTheoremAuditViolation(
            "emitted carrier did not declare a final theorem proof class");
      } else if (carrier.proofSummary.inventory.support !=
                     AcceptanceSupportKind::ExplicitProofBacked ||
                 carrier.proofSummary.theoremDomain.kind ==
                     TheoremDomainKind::TransitionalGap ||
                 carrier.proofSummary.completeness.coverage ==
                     CompletenessCoverageKind::TransitionalGap) {
        ++lastTheoremAudit_.emittedTransitionalTheoremCarriers;
        NoteTheoremAuditViolation(
            "emitted carrier remained transitional at the byte-edit boundary");
      } else {
        NoteTheoremAuditViolation(
            "emitted carrier failed final proof normalization at the "
            "byte-edit boundary");
      }
      ++lastTheoremAudit_.emittedUndischargedCarriers;
      return false;
    }

    ++lastTheoremAudit_.emittedDeclaredClassCarriers;
    ++lastTheoremAudit_.emittedDischargedCarriers;
    REFOLD_LOG_TRACE("proof/normalize",
          "emitted carrier normalized theoremProof={0} kind={1} path={2}",
          *theoremProof, carrier.kind,
          carrier.proofSummary.inventory.currentPath);
    return true;
  };

  // Every emitted non-terminal edit must carry at least one selected
  // accepted-result witness. An unannotated edit is an emission artifact, not a
  // theorem-facing refolding result.
  if (edit.acceptedResults.empty()) {
    const TerminalFallbackProofFailure failure =
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::EmissionArtifactDischarged,
            TerminalFallbackFailureReason::UndischargedEmissionArtifact,
            TerminalFallbackFailureContext::ForStateComponent(
                "TextEdit.acceptedResults"));
    const bool strictRejected = RejectNoLegacyAuditFindingIfStrict(
        MakeLegacyAuditEvidence(
            LegacyPathKind::PathSpecificProofMirror, emissionStage,
            llvm::formatv(
                "emitted edit bytes=[{0},{1}) in {2} has no "
                "AcceptedResultCandidate / ProofSummary carrier",
                edit.start, edit.end, owner)
                .str()),
        failure);
    NoteTheoremAuditViolation("emitted edit reached the byte-edit boundary "
                              "without accepted-result carriers");
    if (!strictRejected) {
      RequestTerminalFallback(
          failure, emissionStage,
          llvm::formatv("emitted edit bytes=[{0},{1}) in {2} has no "
                        "normalized accepted-result carriers",
                        edit.start, edit.end, owner)
              .str());
    }
    return false;
  }

  // Validate every carrier attached to the edit. Multiple carriers are allowed
  // when one concrete edit composes several accepted results, but each carrier
  // must independently discharge.
  for (size_t i = 0; i < edit.acceptedResults.size(); ++i) {
    const std::shared_ptr<const AcceptedResultCandidate> &carrier =
        edit.acceptedResults[i];
    if (!carrier) {
      const TerminalFallbackProofFailure failure =
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact,
              TerminalFallbackFailureContext::ForStateComponent(
                  "TextEdit.acceptedResults.null"));
      const bool strictRejected = RejectNoLegacyAuditFindingIfStrict(
          MakeLegacyAuditEvidence(
              LegacyPathKind::PathSpecificProofMirror, emissionStage,
              llvm::formatv(
                  "emitted edit bytes=[{0},{1}) in {2} has null "
                  "AcceptedResultCandidate carrier #{3}",
                  edit.start, edit.end, owner, i)
                  .str()),
          failure);
      NoteTheoremAuditViolation(
          "emitted edit carried a null accepted-result carrier");
      if (!strictRejected) {
        RequestTerminalFallback(
            failure, emissionStage,
            llvm::formatv("emitted edit bytes=[{0},{1}) in {2} has null "
                          "accepted-result carrier #{3}",
                          edit.start, edit.end, owner, i)
                .str());
      }
      return false;
    }

    if (!carrierIsEmissionDischarged(*carrier)) {
      RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact),
          emissionStage,
          llvm::formatv("emitted edit bytes=[{0},{1}) in {2} carries "
                        "non-discharged result #{3} kind={4}",
                        edit.start, edit.end, owner, i, carrier->kind)
              .str());
      return false;
    }
  }

  return EmittedTextEditHasOrderedAcceptedProofComposition(
      edit, emissionStage, emissionOwner);
}

bool RefoldEngine::EmittedTextEditHasOrderedAcceptedProofComposition(
    const TextEdit &edit, StringRef emissionStage,
    StringRef emissionOwner) const {
  if (edit.acceptedResults.size() <= 1)
    return true;

  ++lastTheoremAudit_.emittedCompositeEdits;

  enum class CarrierSpanDomain : uint8_t { AToken, TUByte };
  struct CarrierSpan {
    CarrierSpanDomain domain;
    uint64_t begin = 0;
    uint64_t end = 0;
    size_t carrierIndex = 0;
    TheoremProofClass theoremProof = TheoremProofClass::Unknown;
  };

  auto domainName = [](CarrierSpanDomain domain) -> StringRef {
    switch (domain) {
    case CarrierSpanDomain::AToken:
      return "AToken";
    case CarrierSpanDomain::TUByte:
      return "TUByte";
    }
    return "Unknown";
  };

  const StringRef owner =
      emissionOwner.empty() ? StringRef("<unknown>") : emissionOwner;

  auto failComposition = [&](StringRef detail) -> bool {
    ++lastTheoremAudit_.emittedUncomposedCompositeEdits;
    NoteTheoremAuditViolation(detail);
    RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::EmissionArtifactDischarged,
            TerminalFallbackFailureReason::UndischargedEmissionArtifact),
        emissionStage,
        llvm::formatv("emitted composite edit bytes=[{0},{1}) in {2}: {3}",
                      edit.start, edit.end, owner, detail)
            .str());
    return false;
  };

  auto extractSpan = [&](const AcceptedResultCandidate &carrier,
                         size_t carrierIndex,
                         TheoremProofClass theoremProof)
      -> std::optional<CarrierSpan> {
    CarrierSpan span;
    span.carrierIndex = carrierIndex;
    span.theoremProof = theoremProof;

    switch (carrier.kind) {
    case AcceptedResultCandidateKind::MacroPatch:
    case AcceptedResultCandidateKind::IncludePatch:
      // Macro and include carriers record A-side preprocessed-token envelopes.
      // They are comparable to one another for proof-composition purposes, but
      // not to the physical byte range of the final TextEdit.
      span.domain = CarrierSpanDomain::AToken;
      span.begin = carrier.begin;
      span.end = carrier.end;
      break;

    case AcceptedResultCandidateKind::TUAnchor:
      if (carrier.proofSummary.hasTUAnchorWitness &&
          carrier.proofSummary.tuAnchorWitness.hasPPGap) {
        span.domain = CarrierSpanDomain::AToken;
        span.begin = span.end = carrier.proofSummary.tuAnchorWitness.ppGap;
      } else if (carrier.hasAnchorByte) {
        span.domain = CarrierSpanDomain::TUByte;
        span.begin = span.end = carrier.anchorByte;
      } else {
        return std::nullopt;
      }
      break;

    case AcceptedResultCandidateKind::TUTextEdit:
      span.domain = CarrierSpanDomain::TUByte;
      span.begin = carrier.begin;
      span.end = carrier.end;
      break;

    case AcceptedResultCandidateKind::TerminalOutOfDomain:
    case AcceptedResultCandidateKind::Unknown:
      return std::nullopt;
    }

    if (span.end < span.begin)
      return std::nullopt;
    return span;
  };

  SmallVector<CarrierSpan, 8> spans;
  spans.reserve(edit.acceptedResults.size());
  for (size_t i = 0; i < edit.acceptedResults.size(); ++i) {
    const std::shared_ptr<const AcceptedResultCandidate> &carrierPtr =
        edit.acceptedResults[i];
    if (!carrierPtr)
      return failComposition("composite edit carried a null proof segment");

    const std::optional<TheoremProofClass> theoremProof =
        NormalizeAcceptedProof(*carrierPtr);
    if (!theoremProof)
      return failComposition(
          "composite edit contained a carrier that did not normalize");

    std::optional<CarrierSpan> span =
        extractSpan(*carrierPtr, i, *theoremProof);
    if (!span)
      return failComposition(
          llvm::formatv("composite carrier #{0} had no comparable proof span "
                        "kind={1}",
                        i, carrierPtr->kind)
              .str());
    spans.push_back(*span);
  }

  // Several construction paths can converge on the exact same concrete source
  // surface.  Those carriers are equivalent witnesses for one segment rather
  // than a sequence of multiple segments, so no ordering/gap proof is needed.
  bool sameSurface = true;
  for (size_t i = 1; i < spans.size(); ++i) {
    if (spans[i].domain != spans[0].domain ||
        spans[i].begin != spans[0].begin || spans[i].end != spans[0].end) {
      sameSurface = false;
      break;
    }
  }
  if (sameSurface) {
    ++lastTheoremAudit_.emittedEquivalentCompositeEdits;
    REFOLD_LOG_TRACE("proof/compose",
          "composite edit bytes=[{0},{1}) in {2} has {3} equivalent "
          "proof carriers over {4}[{5},{6})",
          edit.start, edit.end, owner, spans.size(),
          domainName(spans[0].domain), spans[0].begin, spans[0].end);
    return true;
  }

  // Duplicate physical edits can merge a TU-byte carrier with owner-local
  // A-token carriers.  When one normalized carrier proves the exact physical
  // emitted byte surface, the non-TU carriers are redundant witnesses rather
  // than additional ordered segments.  This preserves existing deterministic
  // duplicate-edit behavior without treating cross-domain carrier order as a
  // proof.
  for (const CarrierSpan &span : spans) {
    if (span.domain == CarrierSpanDomain::TUByte && span.begin == edit.start &&
        span.end == edit.end) {
      ++lastTheoremAudit_.emittedEquivalentCompositeEdits;
      REFOLD_LOG_TRACE("proof/compose",
            "composite edit bytes=[{0},{1}) in {2} is dominated by carrier "
            "#{3} theoremProof={4} over TUByte[{5},{6}); {7} auxiliary "
            "carriers remain individually discharged",
            edit.start, edit.end, owner, span.carrierIndex, span.theoremProof,
            span.begin, span.end, spans.size() - 1);
      return true;
    }
  }

  // Same-point insertion composition consumes no A-side bytes/tokens.  Multiple
  // zero-width carriers may therefore share one physical insertion edit without
  // needing an inter-segment source-gap proof; the emitted text order was fixed
  // by the duplicate-insertion merge that built this TextEdit.
  const bool allZeroWidth = llvm::all_of(spans, [](const CarrierSpan &span) {
    return span.begin == span.end;
  });
  if (allZeroWidth && edit.start == edit.end) {
    ++lastTheoremAudit_.emittedEquivalentCompositeEdits;
    REFOLD_LOG_TRACE("proof/compose",
          "composite insertion edit bytes=[{0},{1}) in {2} has {3} "
          "zero-width proof carriers",
          edit.start, edit.end, owner, spans.size());
    return true;
  }

  // Ordered segment composition is only meaningful inside one coordinate space.
  // Mixing A-token carriers with TU-byte carriers would require a separate
  // cross-domain owner-closure witness, so the proof model rejects that
  // composition instead of silently treating the carrier vector as an unordered
  // bag.
  for (size_t i = 1; i < spans.size(); ++i) {
    if (spans[i].domain != spans[0].domain) {
      return failComposition(
          llvm::formatv(
              "carrier #{0} uses {1} coordinates after {2} coordinates; "
              "no cross-domain composition witness is attached",
              spans[i].carrierIndex, domainName(spans[i].domain),
              domainName(spans[0].domain))
              .str());
    }
  }

  // The carrier list itself must be ordered.  Sorting here would hide a missing
  // construction proof, so the audit checks the preserved order and rejects
  // overlaps or non-empty source gaps.  Future state-gap proof extensions can
  // relax the gap rule by attaching typed state-closed gap witnesses.
  for (size_t i = 1; i < spans.size(); ++i) {
    const CarrierSpan &prev = spans[i - 1];
    const CarrierSpan &cur = spans[i];
    if (cur.begin < prev.end) {
      return failComposition(
          llvm::formatv("carrier #{0} span {1}[{2},{3}) overlaps previous "
                        "carrier #{4} span {1}[{5},{6})",
                        cur.carrierIndex, domainName(cur.domain), cur.begin,
                        cur.end, prev.carrierIndex, prev.begin, prev.end)
              .str());
    }
    if (cur.begin > prev.end) {
      return failComposition(
          llvm::formatv("carrier #{0} span {1}[{2},{3}) leaves non-empty "
                        "unproved gap after carrier #{4} span {1}[{5},{6}); "
                        "current composition proof admits only empty gaps until "
                        "state-gap witnesses are available",
                        cur.carrierIndex, domainName(cur.domain), cur.begin,
                        cur.end, prev.carrierIndex, prev.begin, prev.end)
              .str());
    }
  }

  // If the carrier coordinates are physical TU bytes, they must tile the actual
  // source interval being emitted.  A-token carriers prove their own PP
  // envelope through the normalized per-carrier proof; the final TextEdit byte
  // span is a separately mapped owner surface and is not directly comparable
  // here.
  if (spans[0].domain == CarrierSpanDomain::TUByte &&
      (spans.front().begin != edit.start || spans.back().end != edit.end)) {
    return failComposition(
        llvm::formatv("TU-byte composite carriers tile [{0},{1}) but emitted "
                      "edit covers [{2},{3})",
                      spans.front().begin, spans.back().end, edit.start,
                      edit.end)
            .str());
  }

  ++lastTheoremAudit_.emittedOrderedCompositeEdits;
  REFOLD_LOG_TRACE("proof/compose",
        "composite edit bytes=[{0},{1}) in {2} has {3} ordered contiguous "
        "proof segments in {4} coordinates",
        edit.start, edit.end, owner, spans.size(), domainName(spans[0].domain));
  return true;
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldEngine::BTokenRangeToByteRange(uint64_t bTokBegin,
                                     uint64_t bTokEnd) const {
  // B-token envelopes are half-open, so bTokEnd is allowed to name the
  // sentinel offset immediately after the last B token. Keeping the conversion
  // through bTokOff_ preserves the exact byte spelling chosen by the B lexer.
  if (bTokEnd < bTokBegin)
    return std::nullopt;
  if (bTokEnd >= static_cast<uint64_t>(bTokOff_.size()))
    return std::nullopt;

  const uint64_t begin =
      static_cast<uint64_t>(bTokOff_[static_cast<size_t>(bTokBegin)]);
  const uint64_t end =
      static_cast<uint64_t>(bTokOff_[static_cast<size_t>(bTokEnd)]);
  if (end < begin || end > static_cast<uint64_t>(bSource_.size()))
    return std::nullopt;
  return std::make_pair(begin, end);
}

void RefoldEngine::StampTextEditMaterializedBByteRange(TextEdit &edit,
                                                       uint64_t begin,
                                                       uint64_t end) const {
  if (end < begin || end > static_cast<uint64_t>(bSource_.size()))
    REFOLD_LOG_FATAL("edit-map", "invalid materialized B byte range [{0},{1}) bLen={2}",
          begin, end, bSource_.size());
  edit.materializedBByteBegin = begin;
  edit.materializedBByteEnd = end;
}

void RefoldEngine::StampTextEditMaterializedBTokenRange(
    TextEdit &edit, uint64_t bTokBegin, uint64_t bTokEnd) const {
  std::optional<std::pair<uint64_t, uint64_t>> bytes =
      BTokenRangeToByteRange(bTokBegin, bTokEnd);
  if (!bytes)
    REFOLD_LOG_FATAL("edit-map", "invalid materialized B token range [{0},{1}) bToks={2}",
          bTokBegin, bTokEnd, bToks_.size());
  StampTextEditMaterializedBByteRange(edit, bytes->first, bytes->second);
}

void RefoldEngine::StampTextEditMaterializedBReplayProof(
    TextEdit &edit, const SidebandPragmaEdit &sideband) const {
  const std::pair<uint64_t, uint64_t> bRange =
      sideband.MaterializedBByteRange();
  StampTextEditMaterializedBByteRange(edit, bRange.first, bRange.second);

  const std::pair<uint64_t, uint64_t> outputRange =
      sideband.MaterializedOutputTextRange();
  StampTextEditMaterializedOutputTextRange(edit, outputRange.first,
                                           outputRange.second);
}

std::string RefoldEngine::StripSeparatelyOwnedSidebandReplay(
    StringRef replayText, std::optional<uint64_t> replayBByteBegin,
    std::optional<uint64_t> replayBByteEnd) const {
  if (!replayBByteBegin || !replayBByteEnd ||
      *replayBByteEnd <= *replayBByteBegin || replayText.empty())
    return replayText.str();

  struct RemovalRange {
    uint64_t begin;
    uint64_t end;
  };

  SmallVector<RemovalRange, 4> removals;
  for (const SidebandPragmaEdit &sideband : sidebandPragmaEdits_) {
    // A zero-width source-side pragma insertion is the one sideband class whose
    // visible B bytes may legitimately be carried by a surrounding ordinary
    // insertion island.  Non-insertion sideband edits have their own source
    // atom to replace/delete, so replaying the same raw B bytes through the
    // ordinary token envelope would duplicate that sideband line.
    if (sideband.SourceIsZeroWidthInsertion() ||
        !sideband.EmitsVisibleReplayText())
      continue;

    const auto sidebandB = sideband.MaterializedBByteRange();
    if (sidebandB.first < *replayBByteBegin ||
        *replayBByteEnd < sidebandB.second ||
        sidebandB.first >= sidebandB.second) {
      continue;
    }

    const uint64_t relBegin = sidebandB.first - *replayBByteBegin;
    const uint64_t relEnd = sidebandB.second - *replayBByteBegin;
    if (relEnd > replayText.size())
      continue;

    StringRef replaySlice = replayText.slice(static_cast<size_t>(relBegin),
                                             static_cast<size_t>(relEnd));
    if (replaySlice != sideband.ReplacementText())
      continue;

    removals.push_back({relBegin, relEnd});
  }

  if (removals.empty())
    return replayText.str();

  llvm::sort(removals, [](const RemovalRange &lhs, const RemovalRange &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin < rhs.begin;
    return lhs.end < rhs.end;
  });

  SmallVector<RemovalRange, 4> merged;
  for (const RemovalRange &range : removals) {
    if (range.begin >= range.end)
      continue;
    if (!merged.empty() && range.begin < merged.back().end)
      REFOLD_LOG_FATAL("pragma/sideband",
            "overlapping separately-owned sideband replay ranges "
            "[{0},{1}) and [{2},{3})",
            merged.back().begin, merged.back().end, range.begin, range.end);
    if (!merged.empty() && range.begin == merged.back().end) {
      merged.back().end = range.end;
      continue;
    }
    merged.push_back(range);
  }

  std::string out;
  out.reserve(replayText.size());
  uint64_t cursor = 0;
  for (const RemovalRange &range : merged) {
    out.append(replayText.slice(static_cast<size_t>(cursor),
                                static_cast<size_t>(range.begin))
                   .str());
    cursor = range.end;
  }
  out.append(replayText.drop_front(static_cast<size_t>(cursor)).str());

  return out;
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldEngine::SidebandPragmaMaterializedBByteRangeForInclude(
    uint64_t includeId) const {
  std::optional<uint64_t> begin;
  std::optional<uint64_t> end;

  for (const SidebandPragmaEdit &sideband : sidebandPragmaEdits_) {
    if (!sideband.TargetsInclude(includeId))
      continue;

    // Sideband-only header materialization can have an empty normal-token
    // cover.  In that case the ordinary include-cover resolver has no B-token
    // envelope to report to the edit-map writer, but each sideband edit already
    // carries the exact raw-B byte range that produced its replacement (or the
    // deterministic zero-length B anchor for a deletion).  Union those ranges
    // to make the final include replacement a fully witnessed emitted edit.
    if (!sideband.ReplayIsValid(static_cast<uint64_t>(bSource_.size())))
      return std::nullopt;

    const std::pair<uint64_t, uint64_t> bRange =
        sideband.MaterializedBByteRange();
    begin = begin ? std::min(*begin, bRange.first) : bRange.first;
    end = end ? std::max(*end, bRange.second) : bRange.second;
  }

  if (!begin || !end)
    return std::nullopt;
  return std::make_pair(*begin, *end);
}

void RefoldEngine::StampTextEditMaterializedOutputTextRange(
    TextEdit &edit, uint64_t begin, uint64_t end) const {
  if (end < begin || end > static_cast<uint64_t>(edit.text.size()))
    REFOLD_LOG_FATAL("edit-map",
          "invalid materialized output byte range [{0},{1}) textLen={2}",
          begin, end, edit.text.size());
  edit.materializedOutputTextBegin = begin;
  edit.materializedOutputTextEnd = end;
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldEngine::TextEditMaterializedOutputTextRange(const TextEdit &edit) const {
  if (edit.materializedOutputTextBegin && edit.materializedOutputTextEnd) {
    if (*edit.materializedOutputTextEnd < *edit.materializedOutputTextBegin ||
        *edit.materializedOutputTextEnd >
            static_cast<uint64_t>(edit.text.size()))
      return std::nullopt;
    return std::make_pair(*edit.materializedOutputTextBegin,
                          *edit.materializedOutputTextEnd);
  }

  return std::make_pair(uint64_t{0}, static_cast<uint64_t>(edit.text.size()));
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldEngine::MacroPatchMaterializedOutputTextRange(
    const MacroPatch &patch) const {
  if (!patch.hasMaterializedOutputByteRange)
    return std::nullopt;
  if (patch.materializedOutputByteEnd < patch.materializedOutputByteStart ||
      patch.materializedOutputByteEnd >
          static_cast<uint64_t>(patch.replacement.size()))
    return std::nullopt;
  return std::make_pair(patch.materializedOutputByteStart,
                        patch.materializedOutputByteEnd);
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldEngine::TextEditMaterializedBByteRange(const TextEdit &edit) const {
  if (edit.materializedBByteBegin && edit.materializedBByteEnd) {
    if (*edit.materializedBByteEnd < *edit.materializedBByteBegin ||
        *edit.materializedBByteEnd > static_cast<uint64_t>(bSource_.size()))
      return std::nullopt;
    return std::make_pair(*edit.materializedBByteBegin,
                          *edit.materializedBByteEnd);
  }

  if (edit.directTUHunkBStart && edit.directTUHunkBEnd)
    return BTokenRangeToByteRange(*edit.directTUHunkBStart,
                                  *edit.directTUHunkBEnd);

  return std::nullopt;
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldEngine::MacroPatchMaterializedBByteRange(const MacroPatch &patch) const {
  // Prefer an explicitly stamped envelope.  Structure-preserving macro patches
  // use this to distinguish two edit-map meanings that share the same physical
  // TextEdit: ordinary argument rewrites map the B side to the whole expansion
  // envelope they regenerate, while pure insertions intentionally stay narrow
  // and let the final TextEdit/hunk metadata provide the inserted payload.
  if (patch.hasMaterializedBTokenRange) {
    if (auto bytes = BTokenRangeToByteRange(patch.materializedBTokStart,
                                            patch.materializedBTokEnd))
      return bytes;
  }

  uint64_t macroId = patch.proof.proofRootMacroId ? patch.proof.proofRootMacroId
                                            : patch.macroId;
  if (macroId == 0)
    return std::nullopt;

  const RefoldModel::MacroInvocation *macro = FindMacroInvocationById(macroId);
  if (!macro)
    return std::nullopt;

  if (auto plan = ComputeWholeCoverPlan(*macro))
    return BTokenRangeToByteRange(plan->bTokStart, plan->bTokEnd);

  if (auto env = MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
          macro->cover.begin, macro->cover.end))
    return BTokenRangeToByteRange(env->first, env->second);

  return std::nullopt;
}

std::string RefoldEngine::ApplyTextEditsWithPendingResync(
    StringRef originalFileText, ArrayRef<TextEdit> edits,
    DenseSet<uint64_t> *appliedExpandedMacroRootIds,
    StringRef emissionOwner, std::optional<uint64_t> ownerIncludeId,
    std::vector<MaterializedEditMapping> *materializedEditMappings,
    std::vector<FinalLineControlPruneCandidate>
        *lineControlPruneCandidates,
    std::vector<FinalLineControlSourceMapping>
        *lineControlSourceMappings) const {
  // Source-side ranges are only knowable at this final splice boundary: every
  // earlier candidate still may be merged, dropped, widened, or followed by a
  // pending resync before it becomes physical output.
  if (materializedEditMappings)
    materializedEditMappings->clear();
  if (lineControlPruneCandidates)
    lineControlPruneCandidates->clear();
  if (lineControlSourceMappings)
    lineControlSourceMappings->clear();
  const std::string sourceMappingOwner =
      emissionOwner.empty() ? std::string()
                            : lineDirs_.ToAbsolutePath(emissionOwner);
  if (edits.empty()) {
    if (lineControlSourceMappings && !originalFileText.empty() &&
        !sourceMappingOwner.empty()) {
      lineControlSourceMappings->push_back(FinalLineControlSourceMapping{
          0, static_cast<uint64_t>(originalFileText.size()),
          sourceMappingOwner, 0,
          static_cast<uint64_t>(originalFileText.size()), ownerIncludeId});
    }
    return originalFileText.str();
  }

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

      std::optional<uint64_t> mergedBByteBegin;
      std::optional<uint64_t> mergedBByteEnd;
      bool mergedBByteRangeContiguous = true;
      std::optional<uint64_t> mergedOutByteBegin;
      std::optional<uint64_t> mergedOutByteEnd;
      // Same-offset insertions become one physical edit in source order. The
      // sidecar can describe that merged edit only when the contributing
      // B-side witnesses tile one contiguous byte range in the same order.
      for (size_t k = i; k < j; ++k) {
        const uint64_t fragmentOutputBase =
            static_cast<uint64_t>(merged.text.size());
        merged.text.append(ordered[k].e->text);
        appendShiftedLineControlPruneCandidates(
            merged.lineControlPruneCandidates,
            ordered[k].e->lineControlPruneCandidates, fragmentOutputBase);
        appendShiftedLineControlSourceMappings(
            merged.lineControlSourceMappings,
            ordered[k].e->lineControlSourceMappings, fragmentOutputBase);
        if (ordered[k].e->pending)
          merged.pending = ordered[k].e->pending;
        merged.acceptedResults.insert(merged.acceptedResults.end(),
                                      ordered[k].e->acceptedResults.begin(),
                                      ordered[k].e->acceptedResults.end());
        if (auto bRange = TextEditMaterializedBByteRange(*ordered[k].e)) {
          if (!mergedBByteBegin) {
            mergedBByteBegin = bRange->first;
            mergedBByteEnd = bRange->second;
          } else if (mergedBByteEnd && *mergedBByteEnd == bRange->first) {
            mergedBByteEnd = bRange->second;
          } else {
            mergedBByteRangeContiguous = false;
          }
        } else if (materializedEditMappings) {
          mergedBByteRangeContiguous = false;
        }
        if (auto outRange =
                TextEditMaterializedOutputTextRange(*ordered[k].e)) {
          const uint64_t outBegin = fragmentOutputBase + outRange->first;
          const uint64_t outEnd = fragmentOutputBase + outRange->second;
          mergedOutByteBegin =
              mergedOutByteBegin ? std::min(*mergedOutByteBegin, outBegin)
                                 : outBegin;
          mergedOutByteEnd =
              mergedOutByteEnd ? std::max(*mergedOutByteEnd, outEnd) : outEnd;
        }
      }
      if (mergedBByteBegin && mergedBByteEnd && mergedBByteRangeContiguous)
        StampTextEditMaterializedBByteRange(merged, *mergedBByteBegin,
                                            *mergedBByteEnd);
      if (mergedOutByteBegin && mergedOutByteEnd)
        StampTextEditMaterializedOutputTextRange(merged, *mergedOutByteBegin,
                                                 *mergedOutByteEnd);

      deduplicateLineControlPruneCandidates(merged.lineControlPruneCandidates);
      deduplicateLineControlSourceMappings(merged.lineControlSourceMappings);

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
                                             replacement, emissionOwner,
                                             ownerIncludeId);
        TextEdit merged{s, t, std::move(ro.text), std::move(ro.pending),
                        std::nullopt, {}, {}, {}};
        merged.lineControlPruneCandidates =
            std::move(ro.lineControlPruneCandidates);
        StampTextEditMaterializedBTokenRange(merged, bBegin, bEnd);

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

        // Add a carrier for the actual emitted merged surface so the final
        // proof gate can reason about this replacement as one conservative TU
        // edit.
        AttachAcceptedResultCarrier(
            merged, BuildAcceptedTUTextEditCandidate(
                        AcceptedPathKind::TUByteSpanConservativeEdit, s, t,
                        replacement));

        return merged;
      };

      // Split token-LCS frontiers can produce several direct TU hunk fragments
      // for the same original byte span. If those fragments are adjacent in
      // both A-token and B-token space, they are not competing writers: they
      // are one replacement that was fragmented before byte emission. Rebuild
      // that replacement from the closed B-token interval; otherwise keep the
      // existing fail-closed duplicate-edit policy.
      if (std::optional<TextEdit> mergedDirectDuplicate =
              tryMergeDirectDuplicateFragments()) {
        norm.push_back(std::move(*mergedDirectDuplicate));
        i = j;
        continue;
      }

      RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/apply",
          llvm::formatv(
              "conflicting duplicate non-zero edits for span [{0},{1}) in {2}",
              s, t, emissionOwner)
              .str());
      return originalFileText.str();
    }

    TextEdit merged = *first;
    std::optional<std::pair<uint64_t, uint64_t>> mergedBRange =
        TextEditMaterializedBByteRange(merged);
    bool mergedBRangeAmbiguous = false;
    std::optional<std::pair<uint64_t, uint64_t>> mergedOutRange =
        TextEditMaterializedOutputTextRange(merged);
    bool mergedOutRangeAmbiguous = false;
    for (size_t k = i + 1; k < j; ++k) {
      const TextEdit &dup = *ordered[k].e;

      // Identical non-zero edits collapse to one physical replacement, but any
      // deferred line-resync state produced by the later duplicate must still
      // be preserved on the merged edit.
      if (dup.pending)
        merged.pending = dup.pending;

      // If duplicate edits represent an expanded macro replacement, they must
      // agree on the same expanded root. Different roots for the same concrete
      // byte span would make the emitted edit's proof carrier ambiguous.
      if (merged.expandedMacroRootId && dup.expandedMacroRootId &&
          *merged.expandedMacroRootId != *dup.expandedMacroRootId) {
        RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::EmissionEditSetComposable,
                TerminalFallbackFailureReason::UncomposableEmissionEditSet),
            "edits/apply",
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

      if (std::optional<std::pair<uint64_t, uint64_t>> dupBRange =
              TextEditMaterializedBByteRange(dup)) {
        if (!mergedBRange) {
          mergedBRange = dupBRange;
        } else if (mergedBRange->first != dupBRange->first ||
                   mergedBRange->second != dupBRange->second) {
          mergedBRangeAmbiguous = true;
        }
      }

      if (std::optional<std::pair<uint64_t, uint64_t>> dupOutRange =
              TextEditMaterializedOutputTextRange(dup)) {
        if (!mergedOutRange) {
          mergedOutRange = dupOutRange;
        } else if (mergedOutRange->first != dupOutRange->first ||
                   mergedOutRange->second != dupOutRange->second) {
          mergedOutRangeAmbiguous = true;
        }
      }

      // Keep all accepted-result carriers from the equivalent duplicates. The
      // text replacement is shared, but each proof witness still explains one
      // path that contributed to the emitted edit.
      appendShiftedLineControlPruneCandidates(
          merged.lineControlPruneCandidates, dup.lineControlPruneCandidates,
          /*delta=*/0);
      appendShiftedLineControlSourceMappings(
          merged.lineControlSourceMappings, dup.lineControlSourceMappings,
          /*delta=*/0);
      merged.acceptedResults.insert(merged.acceptedResults.end(),
                                    dup.acceptedResults.begin(),
                                    dup.acceptedResults.end());
    }

    if (mergedBRangeAmbiguous && materializedEditMappings) {
      RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact),
          "edit-map",
          llvm::formatv("duplicate non-zero edits for span [{0},{1}) in {2} "
                        "carry different B-side materialization ranges",
                        s, t, emissionOwner)
              .str());
      return originalFileText.str();
    }
    if (mergedOutRangeAmbiguous && materializedEditMappings) {
      RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionArtifactDischarged,
              TerminalFallbackFailureReason::UndischargedEmissionArtifact),
          "edit-map",
          llvm::formatv("duplicate non-zero edits for span [{0},{1}) in {2} "
                        "carry different refolded-output materialization "
                        "ranges",
                        s, t, emissionOwner)
              .str());
      return originalFileText.str();
    }
    if (mergedBRange)
      StampTextEditMaterializedBByteRange(merged, mergedBRange->first,
                                          mergedBRange->second);
    if (mergedOutRange)
      StampTextEditMaterializedOutputTextRange(merged, mergedOutRange->first,
                                               mergedOutRange->second);

    deduplicateLineControlPruneCandidates(merged.lineControlPruneCandidates);
    deduplicateLineControlSourceMappings(merged.lineControlSourceMappings);

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
                          replacement, emissionOwner, ownerIncludeId);
    closure =
        TextEdit{best->sourceBegin,     best->sourceEnd, std::move(ro.text),
                 std::move(ro.pending), std::nullopt,    {}, {}, {}};
    closure.lineControlPruneCandidates =
        std::move(ro.lineControlPruneCandidates);
    StampTextEditMaterializedBTokenRange(closure, best->bBegin, best->bEnd);
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
  if (!AuditAcceptedEditProofs(norm, "emit/nonterminal", emissionOwner))
    return originalFileText.str();

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
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/apply",
          llvm::formatv("bad normalized edit bounds in {0}: edit#{1}=[{2},{3}) "
                        "fileLen={4} textLen={5}",
                        emissionOwner, editIndex, e.start, e.end, n,
                        e.text.size())
              .str());
      return originalFileText.str();
    }
    if (e.start < validatedCursor) {
      RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/apply",
          llvm::formatv("overlapping normalized edits in {0}: previousEnd={1} "
                        "edit#{2}=[{3},{4}) textLen={5}",
                        emissionOwner, validatedCursor, editIndex, e.start,
                        e.end, e.text.size())
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
    pending = AppendOriginalSliceWithPending(
        out, originalFileText, cursor, e.start, std::move(pending),
        sourceMappingOwner, ownerIncludeId, lineControlPruneCandidates,
        lineControlSourceMappings);

    // Record expanded macro roots that actually made it into the emitted text.
    // Counter stabilization uses this to force later __COUNTER__-sensitive
    // macro occurrences when needed.
    if (appliedExpandedMacroRootIds && e.expandedMacroRootId)
      appliedExpandedMacroRootIds->insert(*e.expandedMacroRootId);

    const uint64_t emittedBegin = static_cast<uint64_t>(out.size());
    out.append(e.text);
    if (lineControlPruneCandidates)
      appendShiftedLineControlPruneCandidates(*lineControlPruneCandidates,
                                              e.lineControlPruneCandidates,
                                              emittedBegin);
    if (lineControlSourceMappings)
      appendShiftedLineControlSourceMappings(*lineControlSourceMappings,
                                             e.lineControlSourceMappings,
                                             emittedBegin);

    // Record only the replacement bytes written by this edit. Copied original
    // slices and emitted #line/resync material are intentionally outside the
    // mapped source range because they are not materialized B edit payload.
    if (materializedEditMappings) {
      std::optional<std::pair<uint64_t, uint64_t>> bRange =
          TextEditMaterializedBByteRange(e);
      if (!bRange) {
        RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::EmissionArtifactDischarged,
        TerminalFallbackFailureReason::UndischargedEmissionArtifact), "edit-map",
            llvm::formatv("emitted edit in {0} at source=[{1},{2}) lacks a "
                          "deterministic B-side materialization range",
                          emissionOwner, e.start, e.end)
                .str());
        materializedEditMappings->clear();
        return originalFileText.str();
      }
      std::optional<std::pair<uint64_t, uint64_t>> outRange =
          TextEditMaterializedOutputTextRange(e);
      if (!outRange) {
        RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::EmissionArtifactDischarged,
                TerminalFallbackFailureReason::UndischargedEmissionArtifact),
            "edit-map",
            llvm::formatv("emitted edit in {0} at source=[{1},{2}) carries "
                          "invalid refolded-output materialization range",
                          emissionOwner, e.start, e.end)
                .str());
        materializedEditMappings->clear();
        return originalFileText.str();
      }
      materializedEditMappings->push_back(MaterializedEditMapping{
          bRange->first, bRange->second, emittedBegin + outRange->first,
          emittedBegin + outRange->second});
    }

    // Carry any resync that the edit itself could not emit locally. It will be
    // discharged by the next copied original slice, or by the final tail copy.
    if (e.pending) {
      pending = e.pending;
    }

    cursor = e.end;
  }

  // Copy the final untouched tail and flush any pending line resync that
  // survived the last replacement.
  pending = AppendOriginalSliceWithPending(
      out, originalFileText, cursor, n, std::move(pending), sourceMappingOwner,
      ownerIncludeId, lineControlPruneCandidates, lineControlSourceMappings);

  auto shiftLineControlMetadataAfterInsertion = [&](uint64_t pos,
                                                     uint64_t len) {
    if (len == 0)
      return;

    if (lineControlPruneCandidates) {
      for (FinalLineControlPruneCandidate &candidate :
           *lineControlPruneCandidates) {
        if (candidate.finalBegin >= pos) {
          candidate.finalBegin += len;
          candidate.finalEnd += len;
        } else if (candidate.finalEnd > pos) {
          // A synthetic directive should never straddle an independently
          // inserted repair.  Preserve a conservative range if it does rather
          // than creating overlapping stale coordinates.
          candidate.finalEnd += len;
        }
      }
    }

    if (lineControlSourceMappings) {
      std::vector<FinalLineControlSourceMapping> adjusted;
      adjusted.reserve(lineControlSourceMappings->size() + 1);
      for (FinalLineControlSourceMapping mapping : *lineControlSourceMappings) {
        if (mapping.finalEnd <= pos) {
          adjusted.push_back(std::move(mapping));
          continue;
        }
        if (mapping.finalBegin >= pos) {
          mapping.finalBegin += len;
          mapping.finalEnd += len;
          adjusted.push_back(std::move(mapping));
          continue;
        }

        // The insertion splits a copied source slice.  Keep precise provenance
        // for the surviving prefix and suffix; the synthetic #line bytes in the
        // middle intentionally receive no source mapping.
        const uint64_t sourceSplit =
            mapping.sourceBegin + (pos - mapping.finalBegin);
        FinalLineControlSourceMapping prefix = mapping;
        prefix.finalEnd = pos;
        prefix.sourceEnd = sourceSplit;
        if (prefix.finalBegin < prefix.finalEnd)
          adjusted.push_back(std::move(prefix));

        FinalLineControlSourceMapping suffix = mapping;
        suffix.finalBegin = pos + len;
        suffix.finalEnd += len;
        suffix.sourceBegin = sourceSplit;
        if (suffix.finalBegin < suffix.finalEnd)
          adjusted.push_back(std::move(suffix));
      }
      *lineControlSourceMappings = std::move(adjusted);
      deduplicateLineControlSourceMappings(*lineControlSourceMappings);
    }
  };

  auto sourceOffsetToFinalOffset =
      [&](uint64_t sourceOffset) -> std::optional<uint64_t> {
    if (!lineControlSourceMappings)
      return std::nullopt;
    for (const FinalLineControlSourceMapping &mapping :
         *lineControlSourceMappings) {
      if (mapping.ownerIncludeId != ownerIncludeId)
        continue;
      if (mapping.physicalFile != sourceMappingOwner)
        continue;
      if (sourceOffset < mapping.sourceBegin ||
          sourceOffset > mapping.sourceEnd)
        continue;
      if (sourceOffset == mapping.sourceEnd)
        return mapping.finalEnd;
      return mapping.finalBegin + (sourceOffset - mapping.sourceBegin);
    }
    return std::nullopt;
  };

  struct ConditionalJoinLineRepair {
    uint64_t sourceOffset = 0;
    uint64_t finalOffset = 0;
    std::string directive;
  };

  std::vector<ConditionalJoinLineRepair> joinRepairs;
  if (lineDirs_.Enabled() && lineControlSourceMappings &&
      !sourceMappingOwner.empty()) {
    for (const RefoldModel::CondGroup *group :
         model_.GetCondGroups(emissionOwner, ownerIncludeId)) {
      if (!group || group->file != emissionOwner ||
          group->parentIncludeId != ownerIncludeId)
        continue;

      bool groupContainsEdit = false;
      for (const TextEdit &edit : norm) {
        const bool zeroWidthInside =
            edit.start == edit.end && group->ContainsByte(edit.start);
        const bool overlap =
            edit.start < group->groupE && edit.end > group->groupB;
        if (zeroWidthInside || overlap) {
          groupContainsEdit = true;
          break;
        }
      }
      if (!groupContainsEdit)
        continue;

      std::optional<LineStateObserverSite> firstObserver =
          FirstOwnerSuffixLineStateObserverSite(ownerIncludeId, emissionOwner,
                                               group->groupE);
      if (!firstObserver || !firstObserver->demand.Any())
        continue;

      // If the first observer is still inside the conditional group, an
      // arm-local repair is the only statement that can dominate it.  The join
      // repair is required only for observers reached after the group has
      // rejoined.
      if (firstObserver->offset < group->groupE)
        continue;

      std::optional<uint64_t> finalOffset =
          sourceOffsetToFinalOffset(firstObserver->offset);
      const OwnerStateBoundary observerBoundary =
          OwnerStateBoundary::FromSource(OwnerSourceRange::From(
              emissionOwner, firstObserver->offset, firstObserver->offset,
              ownerIncludeId));

      auto forEachLineControlDemandComponent =
          [&](const LineStateObserverDemand &componentDemand, auto &&fn) {
            if (componentDemand.needsLine)
              fn(OwnerStateComponent::LineNumber);
            if (componentDemand.needsFile)
              fn(OwnerStateComponent::FileState);
            if (componentDemand.needsFileName)
              fn(OwnerStateComponent::FileName);
          };

      if (!finalOffset) {
        forEachLineControlDemandComponent(
            firstObserver->demand, [&](OwnerStateComponent component) {
              const std::string detail =
                  llvm::formatv("cannot map conditional join observer in {0}: "
                                "group#{1} observerSource={2}",
                                emissionOwner, group->id, firstObserver->offset)
                      .str();
              (void)CheckStateTransitionAcrossEditBoundary(
                  observerBoundary, component, StateMutationKind::Replayed,
                  BuildStateTransitionWitness(
                      SuffixStabilityWitnessKind::TerminalStateFailure,
                      component, observerBoundary, detail),
                  "linedir/conditional-join", detail,
                  /*requireKnownObserver=*/true);
            });
        return originalFileText.str();
      }

      LineDirectiveLocation loc =
          LineDirectiveInserter::LogicalLocationAtOffset(
              originalFileText, firstObserver->offset, emissionOwner, model_,
              emissionOwner, ownerIncludeId);

      // Conditional-join repairs must describe the logical line state that
      // reaches the first suffix observer after the conditional group rejoins.
      // The lexical scanner above can reconstruct ordinary source-local #line
      // state, but producer-backed recovery is needed for source-authored #line
      // operands whose evaluated state was recorded by the producer.

      if (!loc.producerProven) {
        if (std::optional<LineDirectiveLocation> producerLoc =
                ProducerBackedLineControlLocationAt(
                    originalFileText, emissionOwner, ownerIncludeId,
                    firstObserver->offset, firstObserver->offset)) {
          loc = std::move(*producerLoc);
        }
      }

      std::string directive =
          lineDirs_.FormatLineDirective(loc.lineNo, loc.fileSpelling);
      if (directive.empty())
        continue;

      auto finalLineBeginForOffset = [&](uint64_t offset) -> uint64_t {
        const uint64_t boundedOffset =
            std::min<uint64_t>(offset, static_cast<uint64_t>(out.size()));
        uint64_t lineBegin = boundedOffset;
        while (lineBegin > 0 && out[lineBegin - 1] != '\n')
          --lineBegin;
        return lineBegin;
      };

      auto immediatelyPrecededBySameLineDirective =
          [&](uint64_t insertionOffset, StringRef directiveText) -> bool {
        if (insertionOffset == 0 || directiveText.empty())
          return false;

        uint64_t prevLineEnd = insertionOffset;
        if (prevLineEnd > 0 && out[prevLineEnd - 1] == '\n')
          --prevLineEnd;
        uint64_t prevLineBegin = prevLineEnd;
        while (prevLineBegin > 0 && out[prevLineBegin - 1] != '\n')
          --prevLineBegin;

        StringRef prevLine(out.data() + prevLineBegin,
                           static_cast<size_t>(prevLineEnd - prevLineBegin));
        return prevLine.trim() == directiveText.trim();
      };

      const uint64_t insertionOffset = finalLineBeginForOffset(*finalOffset);
      StringRef currentOutAtInsertion(out.data(),
                                      static_cast<size_t>(insertionOffset));

      auto previousLineControlIsInsideThisConditionalGroup = [&]() -> bool {
        std::optional<std::pair<uint64_t, uint64_t>> previousDirective =
            findLastLineControlDirectiveRangeBefore(
                StringRef(out.data(), out.size()), insertionOffset);
        if (!previousDirective)
          return false;

        std::optional<uint64_t> groupFinalBegin =
            sourceOffsetToFinalOffset(group->groupB);
        std::optional<uint64_t> groupFinalEnd =
            sourceOffsetToFinalOffset(group->groupE);
        if (!groupFinalBegin || !groupFinalEnd)
          return false;

        return previousDirective->first >= *groupFinalBegin &&
               previousDirective->second <= *groupFinalEnd;
      };

      if (!LineDirectiveInserter::ShouldEmitLineDirective(
              currentOutAtInsertion, loc.fileSpelling, loc.lineNo, directive)) {
        if (!previousLineControlIsInsideThisConditionalGroup()) {
          forEachLineControlDemandComponent(
              firstObserver->demand, [&](OwnerStateComponent component) {
                const std::string detail =
                    llvm::formatv("suppress post-conditional #line in {0}: "
                                  "group#{1} observerSource={2}; current "
                                  "stream state is equivalent",
                                  emissionOwner, group->id,
                                  firstObserver->offset)
                        .str();
                (void)CheckStateTransitionAcrossEditBoundary(
                    observerBoundary, component,
                    StateMutationKind::PreservedAcrossReplacement,
                    BuildStateTransitionWitness(
                        SuffixStabilityWitnessKind::StateRepair, component,
                        observerBoundary, detail),
                    "linedir/conditional-join", detail,
                    /*requireKnownObserver=*/true);
              });
          continue;
        }
      }

      if (immediatelyPrecededBySameLineDirective(insertionOffset, directive)) {
        forEachLineControlDemandComponent(
            firstObserver->demand, [&](OwnerStateComponent component) {
              const std::string detail =
                  llvm::formatv("suppress duplicate post-conditional #line in "
                                "{0}: group#{1} observerSource={2}",
                                emissionOwner, group->id, firstObserver->offset)
                      .str();
              (void)CheckStateTransitionAcrossEditBoundary(
                  observerBoundary, component,
                  StateMutationKind::PreservedAcrossReplacement,
                  BuildStateTransitionWitness(
                      SuffixStabilityWitnessKind::StateRepair, component,
                      observerBoundary, detail),
                  "linedir/conditional-join", detail,
                  /*requireKnownObserver=*/true);
            });
        continue;
      }

      forEachLineControlDemandComponent(
          firstObserver->demand, [&](OwnerStateComponent component) {
            const std::string detail =
                llvm::formatv("post-conditional synthetic #line repair in {0}: "
                              "group#{1} observerSource={2} final={3}",
                              emissionOwner, group->id, firstObserver->offset,
                              insertionOffset)
                    .str();
            (void)CheckStateTransitionAcrossEditBoundary(
                observerBoundary, component, StateMutationKind::Replayed,
                BuildStateTransitionWitness(
                    SuffixStabilityWitnessKind::StateRepair, component,
                    observerBoundary, detail),
                "linedir/conditional-join", detail,
                /*requireKnownObserver=*/true);
          });

      joinRepairs.push_back(ConditionalJoinLineRepair{
          firstObserver->offset, insertionOffset, std::move(directive)});
    }
  }

  if (!joinRepairs.empty()) {
    llvm::sort(joinRepairs,
               [](const ConditionalJoinLineRepair &lhs,
                  const ConditionalJoinLineRepair &rhs) {
                 if (lhs.finalOffset != rhs.finalOffset)
                   return lhs.finalOffset > rhs.finalOffset;
                 if (lhs.sourceOffset != rhs.sourceOffset)
                   return lhs.sourceOffset > rhs.sourceOffset;
                 return lhs.directive < rhs.directive;
               });
    joinRepairs.erase(
        std::unique(joinRepairs.begin(), joinRepairs.end(),
                    [](const ConditionalJoinLineRepair &lhs,
                       const ConditionalJoinLineRepair &rhs) {
                      return lhs.finalOffset == rhs.finalOffset &&
                             lhs.directive == rhs.directive;
                    }),
        joinRepairs.end());

    for (const ConditionalJoinLineRepair &repair : joinRepairs) {
      out.insert(out.begin() + static_cast<size_t>(repair.finalOffset),
                 repair.directive.begin(), repair.directive.end());
      shiftLineControlMetadataAfterInsertion(
          repair.finalOffset, static_cast<uint64_t>(repair.directive.size()));
    }
  }

  if (lineControlPruneCandidates)
    deduplicateLineControlPruneCandidates(*lineControlPruneCandidates);
  if (lineControlSourceMappings)
    deduplicateLineControlSourceMappings(*lineControlSourceMappings);
  return std::string(out.str());
}

std::optional<RefoldEngine::PendingResync>
RefoldEngine::AppendOriginalSliceWithPending(
    SmallVectorImpl<char> &out, llvm::StringRef original, uint64_t from,
    uint64_t to, std::optional<RefoldEngine::PendingResync> pending,
    StringRef emissionOwner, std::optional<uint64_t> ownerIncludeId,
    std::vector<FinalLineControlPruneCandidate>
        *lineControlPruneCandidates,
    std::vector<FinalLineControlSourceMapping>
        *lineControlSourceMappings) const {
  auto appendMappedOriginalSlice = [&](uint64_t begin, uint64_t end) {
    if (begin >= end)
      return;
    const uint64_t finalBegin = static_cast<uint64_t>(out.size());
    auto slice = original.slice(static_cast<size_t>(begin),
                                static_cast<size_t>(end));
    out.append(slice.begin(), slice.end());
    const uint64_t finalEnd = static_cast<uint64_t>(out.size());
    if (lineControlSourceMappings && !emissionOwner.empty()) {
      lineControlSourceMappings->push_back(FinalLineControlSourceMapping{
          finalBegin, finalEnd, emissionOwner.str(), begin, end,
          ownerIncludeId});
    }
  };

  // Fast path: if there is no pending #line correction, or line directives are
  // disabled, this helper is just a straight copy of the untouched source
  // slice.
  if (!pending || !lineDirs_.Enabled()) {
    appendMappedOriginalSlice(from, to);
    return std::nullopt;
  }

  if (pending->deferToConditionalJoin) {
    appendMappedOriginalSlice(from, to);
    return pending;
  }

  auto checkPendingLineControlRepair =
      [&](uint64_t sourceOffset, StateMutationKind mutation, StringRef detail) {
        const OwnerStateBoundary boundary = OwnerStateBoundary::FromSource(
            OwnerSourceRange::From(pending->fileSpellingForDir, sourceOffset,
                                   sourceOffset, pending->ownerIncludeId));
        return CheckStateTransitionAcrossEditBoundary(
            boundary, OwnerStateComponent::LineNumber, mutation,
            BuildStateTransitionWitness(SuffixStabilityWitnessKind::StateRepair,
                                        OwnerStateComponent::LineNumber,
                                        boundary, detail),
            "line/pending", detail, /*requireKnownObserver=*/false);
      };

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

  auto logicalLocationForPendingFlush =
      [&](uint64_t pos) -> std::optional<LineDirectiveLocation> {
    LineDirectiveLocation loc = LineDirectiveInserter::LogicalLocationAtOffset(
        original, static_cast<size_t>(pos), pending->fileSpellingForDir, model_,
        pending->fileSpellingForDir, pending->ownerIncludeId);
    if (loc.producerProven)
      return loc;

    // If the local scanner found an unmodeled source #line in the bytes copied
    // after the edit, the pending state can no longer be propagated by counting
    // newlines: that directive may have overwritten the logical stream state.
    if (loc.unprovenLineControlDirectiveOffset &&
        *loc.unprovenLineControlDirectiveOffset >= pending->resumeOffset &&
        *loc.unprovenLineControlDirectiveOffset < pos)
      return std::nullopt;

    // The pending object was created only after ApplyResyncOrPend proved the
    // logical state at resumeOffset.  If the eventual flush point is later in
    // untouched source, and no intervening unmodeled line-control directive was
    // found, advance that proved state by the physical non-spliced newlines
    // copied from the original between resumeOffset and pos.  This handles
    // source-authored #line directives whose operands used imported macro
    // state: the local scanner cannot re-evaluate them, but the producer
    // already did.
    if (pending->resumeLineNo == 0 || pos < pending->resumeOffset)
      return std::nullopt;

    const size_t delta = stringutils::countNonSplicedNewlines(
        original, pending->resumeOffset, pos);
    return LineDirectiveLocation(pending->resumeFileSpelling,
                                 pending->resumeLineNo + delta, true);
  };

  // Prefer to flush the pending resync before copying this slice when the
  // emitted output is already at BOL and the untouched input also resumes at a
  // safe line boundary. This handles both true BOL and the indentation prefix
  // of an untouched preprocessor directive line.
  if (stringutils::outAtBOL(StringRef(out.data(), out.size())) &&
      (stringutils::isBOL(original, static_cast<size_t>(from)) ||
       canFlushBeforeUntouchedDirectiveLine(static_cast<size_t>(from)))) {
    std::optional<LineDirectiveLocation> loc =
        logicalLocationForPendingFlush(from);
    if (!loc) {
      appendMappedOriginalSlice(from, to);
      return std::nullopt;
    }

    std::string directive =
        lineDirs_.FormatLineDirective(loc->lineNo, loc->fileSpelling);

    StringRef currentOut(out.data(), out.size());

    // Avoid emitting a redundant directive when the current output state
    // already represents the requested file/line location.
    if (LineDirectiveInserter::ShouldEmitLineDirective(
            currentOut, loc->fileSpelling, loc->lineNo, directive)) {
      (void)checkPendingLineControlRepair(
          from, StateMutationKind::MovedLater,
          llvm::formatv("flush pending synthetic #line at slice begin {0}",
                        from)
              .str());
      const uint64_t begin = static_cast<uint64_t>(out.size());
      out.append(directive.begin(), directive.end());
      const uint64_t end = static_cast<uint64_t>(out.size());
      if (lineControlPruneCandidates &&
          pending->finalLineControlPruneEligible) {
        lineControlPruneCandidates->push_back(
            makeSyntheticLineControlPruneCandidate(
                begin, end, FinalLineDirective::Origin::SyntheticNewlineResync,
                FinalLineControlOwnerKey(pending->fileSpellingForDir,
                                         pending->ownerIncludeId),
                FinalLineControlObligation::CosmeticSyntheticResync));
      }
    } else {
      (void)checkPendingLineControlRepair(
          from, StateMutationKind::PreservedAcrossReplacement,
          llvm::formatv("suppress pending #line at slice begin {0}; "
                        "current output state is equivalent",
                        from)
              .str());
    }

    appendMappedOriginalSlice(from, to);
    return std::nullopt;
  }

  // If the slice does not begin at a safe flush point, copy forward until the
  // first real newline boundary. Line-spliced newlines are not safe because
  // they do not end the logical source line.
  while (i < to) {
    size_t nl = original.find('\n', i);
    if (nl == llvm::StringRef::npos || nl >= static_cast<size_t>(to))
      break;

    appendMappedOriginalSlice(i, static_cast<uint64_t>(nl + 1));

    i = nl + 1;

    if (!stringutils::isLineSplice(original, nl)) {
      std::optional<LineDirectiveLocation> loc =
          logicalLocationForPendingFlush(i);
      if (!loc) {
        pending = std::nullopt;
        break;
      }

      std::string directive =
          lineDirs_.FormatLineDirective(loc->lineNo, loc->fileSpelling);

      StringRef currentOut(out.data(), out.size());
      if (LineDirectiveInserter::ShouldEmitLineDirective(
              currentOut, loc->fileSpelling, loc->lineNo, directive)) {
        (void)checkPendingLineControlRepair(
            i, StateMutationKind::MovedLater,
            llvm::formatv("flush pending synthetic #line at safe newline {0}",
                          i)
                .str());
        const uint64_t begin = static_cast<uint64_t>(out.size());
        out.append(directive.begin(), directive.end());
        const uint64_t end = static_cast<uint64_t>(out.size());
        if (lineControlPruneCandidates &&
            pending->finalLineControlPruneEligible) {
          lineControlPruneCandidates->push_back(
              makeSyntheticLineControlPruneCandidate(
                  begin, end,
                  FinalLineDirective::Origin::SyntheticNewlineResync,
                  FinalLineControlOwnerKey(pending->fileSpellingForDir,
                                           pending->ownerIncludeId),
                  FinalLineControlObligation::CosmeticSyntheticResync));
        }
      } else {
        (void)checkPendingLineControlRepair(
            i, StateMutationKind::PreservedAcrossReplacement,
            llvm::formatv("suppress pending #line at safe newline {0}; "
                          "current output state is equivalent",
                          i)
                .str());
      }

      pending = std::nullopt;
      break;
    }
  }

  // Copy any bytes after the flush point, or the whole slice tail if no safe
  // flush point was found. In the latter case, return the still-pending resync
  // so a later original slice can discharge it.
  if (i < to) {
    appendMappedOriginalSlice(i, to);
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
    REFOLD_LOG_FATAL("path/canon", "failed to canonicalize '{0}': {1}", path,
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
