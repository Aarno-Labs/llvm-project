//===--- FinalLineControlModel.cpp -----------------------------*- C++ -*-===//
//
// Passive final-stream line-control proof scaffolding for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "FinalLineControlModel.h"
#include "StringUtils.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cctype>
#include <limits>

using namespace llvm;

namespace clang {
namespace refold {

void CanonicalizeFinalLineControlSourceMappings(
    std::vector<FinalLineControlSourceMapping> &mappings) {
  mappings.erase(
      std::remove_if(mappings.begin(), mappings.end(),
                     [](const FinalLineControlSourceMapping &mapping) {
                       return mapping.finalBegin >= mapping.finalEnd ||
                              mapping.sourceBegin >= mapping.sourceEnd;
                     }),
      mappings.end());

  std::sort(mappings.begin(), mappings.end(),
            [](const FinalLineControlSourceMapping &lhs,
               const FinalLineControlSourceMapping &rhs) {
              if (lhs.finalBegin != rhs.finalBegin)
                return lhs.finalBegin < rhs.finalBegin;
              if (lhs.finalEnd != rhs.finalEnd)
                return lhs.finalEnd < rhs.finalEnd;
              if (lhs.physicalFile != rhs.physicalFile)
                return lhs.physicalFile < rhs.physicalFile;
              if (lhs.ownerIncludeId != rhs.ownerIncludeId)
                return lhs.ownerIncludeId < rhs.ownerIncludeId;
              if (lhs.sourceBegin != rhs.sourceBegin)
                return lhs.sourceBegin < rhs.sourceBegin;
              return lhs.sourceEnd < rhs.sourceEnd;
            });

  std::vector<FinalLineControlSourceMapping> canonical;
  canonical.reserve(mappings.size());
  for (FinalLineControlSourceMapping mapping : mappings) {
    if (!canonical.empty()) {
      FinalLineControlSourceMapping &prev = canonical.back();
      if (prev.finalBegin == mapping.finalBegin &&
          prev.finalEnd == mapping.finalEnd &&
          prev.physicalFile == mapping.physicalFile &&
          prev.ownerIncludeId == mapping.ownerIncludeId &&
          prev.sourceBegin == mapping.sourceBegin &&
          prev.sourceEnd == mapping.sourceEnd) {
        continue;
      }

      if (prev.finalEnd == mapping.finalBegin &&
          prev.sourceEnd == mapping.sourceBegin &&
          prev.physicalFile == mapping.physicalFile &&
          prev.ownerIncludeId == mapping.ownerIncludeId) {
        prev.finalEnd = mapping.finalEnd;
        prev.sourceEnd = mapping.sourceEnd;
        continue;
      }
    }

    canonical.push_back(std::move(mapping));
  }

  mappings = std::move(canonical);
}

void AdjustFinalLineControlSourceMappingsAfterDeletion(
    std::vector<FinalLineControlSourceMapping> &mappings, uint64_t removedBegin,
    uint64_t removedEnd) {
  if (removedBegin >= removedEnd)
    return;

  const uint64_t removedSize = removedEnd - removedBegin;
  std::vector<FinalLineControlSourceMapping> adjusted;
  adjusted.reserve(mappings.size() + 1);

  for (FinalLineControlSourceMapping mapping : mappings) {
    if (mapping.finalEnd <= removedBegin) {
      adjusted.push_back(std::move(mapping));
      continue;
    }

    if (mapping.finalBegin >= removedEnd) {
      mapping.finalBegin -= removedSize;
      mapping.finalEnd -= removedSize;
      adjusted.push_back(std::move(mapping));
      continue;
    }

    // The deleted bytes intersect this copied-source span. Preserve exact
    // provenance for surviving prefix/suffix slices, but leave no mapping across
    // the deleted hole. This helper is live pruning logic, not trace payload.
    if (mapping.finalBegin < removedBegin) {
      FinalLineControlSourceMapping prefix = mapping;
      prefix.finalEnd = removedBegin;
      prefix.sourceEnd = mapping.sourceBegin + (removedBegin - mapping.finalBegin);
      adjusted.push_back(std::move(prefix));
    }

    if (removedEnd < mapping.finalEnd) {
      FinalLineControlSourceMapping suffix = mapping;
      suffix.sourceBegin = mapping.sourceBegin + (removedEnd - mapping.finalBegin);
      suffix.finalBegin = removedBegin;
      suffix.finalEnd = mapping.finalEnd - removedSize;
      adjusted.push_back(std::move(suffix));
    }
  }

  mappings = std::move(adjusted);
  CanonicalizeFinalLineControlSourceMappings(mappings);
}


namespace {

enum class DirectiveKind : uint8_t {
  None,
  Other,
  ConditionalEnter,
  ConditionalMiddle,
  ConditionalExit,
  LineControl,
};

struct DirectiveInfo {
  DirectiveKind kind = DirectiveKind::None;
  size_t payloadBegin = 0;
  StringRef name;
};

struct ParsedLineControl {
  bool recognized = false;
  bool semanticsKnown = false;
  bool hashNumberForm = false;
  std::optional<uint64_t> line;
  std::optional<std::string> file;
};


/// Canonicalize removable final-line-control candidates so the fixed-point
/// pruner is independent of incidental emitter/vector ordering.
///
/// Multiple emission paths can conservatively describe the same final `#line`
/// byte range.  The physical deletion decision depends on the byte range plus
/// the live final-stream proof state; origin/owner/provenance fields are
/// provenance annotations.  Merge exact-range duplicates deterministically
/// before pruning so candidate admission is stable across vector append order.
static void CanonicalizeFinalLineControlPruneCandidates(
    std::vector<FinalLineControlPruneCandidate> &candidates) {
  candidates.erase(
      std::remove_if(candidates.begin(), candidates.end(),
                     [](const FinalLineControlPruneCandidate &candidate) {
                       return candidate.finalBegin >= candidate.finalEnd;
                     }),
      candidates.end());

  auto ownerLess = [](const std::optional<FinalLineControlOwnerKey> &lhs,
                      const std::optional<FinalLineControlOwnerKey> &rhs) {
    if (lhs.has_value() != rhs.has_value())
      return !lhs.has_value();
    if (!lhs)
      return false;
    if (lhs->physicalFile != rhs->physicalFile)
      return lhs->physicalFile < rhs->physicalFile;
    return lhs->ownerIncludeId < rhs->ownerIncludeId;
  };

  std::sort(candidates.begin(), candidates.end(),
            [&](const FinalLineControlPruneCandidate &lhs,
                const FinalLineControlPruneCandidate &rhs) {
              if (lhs.finalBegin != rhs.finalBegin)
                return lhs.finalBegin < rhs.finalBegin;
              if (lhs.finalEnd != rhs.finalEnd)
                return lhs.finalEnd < rhs.finalEnd;
              if (lhs.origin != rhs.origin)
                return static_cast<unsigned>(lhs.origin) <
                       static_cast<unsigned>(rhs.origin);
              if (ownerLess(lhs.physicalOwner, rhs.physicalOwner))
                return true;
              if (ownerLess(rhs.physicalOwner, lhs.physicalOwner))
                return false;
              return !lhs.producerProven && rhs.producerProven;
            });

  auto sameOwner = [](const std::optional<FinalLineControlOwnerKey> &lhs,
                      const std::optional<FinalLineControlOwnerKey> &rhs) {
    if (lhs.has_value() != rhs.has_value())
      return false;
    if (!lhs)
      return true;
    return lhs->physicalFile == rhs->physicalFile &&
           lhs->ownerIncludeId == rhs->ownerIncludeId;
  };

  std::vector<FinalLineControlPruneCandidate> canonical;
  canonical.reserve(candidates.size());
  for (FinalLineControlPruneCandidate candidate : candidates) {
    if (!canonical.empty() &&
        canonical.back().finalBegin == candidate.finalBegin &&
        canonical.back().finalEnd == candidate.finalEnd) {
      FinalLineControlPruneCandidate &merged = canonical.back();

      if (merged.origin != candidate.origin)
        merged.origin = FinalLineDirective::Origin::Unknown;

      if (!sameOwner(merged.physicalOwner, candidate.physicalOwner))
        merged.physicalOwner = std::nullopt;

      merged.producerProven = merged.producerProven || candidate.producerProven;
      continue;
    }

    canonical.push_back(std::move(candidate));
  }

  candidates = std::move(canonical);
}


void applyMatchingPruneCandidate(
    FinalLineDirective &directive,
    ArrayRef<FinalLineControlPruneCandidate> removableCandidates) {
  for (const FinalLineControlPruneCandidate &candidate : removableCandidates) {
    if (candidate.finalBegin != directive.finalBegin ||
        candidate.finalEnd != directive.finalEnd)
      continue;

    directive.removable = true;
    directive.origin = candidate.origin;
    directive.physicalOwner = candidate.physicalOwner;
    directive.producerProven = candidate.producerProven;
    return;
  }
}


std::optional<FinalLineControlSourceMapping> mapFinalDirectiveToSource(
    uint64_t finalBegin, uint64_t finalEnd,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings) {
  for (const FinalLineControlSourceMapping &mapping : sourceMappings) {
    if (finalBegin < mapping.finalBegin || finalEnd > mapping.finalEnd)
      continue;
    const uint64_t deltaBegin = finalBegin - mapping.finalBegin;
    const uint64_t deltaEnd = finalEnd - mapping.finalBegin;
    const uint64_t sourceBegin = mapping.sourceBegin + deltaBegin;
    const uint64_t sourceEnd = mapping.sourceBegin + deltaEnd;
    if (sourceEnd > mapping.sourceEnd)
      continue;

    FinalLineControlSourceMapping narrowed = mapping;
    narrowed.finalBegin = finalBegin;
    narrowed.finalEnd = finalEnd;
    narrowed.sourceBegin = sourceBegin;
    narrowed.sourceEnd = sourceEnd;
    return narrowed;
  }
  return std::nullopt;
}

std::optional<std::pair<uint64_t, uint64_t>> mapSourceRangeToFinal(
    StringRef finalSource, const FinalLineControlProducerObserver &observer,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings) {
  if (!observer.producerProven || !observer.active)
    return std::nullopt;
  if (observer.sourceEnd <= observer.sourceBegin)
    return std::nullopt;

  for (const FinalLineControlSourceMapping &mapping : sourceMappings) {
    if (mapping.physicalFile != observer.physicalFile)
      continue;
    if (mapping.ownerIncludeId != observer.ownerIncludeId)
      continue;
    if (observer.sourceBegin < mapping.sourceBegin ||
        observer.sourceEnd > mapping.sourceEnd)
      continue;

    const uint64_t finalBegin =
        mapping.finalBegin + (observer.sourceBegin - mapping.sourceBegin);
    const uint64_t finalEnd =
        mapping.finalBegin + (observer.sourceEnd - mapping.sourceBegin);
    if (finalEnd > mapping.finalEnd || finalEnd > finalSource.size())
      continue;

    if (!observer.text.empty() &&
        finalSource.slice(finalBegin, finalEnd) != StringRef(observer.text))
      continue;

    return std::make_pair(finalBegin, finalEnd);
  }

  return std::nullopt;
}

void collectProducerObserverCandidatesForLine(
    FinalLineControlModel &model, StringRef finalSource, uint64_t lineBegin,
    uint64_t lineEnd, uint64_t physicalLineNo,
    const FinalLogicalState &stateBefore,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings,
    ArrayRef<FinalLineControlProducerObserver> producerObservers) {
  for (const FinalLineControlProducerObserver &producerObserver :
       producerObservers) {
    std::optional<std::pair<uint64_t, uint64_t>> finalRange =
        mapSourceRangeToFinal(finalSource, producerObserver, sourceMappings);
    if (!finalRange)
      continue;

    const uint64_t finalBegin = finalRange->first;
    const uint64_t finalEnd = finalRange->second;
    if (finalBegin < lineBegin || finalBegin >= lineEnd)
      continue;

    FinalObserver observer;
    observer.finalOffset = finalBegin;
    observer.finalEnd = finalEnd;
    observer.physicalLine = physicalLineNo;
    observer.kind = producerObserver.kind;
    observer.physicalOwner = FinalLineControlOwnerKey(
        producerObserver.physicalFile, producerObserver.ownerIncludeId);
    observer.activity = producerObserver.active
                            ? FinalObserverActivity::KnownActive
                            : FinalObserverActivity::KnownInactive;
    observer.producerProven = producerObserver.producerProven;
    observer.spellingPreserved = producerObserver.producerProven;
    observer.stateBefore = stateBefore;
    model.AddObserver(std::move(observer));
  }
}

bool producerEventMatchesParsedLiteralState(
    const FinalLineControlProducerEvent &event,
    const ParsedLineControl &parsedLineControl) {
  // When the final source spelling is a literal #line directive, use that
  // literal post-state to disambiguate producer records for the same physical
  // site.  The producer may record more than one event for one directive site
  // (for example, pre-transition bookkeeping plus the actual post-directive
  // state).  Binding the final directive to the first same-site/same-text event
  // can therefore assign stale state such as the pre-directive file/line.
  //
  // If the final spelling is not literal enough for the scanner to know the
  // post-state, do not filter here: macro-expanded #line operands must continue
  // to rely on the producer model.
  if (!parsedLineControl.semanticsKnown)
    return true;
  if (parsedLineControl.line &&
      event.logicalLineAfter != *parsedLineControl.line)
    return false;
  if (parsedLineControl.file &&
      event.logicalFileAfter != *parsedLineControl.file)
    return false;
  return true;
}

const FinalLineControlProducerEvent *findProducerLineControlEvent(
    const FinalLineControlSourceMapping &sourceMapping,
    StringRef finalDirectiveText, const ParsedLineControl &parsedLineControl,
    ArrayRef<FinalLineControlProducerEvent> producerEvents) {
  for (const FinalLineControlProducerEvent &event : producerEvents) {
    if (!event.producerProven || !event.active)
      continue;
    if (!event.siteBegin || !event.siteEnd)
      continue;
    if (event.physicalFile != sourceMapping.physicalFile)
      continue;
    if (event.ownerIncludeId != sourceMapping.ownerIncludeId)
      continue;
    if (*event.siteBegin != sourceMapping.sourceBegin ||
        *event.siteEnd != sourceMapping.sourceEnd)
      continue;
    if (!event.text.empty() && StringRef(event.text) != finalDirectiveText)
      continue;
    if (!producerEventMatchesParsedLiteralState(event, parsedLineControl))
      continue;
    return &event;
  }
  return nullptr;
}

void applyProducerLineControlEvent(
    FinalLineDirective &directive, const FinalLineControlSourceMapping &mapping,
    const FinalLineControlProducerEvent &event) {
  directive.activity = event.active ? FinalLineDirectiveActivity::KnownActive
                                    : FinalLineDirectiveActivity::KnownInactive;
  directive.semanticSource = FinalLineDirectiveSemanticSource::ProducerModel;
  directive.semanticsKnown = event.producerProven;
  directive.producerProven = event.producerProven;
  directive.physicalOwner = FinalLineControlOwnerKey(mapping.physicalFile,
                                                     mapping.ownerIncludeId);
  if (event.producerProven) {
    directive.line = event.logicalLineAfter;
    directive.file = event.logicalFileAfter;
  }
}

/// Mark a source-authored final-stream line-control directive as producer-proven.
///
/// Source-authored `#line` directives are source material, not minimization
/// artifacts.  Once Step #2's exact final-to-source mapping has matched the
/// final directive spelling to a producer LineControlEvent for the same
/// physical directive bytes, record the provenance and known post-state, but do
/// not make the directive removable.  A later synthesized repair may dominate
/// all live observers, but that does not make explicitly authored source text
/// gratuitous under the preservation policy.
void markProducerProvenSourceLineControlPreserved(
    FinalLineDirective &directive, const FinalLineControlSourceMapping &mapping,
    const FinalLineControlProducerEvent &event) {
  if (!event.producerProven || !event.active)
    return;
  if (!directive.semanticsKnown ||
      directive.semanticSource != FinalLineDirectiveSemanticSource::ProducerModel)
    return;
  if (directive.activity != FinalLineDirectiveActivity::KnownActive)
    return;
  if (!directive.line)
    return;

  directive.origin = FinalLineDirective::Origin::PreservedSource;
  directive.removable = false;
  directive.producerProven = true;
  directive.physicalOwner = FinalLineControlOwnerKey(mapping.physicalFile,
                                                     mapping.ownerIncludeId);
}

bool physicalLineContinuesWithBackslash(StringRef line) {
  // The scanner works on physical source lines with the terminating '\n'
  // removed.  For CRLF input, the remaining '\r' is part of the line ending,
  // not source spelling for this purpose.  Do not trim spaces or tabs: C line
  // splicing requires the backslash to be immediately before the physical
  // newline, and treating "\\   \n" as a continuation would make a directive
  // segment proof depend on non-standard recovery behavior.
  if (line.ends_with("\r"))
    line = line.drop_back();
  return line.ends_with("\\");
}

/// Skip whitespace/comments between preprocessing tokens on one final physical
/// line.  A line comment consumes the rest of the physical line.  An unclosed
/// block comment leaves the caller with no known following token on this line.
bool skipDirectiveTokenWhitespace(StringRef line, size_t &i) {
  for (;;) {
    stringutils::skipWsNoLF(line, i, line.size());
    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') {
      i = line.size();
      return true;
    }
    if (i + 1 >= line.size() || line[i] != '/' || line[i + 1] != '*')
      return true;

    size_t close = line.find("*/", i + 2);
    if (close == StringRef::npos) {
      i = line.size();
      return false;
    }
    i = close + 2;
  }
}

/// Locate a directive introducer after replacing comments by whitespace, while
/// carrying block-comment state across final physical lines.
bool locateDirectiveHash(StringRef line, bool &inBlockComment, size_t &hash) {
  size_t i = 0;
  for (;;) {
    if (inBlockComment) {
      size_t close = line.find("*/", i);
      if (close == StringRef::npos)
        return false;
      i = close + 2;
      inBlockComment = false;
      continue;
    }

    stringutils::skipWsNoLF(line, i, line.size());
    if (i >= line.size())
      return false;

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
      return false;

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
      size_t close = line.find("*/", i + 2);
      if (close == StringRef::npos) {
        inBlockComment = true;
        return false;
      }
      i = close + 2;
      continue;
    }

    if (line[i] != '#')
      return false;

    hash = i;
    return true;
  }
}

/// Classify lines that have no directive introducer.  This only needs enough
/// precision for Chunk 2 trace output; final layout liveness is handled later by
/// a dedicated proof pass.
FinalPhysicalLineKind classifyNonDirectiveLine(StringRef line,
                                               bool startsInBlockComment) {
  size_t i = 0;
  bool inBlockComment = startsInBlockComment;
  bool sawComment = startsInBlockComment;
  for (;;) {
    if (inBlockComment) {
      size_t close = line.find("*/", i);
      if (close == StringRef::npos)
        return FinalPhysicalLineKind::CommentOnly;
      i = close + 2;
      inBlockComment = false;
      continue;
    }

    stringutils::skipWsNoLF(line, i, line.size());
    if (i >= line.size())
      return sawComment ? FinalPhysicalLineKind::CommentOnly
                        : FinalPhysicalLineKind::Blank;

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
      return FinalPhysicalLineKind::CommentOnly;

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
      sawComment = true;
      size_t close = line.find("*/", i + 2);
      if (close == StringRef::npos)
        return FinalPhysicalLineKind::CommentOnly;
      i = close + 2;
      continue;
    }

    return FinalPhysicalLineKind::Ordinary;
  }
}

