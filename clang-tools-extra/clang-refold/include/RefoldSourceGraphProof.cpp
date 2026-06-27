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

static bool isDirectiveHorizontalWhitespace(char C) {
  return C == '\r' || stringutils::isNonNewlineWs(C);
}

static bool consumeDirectiveScannerNewline(llvm::StringRef Text,
                                           size_t &Pos) {
  if (Pos >= Text.size())
    return false;
  if (Text[Pos] == '\r') {
    ++Pos;
    if (Pos < Text.size() && Text[Pos] == '\n')
      ++Pos;
    return true;
  }
  if (Text[Pos] == '\n') {
    ++Pos;
    return true;
  }
  return false;
}

static bool consumeDirectiveScannerEscapedNewline(llvm::StringRef Text,
                                                  size_t &Pos) {
  if (Pos >= Text.size() || Text[Pos] != '\\')
    return false;

  size_t AfterBackslash = Pos + 1;
  while (AfterBackslash < Text.size() &&
         isDirectiveHorizontalWhitespace(Text[AfterBackslash]))
    ++AfterBackslash;
  if (!consumeDirectiveScannerNewline(Text, AfterBackslash))
    return false;

  // Escaped-newline deletion removes the backslash-newline pair before
  // directive recognition.  Clang also accepts horizontal whitespace before the
  // newline as an extension, so the directive scanner consumes the same spelling
  // that source-range extension treats as a splice.
  Pos = AfterBackslash;
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
findPreprocessingDirectiveIntroducer(llvm::StringRef Text, size_t Start = 0) {
  bool OnlyTriviaOnLogicalLine = true;
  bool InBlockComment = false;
  bool BlockCommentStartedInDirectivePrefix = false;
  size_t Pos = Start;

  while (Pos < Text.size()) {
    if (consumeDirectiveScannerEscapedNewline(Text, Pos))
      continue;

    if (InBlockComment) {
      if (Pos + 1 < Text.size() && Text[Pos] == '*' && Text[Pos + 1] == '/') {
        // A complete block comment is preprocessing whitespace.  Once the
        // terminator is consumed, directive-prefix scanning must resume in the
        // surrounding logical line; otherwise a real directive such as
        // `/*\n*/#include` is hidden from macro-state and include-replay proofs.
        Pos += 2;
        InBlockComment = false;
        continue;
      }
      if (consumeDirectiveScannerNewline(Text, Pos)) {
        // A newline inside a leading block comment still leaves the eventual
        // comment replacement in directive-prefix trivia.  A newline inside a
        // block comment that began after real source code does not retroactively
        // make the following bytes directive-prefix trivia.
        OnlyTriviaOnLogicalLine = BlockCommentStartedInDirectivePrefix;
        continue;
      }
      ++Pos;
      continue;
    }

    if (Pos + 1 < Text.size() && Text[Pos] == '/' && Text[Pos + 1] == '*') {
      BlockCommentStartedInDirectivePrefix = OnlyTriviaOnLogicalLine;
      Pos += 2;
      InBlockComment = true;
      continue;
    }

    if (Pos + 1 < Text.size() && Text[Pos] == '/' && Text[Pos + 1] == '/') {
      Pos += 2;
      while (Pos < Text.size()) {
        if (consumeDirectiveScannerEscapedNewline(Text, Pos))
          continue;
        if (consumeDirectiveScannerNewline(Text, Pos))
          break;
        ++Pos;
      }
      OnlyTriviaOnLogicalLine = true;
      continue;
    }

    if (consumeDirectiveScannerNewline(Text, Pos)) {
      OnlyTriviaOnLogicalLine = true;
      continue;
    }

    if (!OnlyTriviaOnLogicalLine) {
      ++Pos;
      continue;
    }

    if (isDirectiveHorizontalWhitespace(Text[Pos])) {
      ++Pos;
      continue;
    }

    if (Text[Pos] == '#')
      return Pos;

    OnlyTriviaOnLogicalLine = false;
    ++Pos;
  }

  return std::nullopt;
}

/// Skip preprocessing whitespace after a directive introducer or keyword.
/// Escaped newlines are ignored because preprocessing removes them before
/// directive recognition.  Complete block comments are skipped as whitespace,
/// including comments that span physical lines; Clang accepts constructs such
/// as `#/*\n*/include` for the same reason.  Line comments are not skipped
/// because they terminate the directive logical line.
static bool skipDirectiveLogicalWhitespace(llvm::StringRef Text, size_t &Pos) {
  while (Pos < Text.size()) {
    if (isDirectiveHorizontalWhitespace(Text[Pos])) {
      ++Pos;
      continue;
    }
    if (consumeDirectiveScannerEscapedNewline(Text, Pos))
      continue;
    if (Pos + 1 < Text.size() && Text[Pos] == '/' && Text[Pos + 1] == '*') {
      Pos += 2;
      while (Pos + 1 < Text.size() &&
             !(Text[Pos] == '*' && Text[Pos + 1] == '/')) {
        if (consumeDirectiveScannerEscapedNewline(Text, Pos))
          continue;
        ++Pos;
      }
      if (Pos + 1 >= Text.size())
        return false;
      Pos += 2;
      continue;
    }
    break;
  }
  return true;
}

static bool readDirectiveIdentifier(llvm::StringRef Text, size_t &Pos,
                                    std::string &Identifier) {
  Identifier.clear();
  while (Pos < Text.size()) {
    if (consumeDirectiveScannerEscapedNewline(Text, Pos))
      continue;
    const unsigned char C = static_cast<unsigned char>(Text[Pos]);
    if (std::isalpha(C) || Text[Pos] == '_')
      break;
    return false;
  }

  while (Pos < Text.size()) {
    if (consumeDirectiveScannerEscapedNewline(Text, Pos))
      continue;
    const unsigned char C = static_cast<unsigned char>(Text[Pos]);
    if (!(std::isalnum(C) || Text[Pos] == '_'))
      break;
    Identifier.push_back(Text[Pos]);
    ++Pos;
  }

  return !Identifier.empty();
}

} // namespace

