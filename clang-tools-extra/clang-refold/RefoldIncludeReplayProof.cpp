//===--- RefoldIncludeReplayProof.cpp -------------------------*- C++ -*-===//
//
// This file contains the include/include_next replay proof context extracted
// from the old engine tail-utility layer.  It consumes immutable proof inputs and
// explicit read-only engine services; materialization and edit emission remain
// in RefoldEngine.  The proof policy, trace wording, and candidate ordering are
// intentionally unchanged.
//
//===----------------------------------------------------------------------===//

#include "RefoldIncludeReplayProof.h"
#include "RefoldLineControlFilename.h"
#include "RefoldIncludePathProof.h"
#include "RefoldLog.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <cctype>
#include <system_error>

namespace clang {
namespace refold {

using llvm::SmallString;

std::optional<FinalReplaySurface>
buildFinalReplaySurface(const RefoldModel &model, StringRef finalOutputPath) {
  if (finalOutputPath.empty())
    return std::nullopt;

  // Match the driver-side --check oracle: preprocessToBytes() first resolves
  // the check input against the process working directory and then invokes
  // Clang with that absolute input while restoring pp_ctx.cwd as the compiler
  // working directory.  Direct quoted include lookup therefore starts beside
  // the absolute emitted source path, not beside the producer TU path recorded
  // in the map.
  SmallString<256> absoluteOutputPath(finalOutputPath);
  if (std::error_code ec = llvm::sys::fs::make_absolute(absoluteOutputPath)) {
    REFOLD_LOG_DEBUG("include/replay",
                     "final replay surface unavailable for output '{0}': {1}",
                     finalOutputPath, ec.message());
    return std::nullopt;
  }

  std::filesystem::path outputPath(absoluteOutputPath.str().str());
  FinalReplaySurface surface;
  surface.OutputPath = outputPath.lexically_normal();
  surface.OutputDirectory = surface.OutputPath.parent_path();
  surface.OriginalWorkingDirectory =
      std::filesystem::path(model.GetPPCwd().str());

  // Preserve the spelling Clang will use to derive direct quoted child header
  // names from the emitted source file.  The checker feeds Clang an absolute
  // input path, but also restores pp_ctx.cwd as FileSystemOpts::WorkingDir.
  // When the emitted source directory is the working directory, Clang reports
  // direct quoted children with the usual cwd-relative "./child.h" spelling,
  // not with the absolute directory prefix.  Model that spelling explicitly;
  // otherwise replay proofs for valid final-surface includes such as
  //   #include "headers/child.h"
  // would incorrectly fail a __FILE__ observer proof and force materialization.
  std::error_code relativeEC;
  std::filesystem::path relativeOutputDirectory =
      std::filesystem::relative(surface.OutputDirectory,
                                surface.OriginalWorkingDirectory, relativeEC);
  if (!relativeEC && !relativeOutputDirectory.empty() &&
      !relativeOutputDirectory.is_absolute()) {
    bool escapesWorkingDirectory = false;
    for (const auto &component : relativeOutputDirectory) {
      if (component == std::filesystem::path("..")) {
        escapesWorkingDirectory = true;
        break;
      }
    }
    if (!escapesWorkingDirectory) {
      surface.OutputDirectorySpelling =
          relativeOutputDirectory == std::filesystem::path(".")
              ? std::string(".")
              : relativeOutputDirectory.generic_string();
    }
  }

  if (surface.OutputDirectorySpelling.empty()) {
    SmallString<256> outputDirectorySpelling(
        surface.OutputPath.generic_string());
    llvm::sys::path::remove_filename(outputDirectorySpelling);
    surface.OutputDirectorySpelling =
        outputDirectorySpelling.empty() ? std::string(".")
                                        : outputDirectorySpelling.str().str();
  }

  REFOLD_LOG_DEBUG("include/replay",
                   "final replay surface: output='{0}' dir='{1}' cwd='{2}'",
                   surface.OutputPath.generic_string(),
                   surface.OutputDirectory.generic_string(),
                   surface.OriginalWorkingDirectory.generic_string());
  return surface;
}



namespace {

// Replay-only include operand spelling helpers remain local to this proof
// module.  Shared synthesized-operand safety helpers live in
// RefoldIncludePathProof.*, and producer include identity/spelling helpers live
// in RefoldProofTypes.h so proof modules use the same schema fallback rules.
static bool isSafeReplayIncludePathChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
         c == '-' || c == '.' || c == '/';
}

/// Return true when \p path uses only the restricted ASCII include-path
/// spelling alphabet that replay analysis is willing to inspect.  This is a
/// spelling-level predicate only: replay proof still has to compare the selected
/// physical file and observer spelling against producer metadata.
static bool hasSafeReplayIncludePathSpelling(StringRef path) {
  return !path.empty() && !path.contains('\\') && !path.contains('"') &&
         llvm::all_of(path, [](char c) {
           return isSafeReplayIncludePathChar(c);
         });
}

/// Validate slash-separated include operand components under the exact policy
/// requested by the caller.
///
/// * \p allowAbsolute permits the leading empty component of an absolute path.
/// * \p allowDotComponents permits `.` and `..` components for replay-only
///   operands.  Synthesized relative rewrite operands keep this disabled so
///   emitted includes cannot escape the final surface by construction.
static bool hasValidIncludePathComponents(StringRef path, bool allowAbsolute,
                                          bool allowDotComponents) {
  SmallVector<StringRef, 8> components;
  path.split(components, '/', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  const bool isAbsolute = llvm::sys::path::is_absolute(path);
  if (isAbsolute && !allowAbsolute)
    return false;

  for (auto indexed : llvm::enumerate(components)) {
    StringRef component = indexed.value();
    if (component.empty()) {
      if (allowAbsolute && isAbsolute && indexed.index() == 0)
        continue;
      return false;
    }
    if (!allowDotComponents && (component == "." || component == ".."))
      return false;
  }
  return true;
}

/// Path-only predicate for operands that may be replayed through Clang lookup.
/// It accepts absolute operands and `.`/`..` components because replay proof,
/// not appearance, decides whether such an operand actually names the producer
/// target.
static bool safeReplayIncludeLookupOperandPath(StringRef path) {
  return hasSafeReplayIncludePathSpelling(path) &&
         hasValidIncludePathComponents(path, /*allowAbsolute=*/true,
                                       /*allowDotComponents=*/true);
}

/// Return a quoted include operand for lookup analysis, allowing parent
/// directory components and absolute operands.  This helper never authorizes
/// source-graph side-file creation and never by itself proves preservation
/// safe; it only gives replay proof code the exact operand that Clang would
/// look up after a clean child include is moved onto a materialized parent
/// surface.
static std::optional<std::string>
quotedIncludeReplayLookupOperand(const RefoldModel::IncludeItem &inc) {
  if (inc.angled)
    return std::nullopt;

  StringRef target = inc.target;
  if (target.size() < 2 || target.front() != '"' || target.back() != '"')
    return std::nullopt;

  StringRef path = target.drop_front().drop_back();
  if (!safeReplayIncludeLookupOperandPath(path))
    return std::nullopt;

  return path.str();
}

// Filename string-literal decoding is shared with line-control rewrite so
// include replay uses the exact same fail-closed grammar.

} // namespace

// Convert producer lookup provenance to the ordinary directory replay kinds
// modeled by include replay proof.  This is a private context helper because
// the return type is the context's private replay-candidate carrier.
std::optional<IncludeReplayProofContext::IncludeReplayCandidate::LookupKind>
IncludeReplayProofContext::replayableDirectoryLookupKind(
    IncludeLookupKind kind) {
  switch (kind) {
  case IncludeLookupKind::QuoteDir:
    return IncludeReplayCandidate::LookupKind::QuoteDir;
  case IncludeLookupKind::UserI:
    return IncludeReplayCandidate::LookupKind::UserI;
  case IncludeLookupKind::System:
    return IncludeReplayCandidate::LookupKind::System;
  case IncludeLookupKind::IdirAfter:
    return IncludeReplayCandidate::LookupKind::IdirAfter;
  case IncludeLookupKind::Framework:
  case IncludeLookupKind::Builtin:
  case IncludeLookupKind::SourceRelative:
  case IncludeLookupKind::AbsoluteOperand:
  case IncludeLookupKind::Unknown:
    return std::nullopt;
  }
  llvm_unreachable("Invalid IncludeLookupKind");
}

IncludeReplayProofContext::CleanChildIncludeReplayPlan
IncludeReplayProofContext::planCleanChildIncludeReplayFromMaterializedParent(
    const RefoldModel::IncludeItem &child) const {
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

  auto sourceDirectorySpellingForFile = [](StringRef fileSpelling) {
    SmallString<256> source(fileSpelling);
    llvm::sys::path::remove_filename(source);
    if (source.empty())
      return std::string(".");
    return source.str().str();
  };

  auto includeReplaySurfaceForFile =
      [&](StringRef fileSpelling) -> std::optional<IncludeReplaySurface> {
    std::optional<std::filesystem::path> source =
        canonicalizeForLookupProof(fileSpelling);
    if (!source)
      return std::nullopt;

    IncludeReplaySurface surface;
    surface.SourceDirectoryPath = *source;
    surface.SourceDirectoryPath.remove_filename();
    surface.SourceDirectorySpelling =
        sourceDirectorySpellingForFile(fileSpelling);
    return surface;
  };

  auto finalOutputIncludeReplaySurface = [&]()
      -> std::optional<IncludeReplaySurface> {
    if (!finalReplaySurface_)
      return std::nullopt;

    IncludeReplaySurface surface;
    surface.SourceDirectoryPath = finalReplaySurface_->OutputDirectory;
    surface.SourceDirectorySpelling =
        finalReplaySurface_->OutputDirectorySpelling;
    return surface;
  };

  // Keep the syntactic lookup-kind conversion separate from the question
  // "can this producer HeaderSearch entry be replayed as `dir / operand`?".
  // Framework and builtin entries have real search-chain cursor positions, but
  // their lookup semantics are not modeled here as ordinary directory probes;
  // treating them as paths would make include_next proof too strong.

  auto makeIncludeReplaySearchDir =
      [&](StringRef physicalPath, StringRef enteredSpellingPrefix,
          IncludeReplayCandidate::LookupKind kind,
          std::optional<uint32_t> searchChainIndex = std::nullopt)
      -> std::optional<IncludeReplaySearchDir> {
    if (physicalPath.empty() || enteredSpellingPrefix.empty() ||
        kind == IncludeReplayCandidate::LookupKind::Unknown)
      return std::nullopt;

    IncludeReplaySearchDir dir;
    dir.LookupPath = absolutePathInPPCwd(physicalPath);
    dir.EnteredSpellingPrefix = enteredSpellingPrefix.str();
    dir.Kind = kind;
    dir.SearchChainIndex = searchChainIndex;
    return dir;
  };

  auto appendLegacySearchDirIfSafe =
      [&](SmallVectorImpl<IncludeReplaySearchDir> &dirs, StringRef path,
          IncludeReplayCandidate::LookupKind kind) {
        // Legacy argv reconstruction has only one spelling/path token per
        // entry.  Use that token for both the physical lookup directory and the
        // entered-file spelling prefix, matching the old-map behavior.  The
        // search-chain index is intentionally empty: argv reconstruction is a
        // compatibility replay aid for ordinary includes, not producer-proven
        // HeaderSearch cursor state for #include_next.
        if (std::optional<IncludeReplaySearchDir> dir =
                makeIncludeReplaySearchDir(path, path, kind))
          dirs.push_back(std::move(*dir));
      };

  auto appendUnsupportedOrdinarySearchEntryBarrier =
      [&](RecordedIncludeSearchDirs &dirs,
          const RefoldModel::IncludeSearchEntry &entry) {
        IncludeReplaySearchDir barrier;
        barrier.Kind = IncludeReplayCandidate::LookupKind::Unknown;
        barrier.SearchChainIndex = entry.index;
        barrier.IsUnsupportedBarrier = true;

        // Once ordinary replay reaches an unmodeled HeaderSearch entry, the
        // selected file is unknown: the entry could contain a header map,
        // framework, builtin/VFS-only header, or another producer-side lookup
        // mechanism that is not serialized well enough for deterministic
        // consumer replay.  Preserve the entry's ordinary-include
        // participation when installing the barrier; a quote-only entry cannot
        // shadow an angled include, but every other search-chain entry can.
        dirs.QuotedLookupDirs.push_back(barrier);
        if (entry.kind != IncludeLookupKind::QuoteDir)
          dirs.AngledLookupDirs.push_back(barrier);
      };

  auto appendProducerSearchEntryIfSafe =
      [&](RecordedIncludeSearchDirs &dirs,
          const RefoldModel::IncludeSearchEntry &entry) {
        // New maps carry both the physical/FileManager directory path and the
        // spelling Clang used for the search entry.  Keep those separated so
        // ordinary include replay no longer derives __FILE__ spelling from a
        // canonicalized filesystem path.  For search-chain hits, also carry the
        // exact producer index that selected the target; later include_next
        // proof must compare that cursor against producer resume provenance.
        std::optional<IncludeReplayCandidate::LookupKind> kind =
            replayableDirectoryLookupKind(entry.kind);
        if (!kind) {
          appendUnsupportedOrdinarySearchEntryBarrier(dirs, entry);
          return;
        }

        if (std::optional<IncludeReplaySearchDir> dir =
                makeIncludeReplaySearchDir(entry.path, entry.spelling, *kind,
                                           entry.index)) {
          if (entry.kind == IncludeLookupKind::QuoteDir) {
            dirs.QuotedLookupDirs.push_back(*dir);
          } else {
            dirs.QuotedLookupDirs.push_back(*dir);
            dirs.AngledLookupDirs.push_back(*dir);
          }
        } else {
          // A search-chain entry with missing or otherwise unusable path/
          // spelling data is just as unprovable as an unknown entry.  Preserve
          // lookup order by turning it into a barrier instead of dropping it.
          appendUnsupportedOrdinarySearchEntryBarrier(dirs, entry);
        }
      };

  auto computeProducerIncludeSearchDirs = [&]() {
    RecordedIncludeSearchDirs dirs;

    for (const RefoldModel::IncludeSearchEntry &entry :
         model_.GetIncludeSearchChain()) {
      switch (entry.kind) {
      case IncludeLookupKind::QuoteDir:
        // -iquote entries participate only in quoted include lookup.
        appendProducerSearchEntryIfSafe(dirs, entry);
        break;
      case IncludeLookupKind::UserI:
      case IncludeLookupKind::System:
      case IncludeLookupKind::IdirAfter:
        // Non-quote search-chain entries participate in angled lookup and,
        // after source-relative/-iquote lookup has failed, quoted lookup.
        appendProducerSearchEntryIfSafe(dirs, entry);
        break;
      case IncludeLookupKind::Framework:
      case IncludeLookupKind::Builtin:
      case IncludeLookupKind::Unknown:
        // The ordinary replay evaluator is intentionally limited to concrete
        // directory search entries serialized by the producer.  Unsupported or
        // unknown entries are retained as lookup barriers so replay fails
        // closed if it would have to reason past one.
        appendProducerSearchEntryIfSafe(dirs, entry);
        break;
      case IncludeLookupKind::SourceRelative:
      case IncludeLookupKind::AbsoluteOperand:
        llvm_unreachable("per-edge include lookup kind in search chain");
      }
    }

    return dirs;
  };

  auto computeLegacyArgvIncludeSearchDirs = [&]() {
    SmallVector<IncludeReplaySearchDir, 16> quoteDirs;
    SmallVector<IncludeReplaySearchDir, 32> includeDirs;
    ArrayRef<std::string> argv = model_.GetPPArgv();

    for (size_t i = 0; i < argv.size(); ++i) {
      StringRef arg(argv[i]);
      auto consumeJoinedOrSeparate =
          [&](StringRef joinedPrefix,
              SmallVectorImpl<IncludeReplaySearchDir> &out,
              IncludeReplayCandidate::LookupKind kind) -> bool {
        if (arg == joinedPrefix) {
          if (i + 1 < argv.size())
            appendLegacySearchDirIfSafe(out, argv[++i], kind);
          return true;
        }
        if (arg.starts_with(joinedPrefix) && arg.size() > joinedPrefix.size()) {
          appendLegacySearchDirIfSafe(out, arg.drop_front(joinedPrefix.size()),
                                      kind);
          return true;
        }
        return false;
      };

      if (consumeJoinedOrSeparate("-iquote", quoteDirs,
                                  IncludeReplayCandidate::LookupKind::QuoteDir))
        continue;
      if (consumeJoinedOrSeparate("-I", includeDirs,
                                  IncludeReplayCandidate::LookupKind::UserI))
        continue;
      if (arg == "-isystem" && i + 1 < argv.size()) {
        appendLegacySearchDirIfSafe(
            includeDirs, argv[++i], IncludeReplayCandidate::LookupKind::System);
        continue;
      }
      if (arg == "-idirafter" && i + 1 < argv.size()) {
        appendLegacySearchDirIfSafe(
            includeDirs, argv[++i], IncludeReplayCandidate::LookupKind::IdirAfter);
        continue;
      }
      const StringRef isystemPrefix("-isystem");
      const StringRef idirafterPrefix("-idirafter");
      if (arg.starts_with(isystemPrefix) && arg.size() > isystemPrefix.size()) {
        appendLegacySearchDirIfSafe(
            includeDirs, arg.drop_front(isystemPrefix.size()),
            IncludeReplayCandidate::LookupKind::System);
        continue;
      }
      if (arg.starts_with(idirafterPrefix) &&
          arg.size() > idirafterPrefix.size()) {
        appendLegacySearchDirIfSafe(
            includeDirs, arg.drop_front(idirafterPrefix.size()),
            IncludeReplayCandidate::LookupKind::IdirAfter);
        continue;
      }
    }

    RecordedIncludeSearchDirs dirs;
    // Preserve the legacy replay order: quoted lookup searches -iquote first,
    // then the ordinary include dirs; angled lookup uses only the ordinary
    // include dirs.
    dirs.QuotedLookupDirs.append(quoteDirs.begin(), quoteDirs.end());
    dirs.QuotedLookupDirs.append(includeDirs.begin(), includeDirs.end());
    dirs.AngledLookupDirs.append(includeDirs.begin(), includeDirs.end());
    return dirs;
  };

  auto computeRecordedIncludeSearchDirs = [&]() {
    // Prefer the producer-normalized HeaderSearch chain when present.  It is
    // already parsed and validated by RefoldModel, and unlike argv parsing it
    // preserves Clang's effective order plus each entry's physical path and
    // entered spelling.  The argv parser remains only for old maps that lack
    // pp_ctx.include_search_chain.
    if (!model_.GetIncludeSearchChain().empty())
      return computeProducerIncludeSearchDirs();
    return computeLegacyArgvIncludeSearchDirs();
  };

  auto recordedIncludeSearchDirs = [&]() -> const RecordedIncludeSearchDirs & {
    if (!recordedIncludeSearchDirsCache_)
      recordedIncludeSearchDirsCache_ = computeRecordedIncludeSearchDirs();
    return *recordedIncludeSearchDirsCache_;
  };

  auto appendEnteredFileSpelling = [](StringRef prefix, StringRef operand) {
    SmallString<256> spelling(prefix);
    llvm::sys::path::append(spelling, operand);
    return spelling.str().str();
  };

  auto directSourceRelativeEnteredFileSpelling =
      [&](const IncludeReplaySurface &surface, StringRef operand)
      -> std::string {
    // Direct quoted lookup preserves the including-file directory spelling and
    // appends the operand spelling; it is not a cwd-relative normalization of
    // the physical result.  Examples observed from Clang:
    //   original.c + "headers/child.h" -> "./headers/child.h"
    //   ./original.c + "headers/child.h" -> "./headers/child.h"
    //   sub/original.c + "../leaf.h" -> "sub/../leaf.h"
    //   /abs/original.c + "leaf.h" -> "/abs/leaf.h"
    return appendEnteredFileSpelling(surface.SourceDirectorySpelling, operand);
  };

  auto computeAbsoluteIncludeReplayCandidate =
      [&](StringRef operand) -> std::optional<IncludeReplayCandidate> {
    std::filesystem::path operandPath(operand.str());
    if (!operandPath.is_absolute())
      return std::nullopt;
    if (!pathExists(operandPath))
      return std::nullopt;

    IncludeReplayCandidate candidate;
    candidate.PhysicalPath = operandPath.lexically_normal();
    candidate.EnteredFileSpelling = operandPath.generic_string();
    candidate.EnteredFileName =
        stringutils::pathBasename(candidate.EnteredFileSpelling).str();
    candidate.Kind = IncludeReplayCandidate::LookupKind::AbsoluteOperand;
    return candidate;
  };

  auto candidateFromSearchDir =
      [&](const IncludeReplaySearchDir &dir, StringRef operand)
      -> std::optional<IncludeReplayCandidate> {
    if (dir.IsUnsupportedBarrier)
      return std::nullopt;

    // Check the exact filesystem path Clang would probe before any lexical
    // cleanup.  Collapsing `missing/../leaf.h` first is unsound: POSIX path
    // resolution and Clang header lookup must be able to enter every prefix
    // component before `..` can escape it.
    std::filesystem::path physical = dir.LookupPath / operand.str();
    if (!pathExists(physical))
      return std::nullopt;

    IncludeReplayCandidate candidate;
    candidate.PhysicalPath = physical.lexically_normal();
    candidate.EnteredFileSpelling =
        appendEnteredFileSpelling(dir.EnteredSpellingPrefix, operand);
    candidate.EnteredFileName =
        stringutils::pathBasename(candidate.EnteredFileSpelling).str();
    candidate.Kind = dir.Kind;
    candidate.SearchChainIndex = dir.SearchChainIndex;
    return candidate;
  };

  auto ordinaryIncludeResultFromCandidate =
      [](const IncludeReplayCandidate &candidate) -> OrdinaryIncludeReplayResult {
    OrdinaryIncludeReplayResult result;
    result.PhysicalPath = candidate.PhysicalPath;
    result.EnteredFileSpelling = candidate.EnteredFileSpelling;
    result.EnteredFileName =
        !candidate.EnteredFileName.empty()
            ? candidate.EnteredFileName
            : stringutils::pathBasename(candidate.EnteredFileSpelling).str();
    result.LookupKind = candidate.Kind;
    result.SearchChainIndex = candidate.SearchChainIndex;
    return result;
  };

  auto includeReplayCandidateFromOrdinaryResult =
      [](const OrdinaryIncludeReplayResult &result) -> IncludeReplayCandidate {
    IncludeReplayCandidate candidate;
    candidate.PhysicalPath = result.PhysicalPath;
    candidate.EnteredFileSpelling = result.EnteredFileSpelling;
    candidate.EnteredFileName = result.EnteredFileName;
    candidate.Kind = result.LookupKind;
    candidate.SearchChainIndex = result.SearchChainIndex;
    return candidate;
  };

  auto computeOrdinaryIncludeReplayResult =
      [&](StringRef operand, OrdinaryIncludeDelimiterKind delimiterKind,
          const IncludeReplaySurface *quotedSurface,
          OrdinaryIncludeReplayFailureKind *failureKind)
      -> std::optional<OrdinaryIncludeReplayResult> {
    // This is a replay evaluator only: it answers which header Clang would
    // select for a concrete ordinary include spelling from the supplied final
    // replay surface/search model.  It intentionally does not decide whether
    // preserving or synthesizing that include is semantically valid; callers
    // must still prove physical identity, observer stability, and include_next
    // obligations against producer metadata.
    auto fail = [&](OrdinaryIncludeReplayFailureKind kind)
        -> std::optional<OrdinaryIncludeReplayResult> {
      if (failureKind)
        *failureKind = kind;
      return std::nullopt;
    };
    auto success = [&](const IncludeReplayCandidate &candidate) {
      if (failureKind)
        *failureKind = OrdinaryIncludeReplayFailureKind::None;
      return ordinaryIncludeResultFromCandidate(candidate);
    };

    if (failureKind)
      *failureKind = OrdinaryIncludeReplayFailureKind::Unresolved;

    if (operand.empty())
      return fail(OrdinaryIncludeReplayFailureKind::Unresolved);

    if (std::optional<IncludeReplayCandidate> absolute =
            computeAbsoluteIncludeReplayCandidate(operand))
      return success(*absolute);

    const RecordedIncludeSearchDirs &dirs = recordedIncludeSearchDirs();
    auto replayThroughSearchDirs =
        [&](ArrayRef<IncludeReplaySearchDir> lookupDirs)
        -> std::optional<OrdinaryIncludeReplayResult> {
      for (const IncludeReplaySearchDir &dir : lookupDirs) {
        if (dir.IsUnsupportedBarrier)
          return fail(OrdinaryIncludeReplayFailureKind::UnknownSearchChainEntry);
        if (std::optional<IncludeReplayCandidate> candidate =
                candidateFromSearchDir(dir, operand))
          return success(*candidate);
      }
      return fail(OrdinaryIncludeReplayFailureKind::Unresolved);
    };

    if (delimiterKind == OrdinaryIncludeDelimiterKind::Quoted) {
      if (!quotedSurface)
        return fail(OrdinaryIncludeReplayFailureKind::Unresolved);

      // Quoted include lookup first probes beside the including file.  For a
      // final-output proof, `quotedSurface` is the emitted .c.mod surface; for
      // preserved nested headers, it is derived from the replayed parent header
      // spelling.  In both cases the existence check uses the exact path Clang
      // would probe, and only the returned physical payload is normalized.
      std::filesystem::path direct =
          quotedSurface->SourceDirectoryPath / operand.str();
      if (pathExists(direct)) {
        IncludeReplayCandidate candidate;
        candidate.PhysicalPath = direct.lexically_normal();
        candidate.EnteredFileSpelling =
            directSourceRelativeEnteredFileSpelling(*quotedSurface, operand);
        candidate.EnteredFileName =
            stringutils::pathBasename(candidate.EnteredFileSpelling).str();
        candidate.Kind =
            IncludeReplayCandidate::LookupKind::DirectSourceRelative;
        return success(candidate);
      }

      return replayThroughSearchDirs(dirs.QuotedLookupDirs);
    }

    return replayThroughSearchDirs(dirs.AngledLookupDirs);
  };

  auto computeQuotedIncludeReplayCandidateOnSurface =
      [&](StringRef operand, const IncludeReplaySurface &surface)
      -> std::optional<IncludeReplayCandidate> {
    std::optional<OrdinaryIncludeReplayResult> replay =
        computeOrdinaryIncludeReplayResult(
            operand, OrdinaryIncludeDelimiterKind::Quoted, &surface, nullptr);
    if (!replay)
      return std::nullopt;
    return includeReplayCandidateFromOrdinaryResult(*replay);
  };

  auto computeQuotedIncludeReplayCandidate =
      [&](StringRef operand) -> std::optional<IncludeReplayCandidate> {
    // Ordinary quoted includes preserved or synthesized into the final output
    // must replay from the emitted .c.mod location.  Producer-source surfaces
    // are still used by computeProducerIncludeReplayCandidate() when recovering
    // producer spelling witnesses, but they are not valid acceptance surfaces
    // for final-source include rewrites.
    std::optional<IncludeReplaySurface> surface =
        finalOutputIncludeReplaySurface();
    if (!surface)
      return std::nullopt;
    return computeQuotedIncludeReplayCandidateOnSurface(operand, *surface);
  };

  [[maybe_unused]] auto computeAngledIncludeReplayCandidate =
      [&](StringRef operand) -> std::optional<IncludeReplayCandidate> {
    std::optional<OrdinaryIncludeReplayResult> replay =
        computeOrdinaryIncludeReplayResult(
            operand, OrdinaryIncludeDelimiterKind::Angled, nullptr, nullptr);
    if (!replay)
      return std::nullopt;
    return includeReplayCandidateFromOrdinaryResult(*replay);
  };

  [[maybe_unused]] auto computeIncludeNextReplayCandidate =
      [&](StringRef operand, const IncludeReplaySurface &surface,
          const RefoldModel::IncludeLookupProvenance &containingFile)
      -> std::optional<IncludeNextReplayCandidate> {
    // The replay surface is part of the public helper shape because the caller
    // will usually derive containing-file provenance from a replayed include on
    // that surface.  The include_next lookup itself does not use direct
    // source-relative probing; once the containing-file cursor is known, lookup
    // resumes only through pp_ctx.include_search_chain.
    (void)surface;

    if (operand.empty())
      return std::nullopt;

    // A general include_next proof must start from producer-proven
    // HeaderSearch cursor state.  Source-relative, absolute-operand, unknown,
    // and legacy argv-derived containing-file provenance do not identify a
    // search-chain position, so they cannot prove where lookup should resume.
    if (!isSearchChainIncludeLookupKind(containingFile.kind) ||
        !containingFile.searchChainIndex)
      return std::nullopt;

    const uint32_t resumeIndex = *containingFile.searchChainIndex + 1;
    ArrayRef<RefoldModel::IncludeSearchEntry> chain =
        model_.GetIncludeSearchChain();
    if (chain.empty() || resumeIndex >= chain.size())
      return std::nullopt;

    for (uint32_t i = resumeIndex; i < chain.size(); ++i) {
      const RefoldModel::IncludeSearchEntry &entry = chain[i];
      if (entry.index != i)
        return std::nullopt;

      // Unsupported entries are barriers for include_next proof.  Unlike an
      // ordinary include compatibility replay, include_next is a cursor-state
      // proof over the producer HeaderSearch chain: an unmodeled framework,
      // builtin, unknown, or otherwise non-directory entry between the resume
      // cursor and a later directory hit could have selected or shadowed the
      // target first.  Do not skip past it unless its lookup is explicitly
      // modeled as ordinary directory probing.
      std::optional<IncludeReplayCandidate::LookupKind> replayKind =
          replayableDirectoryLookupKind(entry.kind);
      if (!replayKind)
        return std::nullopt;

      std::optional<IncludeReplaySearchDir> dir =
          makeIncludeReplaySearchDir(entry.path, entry.spelling, *replayKind,
                                     entry.index);
      if (!dir)
        return std::nullopt;

      std::optional<IncludeReplayCandidate> selected =
          candidateFromSearchDir(*dir, operand);
      if (!selected)
        continue;

      if (!selected->SearchChainIndex || *selected->SearchChainIndex != i)
        return std::nullopt;

      IncludeNextReplayCandidate candidate;
      candidate.PhysicalPath = selected->PhysicalPath;
      candidate.EnteredFileSpelling = selected->EnteredFileSpelling;
      candidate.EnteredFileName = selected->EnteredFileName;
      candidate.ResumeSearchChainIndex = resumeIndex;
      candidate.SelectedSearchChainIndex = i;
      candidate.SelectedKind = entry.kind;
      return candidate;
    }

    return std::nullopt;
  };

  auto angledIncludeReplayLookupOperand =
      [](const RefoldModel::IncludeItem &child) -> std::optional<std::string> {
    if (!child.angled)
      return std::nullopt;

    StringRef target = child.target;
    if (target.size() < 2 || target.front() != '<' || target.back() != '>')
      return std::nullopt;

    StringRef path = target.drop_front().drop_back();
    if (!hasSafeReplayIncludePathSpelling(path))
      return std::nullopt;

    return path.str();
  };

  auto computeProducerIncludeReplayCandidate =
      [&](const RefoldModel::IncludeItem &include)
      -> std::optional<IncludeReplayCandidate> {
    if (include.subkind != "#include")
      return std::nullopt;

    if (include.angled) {
      std::optional<std::string> operand =
          angledIncludeReplayLookupOperand(include);
      if (!operand)
        return std::nullopt;
      return computeAngledIncludeReplayCandidate(*operand);
    }

    std::optional<std::string> operand =
        quotedIncludeReplayLookupOperand(include);
    if (!operand)
      return std::nullopt;
    std::optional<IncludeReplaySurface> surface =
        includeReplaySurfaceForFile(include.sitePath);
    if (!surface)
      return std::nullopt;
    return computeQuotedIncludeReplayCandidateOnSurface(*operand, *surface);
  };

  auto includeIdIsDescendantOrSelf = [&](uint64_t owner,
                                         uint64_t root) -> bool {
    uint64_t cur = owner;
    while (true) {
      if (cur == root)
        return true;
      const RefoldModel::IncludeItem *inc = model_.GetIncludeById(cur);
      if (!inc || !inc->parent)
        return false;
      cur = *inc->parent;
    }
  };

  struct IncludeNextObligation {
    uint64_t includeNextId = 0;

    // Present only for new-schema maps whose producer could identify the
    // include edge that contains this #include_next directive.  Old maps and
    // producer-unknown cases remain explicit obligations, but later proof code
    // must fail them closed because there is no containing-file cursor to
    // replay.
    std::optional<uint64_t> containingIncludeId;

    // Header operand with the surrounding quotes/angles stripped.  An empty
    // value means the directive target was not replay-parseable by the same
    // conservative operand rules used by ordinary include replay; the
    // include-next proof must treat that as unproven rather than guessing.
    std::string operand;

    // Producer resume/selection facts for this directive.  When metadata is
    // absent, this remains the default unknown provenance object so the
    // include-next replay proof can distinguish a collected obligation from a
    // proven replay obligation.
    RefoldModel::IncludeNextProvenance producer;
  };

  struct CleanChildIncludeReplayDemand {
    bool observesLine = false;
    bool observesFile = false;
    bool observesFileName = false;
    bool observesBaseFile = false;
    bool observesIncludeLevel = false;

    // Descendant #include_next directives observe HeaderSearch cursor state,
    // not just child physical identity.  Clean child replay may preserve the
    // child include only after every obligation here is discharged against the
    // replayed containing-file cursor and selected target.  Missing producer
    // provenance, an unparseable operand, an unknown search-chain entry, or any
    // physical/spelling mismatch keeps the old fail-closed materialization path.
    SmallVector<IncludeNextObligation, 4> includeNextObligations;

    // Exact producer-observed payloads of preserved file-spelling observers in
    // this include subtree.  These come from the macro expansion tokens in the
    // original preprocessed stream, not from IncludeItem::resolvedPath: current
    // maps may store an absolute physical-ish path in resolvedPath even when
    // Clang exposed a direct source-relative spelling such as
    // "./headers/child.h" through __FILE__.
    SmallVector<std::string, 4> fileSpellingPayloads;
    SmallVector<std::string, 4> fileNamePayloads;

    // Producer-side entered spelling for the child edge itself.  New maps fill
    // this from IncludeItem::enteredFileSpelling.  Legacy maps fill it later
    // from recovered observer payloads, a producer-replay reconstruction, or
    // resolved_path, in that order.  The child edge must be re-entered with the
    // same spelling before descendant quoted lookup and file observers can be
    // considered stable.
    std::optional<std::string> producerChildFileSpelling;

    // Producer-side __FILE_NAME__ spelling for the child edge.  Prefer the
    // producer-emitted entered_file_name when present because it was computed
    // with Clang's own processPathToFileName() logic; otherwise derive a
    // compatibility basename from the selected spelling witness.
    std::optional<std::string> producerChildFileName;

    // A preserved file-spelling observer was present, but its exact expansion
    // payload could not be recovered from the producer token stream.  Include
    // replay cannot prove that observer family in that case, so the clean child
    // must fail closed to materialization/line-state repair.
    bool hasUnprovenFileSpellingObserver = false;

    bool observesFileSpelling() const {
      return observesFile || observesFileName ||
             hasUnprovenFileSpellingObserver;
    }

    bool requiresReplayCandidateProof() const {
      return observesLine || observesFileSpelling() || observesBaseFile ||
             observesIncludeLevel || !includeNextObligations.empty();
    }
  };

  auto appendUniqueString = [](SmallVectorImpl<std::string> &values,
                                StringRef value) {
    if (!llvm::any_of(values, [&](const std::string &existing) {
          return StringRef(existing) == value;
        }))
      values.push_back(value.str());
  };

  auto decodeObservedFileStringLiteralPayload = [&](StringRef spelling)
      -> std::optional<std::string> {
    StringRef rest = spelling.trim();
    std::optional<std::string> decoded = parseLineControlFilenameLiteral(rest);
    if (!decoded || !rest.trim().empty())
      return std::nullopt;
    return decoded;
  };

  auto producerObservedFileSpellingPayload =
      [&](const RefoldModel::MacroInvocation &macro)
      -> std::optional<std::string> {
    if (macro.cover.IsValid()) {
      if (std::optional<std::string> decoded =
              decodeObservedFileStringLiteralPayload(
                  SliceASource(macro.cover.begin, macro.cover.end)))
        return decoded;
    }

    if (macro.invPPByteBegin && macro.invPPByteEnd &&
        *macro.invPPByteBegin <= *macro.invPPByteEnd &&
        *macro.invPPByteEnd <= aSource_.size()) {
      if (std::optional<std::string> decoded =
              decodeObservedFileStringLiteralPayload(aSource_.slice(
                  static_cast<size_t>(*macro.invPPByteBegin),
                  static_cast<size_t>(*macro.invPPByteEnd))))
        return decoded;
    }

    return std::nullopt;
  };

  auto observableMacroOwnerInIncludeSubtree =
      [&](const RefoldModel::MacroInvocation &macro, uint64_t includeId)
      -> std::optional<uint64_t> {
    const RefoldModel::MacroInvocation *site =
        LineStateObservableMacroSite(macro);
    std::optional<uint64_t> owner =
        site && site->ownerIncludeId ? site->ownerIncludeId
                                     : macro.ownerIncludeId;
    if (!owner || !includeIdIsDescendantOrSelf(*owner, includeId))
      return std::nullopt;
    if (!LineStateBuiltinInvocationIsPreservedObserver(macro))
      return std::nullopt;
    return owner;
  };

  auto cleanChildIncludeReplayDemand =
      [&](uint64_t includeId) -> CleanChildIncludeReplayDemand {
    CleanChildIncludeReplayDemand demand;

    const RefoldModel::IncludeItem *rootInclude =
        model_.GetIncludeById(includeId);
    if (rootInclude) {
      // File-spelling proof hierarchy, step 1: new-schema maps carry the exact
      // include-entry spelling directly.  Prefer that producer fact over any
      // recovered macro payloads or replay reconstruction.  Later #line changes
      // are handled by the line-state proof layer; this field proves only the
      // spelling assigned when the child include is entered.
      StringRef explicitSpelling =
          explicitProducerEnteredFileSpelling(*rootInclude);
      if (!explicitSpelling.empty()) {
        demand.producerChildFileSpelling = explicitSpelling.str();
        StringRef explicitFileName = producerEnteredFileName(*rootInclude);
        if (!explicitFileName.empty())
          demand.producerChildFileName = explicitFileName.str();
      }
    }

    const bool lineSensitiveDemandEnabled = lineDirs_.Enabled();

    auto includeNextReplayLookupOperand =
        [&](const RefoldModel::IncludeItem &includeNext)
        -> std::optional<std::string> {
      if (includeNext.subkind != "#include_next")
        return std::nullopt;
      if (includeNext.angled)
        return angledIncludeReplayLookupOperand(includeNext);
      return quotedIncludeReplayLookupOperand(includeNext);
    };

    for (const RefoldModel::IncludeItem &descendant : model_.GetIncludes()) {
      if (descendant.id == includeId)
        continue;
      if (descendant.subkind != "#include_next")
        continue;
      if (!includeIdIsDescendantOrSelf(descendant.id, includeId))
        continue;

      IncludeNextObligation obligation;
      obligation.includeNextId = descendant.id;
      if (descendant.includeNext) {
        obligation.producer = *descendant.includeNext;
        obligation.containingIncludeId =
            descendant.includeNext->containingFileIncludeId;
      }
      if (std::optional<std::string> operand =
              includeNextReplayLookupOperand(descendant))
        obligation.operand = std::move(*operand);

      // Keep every nested #include_next obligation, not just the first.  Later
      // replay proof must discharge all of them before a clean child include
      // can be preserved across parent materialization.  Missing provenance or
      // an empty operand is deliberately represented in the obligation itself
      // so the proof layer can fail closed with useful trace context.
      demand.includeNextObligations.push_back(std::move(obligation));
    }

    for (const auto &macro : model_.GetMacroInvocations()) {
      const bool observesLine = macro.name == "__LINE__";
      const bool observesFile = macro.name == "__FILE__";
      const bool observesFileName = macro.name == "__FILE_NAME__";
      const bool observesBaseFile = macro.name == "__BASE_FILE__";
      const bool observesIncludeLevel = macro.name == "__INCLUDE_LEVEL__";
      if (!observesLine && !observesFile && !observesFileName &&
          !observesBaseFile && !observesIncludeLevel)
        continue;
      std::optional<uint64_t> observableOwner =
          observableMacroOwnerInIncludeSubtree(macro, includeId);
      if (!observableOwner)
        continue;

      // In --no-lines mode the final checker intentionally treats
      // unmodified line-directive-sensitive builtin payloads as ignorable for
      // __LINE__, __FILE__, and __FILE_NAME__: those payloads may differ when
      // the final refolded source is validated from a temporary output path
      // rather than the original source path.  Do not let those observers force
      // a clean child include rewrite/materialization under the active no-lines
      // oracle.
      //
      // Keep __BASE_FILE__ and __INCLUDE_LEVEL__ as hard demands.
      // __BASE_FILE__ observes the top-level preprocessing file, not the child
      // header's entered spelling, so include operand replay cannot prove it;
      // preserving the child include would make the observer depend on the
      // checker/output source rather than the producer top-level source.
      // __INCLUDE_LEVEL__ likewise observes include-stack depth.
      demand.observesLine |= lineSensitiveDemandEnabled && observesLine;
      demand.observesFile |= lineSensitiveDemandEnabled && observesFile;
      demand.observesFileName |= lineSensitiveDemandEnabled && observesFileName;
      demand.observesBaseFile |= observesBaseFile;
      demand.observesIncludeLevel |= observesIncludeLevel;

      if (lineSensitiveDemandEnabled && (observesFile || observesFileName) &&
          *observableOwner == includeId) {
        if (std::optional<std::string> payload =
                producerObservedFileSpellingPayload(macro)) {
          if (observesFile)
            appendUniqueString(demand.fileSpellingPayloads, *payload);
          if (observesFileName)
            appendUniqueString(demand.fileNamePayloads, *payload);
        } else if ((observesFile && !demand.producerChildFileSpelling) ||
                   (observesFileName && !demand.producerChildFileName)) {
          // New-schema entered_file_spelling / entered_file_name are primary
          // proof targets.  Missing decoded payloads only make the demand
          // unproven when no stronger include-entry metadata is available.
          demand.hasUnprovenFileSpellingObserver = true;
        }
      }

      if (demand.observesLine && demand.observesFile &&
          demand.observesFileName && demand.observesBaseFile &&
          demand.observesIncludeLevel &&
          demand.hasUnprovenFileSpellingObserver)
        break;
    }

    // File-spelling proof hierarchy, step 2: old maps without
    // entered_file_spelling may still prove spelling through preserved builtin
    // expansion payloads.  A single unique payload becomes the proof target;
    // multiple distinct payloads are left in the payload vectors so candidate
    // evaluation must satisfy all of them and will fail closed if they conflict.
    if (!demand.producerChildFileSpelling &&
        demand.fileSpellingPayloads.size() == 1)
      demand.producerChildFileSpelling = demand.fileSpellingPayloads.front();
    if (!demand.producerChildFileName && demand.fileNamePayloads.size() == 1)
      demand.producerChildFileName = demand.fileNamePayloads.front();

    // File-spelling proof hierarchy, final fallbacks: if neither explicit
    // metadata nor recovered payloads selected a target, fall back to replaying
    // the original include from the producer surface.  If replay is
    // unavailable, resolved_path remains the final legacy compatibility
    // witness.
    const bool mayUseReplaySpelling = demand.fileSpellingPayloads.empty();
    const bool mayUseReplayFileName = demand.fileNamePayloads.empty();
    if (rootInclude && (mayUseReplaySpelling || mayUseReplayFileName)) {
      std::optional<IncludeReplayCandidate> producerCandidate;
      auto getProducerCandidate =
          [&]() -> std::optional<IncludeReplayCandidate> & {
        if (!producerCandidate)
          producerCandidate =
              computeProducerIncludeReplayCandidate(*rootInclude);
        return producerCandidate;
      };

      if (!demand.producerChildFileSpelling && mayUseReplaySpelling) {
        std::optional<IncludeReplayCandidate> &candidate =
            getProducerCandidate();
        if (candidate)
          demand.producerChildFileSpelling = candidate->EnteredFileSpelling;
      }
      if (!demand.producerChildFileName && mayUseReplayFileName) {
        std::optional<IncludeReplayCandidate> &candidate =
            getProducerCandidate();
        if (candidate)
          demand.producerChildFileName =
              stringutils::pathBasename(candidate->EnteredFileSpelling).str();
      }
    }

    if (rootInclude) {
      StringRef legacySpelling = legacyResolvedIncludePath(*rootInclude);
      if (!legacySpelling.empty()) {
        if (!demand.producerChildFileSpelling && mayUseReplaySpelling)
          demand.producerChildFileSpelling = legacySpelling.str();
        if (!demand.producerChildFileName && mayUseReplayFileName)
          demand.producerChildFileName =
              stringutils::pathBasename(legacySpelling).str();
      }
    }

    if (demand.observesFile && demand.fileSpellingPayloads.empty() &&
        !demand.producerChildFileSpelling)
      demand.hasUnprovenFileSpellingObserver = true;
    if (demand.observesFileName && demand.fileNamePayloads.empty() &&
        !demand.producerChildFileName)
      demand.hasUnprovenFileSpellingObserver = true;

    return demand;
  };

  auto replayCandidateLookupKindAsProducerKind =
      [](IncludeReplayCandidate::LookupKind kind) -> IncludeLookupKind {
    switch (kind) {
    case IncludeReplayCandidate::LookupKind::DirectSourceRelative:
      return IncludeLookupKind::SourceRelative;
    case IncludeReplayCandidate::LookupKind::QuoteDir:
      return IncludeLookupKind::QuoteDir;
    case IncludeReplayCandidate::LookupKind::UserI:
      return IncludeLookupKind::UserI;
    case IncludeReplayCandidate::LookupKind::System:
      return IncludeLookupKind::System;
    case IncludeReplayCandidate::LookupKind::IdirAfter:
      return IncludeLookupKind::IdirAfter;
    case IncludeReplayCandidate::LookupKind::Framework:
      return IncludeLookupKind::Framework;
    case IncludeReplayCandidate::LookupKind::Builtin:
      return IncludeLookupKind::Builtin;
    case IncludeReplayCandidate::LookupKind::AbsoluteOperand:
      return IncludeLookupKind::AbsoluteOperand;
    case IncludeReplayCandidate::LookupKind::Unknown:
      return IncludeLookupKind::Unknown;
    }
    llvm_unreachable("Invalid IncludeReplayCandidate::LookupKind");
  };

  auto replayCandidateLookupProvenance =
      [&](const IncludeReplayCandidate &candidate)
      -> std::optional<RefoldModel::IncludeLookupProvenance> {
    RefoldModel::IncludeLookupProvenance provenance;
    provenance.kind = replayCandidateLookupKindAsProducerKind(candidate.Kind);

    if (isSearchChainIncludeLookupKind(provenance.kind)) {
      if (!candidate.SearchChainIndex)
        return std::nullopt;
      provenance.searchChainIndex = *candidate.SearchChainIndex;
    }

    if (provenance.kind == IncludeLookupKind::Unknown)
      return std::nullopt;
    return provenance;
  };

  auto includeReplayCandidateMatchesProducerLookup =
      [&](const IncludeReplayCandidate &candidate,
          const RefoldModel::IncludeItem &include) -> bool {
    const std::string physicalPath = candidate.PhysicalPath.generic_string();
    if (!samePhysicalIncludeFile(physicalPath, include))
      return false;

    if (!include.lookup)
      return true;

    const IncludeLookupKind candidateKind =
        replayCandidateLookupKindAsProducerKind(candidate.Kind);
    const RefoldModel::IncludeLookupProvenance &producer = *include.lookup;
    if (candidateKind != producer.kind)
      return false;

    // Search-chain provenance is the state later #include_next proof consumes.
    // If the producer selected this edge through pp_ctx.include_search_chain,
    // replay must select the exact same entry.  Legacy argv-derived candidates
    // intentionally lack SearchChainIndex and therefore cannot satisfy this
    // new-schema proof.
    if (isSearchChainIncludeLookupKind(producer.kind))
      return candidate.SearchChainIndex && producer.searchChainIndex &&
             *candidate.SearchChainIndex == *producer.searchChainIndex;

    // Source-relative and absolute-operand hits have no search-chain cursor.
    // Physical equality plus kind equality is the complete ordinary-include
    // replay proof they can provide; any descendant #include_next that needs a
    // cursor will still fail closed when its containing-file proof is checked.
    return !candidate.SearchChainIndex && !producer.searchChainIndex;
  };

  auto directIncludeNextHasProducerSelectionProvenance =
      [](const RefoldModel::IncludeItem &include) -> bool {
    if (include.subkind != "#include_next" || !include.includeNext ||
        !include.includeNext->known)
      return false;

    // This is proof (A) from the design: the producer must have recorded the
    // containing-file cursor, the resume cursor, and the selected search-chain
    // entry.  Without all three facts, a relocated #include_next cannot be
    // replaced by an ordinary include non-heuristically.
    const RefoldModel::IncludeNextProvenance &next = *include.includeNext;
    if (!next.containingFileIncludeId || !next.containingFileSearchChainIndex ||
        !next.resumeSearchChainIndex || !next.selectedSearchChainIndex)
      return false;

    if (*next.resumeSearchChainIndex != *next.containingFileSearchChainIndex + 1)
      return false;

    if (!include.lookup || !include.lookup->searchChainIndex ||
        !isSearchChainIncludeLookupKind(include.lookup->kind))
      return false;

    // The selected target index is stored redundantly as include.lookup plus the
    // finalized include_next selectedSearchChainIndex.  Require the two producer
    // witnesses to agree before comparing any rewritten ordinary include against
    // them; disagreement means the map itself is not a usable proof input.
    return *include.lookup->searchChainIndex == *next.selectedSearchChainIndex;
  };

  auto directIncludeNextSelectionUsesOnlyReplayableDirectories =
      [&](const RefoldModel::IncludeItem &include) -> bool {
    if (!directIncludeNextHasProducerSelectionProvenance(include))
      return false;

    const RefoldModel::IncludeNextProvenance &next = *include.includeNext;
    const uint32_t resumeIndex = *next.resumeSearchChainIndex;
    const uint32_t selectedIndex = *next.selectedSearchChainIndex;
    if (resumeIndex > selectedIndex)
      return false;

    ArrayRef<RefoldModel::IncludeSearchEntry> chain =
        model_.GetIncludeSearchChain();
    if (chain.empty() || selectedIndex >= chain.size())
      return false;

    for (uint32_t i = resumeIndex; i <= selectedIndex; ++i) {
      const RefoldModel::IncludeSearchEntry &entry = chain[i];
      if (entry.index != i)
        return false;

      // Direct #include_next-to-ordinary rewrites may name the selected target
      // without preserving the directive, but they still rely on the producer's
      // include_next cursor proof to know which search-chain entry was selected.
      // If that cursor path crosses a framework, builtin, unknown, malformed,
      // or otherwise unmodeled entry, the safe answer is materialization.
      std::optional<IncludeReplayCandidate::LookupKind> kind =
          replayableDirectoryLookupKind(entry.kind);
      if (!kind)
        return false;
      if (!makeIncludeReplaySearchDir(entry.path, entry.spelling, *kind,
                                      entry.index))
        return false;
    }

    return true;
  };

  auto directIncludeNextOrdinaryRewriteMatchesProducerSelection =
      [&](const IncludeReplayCandidate &candidate,
          const RefoldModel::IncludeItem &include,
          const CleanChildIncludeReplayDemand &demand) -> bool {
    if (!directIncludeNextSelectionUsesOnlyReplayableDirectories(include))
      return false;

    const std::string physicalPath = candidate.PhysicalPath.generic_string();
    if (!samePhysicalIncludeFile(physicalPath, include))
      return false;

    // A relocated #include_next can be replaced by an ordinary include in two
    // distinct replay-proven ways:
    //
    //  * Search-chain equivalent: the ordinary include is selected by the same
    //    producer HeaderSearch entry that #include_next selected.  This keeps a
    //    usable containing-file cursor for any descendant #include_next edges.
    //
    //  * Direct-target naming: a quoted source-relative or absolute ordinary
    //    include names the producer-selected file directly from the final
    //    .c.mod surface.  This does not reproduce the original HeaderSearch
    //    cursor, so it is only a complete proof when the selected target
    //    subtree has no descendant #include_next obligations that could observe
    //    that cursor.  File-spelling observers are still checked later by the
    //    ordinary IncludeReplayProofResult gate before the rewrite is accepted.
    const bool directTargetNaming =
        candidate.Kind ==
            IncludeReplayCandidate::LookupKind::DirectSourceRelative ||
        candidate.Kind == IncludeReplayCandidate::LookupKind::AbsoluteOperand;
    if (directTargetNaming)
      return demand.includeNextObligations.empty();

    // Search-chain candidates must replay through the same producer-selected
    // entry.  Physical equality alone would be too weak here: an earlier or
    // different search entry may select the same inode/path while giving
    // descendant #include_next directives a different resume cursor.
    if (!includeReplayCandidateMatchesProducerLookup(candidate, include))
      return false;

    if (!include.includeNext || !include.includeNext->selectedSearchChainIndex)
      return false;
    return candidate.SearchChainIndex &&
           *candidate.SearchChainIndex ==
               *include.includeNext->selectedSearchChainIndex;
  };

  struct FileObserverDemand {
    bool observesFile = false;
    bool observesFileName = false;
  };

  auto includeSubtreeFileObserverDemand =
      [&](uint64_t includeId) -> FileObserverDemand {
    FileObserverDemand demand;
    if (!lineDirs_.Enabled())
      return demand;

    for (const auto &macro : model_.GetMacroInvocations()) {
      const bool observesFile = macro.name == "__FILE__";
      const bool observesFileName = macro.name == "__FILE_NAME__";
      if (!observesFile && !observesFileName)
        continue;
      if (!observableMacroOwnerInIncludeSubtree(macro, includeId))
        continue;
      demand.observesFile |= observesFile;
      demand.observesFileName |= observesFileName;
      if (demand.observesFile && demand.observesFileName)
        break;
    }
    return demand;
  };

  auto includeNextReplayAsOrdinaryCandidate =
      [&](const IncludeNextReplayCandidate &selected)
      -> std::optional<IncludeReplayCandidate> {
    IncludeReplayCandidate candidate;
    candidate.PhysicalPath = selected.PhysicalPath;
    candidate.EnteredFileSpelling = selected.EnteredFileSpelling;
    candidate.EnteredFileName = selected.EnteredFileName;
    std::optional<IncludeReplayCandidate::LookupKind> kind =
        replayableDirectoryLookupKind(selected.SelectedKind);
    if (!kind)
      return std::nullopt;
    candidate.Kind = *kind;
    candidate.SearchChainIndex = selected.SelectedSearchChainIndex;
    return candidate;
  };

  auto includeNextObligationsAreProven =
      [&](const IncludeReplayCandidate &childCandidate,
          const RefoldModel::IncludeItem &child,
          const CleanChildIncludeReplayDemand &demand) -> bool {
    if (demand.includeNextObligations.empty())
      return true;

    DenseMap<uint64_t, IncludeReplayCandidate> replayMemo;
    DenseSet<uint64_t> replayFailed;
    DenseSet<uint64_t> replayActive;
    replayMemo.insert({child.id, childCandidate});

    auto includeNextReplayLookupOperand =
        [&](const RefoldModel::IncludeItem &includeNext)
        -> std::optional<std::string> {
      if (includeNext.subkind != "#include_next")
        return std::nullopt;
      if (includeNext.angled)
        return angledIncludeReplayLookupOperand(includeNext);
      return quotedIncludeReplayLookupOperand(includeNext);
    };

    std::function<std::optional<IncludeReplayCandidate>(uint64_t)>
        replayIncludeInPreservedChild =
            [&](uint64_t includeId) -> std::optional<IncludeReplayCandidate> {
      if (auto it = replayMemo.find(includeId); it != replayMemo.end())
        return it->second;
      if (replayFailed.contains(includeId) || replayActive.contains(includeId))
        return std::nullopt;

      const RefoldModel::IncludeItem *include = model_.GetIncludeById(includeId);
      if (!include || !includeIdIsDescendantOrSelf(includeId, child.id) ||
          !include->parent) {
        replayFailed.insert(includeId);
        return std::nullopt;
      }

      replayActive.insert(includeId);
      auto fail = [&]() -> std::optional<IncludeReplayCandidate> {
        replayActive.erase(includeId);
        replayFailed.insert(includeId);
        return std::nullopt;
      };

      std::optional<IncludeReplayCandidate> candidate;
      if (include->subkind == "#include") {
        std::optional<IncludeReplayCandidate> parentCandidate =
            replayIncludeInPreservedChild(*include->parent);
        if (!parentCandidate)
          return fail();

        if (include->angled) {
          std::optional<std::string> operand =
              angledIncludeReplayLookupOperand(*include);
          if (!operand)
            return fail();
          candidate = computeAngledIncludeReplayCandidate(*operand);
        } else {
          std::optional<std::string> operand =
              quotedIncludeReplayLookupOperand(*include);
          if (!operand)
            return fail();
          std::optional<IncludeReplaySurface> surface =
              includeReplaySurfaceForFile(parentCandidate->EnteredFileSpelling);
          if (!surface)
            return fail();
          candidate = computeQuotedIncludeReplayCandidateOnSurface(*operand,
                                                                   *surface);
        }
      } else if (include->subkind == "#include_next") {
        if (!include->includeNext || !include->includeNext->known ||
            !include->includeNext->containingFileIncludeId)
          return fail();

        std::optional<std::string> operand =
            includeNextReplayLookupOperand(*include);
        if (!operand)
          return fail();

        std::optional<IncludeReplayCandidate> containingCandidate =
            replayIncludeInPreservedChild(
                *include->includeNext->containingFileIncludeId);
        if (!containingCandidate)
          return fail();

        std::optional<RefoldModel::IncludeLookupProvenance>
            containingProvenance =
                replayCandidateLookupProvenance(*containingCandidate);
        if (!containingProvenance)
          return fail();

        std::optional<IncludeReplaySurface> surface =
            includeReplaySurfaceForFile(
                containingCandidate->EnteredFileSpelling);
        if (!surface)
          return fail();

        std::optional<IncludeNextReplayCandidate> selected =
            computeIncludeNextReplayCandidate(*operand, *surface,
                                              *containingProvenance);
        if (!selected || !include->includeNext->resumeSearchChainIndex ||
            !include->includeNext->selectedSearchChainIndex ||
            !include->includeNext->containingFileSearchChainIndex ||
            !containingProvenance->searchChainIndex)
          return fail();
        if (selected->ResumeSearchChainIndex !=
                *include->includeNext->resumeSearchChainIndex ||
            selected->SelectedSearchChainIndex !=
                *include->includeNext->selectedSearchChainIndex ||
            *containingProvenance->searchChainIndex !=
                *include->includeNext->containingFileSearchChainIndex)
          return fail();
        candidate = includeNextReplayAsOrdinaryCandidate(*selected);
      } else {
        return fail();
      }

      if (!candidate || !includeReplayCandidateMatchesProducerLookup(*candidate,
                                                                     *include))
        return fail();

      replayActive.erase(includeId);
      replayMemo.insert({includeId, *candidate});
      return candidate;
    };

    for (const IncludeNextObligation &obligation :
         demand.includeNextObligations) {
      const RefoldModel::IncludeItem *includeNext =
          model_.GetIncludeById(obligation.includeNextId);
      if (!includeNext || includeNext->subkind != "#include_next")
        return false;
      if (!obligation.producer.known || obligation.operand.empty() ||
          !obligation.containingIncludeId ||
          !obligation.producer.resumeSearchChainIndex ||
          !obligation.producer.containingFileSearchChainIndex ||
          !obligation.producer.selectedSearchChainIndex)
        return false;

      std::optional<IncludeReplayCandidate> containingCandidate =
          replayIncludeInPreservedChild(*obligation.containingIncludeId);
      if (!containingCandidate)
        return false;

      std::optional<RefoldModel::IncludeLookupProvenance> containingProvenance =
          replayCandidateLookupProvenance(*containingCandidate);
      if (!containingProvenance || !containingProvenance->searchChainIndex)
        return false;
      if (*containingProvenance->searchChainIndex !=
          *obligation.producer.containingFileSearchChainIndex)
        return false;

      std::optional<IncludeReplaySurface> surface =
          includeReplaySurfaceForFile(containingCandidate->EnteredFileSpelling);
      if (!surface)
        return false;

      std::optional<IncludeNextReplayCandidate> candidate =
          computeIncludeNextReplayCandidate(obligation.operand, *surface,
                                            *containingProvenance);
      if (!candidate)
        return false;

      if (candidate->ResumeSearchChainIndex !=
              *obligation.producer.resumeSearchChainIndex ||
          candidate->SelectedSearchChainIndex !=
              *obligation.producer.selectedSearchChainIndex)
        return false;

      // The descendant #include_next metadata carries two redundant producer
      // witnesses for the selected target: include_next provenance and the
      // normal per-edge lookup record.  Require them to agree with the replayed
      // selection before treating the obligation as discharged; otherwise a
      // malformed or old mixed-schema map could prove the resume cursor against
      // one witness while preserving a directive whose edge lookup says another.
      if (includeNext->lookup) {
        if (includeNext->lookup->kind != candidate->SelectedKind)
          return false;
        if (isSearchChainIncludeLookupKind(includeNext->lookup->kind)) {
          if (!includeNext->lookup->searchChainIndex ||
              *includeNext->lookup->searchChainIndex !=
                  candidate->SelectedSearchChainIndex)
            return false;
        }
      }

      const std::string physicalPath = candidate->PhysicalPath.generic_string();
      if (!samePhysicalIncludeFile(physicalPath, *includeNext))
        return false;

      // A descendant #include_next can be selected through the same physical
      // file and search-chain entry while still exposing a different presumed
      // filename.  That spelling matters only when the selected subtree keeps
      // filename observers under the active oracle.  Observer-free selected
      // subtrees require only physical identity plus cursor equivalence.
      FileObserverDemand observerDemand =
          includeSubtreeFileObserverDemand(includeNext->id);
      if (observerDemand.observesFile) {
        StringRef producerSpelling = producerEnteredFileSpelling(*includeNext);
        if (producerSpelling.empty() ||
            StringRef(candidate->EnteredFileSpelling) != producerSpelling)
          return false;
      }
      if (observerDemand.observesFileName) {
        StringRef producerName = producerEnteredFileName(*includeNext);
        // Prefer the producer/replay EnteredFileName payload when available.
        // It is the schema field intended to mirror Clang's
        // processPathToFileName() result; deriving a basename from
        // EnteredFileSpelling is only a legacy compatibility fallback.
        const std::string candidateName =
            !candidate->EnteredFileName.empty()
                ? candidate->EnteredFileName
                : stringutils::pathBasename(candidate->EnteredFileSpelling)
                      .str();
        if (producerName.empty() || StringRef(candidateName) != producerName)
          return false;
      }
    }

    return true;
  };

  struct IncludeReplayProofResult {
    bool samePhysicalFile = false;

    // `__FILE__` and `__FILE_NAME__` are related, but they are distinct
    // observer contracts.  Keep the proof bits separate so a candidate that has
    // the right basename but the wrong entered spelling cannot satisfy a
    // `__FILE__` demand, and vice versa.
    bool sameEnteredFileSpelling = true;
    bool sameEnteredFileName = true;

    bool sameIncludeNextStack = true;

    bool proves(const CleanChildIncludeReplayDemand &demand) const {
      return samePhysicalFile &&
             (!demand.observesFile || sameEnteredFileSpelling) &&
             (!demand.observesFileName || sameEnteredFileName) &&
             !demand.hasUnprovenFileSpellingObserver &&
             (demand.includeNextObligations.empty() ||
              sameIncludeNextStack);
    }
  };

  auto evaluateIncludeReplayCandidate =
      [&](const IncludeReplayCandidate &candidate,
          const RefoldModel::IncludeItem &child,
          const CleanChildIncludeReplayDemand &demand)
      -> IncludeReplayProofResult {
    IncludeReplayProofResult result;
    const std::string physicalPath = candidate.PhysicalPath.generic_string();
    result.samePhysicalFile = samePhysicalIncludeFile(physicalPath, child);
    result.sameIncludeNextStack =
        includeNextObligationsAreProven(candidate, child, demand);
    if (demand.observesFileSpelling()) {
      if (demand.hasUnprovenFileSpellingObserver) {
        result.sameEnteredFileSpelling = false;
        result.sameEnteredFileName = false;
      }

      // File-spelling proof hierarchy: explicit entered_file_spelling (or the
      // best legacy witness selected when building the demand) is the primary
      // target for __FILE__ preservation.  Recovered macro payloads are used
      // only when no include-entry spelling witness exists, which keeps old
      // maps conservative without letting payload recovery override new-schema
      // producer metadata.
      if (demand.observesFile) {
        if (demand.producerChildFileSpelling) {
          result.sameEnteredFileSpelling &=
              StringRef(candidate.EnteredFileSpelling) ==
              StringRef(*demand.producerChildFileSpelling);
        } else {
          for (const std::string &expected : demand.fileSpellingPayloads)
            result.sameEnteredFileSpelling &=
                StringRef(candidate.EnteredFileSpelling) ==
                StringRef(expected);
        }
      }

      const std::string candidateFileName =
          !candidate.EnteredFileName.empty()
              ? candidate.EnteredFileName
              : stringutils::pathBasename(candidate.EnteredFileSpelling).str();
      if (demand.observesFileName) {
        if (demand.producerChildFileName) {
          result.sameEnteredFileName &=
              StringRef(candidateFileName) ==
              StringRef(*demand.producerChildFileName);
        } else {
          for (const std::string &expected : demand.fileNamePayloads)
            result.sameEnteredFileName &=
                StringRef(candidateFileName) == StringRef(expected);
        }
      }
    }
    return result;
  };

  struct QuotedChildIncludeRewriteCandidate {
    std::string operand;
    IncludeReplayCandidate replay;
  };

  struct DirectIncludeNextOrdinaryRewriteCandidate {
    std::string operand;
    OrdinaryIncludeDelimiterKind delimiterKind =
        OrdinaryIncludeDelimiterKind::Quoted;

    // Candidate generation must remain broader than acceptance.  Keep
    // unresolved hypotheses in the list so trace mode can explain why each
    // spelling failed, but require Replay to be present before any proof can
    // accept and emit the operand.
    std::optional<IncludeReplayCandidate> replay;
    OrdinaryIncludeReplayFailureKind replayFailure =
        OrdinaryIncludeReplayFailureKind::Unresolved;

    std::string origin;
  };

  auto ordinaryIncludeReplayFailureReasonName =
      [](OrdinaryIncludeReplayFailureKind kind) -> const char * {
    switch (kind) {
    case OrdinaryIncludeReplayFailureKind::None:
      return "none";
    case OrdinaryIncludeReplayFailureKind::Unresolved:
      return "unresolved";
    case OrdinaryIncludeReplayFailureKind::UnknownSearchChainEntry:
      return "unknown-search-chain-entry";
    }
    llvm_unreachable("Invalid OrdinaryIncludeReplayFailureKind");
  };

  auto includeReplayLookupKindName =
      [](IncludeReplayCandidate::LookupKind kind) -> const char * {
    switch (kind) {
    case IncludeReplayCandidate::LookupKind::DirectSourceRelative:
      return "source-relative";
    case IncludeReplayCandidate::LookupKind::QuoteDir:
      return "quote-dir";
    case IncludeReplayCandidate::LookupKind::UserI:
      return "user-i";
    case IncludeReplayCandidate::LookupKind::System:
      return "system";
    case IncludeReplayCandidate::LookupKind::IdirAfter:
      return "idirafter";
    case IncludeReplayCandidate::LookupKind::Framework:
      return "framework";
    case IncludeReplayCandidate::LookupKind::Builtin:
      return "builtin";
    case IncludeReplayCandidate::LookupKind::AbsoluteOperand:
      return "absolute-operand";
    case IncludeReplayCandidate::LookupKind::Unknown:
      return "unknown";
    }
    llvm_unreachable("Invalid IncludeReplayCandidate::LookupKind");
  };

  auto optionalSearchChainIndexForTrace =
      [](std::optional<uint32_t> index) -> std::string {
    if (!index)
      return "(none)";
    return std::to_string(*index);
  };

  auto safeOrdinaryIncludeRewriteOperand = [](StringRef path) {
    // This predicate only checks whether an operand can be written back as a
    // bounded header-name token by this source edit.  It intentionally allows
    // absolute paths and `.`/`..` components: those spellings are not accepted
    // by appearance, and may be rejected later by the final-surface replay
    // evaluator or the producer-equivalence proof.  Candidate classes derived
    // from physical containment apply the stricter no-escape predicate before
    // reaching this generic syntax gate.
    return safeReplayIncludeLookupOperandPath(path);
  };

  auto stableContainedRelativeOperand =
      [&](const std::filesystem::path &target,
          const std::filesystem::path &base) -> std::optional<std::string> {
    // Candidate classes derived from physical containment must not synthesize
    // an escaping path.  If `target` is not actually below `base`, the relative
    // computation either fails, is absolute, or contains `..`; all of those are
    // rejected before replay.
    std::optional<std::filesystem::path> canonicalTarget =
        canonicalizeForLookupProof(target.generic_string());
    std::optional<std::filesystem::path> canonicalBase =
        canonicalizeForLookupProof(base.generic_string());
    if (!canonicalTarget || !canonicalBase)
      return std::nullopt;

    std::error_code ec;
    std::filesystem::path relative =
        std::filesystem::relative(*canonicalTarget, *canonicalBase, ec);
    if (ec || relative.empty() || relative.is_absolute())
      return std::nullopt;

    const std::string operand = relative.generic_string();
    if (!safeSynthesizedRelativeIncludeOperand(operand))
      return std::nullopt;
    return operand;
  };

  auto generateQuotedChildIncludeRewriteCandidates =
      [&](const RefoldModel::IncludeItem &child) {
    SmallVector<QuotedChildIncludeRewriteCandidate, 8> candidates;
    if (child.subkind != "#include" || child.angled)
      return candidates;

    std::optional<std::filesystem::path> producerPhysicalPath =
        producerPhysicalIncludePath(child);
    if (!producerPhysicalPath)
      return candidates;

    auto appendCandidate = [&](StringRef operand) {
      if (operand.empty() || !safeSynthesizedRelativeIncludeOperand(operand))
        return;
      for (const QuotedChildIncludeRewriteCandidate &existing : candidates) {
        if (StringRef(existing.operand) == operand)
          return;
      }

      std::optional<IncludeReplayCandidate> replay =
          computeQuotedIncludeReplayCandidate(operand);
      if (!replay)
        return;

      QuotedChildIncludeRewriteCandidate rewrite;
      rewrite.operand = operand.str();
      rewrite.replay = std::move(*replay);
      candidates.push_back(std::move(rewrite));
    };

    // These are spelling hypotheses, not acceptance shortcuts.  Every operand
    // is immediately replayed from the final .c.mod surface above and later
    // accepted only if physical identity, active file-observer demands, and
    // descendant #include_next obligations all prove against producer metadata.
    //
    // The final-output-relative candidate covers headers copied beside the
    // emitted source.  The search-dir-relative candidates cover the common
    // materialized-parent case where the producer child was originally found
    // relative to a header directory, but the best final spelling is through an
    // existing -I/-iquote entry, e.g. rewriting "leaf.h" to "headers/leaf.h".
    if (finalReplaySurface_) {
      if (std::optional<std::string> operand = stableContainedRelativeOperand(
              *producerPhysicalPath, finalReplaySurface_->OutputDirectory))
        appendCandidate(*operand);
    }

    SmallVector<std::string, 16> seenSearchDirectories;
    const RecordedIncludeSearchDirs &dirs = recordedIncludeSearchDirs();
    for (const IncludeReplaySearchDir &dir : dirs.QuotedLookupDirs) {
      if (dir.IsUnsupportedBarrier || dir.LookupPath.empty())
        continue;
      const std::string key =
          dir.LookupPath.lexically_normal().generic_string();
      if (llvm::is_contained(seenSearchDirectories, key))
        continue;
      seenSearchDirectories.push_back(key);

      if (std::optional<std::string> operand = stableContainedRelativeOperand(
              *producerPhysicalPath, dir.LookupPath))
        appendCandidate(*operand);
    }

    // New-schema entered-file spelling is also useful as a deterministic
    // hypothesis for direct source-relative children.  It still has to replay
    // from the final surface before it can be emitted, so it cannot bypass a
    // physical shadowing or file-observer mismatch.
    StringRef explicitEnteredSpelling =
        explicitProducerEnteredFileSpelling(child);
    if (!explicitEnteredSpelling.empty())
      appendCandidate(explicitEnteredSpelling);

    // Legacy maps may have only resolved_path as the producer spelling.  Keep
    // this behind the split-schema path so old maps get the previous recovery
    // behavior without letting resolved_path compete with explicit metadata.
    if (!child.enteredFileSpelling) {
      StringRef legacySpelling = legacyResolvedIncludePath(child);
      if (!legacySpelling.empty())
        appendCandidate(legacySpelling);
    }

    return candidates;
  };

  auto replayDirectIncludeNextOrdinaryRewriteCandidate =
      [&](StringRef operand, OrdinaryIncludeDelimiterKind delimiterKind,
          OrdinaryIncludeReplayFailureKind *failureKind)
      -> std::optional<IncludeReplayCandidate> {
    if (failureKind)
      *failureKind = OrdinaryIncludeReplayFailureKind::Unresolved;

    if (!safeOrdinaryIncludeRewriteOperand(operand))
      return std::nullopt;

    std::optional<OrdinaryIncludeReplayResult> replay;
    if (delimiterKind == OrdinaryIncludeDelimiterKind::Quoted) {
      std::optional<IncludeReplaySurface> surface =
          finalOutputIncludeReplaySurface();
      if (!surface)
        return std::nullopt;
      replay = computeOrdinaryIncludeReplayResult(
          operand, OrdinaryIncludeDelimiterKind::Quoted, &*surface,
          failureKind);
    } else {
      replay = computeOrdinaryIncludeReplayResult(
          operand, OrdinaryIncludeDelimiterKind::Angled, nullptr, failureKind);
    }

    if (!replay)
      return std::nullopt;
    return includeReplayCandidateFromOrdinaryResult(*replay);
  };

  auto generateDirectIncludeNextOrdinaryRewriteCandidates =
      [&](const RefoldModel::IncludeItem &child) {
    SmallVector<DirectIncludeNextOrdinaryRewriteCandidate, 16> candidates;
    if (child.subkind != "#include_next")
      return candidates;

    std::optional<std::filesystem::path> producerPhysicalPath =
        producerPhysicalIncludePath(child);
    if (!producerPhysicalPath)
      return candidates;

    auto appendCandidate = [&](StringRef operand,
                               OrdinaryIncludeDelimiterKind delimiterKind,
                               StringRef origin) {
      if (operand.empty())
        return;
      for (const DirectIncludeNextOrdinaryRewriteCandidate &existing :
           candidates) {
        if (StringRef(existing.operand) == operand &&
            existing.delimiterKind == delimiterKind)
          return;
      }

      DirectIncludeNextOrdinaryRewriteCandidate candidate;
      candidate.operand = operand.str();
      candidate.delimiterKind = delimiterKind;
      candidate.origin = origin.str();
      candidate.replay = replayDirectIncludeNextOrdinaryRewriteCandidate(
          operand, delimiterKind, &candidate.replayFailure);
      candidates.push_back(std::move(candidate));
    };

    auto appendWithPreferredDelimiters = [&](StringRef operand,
                                             StringRef origin) {
      // Preserve the original delimiter first, then try the other ordinary
      // spelling.  Ordering is only a tie-breaker among candidates that have
      // already replayed; it is never an acceptance proof.
      if (child.angled) {
        appendCandidate(operand, OrdinaryIncludeDelimiterKind::Angled, origin);
        appendCandidate(operand, OrdinaryIncludeDelimiterKind::Quoted, origin);
      } else {
        appendCandidate(operand, OrdinaryIncludeDelimiterKind::Quoted, origin);
        appendCandidate(operand, OrdinaryIncludeDelimiterKind::Angled, origin);
      }
    };

    // 1. Original #include_next operand, rewritten as an ordinary include.
    if (std::optional<std::string> originalOperand =
            child.angled ? angledIncludeReplayLookupOperand(child)
                         : quotedIncludeReplayLookupOperand(child))
      appendWithPreferredDelimiters(*originalOperand, "original-operand");

    // 2. Target path relative to the producer search-chain entry that selected
    // this #include_next target, when the producer recorded such an entry.
    if (child.lookup && replayableDirectoryLookupKind(child.lookup->kind)) {
      if (child.lookup->directoryPath) {
        if (std::optional<std::string> operand = stableContainedRelativeOperand(
                *producerPhysicalPath,
                std::filesystem::path(child.lookup->directoryPath->str())))
          appendWithPreferredDelimiters(*operand,
                                        "selected-search-chain-directory");
      }
    }

    // 3. Target path relative to every modeled recorded search directory that
    // physically contains it.  Unsupported entries are ignored here as operand
    // sources; they remain lookup barriers in the replay evaluator itself.
    SmallVector<std::string, 16> seenSearchDirectories;
    auto appendSearchDirectoryRelativeCandidate =
        [&](const IncludeReplaySearchDir &dir) {
          if (dir.IsUnsupportedBarrier || dir.LookupPath.empty())
            return;
          const std::string key =
              dir.LookupPath.lexically_normal().generic_string();
          if (llvm::is_contained(seenSearchDirectories, key))
            return;
          seenSearchDirectories.push_back(key);

          if (std::optional<std::string> operand =
                  stableContainedRelativeOperand(*producerPhysicalPath,
                                                  dir.LookupPath))
            appendWithPreferredDelimiters(*operand,
                                          "recorded-search-directory");
        };
    const RecordedIncludeSearchDirs &dirs = recordedIncludeSearchDirs();
    for (const IncludeReplaySearchDir &dir : dirs.QuotedLookupDirs)
      appendSearchDirectoryRelativeCandidate(dir);
    for (const IncludeReplaySearchDir &dir : dirs.AngledLookupDirs)
      appendSearchDirectoryRelativeCandidate(dir);

    // 4. Output-directory-relative path.  Once a relocated #include_next is
    //    being rewritten as an ordinary include in the final .c.mod file, the
    //    most source-accurate direct-target spelling is the stable spelling
    //    from that final surface.  Prefer this over producer entered-file
    //    spelling when both replay, because entered-file spelling describes
    //    what Clang reported for the producer include stack; it is not
    //    necessarily the cleanest operand to emit from the relocated surface
    //    (for example, `./headers/b/next.h` versus `headers/b/next.h`).
    //
    //    This is still only a candidate: replay and observer proof below must
    //    prove physical identity, file-spelling stability when observed, and
    //    descendant #include_next obligations before anything is emitted.
    if (finalReplaySurface_) {
      if (std::optional<std::string> operand = stableContainedRelativeOperand(
              *producerPhysicalPath, finalReplaySurface_->OutputDirectory))
        appendWithPreferredDelimiters(*operand, "output-directory-relative");
    }

    // 5. Producer entered-file spelling, when the new-schema producer supplied
    // it explicitly.  Keep this after the final-surface spelling so it repairs
    // real file-observer cases without overriding the canonical emitted-source
    // operand when the two spellings are semantically equivalent.
    StringRef explicitEnteredSpelling =
        explicitProducerEnteredFileSpelling(child);
    if (!explicitEnteredSpelling.empty())
      appendWithPreferredDelimiters(explicitEnteredSpelling,
                                    "producer-entered-file-spelling");

    // 6. Legacy resolved_path spelling, only for old maps that lack the split
    // entered_file_spelling/opened_path facts.  New maps may still carry the
    // historical field, but it should not compete with stronger metadata.
    if (!child.enteredFileSpelling) {
      StringRef legacySpelling = legacyResolvedIncludePath(child);
      if (!legacySpelling.empty())
        appendWithPreferredDelimiters(legacySpelling, "legacy-resolved-path");
    }

    return candidates;
  };

  auto directIncludeNextOrdinaryRewriteRejectReason =
      [&](const DirectIncludeNextOrdinaryRewriteCandidate &rewrite,
          const RefoldModel::IncludeItem &child,
          const CleanChildIncludeReplayDemand &demand,
          const IncludeReplayProofResult *proof,
          bool selectedTargetProven) -> const char * {
    if (!rewrite.replay) {
      const char *failure =
          ordinaryIncludeReplayFailureReasonName(rewrite.replayFailure);
      return StringRef(failure) == "none" ? "unresolved" : failure;
    }

    if (!proof)
      return "unresolved";
    if (!proof->samePhysicalFile)
      return "physical-mismatch";

    if (!selectedTargetProven) {
      if (rewrite.replay->Kind == IncludeReplayCandidate::LookupKind::Unknown)
        return "unknown-search-chain-entry";

      if (child.lookup && isSearchChainIncludeLookupKind(child.lookup->kind) &&
          !rewrite.replay->SearchChainIndex)
        return "missing-search-chain-index";

      return "shadowed-by-earlier-search-entry";
    }

    if ((demand.observesFile || demand.hasUnprovenFileSpellingObserver) &&
        !proof->sameEnteredFileSpelling)
      return "entered-file-spelling-mismatch";
    if (demand.observesFileName && !proof->sameEnteredFileName)
      return "entered-file-name-mismatch";
    if (!demand.includeNextObligations.empty() &&
        !proof->sameIncludeNextStack)
      return "descendant-include-next-obligation-failed";

    return "unresolved";
  };

  auto traceDirectIncludeNextOrdinaryRewriteCandidate =
      [&](const DirectIncludeNextOrdinaryRewriteCandidate &rewrite,
          const RefoldModel::IncludeItem &child,
          const CleanChildIncludeReplayDemand &demand,
          const IncludeReplayProofResult *proof, bool selectedTargetProven,
          StringRef decisionReason) {
    if (!inTraceMode())
      return;

    std::optional<std::filesystem::path> producerPhysical =
        producerPhysicalIncludePath(child);
    const std::string producerPhysicalPath =
        producerPhysical ? producerPhysical->generic_string()
                         : std::string("(none)");
    const std::string replayPhysicalPath =
        rewrite.replay ? rewrite.replay->PhysicalPath.generic_string()
                       : std::string("(none)");
    const std::string replayLookupKind =
        rewrite.replay ? std::string(includeReplayLookupKindName(
                             rewrite.replay->Kind))
                       : std::string("(none)");
    const std::string replaySearchChainIndex =
        rewrite.replay
            ? optionalSearchChainIndexForTrace(
                  rewrite.replay->SearchChainIndex)
            : std::string("(none)");
    const std::string producerLookupKind =
        child.lookup ? toString(child.lookup->kind).str()
                     : std::string("(none)");
    const std::string producerSearchChainIndex =
        child.lookup ? optionalSearchChainIndexForTrace(
                           child.lookup->searchChainIndex)
                     : std::string("(none)");
    const std::string producerIncludeNextSelectedIndex =
        (child.includeNext && child.includeNext->selectedSearchChainIndex)
            ? std::to_string(*child.includeNext->selectedSearchChainIndex)
            : std::string("(none)");
    const std::string descendantObligationResult =
        demand.includeNextObligations.empty()
            ? std::string("none")
            : (!proof ? std::string("not-evaluated")
                      : (proof->sameIncludeNextStack ? std::string("proven")
                                                     : std::string("failed")));

    // Keep this as a single, stable trace record per generated hypothesis.
    // Candidate generation is deliberately broad, so the trace names both the
    // syntactic candidate and every proof fact that can reject it.
    REFOLD_LOG_TRACE(
        "include/next-replay",
        "direct relocated #include_next candidate include_id={0} "
        "origin={1} delimiter={2} operand={3}{4}{5} "
        "replay_physical={6} producer_physical={7} "
        "replay_lookup={8}/{9} producer_lookup={10}/{11} "
        "producer_include_next_selected_index={12} "
        "observers(file={13}, file_name={14}, unproven_file={15}) "
        "descendant_include_next(count={16}, result={17}) "
        "selected_target_proven={18} decision={19}",
        child.id, rewrite.origin,
        IncludeReplayProofContext::ordinaryIncludeDelimiterName(
            rewrite.delimiterKind),
        IncludeReplayProofContext::ordinaryIncludeDelimiterOpen(
            rewrite.delimiterKind),
        rewrite.operand,
        IncludeReplayProofContext::ordinaryIncludeDelimiterClose(
            rewrite.delimiterKind),
        replayPhysicalPath, producerPhysicalPath, replayLookupKind,
        replaySearchChainIndex, producerLookupKind, producerSearchChainIndex,
        producerIncludeNextSelectedIndex, demand.observesFile,
        demand.observesFileName, demand.hasUnprovenFileSpellingObserver,
        demand.includeNextObligations.size(), descendantObligationResult,
        selectedTargetProven, decisionReason);
  };

    CleanChildIncludeReplayPlan plan;

    if (!child.parent)
      return plan;

    const bool isDirectIncludeNext = child.subkind == "#include_next";

    // Ordinary clean includes may still be safely left in place on old or
    // partially-populated maps when no entered-file spelling witness exists: the
    // directive itself remains an ordinary include, so the existing conservative
    // behavior is to avoid inventing a repair.  A direct #include_next is
    // different once its parent is materialized.  Preserving that directive would
    // replay it from a different HeaderSearch cursor, so it must either be
    // rewritten to a separately-proven ordinary include or materialized.
    if (!isDirectIncludeNext && producerEnteredFileSpelling(child).empty())
      return plan;

    const CleanChildIncludeReplayDemand demand =
        cleanChildIncludeReplayDemand(child.id);


    auto markMaterializationIfIncludeNextObligationsRemain = [&] {
      // If a clean child is ultimately materialized while its subtree contains
      // descendant #include_next directives, those directives are no longer in
      // their producer header-search context.  Unless the whole child include is
      // preserved as an include directive, recursive materialization must
      // realize nested include_next edges instead of leaving replay-sensitive
      // directives behind in the relocated text.
      if (!demand.includeNextObligations.empty())
        plan.forceMaterializeDescendantIncludeNext = true;
    };

    if (isDirectIncludeNext && !producerPhysicalIncludePath(child)) {
      plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
      plan.reason = "#include_next has no producer physical target for "
                    "ordinary-include rewrite proof";
      markMaterializationIfIncludeNextObligationsRemain();
      return plan;
    }

    if (isDirectIncludeNext &&
        !directIncludeNextHasProducerSelectionProvenance(child)) {
      plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
      plan.reason = "#include_next has no complete producer search-chain "
                    "selection proof for ordinary-include rewrite";
      markMaterializationIfIncludeNextObligationsRemain();
      return plan;
    }

    // __INCLUDE_LEVEL__ observes include-stack depth.  Physical identity and
    // entered-file spelling do not prove that a relocated clean child include
    // has the same stack depth as the producer run, so the child must be
    // realized through the existing materialization/line-state repair path.
    if (demand.observesIncludeLevel) {
      plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
      plan.reason = "child subtree observes __INCLUDE_LEVEL__ across "
                    "materialized-parent include relocation";
      markMaterializationIfIncludeNextObligationsRemain();
      return plan;
    }

    // __BASE_FILE__ observes the top-level preprocessing file, not the entered
    // child header.  Include operand replay can prove only the physical child
    // file and, for __FILE__/__FILE_NAME__, the child entered-file spelling.
    if (demand.observesBaseFile) {
      plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
      plan.reason = "child subtree observes __BASE_FILE__ across "
                    "materialized-parent include relocation";
      markMaterializationIfIncludeNextObligationsRemain();
      return plan;
    }

    // Do not special-case angled #include_next to materialization here.  Direct
    // #include_next directives are handled uniformly: copying one as
    // #include_next would require the original containing-file HeaderSearch
    // cursor, which a materialized parent surface does not have.  The safe
    // include-preserving alternative is therefore a separately proven ordinary
    // include rewrite; if that exact replay proof fails below, the child is
    // materialized.

    if (child.subkind == "#include" && child.angled) {
      if (!demand.requiresReplayCandidateProof())
        return plan;

      std::optional<std::string> operand =
          angledIncludeReplayLookupOperand(child);
      if (!operand) {
        plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
        plan.reason = "angled child include operand is not replay-provable "
                      "from materialized parent surface";
        markMaterializationIfIncludeNextObligationsRemain();
        return plan;
      }

      std::optional<IncludeReplayCandidate> candidate =
          computeAngledIncludeReplayCandidate(*operand);
      if (!candidate) {
        plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
        plan.reason = "angled child include has no replay-surface lookup "
                      "target";
        markMaterializationIfIncludeNextObligationsRemain();
        return plan;
      }

      IncludeReplayProofResult proof =
          evaluateIncludeReplayCandidate(*candidate, child, demand);
      if (proof.proves(demand))
        return plan;

      plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
      if (!proof.samePhysicalFile)
        plan.reason = "angled child include resolves to a different file at "
                      "replay surface";
      else if (!proof.sameIncludeNextStack)
        plan.reason = "angled child include resolves to the same physical "
                      "file but does not prove descendant #include_next "
                      "resume-stack stability";
      else
        plan.reason = "angled child include resolves to the same physical "
                      "file but does not prove file-observer spelling "
                      "stability";
      markMaterializationIfIncludeNextObligationsRemain();
      return plan;
    }

    if (isDirectIncludeNext) {
      // A direct #include_next is itself the search-stack-sensitive edge being
      // relocated.  Preserving the directive text would require proving that the
      // final containing file was selected by the same HeaderSearch entry, so
      // that lookup resumes at the same cursor.  In this materialized-parent
      // path the containing header has been flattened into another source
      // surface, so there is no include edge whose replay provenance can carry
      // that cursor.
      //
      // The safe include-preserving alternative is a deterministic list of
      // ordinary-include spellings for the producer-selected target.  Each
      // spelling is only a hypothesis: it is immediately replayed from the
      // final .c.mod surface by computeOrdinaryIncludeReplayResult(), and then
      // the proof layer checks physical identity, the producer-selected lookup
      // kind/search-chain index, observer spelling, and descendant include_next
      // cursor obligations before accepting it.
      std::string reason = "#include_next cannot be replayed from a "
                           "materialized parent surface without the original "
                           "include-next search stack";

      SmallVector<DirectIncludeNextOrdinaryRewriteCandidate, 16> rewrites =
          generateDirectIncludeNextOrdinaryRewriteCandidates(child);
      std::string firstRejectedCandidateReason;
      for (const DirectIncludeNextOrdinaryRewriteCandidate &rewrite :
           rewrites) {
        std::optional<IncludeReplayProofResult> proof;
        bool selectedTargetProven = false;

        if (rewrite.replay) {
          proof = evaluateIncludeReplayCandidate(*rewrite.replay, child, demand);
          selectedTargetProven =
              directIncludeNextOrdinaryRewriteMatchesProducerSelection(
                  *rewrite.replay, child, demand);
          if (selectedTargetProven && proof->proves(demand)) {
            traceDirectIncludeNextOrdinaryRewriteCandidate(
                rewrite, child, demand, &*proof, selectedTargetProven,
                "accepted");
            plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::RewriteOperand;
            plan.rewrittenOperand = rewrite.operand;
            plan.rewrittenDelimiterKind = rewrite.delimiterKind;
            plan.reason = std::move(reason);
            return plan;
          }
        }

        const char *rejectReason = directIncludeNextOrdinaryRewriteRejectReason(
            rewrite, child, demand, proof ? &*proof : nullptr,
            selectedTargetProven);
        traceDirectIncludeNextOrdinaryRewriteCandidate(
            rewrite, child, demand, proof ? &*proof : nullptr,
            selectedTargetProven, rejectReason);

        // Keep the materialization explanation concise, but base it on the same
        // stable rejection vocabulary emitted by the trace record.  The full
        // per-candidate proof ledger is intentionally available only at trace
        // level so normal diagnostics do not become enormous.
        if (firstRejectedCandidateReason.empty()) {
          firstRejectedCandidateReason = "; first ordinary include candidate ";
          firstRejectedCandidateReason += IncludeReplayProofContext::ordinaryIncludeDelimiterName(
              rewrite.delimiterKind);
          firstRejectedCandidateReason += " ";
          firstRejectedCandidateReason += IncludeReplayProofContext::ordinaryIncludeDelimiterOpen(
              rewrite.delimiterKind);
          firstRejectedCandidateReason += rewrite.operand;
          firstRejectedCandidateReason += IncludeReplayProofContext::ordinaryIncludeDelimiterClose(
              rewrite.delimiterKind);
          firstRejectedCandidateReason += " from ";
          firstRejectedCandidateReason += rewrite.origin;
          firstRejectedCandidateReason += " rejected: ";
          firstRejectedCandidateReason += rejectReason;
        }
      }

      if (rewrites.empty())
        reason += "; no ordinary include candidate replays from the final "
                  ".c.mod surface";
      else
        reason += firstRejectedCandidateReason;

      plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
      plan.reason = std::move(reason);
      markMaterializationIfIncludeNextObligationsRemain();
      return plan;
    }

    bool mayRewriteUnstableOperand = false;
    std::string reason;

    std::optional<std::string> childReplayOperand =
        quotedIncludeReplayLookupOperand(child);
    if (!childReplayOperand) {
      plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
      plan.reason = "quoted child include operand is not replay-provable "
                    "from materialized parent surface";
      markMaterializationIfIncludeNextObligationsRemain();
      return plan;
    }

    std::optional<IncludeReplayCandidate> replayCandidate =
        computeQuotedIncludeReplayCandidate(*childReplayOperand);
    if (replayCandidate) {
      IncludeReplayProofResult proof =
          evaluateIncludeReplayCandidate(*replayCandidate, child, demand);
      if (proof.proves(demand))
        return plan;

      if (proof.samePhysicalFile) {
        // Physical-file equality is enough for observer-free and line-only
        // subtrees, but not for preserved __FILE__/__FILE_NAME__ observers
        // or descendant #include_next cursor obligations.  Try a direct
        // quoted rewrite that names the producer-observed file spelling;
        // materialize if that exact candidate replay still cannot discharge
        // the active observer/search-stack proof.
        mayRewriteUnstableOperand = true;
        if (!proof.sameIncludeNextStack)
          reason = "quoted operand resolves to the same physical file but "
                   "does not prove descendant #include_next resume-stack "
                   "stability";
        else
          reason = "quoted operand resolves to the same physical file but "
                   "does not prove file-observer spelling stability";
      } else {
        // A concrete replay-surface hit for the original quoted operand is a
        // real shadowing conflict.  Rewriting around that shadow would invent
        // a third lookup event rather than preserving the producer-observed
        // event, so the owner-closed repair is to materialize the child edge.
        reason = "quoted operand resolves to a different file at replay "
                 "surface";
      }
    } else {
      // The original quoted operand has no replay-surface lookup target, so
      // there is no concrete TU-surface shadow to bypass.  Whether the operand
      // was a parent-directory spelling (`../leaf.h`) or a simple
      // header-relative spelling (`leaf.h`), materializing the parent has
      // removed the original including-header directory from quoted lookup.
      // A deterministic TU-surface operand naming the producer-recorded file
      // is therefore an owner-closed include-preserving repair.  The rewrite
      // validation below still requires the exact synthesized candidate replay
      // to satisfy the physical and file-spelling observer proofs.
      mayRewriteUnstableOperand = true;
      reason = includeOperandHasParentComponent(*childReplayOperand)
                   ? "parent-directory quoted operand has no direct replay "
                     "target"
                   : "quoted operand has no replay-surface lookup target";
    }

    if (mayRewriteUnstableOperand) {
      bool sawRewriteCandidate = false;
      std::string firstRewriteFailure;
      for (QuotedChildIncludeRewriteCandidate &rewrite :
           generateQuotedChildIncludeRewriteCandidates(child)) {
        sawRewriteCandidate = true;
        IncludeReplayProofResult proof =
            evaluateIncludeReplayCandidate(rewrite.replay, child, demand);
        if (proof.proves(demand)) {
          plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::RewriteOperand;
          plan.rewrittenOperand = std::move(rewrite.operand);
          plan.reason = std::move(reason);
          return plan;
        }

        // Rewriting the include operand can change the presumed file spelling
        // observed inside the child even when it names the same physical file.
        // Only use an include-preserving rewrite when that exact replay is
        // proven equivalent under the active observer family; otherwise stay
        // owner-closed by materializing the child text.  Keep the first
        // rejection reason only to avoid noisy diagnostics while trace logging
        // remains available for detailed replay failures elsewhere.
        if (firstRewriteFailure.empty()) {
          if (!proof.samePhysicalFile)
            firstRewriteFailure =
                "rewritten operand does not replay the producer physical "
                "child file";
          else if (!proof.sameIncludeNextStack)
            firstRewriteFailure =
                "rewritten operand does not prove descendant #include_next "
                "resume-stack stability";
          else
            firstRewriteFailure =
                "rewritten operand does not prove file-observer spelling "
                "stability";
        }
      }

      if (!reason.empty())
        reason += "; ";
      if (!sawRewriteCandidate)
        reason += "no synthesized quoted operand replays from the final "
                  ".c.mod surface";
      else
        reason += firstRewriteFailure;
    }

    plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
    plan.reason = std::move(reason);
    markMaterializationIfIncludeNextObligationsRemain();
    return plan;

}

} // namespace refold
} // namespace clang