DirectiveInfo classifyDirective(StringRef line, size_t hash) {
  DirectiveInfo out;
  size_t i = hash + 1;
  stringutils::skipWsNoLF(line, i, line.size());

  if (i >= line.size()) {
    out.kind = DirectiveKind::Other;
    out.payloadBegin = i;
    return out;
  }

  if (llvm::isDigit(line[i])) {
    out.kind = DirectiveKind::LineControl;
    out.payloadBegin = i;
    return out;
  }

  if (!stringutils::isIdentStart(line[i])) {
    out.kind = DirectiveKind::Other;
    out.payloadBegin = i;
    return out;
  }

  const size_t nameBegin = i;
  while (i < line.size() && stringutils::isIdentPart(line[i]))
    ++i;
  out.name = line.slice(nameBegin, i);
  out.payloadBegin = i;

  if (out.name == "line")
    out.kind = DirectiveKind::LineControl;
  else if (out.name == "if" || out.name == "ifdef" || out.name == "ifndef")
    out.kind = DirectiveKind::ConditionalEnter;
  else if (out.name == "elif" || out.name == "elifdef" ||
           out.name == "elifndef" || out.name == "else")
    out.kind = DirectiveKind::ConditionalMiddle;
  else if (out.name == "endif")
    out.kind = DirectiveKind::ConditionalExit;
  else
    out.kind = DirectiveKind::Other;

  return out;
}