std::optional<std::string>
safeSourceGraphRelativeIncludePath(const RefoldModel::IncludeItem &Include) {
  if (Include.angled)
    return std::nullopt;

  llvm::StringRef Target = Include.target;
  if (Target.size() < 2 || Target.front() != '"' || Target.back() != '"')
    return std::nullopt;

  llvm::StringRef Path = Target.drop_front().drop_back();
  if (!safeSynthesizedRelativeIncludeOperandPath(Path))
    return std::nullopt;

  return Path.str();
}

bool lineHasPreprocessingDirectiveIntroducer(llvm::StringRef Text) {
  return findPreprocessingDirectiveIntroducer(Text).has_value();
}

MaterializedIncludeReplayAlias classifyMaterializedIncludeReplayAlias(
    llvm::StringRef MaterializedText, llvm::StringRef SourceGraphPath) {
  size_t SearchPos = 0;
  while (std::optional<size_t> Hash =
             findPreprocessingDirectiveIntroducer(MaterializedText, SearchPos)) {
    size_t Pos = *Hash + 1;
    SearchPos = Pos;

    if (!skipDirectiveLogicalWhitespace(MaterializedText, Pos))
      return MaterializedIncludeReplayAlias::UnprovenInclude;

    std::string Keyword;
    if (!readDirectiveIdentifier(MaterializedText, Pos, Keyword))
      continue;
    if (Keyword != "include")
      continue;

    if (!skipDirectiveLogicalWhitespace(MaterializedText, Pos))
      return MaterializedIncludeReplayAlias::UnprovenInclude;

    if (Pos >= MaterializedText.size() || MaterializedText[Pos] != '"')
      return MaterializedIncludeReplayAlias::UnprovenInclude;

    const size_t PathBegin = ++Pos;
    while (Pos < MaterializedText.size() && MaterializedText[Pos] != '"') {
      // Keep this classifier conservative.  A quoted include whose operand
      // itself uses a splice or reaches an unescaped newline is not a simple
      // path-level equality proof, so source-graph replay must fail closed.
      if (MaterializedText[Pos] == '\\' || MaterializedText[Pos] == '\n' ||
          MaterializedText[Pos] == '\r')
        return MaterializedIncludeReplayAlias::UnprovenInclude;
      ++Pos;
    }
    if (Pos >= MaterializedText.size())
      return MaterializedIncludeReplayAlias::UnprovenInclude;

    if (MaterializedText.slice(PathBegin, Pos) == SourceGraphPath)
      return MaterializedIncludeReplayAlias::SamePath;

    SearchPos = Pos + 1;
  }

  return MaterializedIncludeReplayAlias::None;
}

