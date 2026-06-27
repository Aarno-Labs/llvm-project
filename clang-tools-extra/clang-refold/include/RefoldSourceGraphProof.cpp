//===--- RefoldSourceGraphProof.cpp ---------------------------*- C++ -*-===//
//
// Source-graph preservation proof helpers for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "include/RefoldSourceGraphProof.h"
#include "include/IncludeSpellingHelpers.h"
#include "util/StringUtils.h"

#include <cctype>
#include <cstddef>

namespace clang {
namespace refold {
namespace source_graph {

namespace {

static bool isDirectiveHorizontalWhitespace(char c) {
  return c == '\r' || stringutils::isNonNewlineWs(c);
}

static bool consumeDirectiveScannerNewline(llvm::StringRef text,
                                           size_t &pos) {
  if (pos >= text.size())
    return false;
  if (text[pos] == '\r') {
    ++pos;
    if (pos < text.size() && text[pos] == '\n')
      ++pos;
    return true;
  }
  if (text[pos] == '\n') {
    ++pos;
    return true;
  }
  return false;
}

static bool consumeDirectiveScannerEscapedNewline(llvm::StringRef text,
                                                  size_t &pos) {
  if (pos >= text.size() || text[pos] != '\\')
    return false;

  size_t afterBackslash = pos + 1;
  while (afterBackslash < text.size() &&
         isDirectiveHorizontalWhitespace(text[afterBackslash]))
    ++afterBackslash;
  if (!consumeDirectiveScannerNewline(text, afterBackslash))
    return false;

  // Escaped-newline deletion removes the backslash-newline pair before
  // directive recognition.  Clang also accepts horizontal whitespace before the
  // newline as an extension, so the directive scanner consumes the same spelling
  // that source-range extension treats as a splice.
  pos = afterBackslash;
  return true;
}

/// Return the byte offset of the next preprocessing directive introducer.
///
/// This is intentionally a tiny preprocessing-aware scanner rather than a raw
/// physical-line test.  Clang recognizes directives after deleting escaped
/// newlines and after replacing comments with whitespace.  In particular,
/// `#\\\ninclude` is `#include`, and a complete block comment may span a
/// physical newline before the `#` that starts the directive.
///
/// Line comments are different: after escaped-newline deletion they consume the
/// rest of the logical line, so a `#` inside `// ...` is not a directive
/// introducer.  An unterminated block comment simply prevents later bytes from
/// being observed as directives by this scanner; callers that need stronger
/// recovery already fail closed at the proof site.
static std::optional<size_t>
findPreprocessingDirectiveIntroducer(llvm::StringRef text, size_t start = 0) {
  bool onlyTriviaOnLogicalLine = true;
  bool inBlockComment = false;
  bool blockCommentStartedInDirectivePrefix = false;
  size_t pos = start;

  while (pos < text.size()) {
    if (consumeDirectiveScannerEscapedNewline(text, pos))
      continue;

    if (inBlockComment) {
      if (pos + 1 < text.size() && text[pos] == '*' && text[pos + 1] == '/') {
        // A complete block comment is preprocessing whitespace.  Once the
        // terminator is consumed, directive-prefix scanning must resume in the
        // surrounding logical line; otherwise a real directive such as
        // `/*\n*/#include` is hidden from macro-state and include-replay proofs.
        pos += 2;
        inBlockComment = false;
        continue;
      }
      if (consumeDirectiveScannerNewline(text, pos)) {
        // A newline inside a leading block comment still leaves the eventual
        // comment replacement in directive-prefix trivia.  A newline inside a
        // block comment that began after real source code does not retroactively
        // make the following bytes directive-prefix trivia.
        onlyTriviaOnLogicalLine = blockCommentStartedInDirectivePrefix;
        continue;
      }
      ++pos;
      continue;
    }

    if (pos + 1 < text.size() && text[pos] == '/' && text[pos + 1] == '*') {
      blockCommentStartedInDirectivePrefix = onlyTriviaOnLogicalLine;
      pos += 2;
      inBlockComment = true;
      continue;
    }

    if (pos + 1 < text.size() && text[pos] == '/' && text[pos + 1] == '/') {
      pos += 2;
      while (pos < text.size()) {
        if (consumeDirectiveScannerEscapedNewline(text, pos))
          continue;
        if (consumeDirectiveScannerNewline(text, pos))
          break;
        ++pos;
      }
      onlyTriviaOnLogicalLine = true;
      continue;
    }

    if (consumeDirectiveScannerNewline(text, pos)) {
      onlyTriviaOnLogicalLine = true;
      continue;
    }

    if (!onlyTriviaOnLogicalLine) {
      ++pos;
      continue;
    }

    if (isDirectiveHorizontalWhitespace(text[pos])) {
      ++pos;
      continue;
    }

    if (text[pos] == '#')
      return pos;

    onlyTriviaOnLogicalLine = false;
    ++pos;
  }

  return std::nullopt;
}

/// Skip preprocessing whitespace after a directive introducer or keyword.
/// Escaped newlines are ignored because preprocessing removes them before
/// directive recognition.  Complete block comments are skipped as whitespace,
/// including comments that span physical lines; Clang accepts constructs such
/// as `#/*\n*/include` for the same reason.  Line comments are not skipped
/// because they terminate the directive logical line.
static bool skipDirectiveLogicalWhitespace(llvm::StringRef text, size_t &pos) {
  while (pos < text.size()) {
    if (isDirectiveHorizontalWhitespace(text[pos])) {
      ++pos;
      continue;
    }
    if (consumeDirectiveScannerEscapedNewline(text, pos))
      continue;
    if (pos + 1 < text.size() && text[pos] == '/' && text[pos + 1] == '*') {
      pos += 2;
      while (pos + 1 < text.size() &&
             !(text[pos] == '*' && text[pos + 1] == '/')) {
        if (consumeDirectiveScannerEscapedNewline(text, pos))
          continue;
        ++pos;
      }
      if (pos + 1 >= text.size())
        return false;
      pos += 2;
      continue;
    }
    break;
  }
  return true;
}

static bool readDirectiveIdentifier(llvm::StringRef text, size_t &pos,
                                    std::string &identifier) {
  identifier.clear();
  while (pos < text.size()) {
    if (consumeDirectiveScannerEscapedNewline(text, pos))
      continue;
    const unsigned char c = static_cast<unsigned char>(text[pos]);
    if (std::isalpha(c) || text[pos] == '_')
      break;
    return false;
  }

  while (pos < text.size()) {
    if (consumeDirectiveScannerEscapedNewline(text, pos))
      continue;
    const unsigned char c = static_cast<unsigned char>(text[pos]);
    if (!(std::isalnum(c) || text[pos] == '_'))
      break;
    identifier.push_back(text[pos]);
    ++pos;
  }

  return !identifier.empty();
}

} // namespace

std::optional<std::string>
safeSourceGraphRelativeIncludePath(const RefoldModel::IncludeItem &include) {
  if (include.angled)
    return std::nullopt;

  llvm::StringRef Target = include.target;
  if (Target.size() < 2 || Target.front() != '"' || Target.back() != '"')
    return std::nullopt;

  llvm::StringRef Path = Target.drop_front().drop_back();
  if (!safeSynthesizedRelativeIncludeOperandPath(Path))
    return std::nullopt;

  return Path.str();
}

bool lineHasPreprocessingDirectiveIntroducer(llvm::StringRef text) {
  return findPreprocessingDirectiveIntroducer(text).has_value();
}

MaterializedIncludeReplayAlias classifyMaterializedIncludeReplayAlias(
    llvm::StringRef materializedText, llvm::StringRef sourceGraphPath) {
  size_t searchPos = 0;
  while (std::optional<size_t> hash =
             findPreprocessingDirectiveIntroducer(materializedText, searchPos)) {
    size_t pos = *hash + 1;
    searchPos = pos;

    if (!skipDirectiveLogicalWhitespace(materializedText, pos))
      return MaterializedIncludeReplayAlias::UnprovenInclude;

    std::string keyword;
    if (!readDirectiveIdentifier(materializedText, pos, keyword))
      continue;
    if (keyword != "include")
      continue;

    if (!skipDirectiveLogicalWhitespace(materializedText, pos))
      return MaterializedIncludeReplayAlias::UnprovenInclude;

    if (pos >= materializedText.size() || materializedText[pos] != '"')
      return MaterializedIncludeReplayAlias::UnprovenInclude;

    const size_t pathBegin = ++pos;
    while (pos < materializedText.size() && materializedText[pos] != '"') {
      // Keep this classifier conservative.  A quoted include whose operand
      // itself uses a splice or reaches an unescaped newline is not a simple
      // path-level equality proof, so source-graph replay must fail closed.
      if (materializedText[pos] == '\\' || materializedText[pos] == '\n' ||
          materializedText[pos] == '\r')
        return MaterializedIncludeReplayAlias::UnprovenInclude;
      ++pos;
    }
    if (pos >= materializedText.size())
      return MaterializedIncludeReplayAlias::UnprovenInclude;

    if (materializedText.slice(pathBegin, pos) == sourceGraphPath)
      return MaterializedIncludeReplayAlias::SamePath;

    searchPos = pos + 1;
  }

  return MaterializedIncludeReplayAlias::None;
}

bool sourceGraphIncludePathIsUnaliasedOrCoherent(
    const SourceGraphProofInputs &inputs,
    const RefoldModel::IncludeItem &include,
    llvm::StringRef sourceGraphPath, llvm::StringRef candidateBytes,
    const SourceGraphProofServices &services) {
  // A source-graph output written under an original quoted include path is a
  // path-level edit, not an include-site-local edit: every surviving
  // `#include "that/path.h"` in the emitted TU will read the generated bytes.
  // Therefore preserving one include edge is admissible only when all same-path
  // top-level include sites are either materialized away, or are themselves
  // source-graph-preserved with exactly the same owner bytes.  Otherwise the
  // sidecar would either change an untouched alias or create conflicting bytes
  // for the same generated file.
  for (const RefoldModel::IncludeItem &other : inputs.model.GetIncludes()) {
    if (other.id == include.id)
      continue;
    if (other.parent || !services.paths.PathsEqual(other.sitePath, inputs.tuPath))
      continue;

    std::optional<std::string> otherPath =
        safeSourceGraphRelativeIncludePath(other);
    if (!otherPath || llvm::StringRef(*otherPath) != sourceGraphPath)
      continue;

    auto otherExpansionIt = inputs.includeExpansion.find(other.id);
    if (otherExpansionIt == inputs.includeExpansion.end())
      return false;

    if (!services.includeHasIncluderSuppliedLineControlMacroState(other)) {
      // The alias is dirty but does not satisfy the source-graph proof, so the
      // normal single-output path will materialize it into the TU.  It will not
      // survive as a same-path include edge.
      continue;
    }

    if (llvm::StringRef(otherExpansionIt->second) != candidateBytes)
      return false;
  }

  for (const RefoldModel::IncludeItem &other : inputs.model.GetIncludes()) {
    if (other.id == include.id)
      continue;
    if (other.parent || !services.paths.PathsEqual(other.sitePath, inputs.tuPath))
      continue;

    auto otherExpansionIt = inputs.includeExpansion.find(other.id);
    if (otherExpansionIt == inputs.includeExpansion.end())
      continue;

    const MaterializedIncludeReplayAlias replayAlias =
        classifyMaterializedIncludeReplayAlias(otherExpansionIt->second,
                                               sourceGraphPath);
    if (replayAlias == MaterializedIncludeReplayAlias::SamePath)
      return false;
    if (replayAlias == MaterializedIncludeReplayAlias::UnprovenInclude)
      return false;
  }

  const MaterializedIncludeReplayAlias candidateReplayAlias =
      classifyMaterializedIncludeReplayAlias(candidateBytes, sourceGraphPath);
  if (candidateReplayAlias == MaterializedIncludeReplayAlias::SamePath)
    return false;
  if (candidateReplayAlias == MaterializedIncludeReplayAlias::UnprovenInclude)
    return false;

  return true;
}


SourceGraphOutput makeSourceGraphOutput(
    const RefoldModel::IncludeItem &include, llvm::StringRef relativePath,
    llvm::StringRef bytes, bool cleanupOnly) {
  SourceGraphOutput output;
  output.includeId = include.id;
  output.relativePath = relativePath.str();
  output.originalTarget = include.target.str();
  if (include.resolvedPath)
    output.resolvedPath = include.resolvedPath->str();
  output.bytes = bytes.str();
  output.cleanupOnly = cleanupOnly;
  return output;
}

SourceGraphOwnerPreservationPlan planSourceGraphOwnerPreservation(
    const SourceGraphProofInputs &inputs,
    const RefoldModel::IncludeItem &include, llvm::StringRef candidateBytes,
    const SourceGraphProofServices &services) {
  SourceGraphOwnerPreservationPlan plan;

  // This is intentionally narrower than "dirty header".  Most header edits in
  // the existing single-output backend are supposed to materialize into the TU.
  // The source-graph path is selected only when the modified header contains
  // producer-proven source line-control state whose operands depend on macro
  // definitions supplied by the immediate includer.  Materializing that owner
  // into the TU is token-sound, but it is no longer the most precise
  // source-graph refolding because the header remains the owner of the repaired
  // source line-control directive.
  if (!services.includeHasIncluderSuppliedLineControlMacroState(include))
    return plan;

  std::optional<std::string> sourceGraphPath =
      safeSourceGraphRelativeIncludePath(include);
  if (!sourceGraphPath)
    return plan;

  if (services.includeSubtreeHasLayoutOnlyMaterializationSeed(include.id)) {
    // Source-graph sidecars are path-level artifacts.  They can replace the
    // bytes read from a header path, but they cannot realize an edit to the
    // caller's PP layout at this particular include edge.  A byte-only raw
    // layout hunk seeded from an include boundary is therefore an
    // include-site-local obligation and must be emitted by replacing the include
    // directive in the owner surface, not by writing a sidecar that every
    // surviving same-path include would observe.
    plan.rejectedCleanupRelativePath = *sourceGraphPath;
    return plan;
  }

  if (!sourceGraphIncludePathIsUnaliasedOrCoherent(
          inputs, include, *sourceGraphPath, candidateBytes, services)) {
    plan.rejectedCleanupRelativePath = *sourceGraphPath;
    return plan;
  }

  plan.preservedRelativePath = *sourceGraphPath;
  return plan;
}


SourceGraphOwnerPreservationOutputPlan planSourceGraphOwnerPreservationOutput(
    const SourceGraphProofInputs &inputs,
    const RefoldModel::IncludeItem &include, llvm::StringRef candidateBytes,
    const SourceGraphProofServices &services) {
  SourceGraphOwnerPreservationOutputPlan outputPlan;
  SourceGraphOwnerPreservationPlan plan =
      planSourceGraphOwnerPreservation(inputs, include, candidateBytes, services);

  if (plan.rejectedCleanupRelativePath) {
    // Source-graph sidecars are path-level artifacts beside the emitted TU.  A
    // previous run may have written a sidecar for a path that this run no
    // longer proves admissible, for example after a same-spelling include
    // becomes a surviving alias.  The driver may remove the stale file only if
    // its bytes still exactly match this rejected generated body and the file
    // is not the producer-resolved input header.
    outputPlan.rejectedCleanupOutput = makeSourceGraphOutput(
        include, *plan.rejectedCleanupRelativePath, candidateBytes,
        /*CleanupOnly=*/true);
  }

  if (plan.preservedRelativePath) {
    outputPlan.preservedOutput = makeSourceGraphOutput(
        include, *plan.preservedRelativePath, candidateBytes,
        /*CleanupOnly=*/false);
  }

  return outputPlan;
}

} // namespace source_graph
} // namespace refold
} // namespace clang