bool parseUnsignedDecimal(StringRef line, size_t &i, uint64_t &value) {
  skipDirectiveTokenWhitespace(line, i);
  if (i >= line.size() || !llvm::isDigit(line[i]))
    return false;

  uint64_t result = 0;
  do {
    const uint64_t digit = static_cast<uint64_t>(line[i] - '0');
    if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10)
      return false;
    result = result * 10 + digit;
    ++i;
  } while (i < line.size() && llvm::isDigit(line[i]));

  value = result;
  return true;
}

/// Parse an ordinary narrow string-literal token and return its raw spelling
/// without the surrounding quotes.  Escape decoding is intentionally deferred to
/// producer/model support; if a future pruning proof needs exact `__FILE__`
/// semantics for escaped filenames, it should consume the producer-proven line
/// control event rather than guessing here.
bool parseRawOrdinaryStringLiteral(StringRef line, size_t &i,
                                   std::string &value) {
  skipDirectiveTokenWhitespace(line, i);
  if (i >= line.size() || line[i] != '"')
    return false;

  const size_t contentBegin = i + 1;
  ++i;
  bool escaped = false;
  while (i < line.size()) {
    const char c = line[i];
    if (escaped) {
      escaped = false;
      ++i;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      ++i;
      continue;
    }
    if (c == '"') {
      value = line.slice(contentBegin, i).str();
      ++i;
      return true;
    }
    ++i;
  }

  return false;
}

bool skipTrailingIgnorable(StringRef line, size_t &i) {
  for (;;) {
    stringutils::skipWsNoLF(line, i, line.size());
    if (i >= line.size())
      return true;
    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') {
      i = line.size();
      return true;
    }
    if (i + 1 >= line.size() || line[i] != '/' || line[i + 1] != '*')
      return false;
    size_t close = line.find("*/", i + 2);
    if (close == StringRef::npos)
      return false;
    i = close + 2;
  }
}

bool skipHashNumberLineMarkerFlags(StringRef line, size_t &i) {
  for (;;) {
    const size_t beforeWhitespace = i;
    if (!skipDirectiveTokenWhitespace(line, i))
      return false;
    if (i >= line.size())
      return true;
    if (!llvm::isDigit(line[i])) {
      i = beforeWhitespace;
      return skipTrailingIgnorable(line, i);
    }

    uint64_t ignored = 0;
    if (!parseUnsignedDecimal(line, i, ignored))
      return false;
  }
}

ParsedLineControl parseLineControl(StringRef line, const DirectiveInfo &info) {
  ParsedLineControl out;
  out.recognized = true;

  size_t i = info.payloadBegin;
  uint64_t parsedLine = 0;
  if (!parseUnsignedDecimal(line, i, parsedLine))
    return out;

  out.line = parsedLine;

  std::optional<std::string> parsedFile;
  size_t beforeOptionalFile = i;
  if (!skipDirectiveTokenWhitespace(line, i))
    return out;
  if (i < line.size() && line[i] == '"') {
    std::string file;
    if (!parseRawOrdinaryStringLiteral(line, i, file))
      return out;
    parsedFile = std::move(file);
  } else {
    i = beforeOptionalFile;
  }

  out.file = std::move(parsedFile);
  out.hashNumberForm = info.name.empty();
  if (out.hashNumberForm)
    out.semanticsKnown = skipHashNumberLineMarkerFlags(line, i);
  else
    out.semanticsKnown = skipTrailingIgnorable(line, i);
  return out;
}

FinalPhysicalLineKind physicalKindForDirective(DirectiveKind kind) {
  switch (kind) {
  case DirectiveKind::None:
    return FinalPhysicalLineKind::Unknown;
  case DirectiveKind::Other:
    return FinalPhysicalLineKind::OtherDirective;
  case DirectiveKind::ConditionalEnter:
  case DirectiveKind::ConditionalMiddle:
  case DirectiveKind::ConditionalExit:
    return FinalPhysicalLineKind::ConditionalDirective;
  case DirectiveKind::LineControl:
    return FinalPhysicalLineKind::LineDirective;
  }
  llvm_unreachable("invalid directive kind");
}

bool directiveMayChangeFile(const ParsedLineControl &parsed) {
  return !parsed.semanticsKnown || parsed.file.has_value();
}

std::optional<FinalObserver::Kind> observerKindForIdentifier(StringRef ident) {
  if (ident == "__LINE__")
    return FinalObserver::Kind::Line;
  if (ident == "__FILE__")
    return FinalObserver::Kind::File;
  if (ident == "__FILE_NAME__")
    return FinalObserver::Kind::FileName;
  return std::nullopt;
}

bool skipEscapedQuotedLiteral(StringRef line, size_t &i, char quote) {
  if (i >= line.size() || line[i] != quote)
    return false;

  ++i;
  bool escaped = false;
  while (i < line.size()) {
    const char c = line[i++];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == quote)
      return true;
  }

  return true;
}

bool skipOrdinaryStringOrCharLiteral(StringRef line, size_t &i) {
  static const StringRef Prefixes[] = {
      "u8\"", "u8'", "u\"", "u'", "U\"", "U'", "L\"", "L'", "\"", "'"};

  for (StringRef prefix : Prefixes) {
    if (!line.substr(i).starts_with(prefix))
      continue;
    size_t quote = i + prefix.size() - 1;
    const char quoteChar = line[quote];
    i = quote;
    return skipEscapedQuotedLiteral(line, i, quoteChar);
  }

  return false;
}

bool skipRawStringLiteral(StringRef line, size_t &i) {
  static const StringRef Prefixes[] = {"u8R\"", "uR\"", "UR\"",
                                      "LR\"", "R\""};

  for (StringRef prefix : Prefixes) {
    if (!line.substr(i).starts_with(prefix))
      continue;

    const size_t delimiterBegin = i + prefix.size();
    size_t openParen = delimiterBegin;
    while (openParen < line.size() && line[openParen] != '(') {
      const char c = line[openParen];
      if (stringutils::isWsNoLF(c) || c == ')' || c == '\\')
        return false;
      ++openParen;
    }
    if (openParen >= line.size()) {
      i = line.size();
      return true;
    }

    const std::string delimiter = line.slice(delimiterBegin, openParen).str();
    const std::string terminator = ")" + delimiter + "\"";
    const size_t close = line.find(terminator, openParen + 1);
    if (close == StringRef::npos) {
      i = line.size();
      return true;
    }

    i = close + terminator.size();
    return true;
  }

  return false;
}

bool skipStringOrCharLiteral(StringRef line, size_t &i) {
  const size_t before = i;
  if (skipRawStringLiteral(line, i))
    return true;
  i = before;
  return skipOrdinaryStringOrCharLiteral(line, i);
}

/// Record direct source-spelled location builtins in ordinary final source
/// lines.  Preprocessing directive lines are intentionally excluded: a builtin
/// in a macro replacement list observes line state at a later expansion site,
/// not where the `#define` text appears.  Those expansion-site observers need
/// producer/model evidence and are added by a later chunk.
void collectLexicalObserverCandidates(FinalLineControlModel &model,
                                      StringRef line,
                                      bool startsInBlockComment,
                                      uint64_t finalLineBegin,
                                      uint64_t physicalLineNo,
                                      uint32_t conditionalDepth,
                                      const FinalLogicalState &stateBefore) {
  bool inBlockComment = startsInBlockComment;
  size_t i = 0;
  while (i < line.size()) {
    if (inBlockComment) {
      const size_t close = line.find("*/", i);
      if (close == StringRef::npos)
        return;
      i = close + 2;
      inBlockComment = false;
      continue;
    }

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
      return;

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
      const size_t close = line.find("*/", i + 2);
      if (close == StringRef::npos)
        return;
      i = close + 2;
      continue;
    }

    if (skipStringOrCharLiteral(line, i))
      continue;

    if (!stringutils::isIdentStart(line[i])) {
      ++i;
      continue;
    }

    const size_t identBegin = i;
    ++i;
    while (i < line.size() && stringutils::isIdentPart(line[i]))
      ++i;

    const StringRef ident = line.slice(identBegin, i);
    std::optional<FinalObserver::Kind> kind = observerKindForIdentifier(ident);
    if (!kind)
      continue;

    FinalObserver observer;
    observer.finalOffset = finalLineBegin + identBegin;
    observer.finalEnd = finalLineBegin + i;
    observer.physicalLine = physicalLineNo;
    observer.kind = *kind;
    observer.activity = conditionalDepth == 0
                            ? FinalObserverActivity::KnownActive
                            : FinalObserverActivity::Unknown;
    observer.producerProven = false;
    observer.spellingPreserved = true;
    observer.stateBefore = stateBefore;
    model.AddObserver(std::move(observer));
  }
}