bool sourceGraphIncludePathIsUnaliasedOrCoherent(
    const SourceGraphProofInputs &Inputs,
    const RefoldModel::IncludeItem &Include,
    llvm::StringRef SourceGraphPath, llvm::StringRef CandidateBytes,
    const SourceGraphProofServices &Services) {
  // A source-graph output written under an original quoted include path is a
  // path-level edit, not an include-site-local edit: every surviving
  // `#include "that/path.h"` in the emitted TU will read the generated bytes.
  // Therefore preserving one include edge is admissible only when all same-path
  // top-level include sites are either materialized away, or are themselves
  // source-graph-preserved with exactly the same owner bytes.  Otherwise the
  // sidecar would either change an untouched alias or create conflicting bytes
  // for the same generated file.
  for (const RefoldModel::IncludeItem &Other : Inputs.Model.GetIncludes()) {
    if (Other.id == Include.id)
      continue;
    if (Other.parent || !Services.Paths.PathsEqual(Other.sitePath, Inputs.TuPath))
      continue;

    std::optional<std::string> OtherPath =
        safeSourceGraphRelativeIncludePath(Other);
    if (!OtherPath || llvm::StringRef(*OtherPath) != SourceGraphPath)
      continue;

    auto OtherExpansionIt = Inputs.IncludeExpansion.find(Other.id);
    if (OtherExpansionIt == Inputs.IncludeExpansion.end())
      return false;

    if (!Services.IncludeHasIncluderSuppliedLineControlMacroState(Other)) {
      // The alias is dirty but does not satisfy the source-graph proof, so the
      // normal single-output path will materialize it into the TU.  It will not
      // survive as a same-path include edge.
      continue;
    }

    if (llvm::StringRef(OtherExpansionIt->second) != CandidateBytes)
      return false;
  }

  for (const RefoldModel::IncludeItem &Other : Inputs.Model.GetIncludes()) {
    if (Other.id == Include.id)
      continue;
    if (Other.parent || !Services.Paths.PathsEqual(Other.sitePath, Inputs.TuPath))
      continue;

    auto OtherExpansionIt = Inputs.IncludeExpansion.find(Other.id);
    if (OtherExpansionIt == Inputs.IncludeExpansion.end())
      continue;

    const MaterializedIncludeReplayAlias ReplayAlias =
        classifyMaterializedIncludeReplayAlias(OtherExpansionIt->second,
                                               SourceGraphPath);
    if (ReplayAlias == MaterializedIncludeReplayAlias::SamePath)
      return false;
    if (ReplayAlias == MaterializedIncludeReplayAlias::UnprovenInclude)
      return false;
  }

  const MaterializedIncludeReplayAlias CandidateReplayAlias =
      classifyMaterializedIncludeReplayAlias(CandidateBytes, SourceGraphPath);
  if (CandidateReplayAlias == MaterializedIncludeReplayAlias::SamePath)
    return false;
  if (CandidateReplayAlias == MaterializedIncludeReplayAlias::UnprovenInclude)
    return false;

  return true;
}