struct DirectiveLogicalStateRecord {
  bool present = false;
  FinalLogicalState stateBefore;
};

enum class ComponentReachKind : uint8_t {
  Reaches,
  DominatedByKnownDirective,
  BlockedByUnknownDirective,
};

struct ComponentReachResult {
  ComponentReachKind kind = ComponentReachKind::Reaches;
  std::optional<size_t> directiveIndex = std::nullopt;
};

FinalLineObserverComponent componentForObserverKind(FinalObserver::Kind kind) {
  switch (kind) {
  case FinalObserver::Kind::Line:
    return FinalLineObserverComponent::Line;
  case FinalObserver::Kind::File:
    return FinalLineObserverComponent::File;
  case FinalObserver::Kind::FileName:
    return FinalLineObserverComponent::FileName;
  }
  llvm_unreachable("invalid final observer kind");
}

StringRef finalBasename(StringRef file) {
  const size_t slash = file.find_last_of("/\\");
  if (slash == StringRef::npos)
    return file;
  return file.drop_front(slash + 1);
}

bool directiveMayOverwriteComponent(const FinalLineDirective &directive,
                                    FinalLineObserverComponent component) {
  if (directive.activity == FinalLineDirectiveActivity::KnownInactive)
    return false;

  if (!directive.semanticsKnown)
    return true;

  switch (component) {
  case FinalLineObserverComponent::Line:
    return directive.line.has_value();
  case FinalLineObserverComponent::File:
  case FinalLineObserverComponent::FileName:
    return directive.file.has_value();
  }
  llvm_unreachable("invalid final observer component");
}

bool directiveKnownActiveWithKnownSemantics(
    const FinalLineDirective &directive) {
  return directive.activity == FinalLineDirectiveActivity::KnownActive &&
         directive.semanticsKnown;
}

bool directiveAffectsComponentRelativeToRemoval(
    const FinalLineDirective &directive, const FinalLogicalState &stateBefore,
    FinalLineObserverComponent component) {
  switch (component) {
  case FinalLineObserverComponent::Line: {
    if (!directive.line)
      return false;
    if (!stateBefore.lineKnown)
      return true;

    // Final pruning removes the directive line itself, not merely the `#line`
    // operator while leaving an otherwise ordinary physical line behind.  The
    // first following line therefore inherits the state that existed before the
    // directive.  A `#line N` is observer-live only if it differs from that
    // removal state and reaches a preserved `__LINE__` observer.
    return *directive.line != stateBefore.logicalLine;
  }
  case FinalLineObserverComponent::File: {
    if (!directive.file)
      return false;
    if (!stateBefore.fileKnown)
      return true;
    return *directive.file != stateBefore.logicalFile;
  }
  case FinalLineObserverComponent::FileName: {
    if (!directive.file)
      return false;
    if (!stateBefore.fileKnown)
      return true;
    return finalBasename(*directive.file) != finalBasename(stateBefore.logicalFile);
  }
  }
  llvm_unreachable("invalid final observer component");
}

void markLive(FinalLineDirectiveObserverLiveness &live,
              FinalLineObserverComponent component) {
  switch (component) {
  case FinalLineObserverComponent::Line:
    live.lineLive = true;
    break;
  case FinalLineObserverComponent::File:
    live.fileLive = true;
    break;
  case FinalLineObserverComponent::FileName:
    live.fileNameLive = true;
    break;
  }
}

void markUnknownDependence(FinalLineDirectiveObserverLiveness &live,
                           FinalLineObserverComponent component) {
  switch (component) {
  case FinalLineObserverComponent::Line:
    live.lineUnknownDependence = true;
    break;
  case FinalLineObserverComponent::File:
    live.fileUnknownDependence = true;
    break;
  case FinalLineObserverComponent::FileName:
    live.fileNameUnknownDependence = true;
    break;
  }
}

ComponentReachResult componentReachFromDirectiveToObserver(
    ArrayRef<FinalLineDirective> directives, size_t directiveIndex,
    const FinalObserver &observer, FinalLineObserverComponent component) {
  for (size_t j = directiveIndex + 1; j < directives.size(); ++j) {
    const FinalLineDirective &later = directives[j];
    if (later.finalBegin >= observer.finalOffset)
      break;

    if (!directiveMayOverwriteComponent(later, component))
      continue;

    if (directiveKnownActiveWithKnownSemantics(later))
      return {ComponentReachKind::DominatedByKnownDirective, j};

    return {ComponentReachKind::BlockedByUnknownDirective, j};
  }

  return {ComponentReachKind::Reaches, std::nullopt};
}

std::vector<DirectiveLogicalStateRecord>
buildDirectiveLogicalStateRecords(const FinalLineControlModel &model) {
  std::vector<DirectiveLogicalStateRecord> records(model.Directives().size());
  for (const FinalPhysicalLine &line : model.PhysicalLines()) {
    if (!line.directiveIndex)
      continue;
    const size_t idx = *line.directiveIndex;
    if (idx >= records.size())
      continue;
    records[idx].present = true;
    records[idx].stateBefore = line.stateBefore;
  }
  return records;
}


struct FinalLayoutSegment {
  size_t beginLine = 0;
  size_t endLine = 0;
  std::optional<size_t> leftVisibleLine = std::nullopt;
  std::optional<size_t> rightVisibleLine = std::nullopt;
  std::vector<size_t> directiveIndices;
  bool hasNonLineLayoutMaterial = false;
  bool hasUnknownLayoutMaterial = false;
  bool hasBlankLine = false;
  bool hasCommentOnlyLine = false;
  bool hasDirectiveOnlyLine = false;
  bool hasUnknownLine = false;
  std::optional<size_t> firstNonLineMaterialLine = std::nullopt;
  std::optional<size_t> lastNonLineMaterialLine = std::nullopt;

  bool IsPrefixBeforeVisible() const {
    return !leftVisibleLine && rightVisibleLine.has_value();
  }

  bool IsGapBetweenVisibleLines() const {
    return leftVisibleLine.has_value() && rightVisibleLine.has_value();
  }

  bool HasRightVisibleTokenLine() const { return rightVisibleLine.has_value(); }

  bool HasBlankLineSensitiveMaterial() const {
    return hasBlankLine || hasCommentOnlyLine || hasDirectiveOnlyLine ||
           hasUnknownLine || hasUnknownLayoutMaterial;
  }

  bool HasOnlyKnownZeroTokenLayoutMaterial() const {
    return hasNonLineLayoutMaterial && !hasUnknownLayoutMaterial;
  }
};

bool finalLineProducesVisibleTokens(const FinalPhysicalLine &line) {
  return line.kind == FinalPhysicalLineKind::Ordinary;
}

bool finalLineIsLineControl(const FinalPhysicalLine &line) {
  return line.kind == FinalPhysicalLineKind::LineDirective &&
         line.directiveIndex.has_value();
}

enum class FinalLayoutMaterialClass : uint8_t {
  None,
  KnownZeroToken,
  UnknownEffect,
};

/// Return true for preprocessing directives whose directive line is known not
/// to contribute text to `clang -E -P` output.
///
/// This is intentionally small.  Directives such as `#include` and `#pragma`
/// can contribute output or depend on external state, and conditional
/// directives change which physical lines are active.  Those remain
/// UnknownEffect so layout pruning keeps surrounding #line directives
/// fail-closed rather than manufacturing a local layout proof.
bool directiveNameIsKnownZeroTokenForLayout(StringRef name) {
  return name == "define" || name == "undef";
}

FinalLayoutMaterialClass classifyFinalLineLayoutMaterial(
    const FinalPhysicalLine &line) {
  switch (line.kind) {
  case FinalPhysicalLineKind::Blank:
  case FinalPhysicalLineKind::CommentOnly:
    return FinalLayoutMaterialClass::KnownZeroToken;
  case FinalPhysicalLineKind::OtherDirective:
    return directiveNameIsKnownZeroTokenForLayout(line.directiveName)
               ? FinalLayoutMaterialClass::KnownZeroToken
               : FinalLayoutMaterialClass::UnknownEffect;
  case FinalPhysicalLineKind::ConditionalDirective:
  case FinalPhysicalLineKind::Unknown:
    return FinalLayoutMaterialClass::UnknownEffect;
  case FinalPhysicalLineKind::Ordinary:
  case FinalPhysicalLineKind::LineDirective:
    return FinalLayoutMaterialClass::None;
  }
  llvm_unreachable("invalid final physical line kind");
}

void noteNonLineLayoutMaterial(FinalLayoutSegment &segment,
                               const FinalPhysicalLine &line,
                               size_t physicalLineIndex) {
  const FinalLayoutMaterialClass materialClass =
      classifyFinalLineLayoutMaterial(line);
  if (materialClass == FinalLayoutMaterialClass::None)
    return;

  segment.hasNonLineLayoutMaterial = true;
  if (materialClass == FinalLayoutMaterialClass::UnknownEffect)
    segment.hasUnknownLayoutMaterial = true;

  if (!segment.firstNonLineMaterialLine)
    segment.firstNonLineMaterialLine = physicalLineIndex;
  segment.lastNonLineMaterialLine = physicalLineIndex;
  switch (line.kind) {
  case FinalPhysicalLineKind::Blank:
    segment.hasBlankLine = true;
    break;
  case FinalPhysicalLineKind::CommentOnly:
    segment.hasCommentOnlyLine = true;
    break;
  case FinalPhysicalLineKind::OtherDirective:
  case FinalPhysicalLineKind::ConditionalDirective:
    segment.hasDirectiveOnlyLine = true;
    break;
  case FinalPhysicalLineKind::Unknown:
    segment.hasUnknownLine = true;
    break;
  case FinalPhysicalLineKind::Ordinary:
  case FinalPhysicalLineKind::LineDirective:
    break;
  }
}

bool directiveKnownActiveWithKnownLayoutSemantics(
    const FinalLineDirective &directive) {
  return directive.activity == FinalLineDirectiveActivity::KnownActive &&
         directive.semanticsKnown;
}

std::vector<FinalLayoutSegment>
analyzeFinalLayoutSegments(const FinalLineControlModel &model) {
  std::vector<FinalLayoutSegment> segments;
  const ArrayRef<FinalPhysicalLine> lines = model.PhysicalLines();

  std::optional<size_t> previousVisibleLine = std::nullopt;
  size_t i = 0;
  while (i < lines.size()) {
    if (finalLineProducesVisibleTokens(lines[i])) {
      previousVisibleLine = i;
      ++i;
      continue;
    }

    FinalLayoutSegment segment;
    segment.beginLine = i;
    segment.leftVisibleLine = previousVisibleLine;

    while (i < lines.size() && !finalLineProducesVisibleTokens(lines[i])) {
      const FinalPhysicalLine &line = lines[i];
      if (finalLineIsLineControl(line))
        segment.directiveIndices.push_back(*line.directiveIndex);
      noteNonLineLayoutMaterial(segment, line, i);
      ++i;
    }

    segment.endLine = i;
    if (i < lines.size() && finalLineProducesVisibleTokens(lines[i]))
      segment.rightVisibleLine = i;

    if (!segment.directiveIndices.empty())
      segments.push_back(std::move(segment));
  }

  return segments;
}

std::optional<size_t> directivePhysicalLineInSegment(
    const FinalLayoutSegment &segment, ArrayRef<FinalPhysicalLine> lines,
    size_t directiveIndex) {
  for (size_t lineIndex = segment.beginLine; lineIndex < segment.endLine;
       ++lineIndex) {
    if (lineIndex >= lines.size())
      break;
    if (lines[lineIndex].directiveIndex &&
        *lines[lineIndex].directiveIndex == directiveIndex)
      return lineIndex;
  }
  return std::nullopt;
}

bool directiveCanSeparateLayoutMaterialFromVisibleLine(
    const FinalLayoutSegment &segment, ArrayRef<FinalPhysicalLine> lines,
    size_t directiveIndex) {
  if (!segment.HasRightVisibleTokenLine() || !segment.hasNonLineLayoutMaterial ||
      !segment.lastNonLineMaterialLine)
    return false;

  std::optional<size_t> directiveLine =
      directivePhysicalLineInSegment(segment, lines, directiveIndex);
  if (!directiveLine)
    return false;

  // A final-stream layout barrier must occur after the zero-token material it
  // is claimed to normalize and before the visible line whose -E -P placement
  // is being protected.  A #line that appears before a comment/directive-only
  // gap is not itself the final barrier for that gap; either a later directive
  // discharges the obligation or the gap remains fail-closed.
  return *segment.lastNonLineMaterialLine < *directiveLine &&
         (!segment.rightVisibleLine || *directiveLine < *segment.rightVisibleLine);
}

std::optional<size_t> canonicalKnownLayoutDirectiveForSegment(
    const FinalLayoutSegment &segment, ArrayRef<FinalPhysicalLine> lines,
    ArrayRef<FinalLineDirective> directives) {
  if (!segment.HasRightVisibleTokenLine() || !segment.hasNonLineLayoutMaterial ||
      !segment.lastNonLineMaterialLine)
    return std::nullopt;

  for (size_t reverse = segment.directiveIndices.size(); reverse > 0; --reverse) {
    const size_t directiveIndex = segment.directiveIndices[reverse - 1];
    if (directiveIndex >= directives.size())
      continue;
    if (!directiveCanSeparateLayoutMaterialFromVisibleLine(segment, lines,
                                                          directiveIndex))
      continue;
    if (directiveKnownActiveWithKnownLayoutSemantics(directives[directiveIndex]))
      return directiveIndex;
  }

  return std::nullopt;
}

void markLayoutLive(FinalLineDirectiveLayoutLiveness &live,
                    FinalLayoutObligation::Kind kind) {
  switch (kind) {
  case FinalLayoutObligation::Kind::ZeroTokenPrefixBarrier:
    live.zeroTokenPrefixBarrierLive = true;
    break;
  case FinalLayoutObligation::Kind::ZeroTokenGapBarrier:
    live.zeroTokenGapBarrierLive = true;
    break;
  case FinalLayoutObligation::Kind::FirstVisibleTokenAlignment:
    live.firstVisibleTokenAlignmentLive = true;
    break;
  case FinalLayoutObligation::Kind::BlankLinePreservation:
    live.blankLinePreservationLive = true;
    break;
  }
}


/// Return true iff \p line is locally known to produce no preprocessing tokens
/// in the final `clang -E -P` stream.  This helper is intentionally narrower
/// than the general layout classifier: it is used only to prove that a
/// synthetic resync directive appears immediately before a simple include and
/// that a later line directive repairs the parent stream before any source line
/// could observe the resync.
bool isLocallyZeroTokenFinalLineForPreIncludeResync(
    const FinalPhysicalLine &line) {
  if (line.kind == FinalPhysicalLineKind::Blank ||
      line.kind == FinalPhysicalLineKind::CommentOnly)
    return true;
  if (line.kind == FinalPhysicalLineKind::ConditionalDirective)
    return true;
  if (line.kind != FinalPhysicalLineKind::OtherDirective)
    return false;

  StringRef name(line.directiveName);
  return name == "define" || name == "undef";
}

/// Return true iff \p line is a simple literal include directive.  Macro-based
/// include operands are deliberately rejected: macro expansion in a directive
/// operand could itself observe the current logical line/file state, so a
/// pre-include resync may not be elided by this proof.
bool isSimpleLiteralIncludeDirectiveLine(const FinalPhysicalLine &line) {
  if (line.kind != FinalPhysicalLineKind::OtherDirective ||
      line.directiveName != "include")
    return false;

  StringRef text(line.rawText);
  text = text.ltrim(" \t\v\f\r");
  if (!text.consume_front("#"))
    return false;
  text = text.ltrim(" \t\v\f\r");
  if (!text.consume_front("include"))
    return false;
  if (!text.empty()) {
    const char next = text.front();
    if (std::isalnum(static_cast<unsigned char>(next)) || next == '_')
      return false;
  }
  text = text.ltrim(" \t\v\f\r");
  return text.starts_with("\"") || text.starts_with("<");
}

std::optional<size_t> findPhysicalLineForDirective(
    const FinalLineControlModel &model, size_t directiveIndex) {
  const ArrayRef<FinalPhysicalLine> lines = model.PhysicalLines();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].directiveIndex && *lines[i].directiveIndex == directiveIndex)
      return i;
  }
  return std::nullopt;
}

bool syntheticIncludeEntryStateIsDominatedBeforePreservedObservers(
    const FinalLineControlModel &model, size_t directiveIndex);

/// A local synthetic newline-resync immediately before a preserved `#include`
/// is often stale: the include pushes its child logical file/line state before
/// any child token can observe the parent resync, and a later parent-side
/// `#line` may repair the stream before the parent suffix observes it.
///
/// This recognizes only that proof shape:
///   synthetic #line
///   #include "literal.h"       or       #include <literal.h>
///   zero-token preprocessor material, or material guarded by the same
///     conditional island as the include arm
///   parent-side #line repair
///
/// The guarded-material allowance is what makes nested conditional/include
/// refolding work without retaining stale local resyncs.  A selected arm can
/// contain a preserved include and then rejoin through inactive sibling arms;
/// those sibling arms may contain ordinary source text in the final file even
/// though the candidate resync cannot reach them on the validated PP path.  We
/// therefore allow non-zero-token-looking physical lines only when the resync
/// itself is inside conditional structure and the intervening line is still
/// inside conditional structure.  The proof remains fail-closed for top-level
/// active source text and is still discharged only after executable validation
/// accepts the physical deletion.
///
/// Any preserved final observer before the repair keeps the directive
/// fail-closed, because such an observer could read the stale parent state.
bool syntheticNewlineResyncIsStaleBeforeSimpleInclude(
    const FinalLineControlModel &model, size_t directiveIndex) {
  const std::optional<size_t> directiveLineIndex =
      findPhysicalLineForDirective(model, directiveIndex);
  if (!directiveLineIndex)
    return false;

  const ArrayRef<FinalPhysicalLine> lines = model.PhysicalLines();
  const FinalPhysicalLine &directiveLine = lines[*directiveLineIndex];
  const bool directiveInsideConditional =
      directiveLine.conditionalDepthBefore != 0 ||
      directiveLine.conditionalDepthAfter != 0;

  size_t i = *directiveLineIndex + 1;
  while (i < lines.size() &&
         (lines[i].kind == FinalPhysicalLineKind::Blank ||
          lines[i].kind == FinalPhysicalLineKind::CommentOnly))
    ++i;
  if (i >= lines.size() || !isSimpleLiteralIncludeDirectiveLine(lines[i]))
    return false;

  const uint64_t guardedBegin = directiveLine.finalEnd;
  std::optional<uint64_t> repairBegin;
  for (++i; i < lines.size(); ++i) {
    const FinalPhysicalLine &line = lines[i];
    if (line.kind == FinalPhysicalLineKind::LineDirective) {
      repairBegin = line.finalBegin;
      break;
    }
    if (isLocallyZeroTokenFinalLineForPreIncludeResync(line))
      continue;

    const bool lineStillInsideConditional =
        line.conditionalDepthBefore != 0 || line.conditionalDepthAfter != 0;
    if (directiveInsideConditional && lineStillInsideConditional)
      continue;

    return false;
  }
  if (!repairBegin)
    return false;

  for (const FinalObserver &observer : model.Observers()) {
    if (!observer.spellingPreserved)
      continue;
    if (observer.activity == FinalObserverActivity::KnownInactive)
      continue;
    if (observer.finalOffset >= guardedBegin &&
        observer.finalOffset < *repairBegin)
      return false;
  }

  return true;
}