SourceGraphOutput makeSourceGraphOutput(
    const RefoldModel::IncludeItem &Include, llvm::StringRef RelativePath,
    llvm::StringRef Bytes, bool CleanupOnly) {
  SourceGraphOutput Output;
  Output.includeId = Include.id;
  Output.relativePath = RelativePath.str();
  Output.originalTarget = Include.target.str();
  if (Include.resolvedPath)
    Output.resolvedPath = Include.resolvedPath->str();
  Output.bytes = Bytes.str();
  Output.cleanupOnly = CleanupOnly;
  return Output;
}

SourceGraphOwnerPreservationPlan planSourceGraphOwnerPreservation(
    const SourceGraphProofInputs &Inputs,
    const RefoldModel::IncludeItem &Include, llvm::StringRef CandidateBytes,
    const SourceGraphProofServices &Services) {
  SourceGraphOwnerPreservationPlan Plan;

  // This is intentionally narrower than "dirty header".  Most header edits in
  // the existing single-output backend are supposed to materialize into the TU.
  // The source-graph path is selected only when the modified header contains
  // producer-proven source line-control state whose operands depend on macro
  // definitions supplied by the immediate includer.  Materializing that owner
  // into the TU is token-sound, but it is no longer the most precise
  // source-graph refolding because the header remains the owner of the repaired
  // source line-control directive.
  if (!Services.IncludeHasIncluderSuppliedLineControlMacroState(Include))
    return Plan;

  std::optional<std::string> SourceGraphPath =
      safeSourceGraphRelativeIncludePath(Include);
  if (!SourceGraphPath)
    return Plan;

  if (Services.IncludeSubtreeHasLayoutOnlyMaterializationSeed(Include.id)) {
    // Source-graph sidecars are path-level artifacts.  They can replace the
    // bytes read from a header path, but they cannot realize an edit to the
    // caller's PP layout at this particular include edge.  A byte-only raw
    // layout hunk seeded from an include boundary is therefore an
    // include-site-local obligation and must be emitted by replacing the include
    // directive in the owner surface, not by writing a sidecar that every
    // surviving same-path include would observe.
    Plan.RejectedCleanupRelativePath = *SourceGraphPath;
    return Plan;
  }

  if (!sourceGraphIncludePathIsUnaliasedOrCoherent(
          Inputs, Include, *SourceGraphPath, CandidateBytes, Services)) {
    Plan.RejectedCleanupRelativePath = *SourceGraphPath;
    return Plan;
  }

  Plan.PreservedRelativePath = *SourceGraphPath;
  return Plan;
}


SourceGraphOwnerPreservationOutputPlan planSourceGraphOwnerPreservationOutput(
    const SourceGraphProofInputs &Inputs,
    const RefoldModel::IncludeItem &Include, llvm::StringRef CandidateBytes,
    const SourceGraphProofServices &Services) {
  SourceGraphOwnerPreservationOutputPlan OutputPlan;
  SourceGraphOwnerPreservationPlan Plan =
      planSourceGraphOwnerPreservation(Inputs, Include, CandidateBytes, Services);

  if (Plan.RejectedCleanupRelativePath) {
    // Source-graph sidecars are path-level artifacts beside the emitted TU.  A
    // previous run may have written a sidecar for a path that this run no
    // longer proves admissible, for example after a same-spelling include
    // becomes a surviving alias.  The driver may remove the stale file only if
    // its bytes still exactly match this rejected generated body and the file
    // is not the producer-resolved input header.
    OutputPlan.RejectedCleanupOutput = makeSourceGraphOutput(
        Include, *Plan.RejectedCleanupRelativePath, CandidateBytes,
        /*CleanupOnly=*/true);
  }

  if (Plan.PreservedRelativePath) {
    OutputPlan.PreservedOutput = makeSourceGraphOutput(
        Include, *Plan.PreservedRelativePath, CandidateBytes,
        /*CleanupOnly=*/false);
  }

  return OutputPlan;
}

} // namespace source_graph
} // namespace refold
} // namespace clang