/// Return true iff a synthetic TU prologue is stale because zero-token TU
/// prefix material is followed by an optional literal include and then by a
/// line-control chain whose later known-active repair dominates every preserved
/// location observer.
///
/// This is the final-stream version of the same proof used for local
/// pre-include resyncs, but it is intentionally phrased as dominance rather
/// than adjacency:
///
///   synthetic TU prologue
///   blank/comment-only TU prefix material
///   optional simple literal #include
///   zero or more source-authored #line directives, possibly unknown locally
///   known-active #line repair for each component observed later
///   preserved __LINE__ / __FILE__ / __FILE_NAME__ observers
///
/// Blank and comment-only prefix lines cannot observe logical location state,
/// so they are not a reason to keep the prologue.  Other preprocessing
/// directives before the repair are rejected here instead of guessed about: a
/// `#define`, conditional directive, pragma, or macro-based include may have
/// source-state or layout interactions that require a separate proof.  Unknown
/// source-authored #line directives are allowed only as part of the line-control
/// chain, and the candidate is still physically deleted only after executable
/// clang -E -P validation accepts the final stream.
bool syntheticTUPrologueIsDominatedBeforePreservedObservers(
    const FinalLineControlModel &model, size_t directiveIndex) {
  const std::optional<size_t> directiveLineIndex =
      findPhysicalLineForDirective(model, directiveIndex);
  if (!directiveLineIndex)
    return false;

  const ArrayRef<FinalPhysicalLine> lines = model.PhysicalLines();
  const ArrayRef<FinalLineDirective> directives = model.Directives();
  const FinalPhysicalLine &directiveLine = lines[*directiveLineIndex];
  if (directiveLine.finalBegin != 0)
    return false;

  size_t i = *directiveLineIndex + 1;
  while (i < lines.size() &&
         (lines[i].kind == FinalPhysicalLineKind::Blank ||
          lines[i].kind == FinalPhysicalLineKind::CommentOnly))
    ++i;

  if (i < lines.size() && isSimpleLiteralIncludeDirectiveLine(lines[i]))
    ++i;

  bool foundKnownRepair = false;
  for (; i < lines.size(); ++i) {
    const FinalPhysicalLine &line = lines[i];
    if (line.kind != FinalPhysicalLineKind::LineDirective)
      return false;

    if (!line.directiveIndex || *line.directiveIndex >= directives.size())
      continue;

    if (directiveKnownActiveWithKnownSemantics(directives[*line.directiveIndex])) {
      foundKnownRepair = true;
      break;
    }
  }

  if (!foundKnownRepair)
    return false;

  return syntheticIncludeEntryStateIsDominatedBeforePreservedObservers(
      model, directiveIndex);
}


/// Return true iff every preserved line/file observer after a synthetic include
/// entry is dominated by a later known-active line-control directive before the
/// observer can read the include-entry state.
///
/// Unknown intervening line-control is not itself a proof of deadness, but it is
/// also not a proof that the include-entry state reaches the observer: a later
/// known-active directive for the observed component makes all earlier line
/// state irrelevant.  This captures header fragments such as:
///
///   #line 1 "header.h"          // synthetic include entry
///   ... zero/token material ...
///   #line MACRO_LINE MACRO_FILE  // semantics unknown to the final scanner
///   ... inserted material ...
///   #line 510 "logical.h"       // known repair
///   int value = __LINE__;
///
/// The executable final-prune validator still has to accept the physical
/// deletion.  This helper only proves that preserved observers do not require
/// the include-entry state.
bool syntheticIncludeEntryStateIsDominatedBeforePreservedObservers(
    const FinalLineControlModel &model, size_t directiveIndex) {
  const ArrayRef<FinalLineDirective> directives = model.Directives();
  if (directiveIndex >= directives.size())
    return false;

  const FinalLineDirective &candidate = directives[directiveIndex];
  for (const FinalObserver &observer : model.Observers()) {
    if (!observer.spellingPreserved)
      continue;
    if (observer.activity == FinalObserverActivity::KnownInactive)
      continue;
    if (observer.finalOffset < candidate.finalEnd)
      continue;

    const FinalLineObserverComponent component =
        componentForObserverKind(observer.kind);
    bool dominated = false;
    for (size_t j = directiveIndex + 1; j < directives.size(); ++j) {
      const FinalLineDirective &later = directives[j];
      if (later.finalBegin >= observer.finalOffset)
        break;

      if (!directiveMayOverwriteComponent(later, component))
        continue;

      if (directiveKnownActiveWithKnownSemantics(later)) {
        dominated = true;
        break;
      }

      // Keep scanning.  Unknown line-control can make the candidate state even
      // less likely to reach the observer, but only a later known-active repair
      // is allowed to discharge this proof.
    }

    if (!dominated)
      return false;
  }

  return true;
}

void collectFinalLayoutObligations(FinalLineControlModel &model) {
  const ArrayRef<FinalLineDirective> directives = model.Directives();
  const ArrayRef<FinalPhysicalLine> lines = model.PhysicalLines();
  const std::vector<FinalLayoutSegment> segments =
      analyzeFinalLayoutSegments(model);

  for (const FinalLayoutSegment &segment : segments) {
    if (!segment.HasRightVisibleTokenLine() || !segment.hasNonLineLayoutMaterial)
      continue;

    // Layout obligations are proof objects, so create them only when every
    // non-line material line in the segment is locally known to be zero-token
    // under `clang -E -P`.  Segments containing includes, pragmas, conditionals,
    // unknown directives, or unknown physical lines are handled fail-closed in
    // ComputeFinalLayoutLiveness instead of manufacturing a possibly false
    // layout proof.
    if (!segment.HasOnlyKnownZeroTokenLayoutMaterial())
      continue;

    const std::optional<size_t> directiveIndex =
        canonicalKnownLayoutDirectiveForSegment(segment, lines, directives);
    if (!directiveIndex)
      continue;

    const uint64_t obligationOffset = lines[*segment.rightVisibleLine].finalBegin;
    auto addObligation = [&](FinalLayoutObligation::Kind kind) {
      FinalLayoutObligation obligation;
      obligation.finalOffset = obligationOffset;
      obligation.kind = kind;
      obligation.directiveIndex = directiveIndex;
      model.AddLayoutObligation(std::move(obligation));
    };

    if (segment.IsPrefixBeforeVisible()) {
      addObligation(FinalLayoutObligation::Kind::ZeroTokenPrefixBarrier);
      addObligation(FinalLayoutObligation::Kind::FirstVisibleTokenAlignment);
    } else if (segment.IsGapBetweenVisibleLines()) {
      addObligation(FinalLayoutObligation::Kind::ZeroTokenGapBarrier);
    }

    if (segment.HasBlankLineSensitiveMaterial()) {
      addObligation(FinalLayoutObligation::Kind::BlankLinePreservation);
    }
  }
}

} // namespace

std::vector<FinalLineDirectiveObserverLiveness>
ComputeFinalObserverLiveness(const FinalLineControlModel &model) {
  std::vector<FinalLineDirectiveObserverLiveness> out;
  out.reserve(model.Directives().size());

  const ArrayRef<FinalLineDirective> directives = model.Directives();
  const ArrayRef<FinalObserver> observers = model.Observers();
  const std::vector<DirectiveLogicalStateRecord> directiveStates =
      buildDirectiveLogicalStateRecords(model);

  for (size_t i = 0; i < directives.size(); ++i) {
    const FinalLineDirective &directive = directives[i];

    FinalLineDirectiveObserverLiveness live;
    live.directiveIndex = i;
    live.finalBegin = directive.finalBegin;
    live.finalEnd = directive.finalEnd;


    if (directive.activity == FinalLineDirectiveActivity::KnownInactive) {
      markUnknownDependence(live, FinalLineObserverComponent::Line);
      out.push_back(std::move(live));
      continue;
    }

    if (directive.activity != FinalLineDirectiveActivity::KnownActive) {
      markUnknownDependence(live, FinalLineObserverComponent::Line);
      markUnknownDependence(live, FinalLineObserverComponent::File);
      markUnknownDependence(live, FinalLineObserverComponent::FileName);
      out.push_back(std::move(live));
      continue;
    }

    if (!directive.semanticsKnown) {
      markUnknownDependence(live, FinalLineObserverComponent::Line);
      markUnknownDependence(live, FinalLineObserverComponent::File);
      markUnknownDependence(live, FinalLineObserverComponent::FileName);
      out.push_back(std::move(live));
      continue;
    }

    if (i >= directiveStates.size() || !directiveStates[i].present) {
      markUnknownDependence(live, FinalLineObserverComponent::Line);
      markUnknownDependence(live, FinalLineObserverComponent::File);
      markUnknownDependence(live, FinalLineObserverComponent::FileName);
      out.push_back(std::move(live));
      continue;
    }

    const FinalLogicalState &stateBefore = directiveStates[i].stateBefore;
    const bool affectsLine = directiveAffectsComponentRelativeToRemoval(
        directive, stateBefore, FinalLineObserverComponent::Line);
    const bool affectsFile = directiveAffectsComponentRelativeToRemoval(
        directive, stateBefore, FinalLineObserverComponent::File);
    const bool affectsFileName = directiveAffectsComponentRelativeToRemoval(
        directive, stateBefore, FinalLineObserverComponent::FileName);

    if (!affectsLine && !affectsFile && !affectsFileName) {
      live.observerDead = true;
      live.removableIfLayoutDead = true;
      out.push_back(std::move(live));
      continue;
    }

    auto componentAffected = [&](FinalLineObserverComponent component) -> bool {
      switch (component) {
      case FinalLineObserverComponent::Line:
        return affectsLine;
      case FinalLineObserverComponent::File:
        return affectsFile;
      case FinalLineObserverComponent::FileName:
        return affectsFileName;
      }
      llvm_unreachable("invalid final observer component");
    };

    for (const FinalObserver &observer : observers) {
      if (!observer.spellingPreserved)
        continue;
      if (observer.activity == FinalObserverActivity::KnownInactive)
        continue;
      if (observer.finalOffset < directive.finalEnd)
        continue;

      const FinalLineObserverComponent component =
          componentForObserverKind(observer.kind);
      if (!componentAffected(component))
        continue;

      const ComponentReachResult reach =
          componentReachFromDirectiveToObserver(directives, i, observer,
                                                component);
      switch (reach.kind) {
      case ComponentReachKind::Reaches:
        if (observer.activity == FinalObserverActivity::KnownActive) {
          markLive(live, component);
        } else {
          markUnknownDependence(live, component);
        }
        break;
      case ComponentReachKind::DominatedByKnownDirective:
        break;
      case ComponentReachKind::BlockedByUnknownDirective:
        markUnknownDependence(live, component);
        break;
      }
    }

    live.observerDead = !live.HasObserverLiveComponent() &&
                        !live.HasUnknownDependence();
    live.removableIfLayoutDead = live.observerDead;
    out.push_back(std::move(live));
  }

  return out;
}

std::vector<FinalLineDirectiveLayoutLiveness>
ComputeFinalLayoutLiveness(const FinalLineControlModel &model) {
  std::vector<FinalLineDirectiveLayoutLiveness> out;
  out.reserve(model.Directives().size());

  const ArrayRef<FinalLineDirective> directives = model.Directives();
  for (size_t i = 0; i < directives.size(); ++i) {
    FinalLineDirectiveLayoutLiveness live;
    live.directiveIndex = i;
    live.finalBegin = directives[i].finalBegin;
    live.finalEnd = directives[i].finalEnd;
    out.push_back(std::move(live));
  }

  std::vector<bool> hasPhysicalLineRecord(directives.size(), false);
  for (const FinalPhysicalLine &line : model.PhysicalLines()) {
    if (line.directiveIndex && *line.directiveIndex < hasPhysicalLineRecord.size())
      hasPhysicalLineRecord[*line.directiveIndex] = true;
  }

  for (size_t i = 0; i < directives.size(); ++i) {
    if (!hasPhysicalLineRecord[i])
      out[i].layoutUnknownDependence = true;
  }

  const std::vector<FinalLayoutSegment> segments =
      analyzeFinalLayoutSegments(model);
  for (const FinalLayoutSegment &segment : segments) {
    const bool layoutSensitive = segment.HasRightVisibleTokenLine() &&
                                 segment.hasNonLineLayoutMaterial;
    const std::optional<size_t> canonicalDirective =
        canonicalKnownLayoutDirectiveForSegment(segment, model.PhysicalLines(),
                                               directives);

    for (size_t directiveIndex : segment.directiveIndices) {
      if (directiveIndex >= out.size())
        continue;

      FinalLineDirectiveLayoutLiveness &live = out[directiveIndex];
      const FinalLineDirective &directive = directives[directiveIndex];

      if (!layoutSensitive) {
        if (!segment.HasRightVisibleTokenLine() &&
            segment.hasUnknownLayoutMaterial)
          live.layoutUnknownDependence = true;
        continue;
      }

      const bool canSeparateLayoutMaterial =
          directiveCanSeparateLayoutMaterialFromVisibleLine(
              segment, model.PhysicalLines(), directiveIndex);

      if (segment.hasUnknownLayoutMaterial) {
        live.layoutUnknownDependence = true;
        continue;
      }

      if (!canSeparateLayoutMaterial) {
        continue;
      }

      if (!directiveKnownActiveWithKnownLayoutSemantics(directive)) {
        live.layoutUnknownDependence = true;
        continue;
      }

      if (canonicalDirective && *canonicalDirective == directiveIndex)
        continue;

      if (!canonicalDirective) {
        // With only known zero-token material, the only layout-live directive in
        // the segment would be a known active directive after the last material
        // line.  Since this directive has that shape and no canonical directive
        // exists, keep the case fail-closed rather than silently claiming a
        // malformed scanner state is layout-dead.
        live.layoutUnknownDependence = true;
      }
    }
  }

  for (const FinalLayoutObligation &obligation : model.LayoutObligations()) {
    if (!obligation.directiveIndex || *obligation.directiveIndex >= out.size())
      continue;

    FinalLineDirectiveLayoutLiveness &live = out[*obligation.directiveIndex];
    markLayoutLive(live, obligation.kind);
  }

  for (FinalLineDirectiveLayoutLiveness &live : out) {
    live.layoutDead = !live.HasLayoutLiveComponent() &&
                      !live.layoutUnknownDependence;
    live.removableIfObserverDead = live.layoutDead;
  }

  return out;
}

FinalLineControlModel CollectFinalLineControlModel(
    StringRef finalSource,
    ArrayRef<FinalLineControlPruneCandidate> removableCandidates,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings,
    ArrayRef<FinalLineControlProducerEvent> producerEvents,
    ArrayRef<FinalLineControlProducerObserver> producerObservers) {
  std::vector<FinalLineControlPruneCandidate> canonicalCandidates(
      removableCandidates.begin(), removableCandidates.end());
  CanonicalizeFinalLineControlPruneCandidates(canonicalCandidates);

  std::vector<FinalLineControlSourceMapping> canonicalSourceMappings(
      sourceMappings.begin(), sourceMappings.end());
  CanonicalizeFinalLineControlSourceMappings(canonicalSourceMappings);

  FinalLineControlModel model;
  for (const FinalLineControlSourceMapping &mapping : canonicalSourceMappings)
    model.AddSourceMapping(mapping);
  FinalLogicalState state = FinalLogicalState::Initial();

  bool inBlockComment = false;
  bool inDirectiveContinuation = false;
  std::string continuedDirectiveName;
  uint32_t conditionalDepth = 0;
  bool unresolvedConditionalLineControl = false;
  bool unresolvedConditionalFileControl = false;

  size_t lineBegin = 0;
  uint64_t physicalLineNo = 1;
  while (lineBegin < finalSource.size()) {
    size_t lineEndNoNl = finalSource.find('\n', lineBegin);
    const bool hasNewline = lineEndNoNl != StringRef::npos;
    if (!hasNewline)
      lineEndNoNl = finalSource.size();

    const size_t lineEnd = hasNewline ? lineEndNoNl + 1 : lineEndNoNl;
    const StringRef physicalLine = finalSource.slice(lineBegin, lineEndNoNl);

    FinalPhysicalLine lineRecord;
    lineRecord.finalBegin = lineBegin;
    lineRecord.finalEnd = lineEnd;
    lineRecord.physicalLine = physicalLineNo;
    lineRecord.conditionalDepthBefore = conditionalDepth;
    lineRecord.stateBefore = state;
    lineRecord.rawText = finalSource.slice(lineBegin, lineEnd).str();

    size_t hash = 0;
    const bool lineStartsInBlockComment = inBlockComment;
    const bool lineIsDirectiveContinuation = inDirectiveContinuation;
    bool hasDirective = false;
    DirectiveInfo directive;
    ParsedLineControl parsedLineControl;
    if (lineIsDirectiveContinuation) {
      // This physical line is part of a preceding preprocessing directive after
      // phase-2 line splicing.  It inherits that directive's identifier for
      // layout classification, so a multi-line #define remains known zero-token
      // material while continuations of #include/#pragma remain fail-closed.
      lineRecord.kind = FinalPhysicalLineKind::OtherDirective;
      lineRecord.directiveName = continuedDirectiveName;
    } else {
      hasDirective = locateDirectiveHash(physicalLine, inBlockComment, hash);
      if (hasDirective) {
        directive = classifyDirective(physicalLine, hash);
        lineRecord.kind = physicalKindForDirective(directive.kind);
        lineRecord.directiveName = directive.name.str();
        if (directive.kind == DirectiveKind::LineControl)
          parsedLineControl = parseLineControl(physicalLine, directive);
      } else {
        lineRecord.kind =
            classifyNonDirectiveLine(physicalLine, lineStartsInBlockComment);
      }
    }

    if (lineRecord.kind == FinalPhysicalLineKind::Ordinary) {
      collectLexicalObserverCandidates(model, physicalLine,
                                       lineStartsInBlockComment, lineBegin,
                                       physicalLineNo, conditionalDepth,
                                       lineRecord.stateBefore);
    }

    collectProducerObserverCandidatesForLine(
        model, finalSource, lineBegin, lineEnd, physicalLineNo,
        lineRecord.stateBefore, canonicalSourceMappings, producerObservers);

    if (directive.kind == DirectiveKind::LineControl) {
      const StringRef lineControlText = finalSource.slice(lineBegin, lineEnd);
      FinalLineDirective lineDirective(
          lineBegin, lineEnd, FinalLineDirective::Origin::Unknown);
      lineDirective.line = parsedLineControl.line;
      lineDirective.file = parsedLineControl.file;
      lineDirective.semanticsKnown = parsedLineControl.semanticsKnown;
      lineDirective.semanticSource = parsedLineControl.semanticsKnown
                                         ? FinalLineDirectiveSemanticSource::LiteralFinalSource
                                         : FinalLineDirectiveSemanticSource::Unknown;
      lineDirective.activity = conditionalDepth == 0
                                   ? FinalLineDirectiveActivity::KnownActive
                                   : FinalLineDirectiveActivity::Unknown;

      applyMatchingPruneCandidate(lineDirective, canonicalCandidates);

      if (std::optional<FinalLineControlSourceMapping> sourceMapping =
              mapFinalDirectiveToSource(lineBegin, lineEnd, canonicalSourceMappings)) {
        if (const FinalLineControlProducerEvent *event =
                findProducerLineControlEvent(*sourceMapping, lineControlText,
                                             parsedLineControl, producerEvents)) {
          applyProducerLineControlEvent(lineDirective, *sourceMapping, *event);
          markProducerProvenSourceLineControlPreserved(lineDirective,
                                                       *sourceMapping, *event);
        }
      }

      const bool knownActiveProducerDirective =
          lineDirective.activity == FinalLineDirectiveActivity::KnownActive &&
          lineDirective.semanticsKnown && lineDirective.line.has_value();
      const size_t directiveIndex = model.AddDirective(std::move(lineDirective));
      lineRecord.directiveIndex = directiveIndex;

      const FinalLineDirective &recordedDirective =
          model.Directives()[directiveIndex];
      if (conditionalDepth == 0 || knownActiveProducerDirective) {
        if (recordedDirective.semanticsKnown && recordedDirective.line) {
          state.ApplyKnownLineDirective(*recordedDirective.line,
                                        recordedDirective.file);
        } else {
          state.MarkUnknown();
        }
      } else {
        unresolvedConditionalLineControl = true;
        if (directiveMayChangeFile(parsedLineControl))
          unresolvedConditionalFileControl = true;
        state.AdvancePhysicalLine();
      }
    } else {
      state.AdvancePhysicalLine();
    }

    switch (directive.kind) {
    case DirectiveKind::ConditionalEnter:
      ++conditionalDepth;
      break;
    case DirectiveKind::ConditionalMiddle:
      break;
    case DirectiveKind::ConditionalExit:
      if (conditionalDepth > 0)
        --conditionalDepth;
      if (conditionalDepth == 0) {
        if (unresolvedConditionalLineControl)
          state.MarkLineUnknown();
        if (unresolvedConditionalFileControl)
          state.MarkFileUnknown();
        unresolvedConditionalLineControl = false;
        unresolvedConditionalFileControl = false;
      }
      break;
    case DirectiveKind::None:
    case DirectiveKind::Other:
    case DirectiveKind::LineControl:
      break;
    }

    const bool continuesDirective =
        (lineIsDirectiveContinuation || hasDirective) &&
        physicalLineContinuesWithBackslash(physicalLine);
    if (continuesDirective) {
      if (hasDirective)
        continuedDirectiveName = directive.name.str();
    } else {
      continuedDirectiveName.clear();
    }
    inDirectiveContinuation = continuesDirective;

    lineRecord.conditionalDepthAfter = conditionalDepth;
    model.AddPhysicalLine(std::move(lineRecord));

    if (!hasNewline)
      break;
    lineBegin = lineEnd;
    ++physicalLineNo;
  }

  collectFinalLayoutObligations(model);
  return model;
}


FinalLineControlPruneResult
PruneFinalLineControlDirectives(
    StringRef finalSource,
    ArrayRef<FinalLineControlPruneCandidate> removableCandidates,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings,
    ArrayRef<FinalLineControlProducerEvent> producerEvents,
    ArrayRef<FinalLineControlProducerObserver> producerObservers,
    FinalLineControlValidationCallback validationCallback) {
  FinalLineControlPruneResult result;
  std::string current = finalSource.str();
  std::vector<FinalLineControlPruneCandidate> currentCandidates(
      removableCandidates.begin(), removableCandidates.end());
  CanonicalizeFinalLineControlPruneCandidates(currentCandidates);
  std::vector<FinalLineControlSourceMapping> currentSourceMappings(
      sourceMappings.begin(), sourceMappings.end());
  CanonicalizeFinalLineControlSourceMappings(currentSourceMappings);

  for (;;) {
    FinalLineControlModel model =
        CollectFinalLineControlModel(current, currentCandidates,
                                     currentSourceMappings, producerEvents,
                                     producerObservers);
    const ArrayRef<FinalLineDirective> directives = model.Directives();
    const std::vector<FinalLineDirectiveObserverLiveness> observerLiveness =
        ComputeFinalObserverLiveness(model);
    const std::vector<FinalLineDirectiveLayoutLiveness> layoutLiveness =
        ComputeFinalLayoutLiveness(model);

    bool removedThisIteration = false;

    for (size_t i = 0; i < directives.size(); ++i) {
      const FinalLineDirective &directive = directives[i];
      const bool validRange = directive.finalBegin < directive.finalEnd &&
                              directive.finalEnd <= current.size();

      auto isValidationDischargeableSyntheticIncludeEntry =
          [](FinalLineDirective::Origin origin) {
            switch (origin) {
            case FinalLineDirective::Origin::SyntheticIncludeEntry:
              return true;
            case FinalLineDirective::Origin::PreservedSource:
            case FinalLineDirective::Origin::SyntheticIncludeReturn:
            case FinalLineDirective::Origin::SyntheticNewlineResync:
            case FinalLineDirective::Origin::SyntheticSourceLineResume:
            case FinalLineDirective::Origin::SyntheticTUPrologue:
            case FinalLineDirective::Origin::SyntheticLayoutBarrier:
            case FinalLineDirective::Origin::Unknown:
              return false;
            }
            llvm_unreachable("invalid final line directive origin");
          };

      const bool hasObserverRecord = i < observerLiveness.size();
      const bool hasLayoutRecord = i < layoutLiveness.size();
      const bool hasConcreteObserverLiveComponent =
          hasObserverRecord && observerLiveness[i].HasObserverLiveComponent();
      const bool hasConcreteLayoutLiveComponent =
          hasLayoutRecord && layoutLiveness[i].HasLayoutLiveComponent();
      const bool includeEntryObserversDominated =
          isValidationDischargeableSyntheticIncludeEntry(directive.origin) &&
          syntheticIncludeEntryStateIsDominatedBeforePreservedObservers(model,
                                                                       i);

      // Synthetic include-entry line directives are conservative wrappers for
      // materialized header text.  They can be stale in two general shapes:
      //
      //   * no preserved line/file observer exists in the materialized region;
      //   * every later preserved observer is dominated by a subsequent
      //     known-active line-control repair before the observer is reached.
      //
      // Layout liveness is intentionally not a hard blocker for this narrow
      // synthetic-origin path: the executable final-prune validator must still
      // prove that deleting the directive preserves the preprocessed token
      // stream.  Source-authored directives and return/resync/TU-prologue
      // repairs still require the ordinary model proof before validation is
      // allowed to delete them.
      const bool validationMayDischargeSyntheticIncludeEntry =
          directive.removable &&
          isValidationDischargeableSyntheticIncludeEntry(directive.origin) &&
          directive.semanticsKnown && validRange && hasObserverRecord &&
          hasLayoutRecord && !hasConcreteObserverLiveComponent &&
          includeEntryObserversDominated;

      const bool validationMayDischargePreIncludeNewlineResync =
          directive.removable &&
          directive.origin == FinalLineDirective::Origin::SyntheticNewlineResync &&
          directive.semanticsKnown && validRange && hasObserverRecord &&
          hasLayoutRecord && !hasConcreteObserverLiveComponent &&
          !hasConcreteLayoutLiveComponent &&
          syntheticNewlineResyncIsStaleBeforeSimpleInclude(model, i);

      const bool validationMayDischargeTUPrologueDominatedByRepair =
          directive.removable &&
          directive.origin == FinalLineDirective::Origin::SyntheticTUPrologue &&
          directive.semanticsKnown && validRange && hasObserverRecord &&
          hasLayoutRecord && !hasConcreteObserverLiveComponent &&
          syntheticTUPrologueIsDominatedBeforePreservedObservers(model, i);

      const bool modelProvedCanRemove =
          directive.removable &&
          directive.activity == FinalLineDirectiveActivity::KnownActive &&
          directive.semanticsKnown && hasObserverRecord &&
          hasLayoutRecord && observerLiveness[i].observerDead &&
          layoutLiveness[i].layoutDead && validRange;
      const bool canRemove =
          modelProvedCanRemove || validationMayDischargeSyntheticIncludeEntry ||
          validationMayDischargePreIncludeNewlineResync ||
          validationMayDischargeTUPrologueDominatedByRepair;

      if (!canRemove)
        continue;

      const uint64_t removedBegin = directive.finalBegin;
      const uint64_t removedEnd = directive.finalEnd;
      const uint64_t removedSize = removedEnd - removedBegin;

      std::string candidateOutput = current;
      candidateOutput.erase(static_cast<size_t>(removedBegin),
                            static_cast<size_t>(removedSize));

      if (!validationCallback)
        continue;

      std::string validationReason;
      if (!validationCallback(current, candidateOutput, validationReason))
        continue;

      result.removedRanges.push_back({removedBegin, removedEnd});
      current = std::move(candidateOutput);

      std::vector<FinalLineControlPruneCandidate> adjustedCandidates;
      adjustedCandidates.reserve(currentCandidates.size());
      for (FinalLineControlPruneCandidate candidate : currentCandidates) {
        if (candidate.finalBegin == removedBegin && candidate.finalEnd == removedEnd)
          continue;

        if (candidate.finalEnd <= removedBegin) {
          adjustedCandidates.push_back(std::move(candidate));
          continue;
        }

        if (candidate.finalBegin >= removedEnd) {
          candidate.finalBegin -= removedSize;
          candidate.finalEnd -= removedSize;
          adjustedCandidates.push_back(std::move(candidate));
          continue;
        }

        // A candidate overlapping a removed directive but not exactly equal to
        // it no longer has a stable final-stream byte range.  Drop that
        // candidate so the next iteration fails closed rather than pruning a
        // shifted or partially removed directive by guesswork.
      }
      currentCandidates = std::move(adjustedCandidates);
      CanonicalizeFinalLineControlPruneCandidates(currentCandidates);

      AdjustFinalLineControlSourceMappingsAfterDeletion(currentSourceMappings,
                                                        removedBegin,
                                                        removedEnd);
      result.changed = true;
      removedThisIteration = true;
      break;
    }

    ++result.iterations;
    if (!removedThisIteration)
      break;
  }

  result.output = std::move(current);
  return result;
}

} // namespace refold
} // namespace clang
