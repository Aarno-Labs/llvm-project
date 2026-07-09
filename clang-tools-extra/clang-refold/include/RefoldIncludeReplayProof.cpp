//===--- RefoldIncludeReplayProof.cpp ---------------------------*- C++ -*-===//
//
// Include/include_next replay proof context implementation.
//
// This file consumes immutable proof inputs and explicit read-only services to
// decide whether an include edge can be replayed from the final source surface.
// Materialization and edit emission are handled by callers after the proof
// layer returns a plan.
//
//===----------------------------------------------------------------------===//

#include "include/RefoldIncludeReplayProof.h"

#include "core/RefoldLog.h"
#include "include/IncludeSpellingHelpers.h"
#include "line-control/RefoldLineControlFilename.h"

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
  surface.outputPath = outputPath.lexically_normal();
  surface.outputDirectory = surface.outputPath.parent_path();
  surface.originalWorkingDirectory =
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
  std::filesystem::path relativeOutputDirectory = std::filesystem::relative(
      surface.outputDirectory, surface.originalWorkingDirectory, relativeEC);
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
      surface.outputDirectorySpelling =
          relativeOutputDirectory == std::filesystem::path(".")
              ? std::string(".")
              : relativeOutputDirectory.generic_string();
    }
  }

  if (surface.outputDirectorySpelling.empty()) {
    SmallString<256> outputDirectorySpelling(
        surface.outputPath.generic_string());
    llvm::sys::path::remove_filename(outputDirectorySpelling);
    surface.outputDirectorySpelling = outputDirectorySpelling.empty()
                                          ? std::string(".")
                                          : outputDirectorySpelling.str().str();
  }

  REFOLD_LOG_DEBUG("include/replay",
                   "final replay surface: output='{0}' dir='{1}' cwd='{2}'",
                   surface.outputPath.generic_string(),
                   surface.outputDirectory.generic_string(),
                   surface.originalWorkingDirectory.generic_string());
  return surface;
}

namespace {

// Replay-only include operand spelling helpers remain local to this proof
// module.  Shared synthesized-operand safety helpers live in
// IncludeSpellingHelpers.h, and producer include identity/spelling helpers live
// in proof/RefoldProofVocabulary.h so proof modules use the same schema
// fallback rules.
static bool isSafeReplayIncludePathChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
         c == '.' || c == '/';
}

/// Return true when \p path uses only the restricted ASCII include-path
/// spelling alphabet that replay analysis is willing to inspect.  This is a
/// spelling-level predicate only: replay proof still has to compare the
/// selected physical file and observer spelling against producer metadata.
static bool hasSafeReplayIncludePathSpelling(StringRef path) {
  return !path.empty() && !path.contains('\\') && !path.contains('"') &&
         llvm::all_of(path,
                      [](char c) { return isSafeReplayIncludePathChar(c); });
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

/// Return the directory spelling Clang derives from an entered source file.
/// The value is intentionally spelling-oriented rather than canonicalized: it
/// feeds __FILE__ observer proof for direct quoted include replay.
static std::string sourceDirectorySpellingForFile(StringRef fileSpelling) {
  SmallString<256> source(fileSpelling);
  llvm::sys::path::remove_filename(source);
  if (source.empty())
    return std::string(".");
  return source.str().str();
}

/// Append a replay operand to an entered search-directory spelling exactly as
/// Clang would form the entered filename for an ordinary include hit.
static std::string appendEnteredFileSpelling(StringRef prefix,
                                             StringRef operand) {
  SmallString<256> spelling(prefix);
  llvm::sys::path::append(spelling, operand);
  return spelling.str().str();
}

/// Return an angled include operand for lookup analysis.  Unlike emitted
/// rewrite operands, replay lookup operands may still be rejected later by the
/// physical-target and producer-provenance proof checks.
static std::optional<std::string>
angledIncludeReplayLookupOperand(const RefoldModel::IncludeItem &inc) {
  if (!inc.angled)
    return std::nullopt;

  StringRef target = inc.target;
  if (target.size() < 2 || target.front() != '<' || target.back() != '>')
    return std::nullopt;

  StringRef path = target.drop_front().drop_back();
  if (!hasSafeReplayIncludePathSpelling(path))
    return std::nullopt;

  return path.str();
}

/// Format an optional HeaderSearch index for deterministic trace output.
static std::string
optionalSearchChainIndexForTrace(std::optional<uint32_t> index) {
  if (!index)
    return "(none)";
  return std::to_string(*index);
}

/// Syntax gate for ordinary include operands that may be written back into the
/// final source.  This deliberately permits absolute and dot-component
/// spellings because replay/provenance checks, not syntax alone, decide
/// semantic admissibility.
static bool safeOrdinaryIncludeRewriteOperand(StringRef path) {
  return safeReplayIncludeLookupOperandPath(path);
}

/// Return whether a physical lookup path exists using the exact generic
/// spelling that replay evaluation passes to LLVM's filesystem layer.  This is
/// context-free: callers must resolve relative spellings against the correct
/// replay surface before probing.
static bool pathExists(const std::filesystem::path &path) {
  const std::string spelling = path.generic_string();
  return llvm::sys::fs::exists(StringRef(spelling));
}

/// Append value only if the same spelling is not already present.  Demand
/// construction uses this to keep observer payload lists deterministic without
/// turning duplicate macro observations into separate proof obligations.
static void appendUniqueString(SmallVectorImpl<std::string> &values,
                               StringRef value) {
  if (!llvm::any_of(values, [&](const std::string &existing) {
        return StringRef(existing) == value;
      }))
    values.push_back(value.str());
}

/// Decode one producer-emitted file-observer string literal payload.  The parse
/// must consume the whole trimmed spelling so malformed or concatenated tokens
/// remain unproven instead of partially decoded.
static std::optional<std::string>
decodeObservedFileStringLiteralPayload(StringRef spelling) {
  StringRef rest = spelling.trim();
  std::optional<std::string> decoded = parseLineControlFilenameLiteral(rest);
  if (!decoded || !rest.trim().empty())
    return std::nullopt;
  return decoded;
}

} // namespace

// Convert producer lookup provenance to the ordinary directory replay kinds
// modeled by include replay proof.  This is a private context helper because
// the return type is the context's private replay-candidate carrier.
std::optional<IncludeReplayProofContext::IncludeReplayCandidate::LookupKind>
IncludeReplayProofContext::ReplayableDirectoryLookupKind(
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

const char *IncludeReplayProofContext::OrdinaryIncludeReplayFailureReasonName(
    OrdinaryIncludeReplayFailureKind kind) {
  switch (kind) {
  case OrdinaryIncludeReplayFailureKind::None:
    return "none";
  case OrdinaryIncludeReplayFailureKind::Unresolved:
    return "unresolved";
  case OrdinaryIncludeReplayFailureKind::UnknownSearchChainEntry:
    return "unknown-search-chain-entry";
  }
  llvm_unreachable("Invalid OrdinaryIncludeReplayFailureKind");
}

const char *IncludeReplayProofContext::IncludeReplayLookupKindName(
    IncludeReplayCandidate::LookupKind kind) {
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
}

/// Return whether a direct `#include_next` carries the full producer cursor
/// and selected-entry facts needed for an ordinary-include rewrite proof.
static bool directIncludeNextHasProducerSelectionProvenance(
    const RefoldModel::IncludeItem &include) {
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
}

std::filesystem::path
IncludeReplayProofContext::AbsolutePathInPPCwd(StringRef path) const {
  std::filesystem::path fsPath(path.str());
  if (fsPath.is_absolute())
    return fsPath;
  return std::filesystem::path(model_.GetPPCwd().str()) / fsPath;
}

std::optional<std::filesystem::path>
IncludeReplayProofContext::CanonicalizeForLookupProof(StringRef path) const {
  std::error_code ec;
  std::filesystem::path canonical =
      std::filesystem::weakly_canonical(AbsolutePathInPPCwd(path), ec);
  if (ec)
    return std::nullopt;
  return canonical;
}

std::optional<IncludeReplayProofContext::IncludeReplaySurface>
IncludeReplayProofContext::IncludeReplaySurfaceForFile(
    StringRef fileSpelling) const {
  std::optional<std::filesystem::path> source =
      CanonicalizeForLookupProof(fileSpelling);
  if (!source)
    return std::nullopt;

  IncludeReplaySurface surface;
  surface.sourceDirectoryPath = *source;
  surface.sourceDirectoryPath.remove_filename();
  surface.sourceDirectorySpelling =
      sourceDirectorySpellingForFile(fileSpelling);
  return surface;
}

std::optional<IncludeReplayProofContext::IncludeReplaySurface>
IncludeReplayProofContext::FinalOutputIncludeReplaySurface() const {
  if (!finalReplaySurface_)
    return std::nullopt;

  IncludeReplaySurface surface;
  surface.sourceDirectoryPath = finalReplaySurface_->outputDirectory;
  surface.sourceDirectorySpelling =
      finalReplaySurface_->outputDirectorySpelling;
  return surface;
}

std::optional<std::string>
IncludeReplayProofContext::StableContainedRelativeOperand(
    const std::filesystem::path &target,
    const std::filesystem::path &base) const {
  // Candidate classes derived from physical containment must not synthesize an
  // escaping path.  If target is not actually below base, the relative
  // computation either fails, is absolute, or contains `..`; all of those are
  // rejected before replay.
  std::optional<std::filesystem::path> canonicalTarget =
      CanonicalizeForLookupProof(target.generic_string());
  std::optional<std::filesystem::path> canonicalBase =
      CanonicalizeForLookupProof(base.generic_string());
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
}

std::optional<IncludeReplayProofContext::IncludeReplaySearchDir>
IncludeReplayProofContext::MakeIncludeReplaySearchDir(
    StringRef physicalPath, StringRef enteredSpellingPrefix,
    IncludeReplayCandidate::LookupKind kind,
    std::optional<uint32_t> searchChainIndex) const {
  if (physicalPath.empty() || enteredSpellingPrefix.empty() ||
      kind == IncludeReplayCandidate::LookupKind::Unknown)
    return std::nullopt;

  IncludeReplaySearchDir dir;
  dir.lookupPath = AbsolutePathInPPCwd(physicalPath);
  dir.enteredSpellingPrefix = enteredSpellingPrefix.str();
  dir.kind = kind;
  dir.searchChainIndex = searchChainIndex;
  return dir;
}

void IncludeReplayProofContext::AppendLegacySearchDirIfSafe(
    SmallVectorImpl<IncludeReplaySearchDir> &dirs, StringRef path,
    IncludeReplayCandidate::LookupKind kind) const {
  // Legacy argv reconstruction has only one spelling/path token per entry.
  // Use that token for both the physical lookup directory and the entered-file
  // spelling prefix, matching the old-map behavior.  The search-chain index is
  // intentionally empty: argv reconstruction is a compatibility replay aid for
  // ordinary includes, not producer-proven HeaderSearch cursor state for
  // #include_next.
  if (std::optional<IncludeReplaySearchDir> dir =
          MakeIncludeReplaySearchDir(path, path, kind))
    dirs.push_back(std::move(*dir));
}

void IncludeReplayProofContext::AppendUnsupportedOrdinarySearchEntryBarrier(
    RecordedIncludeSearchDirs &dirs,
    const RefoldModel::IncludeSearchEntry &entry) {
  IncludeReplaySearchDir barrier;
  barrier.kind = IncludeReplayCandidate::LookupKind::Unknown;
  barrier.searchChainIndex = entry.index;
  barrier.isUnsupportedBarrier = true;

  // Once ordinary replay reaches an unmodeled HeaderSearch entry, the selected
  // file is unknown: the entry could contain a header map, framework,
  // builtin/VFS-only header, or another producer-side lookup mechanism that is
  // not serialized well enough for deterministic consumer replay.  Preserve
  // the entry's ordinary-include participation when installing the barrier; a
  // quote-only entry cannot shadow an angled include, but every other
  // search-chain entry can.
  dirs.quotedLookupDirs.push_back(barrier);
  if (entry.kind != IncludeLookupKind::QuoteDir)
    dirs.angledLookupDirs.push_back(barrier);
}

void IncludeReplayProofContext::AppendProducerSearchEntryIfSafe(
    RecordedIncludeSearchDirs &dirs,
    const RefoldModel::IncludeSearchEntry &entry) const {
  // New maps carry both the physical/FileManager directory path and the
  // spelling Clang used for the search entry.  Keep those separated so ordinary
  // include replay no longer derives __FILE__ spelling from a canonicalized
  // filesystem path.  For search-chain hits, also carry the exact producer
  // index that selected the target; later include_next proof must compare that
  // cursor against producer resume provenance.
  std::optional<IncludeReplayCandidate::LookupKind> kind =
      ReplayableDirectoryLookupKind(entry.kind);
  if (!kind) {
    AppendUnsupportedOrdinarySearchEntryBarrier(dirs, entry);
    return;
  }

  if (std::optional<IncludeReplaySearchDir> dir = MakeIncludeReplaySearchDir(
          entry.path, entry.spelling, *kind, entry.index)) {
    if (entry.kind == IncludeLookupKind::QuoteDir) {
      dirs.quotedLookupDirs.push_back(*dir);
    } else {
      dirs.quotedLookupDirs.push_back(*dir);
      dirs.angledLookupDirs.push_back(*dir);
    }
  } else {
    // A search-chain entry with missing or otherwise unusable path/spelling
    // data is just as unprovable as an unknown entry.  Preserve lookup order by
    // turning it into a barrier instead of dropping it.
    AppendUnsupportedOrdinarySearchEntryBarrier(dirs, entry);
  }
}

bool IncludeReplayProofContext::TryConsumeJoinedOrSeparateIncludeArg(
    StringRef arg, ArrayRef<std::string> argv, size_t &index,
    StringRef joinedPrefix, SmallVectorImpl<IncludeReplaySearchDir> &out,
    IncludeReplayCandidate::LookupKind kind) const {
  if (arg == joinedPrefix) {
    if (index + 1 < argv.size())
      AppendLegacySearchDirIfSafe(out, argv[++index], kind);
    return true;
  }
  if (arg.starts_with(joinedPrefix) && arg.size() > joinedPrefix.size()) {
    AppendLegacySearchDirIfSafe(out, arg.drop_front(joinedPrefix.size()), kind);
    return true;
  }
  return false;
}

IncludeReplayProofContext::RecordedIncludeSearchDirs
IncludeReplayProofContext::ComputeProducerIncludeSearchDirs() const {
  RecordedIncludeSearchDirs dirs;

  for (const RefoldModel::IncludeSearchEntry &entry :
       model_.GetIncludeSearchChain()) {
    switch (entry.kind) {
    case IncludeLookupKind::QuoteDir:
      // -iquote entries participate only in quoted include lookup.
      AppendProducerSearchEntryIfSafe(dirs, entry);
      break;
    case IncludeLookupKind::UserI:
    case IncludeLookupKind::System:
    case IncludeLookupKind::IdirAfter:
      // Non-quote search-chain entries participate in angled lookup and, after
      // source-relative/-iquote lookup has failed, quoted lookup.
      AppendProducerSearchEntryIfSafe(dirs, entry);
      break;
    case IncludeLookupKind::Framework:
    case IncludeLookupKind::Builtin:
    case IncludeLookupKind::Unknown:
      // The ordinary replay evaluator is intentionally limited to concrete
      // directory search entries serialized by the producer.  Unsupported or
      // unknown entries are retained as lookup barriers so replay fails closed
      // if it would have to reason past one.
      AppendProducerSearchEntryIfSafe(dirs, entry);
      break;
    case IncludeLookupKind::SourceRelative:
    case IncludeLookupKind::AbsoluteOperand:
      llvm_unreachable("per-edge include lookup kind in search chain");
    }
  }

  return dirs;
}

IncludeReplayProofContext::RecordedIncludeSearchDirs
IncludeReplayProofContext::ComputeLegacyArgvIncludeSearchDirs() const {
  SmallVector<IncludeReplaySearchDir, 16> quoteDirs;
  SmallVector<IncludeReplaySearchDir, 32> includeDirs;
  ArrayRef<std::string> argv = model_.GetPPArgv();

  for (size_t i = 0; i < argv.size(); ++i) {
    StringRef arg(argv[i]);
    if (TryConsumeJoinedOrSeparateIncludeArg(
            arg, argv, i, "-iquote", quoteDirs,
            IncludeReplayCandidate::LookupKind::QuoteDir))
      continue;
    if (TryConsumeJoinedOrSeparateIncludeArg(
            arg, argv, i, "-I", includeDirs,
            IncludeReplayCandidate::LookupKind::UserI))
      continue;
    if (arg == "-isystem" && i + 1 < argv.size()) {
      AppendLegacySearchDirIfSafe(includeDirs, argv[++i],
                                  IncludeReplayCandidate::LookupKind::System);
      continue;
    }
    if (arg == "-idirafter" && i + 1 < argv.size()) {
      AppendLegacySearchDirIfSafe(
          includeDirs, argv[++i],
          IncludeReplayCandidate::LookupKind::IdirAfter);
      continue;
    }
    const StringRef isystemPrefix("-isystem");
    const StringRef idirafterPrefix("-idirafter");
    if (arg.starts_with(isystemPrefix) && arg.size() > isystemPrefix.size()) {
      AppendLegacySearchDirIfSafe(includeDirs,
                                  arg.drop_front(isystemPrefix.size()),
                                  IncludeReplayCandidate::LookupKind::System);
      continue;
    }
    if (arg.starts_with(idirafterPrefix) &&
        arg.size() > idirafterPrefix.size()) {
      AppendLegacySearchDirIfSafe(
          includeDirs, arg.drop_front(idirafterPrefix.size()),
          IncludeReplayCandidate::LookupKind::IdirAfter);
      continue;
    }
  }

  RecordedIncludeSearchDirs dirs;
  // Preserve the legacy replay order: quoted lookup searches -iquote first,
  // then the ordinary include dirs; angled lookup uses only the ordinary
  // include dirs.
  dirs.quotedLookupDirs.append(quoteDirs.begin(), quoteDirs.end());
  dirs.quotedLookupDirs.append(includeDirs.begin(), includeDirs.end());
  dirs.angledLookupDirs.append(includeDirs.begin(), includeDirs.end());
  return dirs;
}

IncludeReplayProofContext::RecordedIncludeSearchDirs
IncludeReplayProofContext::ComputeRecordedIncludeSearchDirs() const {
  // Prefer the producer-normalized HeaderSearch chain when present.  It is
  // already parsed and validated by RefoldModel, and unlike argv parsing it
  // preserves Clang's effective order plus each entry's physical path and
  // entered spelling.  The argv parser remains only for old maps that lack
  // pp_ctx.include_search_chain.
  if (!model_.GetIncludeSearchChain().empty())
    return ComputeProducerIncludeSearchDirs();
  return ComputeLegacyArgvIncludeSearchDirs();
}

const IncludeReplayProofContext::RecordedIncludeSearchDirs &
IncludeReplayProofContext::RecordedIncludeSearchDirsForReplay() const {
  if (!recordedIncludeSearchDirsCache_)
    recordedIncludeSearchDirsCache_ = ComputeRecordedIncludeSearchDirs();
  return *recordedIncludeSearchDirsCache_;
}

std::string IncludeReplayProofContext::DirectSourceRelativeEnteredFileSpelling(
    const IncludeReplaySurface &surface, StringRef operand) {
  // Direct quoted lookup preserves the including-file directory spelling and
  // appends the operand spelling; it is not a cwd-relative normalization of
  // the physical result.  Examples observed from Clang:
  //   original.c + "headers/child.h" -> "./headers/child.h"
  //   ./original.c + "headers/child.h" -> "./headers/child.h"
  //   sub/original.c + "../leaf.h" -> "sub/../leaf.h"
  //   /abs/original.c + "leaf.h" -> "/abs/leaf.h"
  return appendEnteredFileSpelling(surface.sourceDirectorySpelling, operand);
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::ComputeAbsoluteIncludeReplayCandidate(
    StringRef operand) {
  std::filesystem::path operandPath(operand.str());
  if (!operandPath.is_absolute())
    return std::nullopt;
  if (!pathExists(operandPath))
    return std::nullopt;

  IncludeReplayCandidate candidate;
  candidate.physicalPath = operandPath.lexically_normal();
  candidate.enteredFileSpelling = operandPath.generic_string();
  candidate.enteredFileName =
      stringutils::pathBasename(candidate.enteredFileSpelling).str();
  candidate.kind = IncludeReplayCandidate::LookupKind::AbsoluteOperand;
  return candidate;
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::CandidateFromSearchDir(
    const IncludeReplaySearchDir &dir, StringRef operand) {
  if (dir.isUnsupportedBarrier)
    return std::nullopt;

  // Check the exact filesystem path Clang would probe before any lexical
  // cleanup.  Collapsing `missing/../leaf.h` first is unsound: POSIX path
  // resolution and Clang header lookup must be able to enter every prefix
  // component before `..` can escape it.
  std::filesystem::path physical = dir.lookupPath / operand.str();
  if (!pathExists(physical))
    return std::nullopt;

  IncludeReplayCandidate candidate;
  candidate.physicalPath = physical.lexically_normal();
  candidate.enteredFileSpelling =
      appendEnteredFileSpelling(dir.enteredSpellingPrefix, operand);
  candidate.enteredFileName =
      stringutils::pathBasename(candidate.enteredFileSpelling).str();
  candidate.kind = dir.kind;
  candidate.searchChainIndex = dir.searchChainIndex;
  return candidate;
}

IncludeReplayProofContext::OrdinaryIncludeReplayResult
IncludeReplayProofContext::OrdinaryIncludeResultFromCandidate(
    const IncludeReplayCandidate &candidate) {
  OrdinaryIncludeReplayResult result;
  result.physicalPath = candidate.physicalPath;
  result.enteredFileSpelling = candidate.enteredFileSpelling;
  result.enteredFileName =
      !candidate.enteredFileName.empty()
          ? candidate.enteredFileName
          : stringutils::pathBasename(candidate.enteredFileSpelling).str();
  result.lookupKind = candidate.kind;
  result.searchChainIndex = candidate.searchChainIndex;
  return result;
}

IncludeReplayProofContext::IncludeReplayCandidate
IncludeReplayProofContext::IncludeReplayCandidateFromOrdinaryResult(
    const OrdinaryIncludeReplayResult &result) {
  IncludeReplayCandidate candidate;
  candidate.physicalPath = result.physicalPath;
  candidate.enteredFileSpelling = result.enteredFileSpelling;
  candidate.enteredFileName = result.enteredFileName;
  candidate.kind = result.lookupKind;
  candidate.searchChainIndex = result.searchChainIndex;
  return candidate;
}

std::optional<IncludeReplayProofContext::OrdinaryIncludeReplayResult>
IncludeReplayProofContext::ComputeOrdinaryIncludeReplayResult(
    StringRef operand, OrdinaryIncludeDelimiterKind delimiterKind,
    const IncludeReplaySurface *quotedSurface,
    OrdinaryIncludeReplayFailureKind *failureKind) const {
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
    return OrdinaryIncludeResultFromCandidate(candidate);
  };

  if (failureKind)
    *failureKind = OrdinaryIncludeReplayFailureKind::Unresolved;

  if (operand.empty())
    return fail(OrdinaryIncludeReplayFailureKind::Unresolved);

  if (std::optional<IncludeReplayCandidate> absolute =
          ComputeAbsoluteIncludeReplayCandidate(operand))
    return success(*absolute);

  const RecordedIncludeSearchDirs &dirs = RecordedIncludeSearchDirsForReplay();
  auto replayThroughSearchDirs =
      [&](ArrayRef<IncludeReplaySearchDir> lookupDirs)
      -> std::optional<OrdinaryIncludeReplayResult> {
    for (const IncludeReplaySearchDir &dir : lookupDirs) {
      if (dir.isUnsupportedBarrier)
        return fail(OrdinaryIncludeReplayFailureKind::UnknownSearchChainEntry);
      if (std::optional<IncludeReplayCandidate> candidate =
              CandidateFromSearchDir(dir, operand))
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
        quotedSurface->sourceDirectoryPath / operand.str();
    if (pathExists(direct)) {
      IncludeReplayCandidate candidate;
      candidate.physicalPath = direct.lexically_normal();
      candidate.enteredFileSpelling =
          DirectSourceRelativeEnteredFileSpelling(*quotedSurface, operand);
      candidate.enteredFileName =
          stringutils::pathBasename(candidate.enteredFileSpelling).str();
      candidate.kind = IncludeReplayCandidate::LookupKind::DirectSourceRelative;
      return success(candidate);
    }

    return replayThroughSearchDirs(dirs.quotedLookupDirs);
  }

  return replayThroughSearchDirs(dirs.angledLookupDirs);
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::ComputeQuotedIncludeReplayCandidateOnSurface(
    StringRef operand, const IncludeReplaySurface &surface) const {
  std::optional<OrdinaryIncludeReplayResult> replay =
      ComputeOrdinaryIncludeReplayResult(
          operand, OrdinaryIncludeDelimiterKind::Quoted, &surface, nullptr);
  if (!replay)
    return std::nullopt;
  return IncludeReplayCandidateFromOrdinaryResult(*replay);
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::ComputeQuotedIncludeReplayCandidate(
    StringRef operand) const {
  // Ordinary quoted includes preserved or synthesized into the final output
  // must replay from the emitted .c.mod location.  Producer-source surfaces
  // are still used by ComputeProducerIncludeReplayCandidate() when recovering
  // producer spelling witnesses, but they are not valid acceptance surfaces
  // for final-source include rewrites.
  std::optional<IncludeReplaySurface> surface =
      FinalOutputIncludeReplaySurface();
  if (!surface)
    return std::nullopt;
  return ComputeQuotedIncludeReplayCandidateOnSurface(operand, *surface);
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::ComputeAngledIncludeReplayCandidate(
    StringRef operand) const {
  std::optional<OrdinaryIncludeReplayResult> replay =
      ComputeOrdinaryIncludeReplayResult(
          operand, OrdinaryIncludeDelimiterKind::Angled, nullptr, nullptr);
  if (!replay)
    return std::nullopt;
  return IncludeReplayCandidateFromOrdinaryResult(*replay);
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::ComputeProducerIncludeReplayCandidate(
    const RefoldModel::IncludeItem &include) const {
  if (include.subkind != "#include")
    return std::nullopt;

  if (include.angled) {
    std::optional<std::string> operand =
        angledIncludeReplayLookupOperand(include);
    if (!operand)
      return std::nullopt;
    return ComputeAngledIncludeReplayCandidate(*operand);
  }

  std::optional<std::string> operand =
      quotedIncludeReplayLookupOperand(include);
  if (!operand)
    return std::nullopt;
  std::optional<IncludeReplaySurface> surface =
      IncludeReplaySurfaceForFile(include.sitePath);
  if (!surface)
    return std::nullopt;
  return ComputeQuotedIncludeReplayCandidateOnSurface(*operand, *surface);
}

std::optional<IncludeReplayProofContext::IncludeNextReplayCandidate>
IncludeReplayProofContext::ComputeIncludeNextReplayCandidate(
    StringRef operand, const IncludeReplaySurface &surface,
    const RefoldModel::IncludeLookupProvenance &containingFile) const {
  // The replay surface is part of the public helper shape because the caller
  // will usually derive containing-file provenance from a replayed include on
  // that surface.  The include_next lookup itself does not use direct
  // source-relative probing; once the containing-file cursor is known, lookup
  // resumes only through pp_ctx.include_search_chain.
  (void)surface;

  if (operand.empty())
    return std::nullopt;

  // A general include_next proof must start from producer-proven HeaderSearch
  // cursor state.  Source-relative, absolute-operand, unknown, and legacy
  // argv-derived containing-file provenance do not identify a search-chain
  // position, so they cannot prove where lookup should resume.
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
        ReplayableDirectoryLookupKind(entry.kind);
    if (!replayKind)
      return std::nullopt;

    std::optional<IncludeReplaySearchDir> dir = MakeIncludeReplaySearchDir(
        entry.path, entry.spelling, *replayKind, entry.index);
    if (!dir)
      return std::nullopt;

    std::optional<IncludeReplayCandidate> selected =
        CandidateFromSearchDir(*dir, operand);
    if (!selected)
      continue;

    if (!selected->searchChainIndex || *selected->searchChainIndex != i)
      return std::nullopt;

    IncludeNextReplayCandidate candidate;
    candidate.physicalPath = selected->physicalPath;
    candidate.enteredFileSpelling = selected->enteredFileSpelling;
    candidate.enteredFileName = selected->enteredFileName;
    candidate.resumeSearchChainIndex = resumeIndex;
    candidate.selectedSearchChainIndex = i;
    candidate.selectedKind = entry.kind;
    return candidate;
  }

  return std::nullopt;
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::IncludeNextReplayAsOrdinaryCandidate(
    const IncludeNextReplayCandidate &selected) {
  IncludeReplayCandidate candidate;
  candidate.physicalPath = selected.physicalPath;
  candidate.enteredFileSpelling = selected.enteredFileSpelling;
  candidate.enteredFileName = selected.enteredFileName;
  std::optional<IncludeReplayCandidate::LookupKind> kind =
      ReplayableDirectoryLookupKind(selected.selectedKind);
  if (!kind)
    return std::nullopt;
  candidate.kind = *kind;
  candidate.searchChainIndex = selected.selectedSearchChainIndex;
  return candidate;
}

IncludeLookupKind
IncludeReplayProofContext::ReplayCandidateLookupKindAsProducerKind(
    IncludeReplayCandidate::LookupKind kind) {
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
}

std::optional<RefoldModel::IncludeLookupProvenance>
IncludeReplayProofContext::ReplayCandidateLookupProvenance(
    const IncludeReplayCandidate &candidate) {
  RefoldModel::IncludeLookupProvenance provenance;
  provenance.kind = ReplayCandidateLookupKindAsProducerKind(candidate.kind);

  if (isSearchChainIncludeLookupKind(provenance.kind)) {
    if (!candidate.searchChainIndex)
      return std::nullopt;
    provenance.searchChainIndex = *candidate.searchChainIndex;
  }

  if (provenance.kind == IncludeLookupKind::Unknown)
    return std::nullopt;
  return provenance;
}

bool IncludeReplayProofContext::IncludeReplayCandidateMatchesProducerLookup(
    const IncludeReplayCandidate &candidate,
    const RefoldModel::IncludeItem &include) const {
  const std::string physicalPath = candidate.physicalPath.generic_string();
  if (!services_.samePhysicalIncludeFile(physicalPath, include))
    return false;

  if (!include.lookup)
    return true;

  const IncludeLookupKind candidateKind =
      ReplayCandidateLookupKindAsProducerKind(candidate.kind);
  const RefoldModel::IncludeLookupProvenance &producer = *include.lookup;
  if (candidateKind != producer.kind)
    return false;

  // Search-chain provenance is the state later #include_next proof consumes.
  // If the producer selected this edge through pp_ctx.include_search_chain,
  // replay must select the exact same entry.  Legacy argv-derived candidates
  // intentionally lack SearchChainIndex and therefore cannot satisfy this
  // new-schema proof.
  if (isSearchChainIncludeLookupKind(producer.kind))
    return candidate.searchChainIndex && producer.searchChainIndex &&
           *candidate.searchChainIndex == *producer.searchChainIndex;

  // Source-relative and absolute-operand hits have no search-chain cursor.
  // Physical equality plus kind equality is the complete ordinary-include
  // replay proof they can provide; any descendant #include_next that needs a
  // cursor will still fail closed when its containing-file proof is checked.
  return !candidate.searchChainIndex && !producer.searchChainIndex;
}

bool IncludeReplayProofContext::
    DirectIncludeNextSelectionUsesOnlyReplayableDirectories(
        const RefoldModel::IncludeItem &include) const {
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
    // If that cursor path crosses a framework, builtin, unknown, malformed, or
    // otherwise unmodeled entry, the safe answer is materialization.
    std::optional<IncludeReplayCandidate::LookupKind> kind =
        ReplayableDirectoryLookupKind(entry.kind);
    if (!kind)
      return false;
    if (!MakeIncludeReplaySearchDir(entry.path, entry.spelling, *kind,
                                    entry.index))
      return false;
  }

  return true;
}

bool IncludeReplayProofContext::
    DirectIncludeNextOrdinaryRewriteMatchesProducerSelection(
        const IncludeReplayCandidate &candidate,
        const RefoldModel::IncludeItem &include,
        const CleanChildIncludeReplayDemand &demand) const {
  if (!DirectIncludeNextSelectionUsesOnlyReplayableDirectories(include))
    return false;

  const std::string physicalPath = candidate.physicalPath.generic_string();
  if (!services_.samePhysicalIncludeFile(physicalPath, include))
    return false;

  // A relocated #include_next can be replaced by an ordinary include in two
  // distinct replay-proven ways:
  //
  //  * Search-chain equivalent: the ordinary include is selected by the same
  //    producer HeaderSearch entry that #include_next selected.  This keeps a
  //    usable containing-file cursor for any descendant #include_next edges.
  //
  //  * Direct-target naming: a quoted source-relative or absolute ordinary
  //    include names the producer-selected file directly from the final .c.mod
  //    surface.  This does not reproduce the original HeaderSearch cursor, so
  //    it is only a complete proof when the selected target subtree has no
  //    descendant #include_next obligations that could observe that cursor.
  //    File-spelling observers are still checked later by the ordinary
  //    IncludeReplayProofResult gate before the rewrite is accepted.
  const bool directTargetNaming =
      candidate.kind ==
          IncludeReplayCandidate::LookupKind::DirectSourceRelative ||
      candidate.kind == IncludeReplayCandidate::LookupKind::AbsoluteOperand;
  if (directTargetNaming)
    return demand.includeNextObligations.empty();

  // Search-chain candidates must replay through the same producer-selected
  // entry.  Physical equality alone would be too weak here: an earlier or
  // different search entry may select the same inode/path while giving
  // descendant #include_next directives a different resume cursor.
  if (!IncludeReplayCandidateMatchesProducerLookup(candidate, include))
    return false;

  if (!include.includeNext || !include.includeNext->selectedSearchChainIndex)
    return false;
  return candidate.searchChainIndex &&
         *candidate.searchChainIndex ==
             *include.includeNext->selectedSearchChainIndex;
}

bool IncludeReplayProofContext::IncludeIdIsDescendantOrSelf(
    uint64_t owner, uint64_t root) const {
  uint64_t cur = owner;
  while (true) {
    if (cur == root)
      return true;
    const RefoldModel::IncludeItem *inc = model_.GetIncludeById(cur);
    if (!inc || !inc->parent)
      return false;
    cur = *inc->parent;
  }
}

std::optional<std::string>
IncludeReplayProofContext::ProducerObservedFileSpellingPayload(
    const RefoldModel::MacroInvocation &macro) const {
  if (macro.cover.IsValid()) {
    if (std::optional<std::string> decoded =
            decodeObservedFileStringLiteralPayload(
                services_.sliceASource(macro.cover.begin, macro.cover.end)))
      return decoded;
  }

  if (macro.invPPByteBegin && macro.invPPByteEnd &&
      *macro.invPPByteBegin <= *macro.invPPByteEnd &&
      *macro.invPPByteEnd <= aSource_.size()) {
    if (std::optional<std::string> decoded =
            decodeObservedFileStringLiteralPayload(
                aSource_.slice(static_cast<size_t>(*macro.invPPByteBegin),
                               static_cast<size_t>(*macro.invPPByteEnd))))
      return decoded;
  }

  return std::nullopt;
}

std::optional<uint64_t>
IncludeReplayProofContext::ObservableMacroOwnerInIncludeSubtree(
    const RefoldModel::MacroInvocation &macro, uint64_t includeId) const {
  const RefoldModel::MacroInvocation *site =
      services_.lineStateObservableMacroSite(macro);
  std::optional<uint64_t> owner = site && site->ownerIncludeId
                                      ? site->ownerIncludeId
                                      : macro.ownerIncludeId;
  if (!owner || !IncludeIdIsDescendantOrSelf(*owner, includeId))
    return std::nullopt;
  if (!services_.lineStateBuiltinInvocationIsPreservedObserver(macro))
    return std::nullopt;
  return owner;
}

IncludeReplayProofContext::CleanChildIncludeReplayDemand
IncludeReplayProofContext::BuildCleanChildIncludeReplayDemand(
    uint64_t includeId) const {
  CleanChildIncludeReplayDemand demand;

  const RefoldModel::IncludeItem *rootInclude =
      model_.GetIncludeById(includeId);
  if (rootInclude) {
    // File-spelling proof hierarchy: new-schema maps carry the exact
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

  for (const RefoldModel::IncludeItem &descendant : model_.GetIncludes()) {
    if (descendant.id == includeId)
      continue;
    if (descendant.subkind != "#include_next")
      continue;
    if (!IncludeIdIsDescendantOrSelf(descendant.id, includeId))
      continue;

    IncludeNextObligation obligation;
    obligation.includeNextId = descendant.id;
    if (descendant.includeNext) {
      obligation.producer = *descendant.includeNext;
      obligation.containingIncludeId =
          descendant.includeNext->containingFileIncludeId;
    }
    std::optional<std::string> operand =
        descendant.angled ? angledIncludeReplayLookupOperand(descendant)
                          : quotedIncludeReplayLookupOperand(descendant);
    if (operand)
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
        ObservableMacroOwnerInIncludeSubtree(macro, includeId);
    if (!observableOwner)
      continue;

    // In --no-lines mode the final checker intentionally treats unmodified
    // line-directive-sensitive builtin payloads as ignorable for __LINE__,
    // __FILE__, and __FILE_NAME__: those payloads may differ when the final
    // refolded source is validated from a temporary output path rather than
    // the original source path.  Do not let those observers force a clean
    // child include rewrite/materialization under the active no-lines oracle.
    //
    // Keep __BASE_FILE__ and __INCLUDE_LEVEL__ as hard demands.  __BASE_FILE__
    // observes the top-level preprocessing file, not the child header's
    // entered spelling, so include operand replay cannot prove it; preserving
    // the child include would make the observer depend on the checker/output
    // source rather than the producer top-level source.  __INCLUDE_LEVEL__
    // likewise observes include-stack depth.
    demand.observesLine |= lineSensitiveDemandEnabled && observesLine;
    demand.observesFile |= lineSensitiveDemandEnabled && observesFile;
    demand.observesFileName |= lineSensitiveDemandEnabled && observesFileName;
    demand.observesBaseFile |= observesBaseFile;
    demand.observesIncludeLevel |= observesIncludeLevel;

    if (lineSensitiveDemandEnabled && (observesFile || observesFileName) &&
        *observableOwner == includeId) {
      if (std::optional<std::string> payload =
              ProducerObservedFileSpellingPayload(macro)) {
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

    if (demand.observesLine && demand.observesFile && demand.observesFileName &&
        demand.observesBaseFile && demand.observesIncludeLevel &&
        demand.hasUnprovenFileSpellingObserver)
      break;
  }

  // Fallback file-spelling proof hierarchy: old maps without
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
  // the original include from the producer surface.  If replay is unavailable,
  // resolved_path remains the final legacy compatibility witness.
  const bool mayUseReplaySpelling = demand.fileSpellingPayloads.empty();
  const bool mayUseReplayFileName = demand.fileNamePayloads.empty();
  if (rootInclude && (mayUseReplaySpelling || mayUseReplayFileName)) {
    std::optional<IncludeReplayCandidate> producerCandidate;
    if ((!demand.producerChildFileSpelling && mayUseReplaySpelling) ||
        (!demand.producerChildFileName && mayUseReplayFileName))
      producerCandidate = ComputeProducerIncludeReplayCandidate(*rootInclude);

    if (!demand.producerChildFileSpelling && mayUseReplaySpelling &&
        producerCandidate)
      demand.producerChildFileSpelling = producerCandidate->enteredFileSpelling;
    if (!demand.producerChildFileName && mayUseReplayFileName &&
        producerCandidate)
      demand.producerChildFileName =
          stringutils::pathBasename(producerCandidate->enteredFileSpelling)
              .str();
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
}

IncludeReplayProofContext::FileObserverDemand
IncludeReplayProofContext::IncludeSubtreeFileObserverDemand(
    uint64_t includeId) const {
  FileObserverDemand demand;
  if (!lineDirs_.Enabled())
    return demand;

  for (const auto &macro : model_.GetMacroInvocations()) {
    const bool observesFile = macro.name == "__FILE__";
    const bool observesFileName = macro.name == "__FILE_NAME__";
    if (!observesFile && !observesFileName)
      continue;
    if (!ObservableMacroOwnerInIncludeSubtree(macro, includeId))
      continue;
    demand.observesFile |= observesFile;
    demand.observesFileName |= observesFileName;
    if (demand.observesFile && demand.observesFileName)
      break;
  }
  return demand;
}

IncludeReplayProofContext::PreservedChildIncludeReplayResolver::
    PreservedChildIncludeReplayResolver(
        const IncludeReplayProofContext &context,
        const RefoldModel::IncludeItem &rootChild,
        const IncludeReplayCandidate &rootCandidate)
    : context_(context), rootChild_(rootChild) {
  replayMemo_.insert({rootChild.id, rootCandidate});
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::PreservedChildIncludeReplayResolver::Fail(
    uint64_t includeId) {
  replayActive_.erase(includeId);
  replayFailed_.insert(includeId);
  return std::nullopt;
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::PreservedChildIncludeReplayResolver::
    ReplayOrdinaryInclude(const RefoldModel::IncludeItem &include) {
  std::optional<IncludeReplayCandidate> parentCandidate =
      Replay(*include.parent);
  if (!parentCandidate)
    return std::nullopt;

  if (include.angled) {
    std::optional<std::string> operand =
        angledIncludeReplayLookupOperand(include);
    if (!operand)
      return std::nullopt;
    return context_.ComputeAngledIncludeReplayCandidate(*operand);
  }

  std::optional<std::string> operand =
      quotedIncludeReplayLookupOperand(include);
  if (!operand)
    return std::nullopt;
  std::optional<IncludeReplaySurface> surface =
      context_.IncludeReplaySurfaceForFile(
          parentCandidate->enteredFileSpelling);
  if (!surface)
    return std::nullopt;
  return context_.ComputeQuotedIncludeReplayCandidateOnSurface(*operand,
                                                               *surface);
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::PreservedChildIncludeReplayResolver::
    ReplayIncludeNext(const RefoldModel::IncludeItem &include) {
  if (!include.includeNext || !include.includeNext->known ||
      !include.includeNext->containingFileIncludeId)
    return std::nullopt;

  std::optional<std::string> operand =
      include.angled ? angledIncludeReplayLookupOperand(include)
                     : quotedIncludeReplayLookupOperand(include);
  if (!operand)
    return std::nullopt;

  std::optional<IncludeReplayCandidate> containingCandidate =
      Replay(*include.includeNext->containingFileIncludeId);
  if (!containingCandidate)
    return std::nullopt;

  std::optional<RefoldModel::IncludeLookupProvenance> containingProvenance =
      context_.ReplayCandidateLookupProvenance(*containingCandidate);
  if (!containingProvenance)
    return std::nullopt;

  std::optional<IncludeReplaySurface> surface =
      context_.IncludeReplaySurfaceForFile(
          containingCandidate->enteredFileSpelling);
  if (!surface)
    return std::nullopt;

  std::optional<IncludeNextReplayCandidate> selected =
      context_.ComputeIncludeNextReplayCandidate(*operand, *surface,
                                                 *containingProvenance);
  if (!selected || !include.includeNext->resumeSearchChainIndex ||
      !include.includeNext->selectedSearchChainIndex ||
      !include.includeNext->containingFileSearchChainIndex ||
      !containingProvenance->searchChainIndex)
    return std::nullopt;
  if (selected->resumeSearchChainIndex !=
          *include.includeNext->resumeSearchChainIndex ||
      selected->selectedSearchChainIndex !=
          *include.includeNext->selectedSearchChainIndex ||
      *containingProvenance->searchChainIndex !=
          *include.includeNext->containingFileSearchChainIndex)
    return std::nullopt;
  return context_.IncludeNextReplayAsOrdinaryCandidate(*selected);
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::PreservedChildIncludeReplayResolver::Replay(
    uint64_t includeId) {
  if (auto it = replayMemo_.find(includeId); it != replayMemo_.end())
    return it->second;
  if (replayFailed_.contains(includeId) || replayActive_.contains(includeId))
    return std::nullopt;

  const RefoldModel::IncludeItem *include =
      context_.model_.GetIncludeById(includeId);
  if (!include ||
      !context_.IncludeIdIsDescendantOrSelf(includeId, rootChild_.id) ||
      !include->parent)
    return Fail(includeId);

  replayActive_.insert(includeId);

  std::optional<IncludeReplayCandidate> candidate;
  if (include->subkind == "#include")
    candidate = ReplayOrdinaryInclude(*include);
  else if (include->subkind == "#include_next")
    candidate = ReplayIncludeNext(*include);
  else
    return Fail(includeId);

  if (!candidate || !context_.IncludeReplayCandidateMatchesProducerLookup(
                        *candidate, *include))
    return Fail(includeId);

  replayActive_.erase(includeId);
  replayMemo_.insert({includeId, *candidate});
  return candidate;
}

bool IncludeReplayProofContext::IncludeNextObligationsAreProven(
    const IncludeReplayCandidate &childCandidate,
    const RefoldModel::IncludeItem &child,
    const CleanChildIncludeReplayDemand &demand) const {
  if (demand.includeNextObligations.empty())
    return true;

  PreservedChildIncludeReplayResolver resolver(*this, child, childCandidate);

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
        resolver.Replay(*obligation.containingIncludeId);
    if (!containingCandidate)
      return false;

    std::optional<RefoldModel::IncludeLookupProvenance> containingProvenance =
        ReplayCandidateLookupProvenance(*containingCandidate);
    if (!containingProvenance || !containingProvenance->searchChainIndex)
      return false;
    if (*containingProvenance->searchChainIndex !=
        *obligation.producer.containingFileSearchChainIndex)
      return false;

    std::optional<IncludeReplaySurface> surface =
        IncludeReplaySurfaceForFile(containingCandidate->enteredFileSpelling);
    if (!surface)
      return false;

    std::optional<IncludeNextReplayCandidate> candidate =
        ComputeIncludeNextReplayCandidate(obligation.operand, *surface,
                                          *containingProvenance);
    if (!candidate)
      return false;

    if (candidate->resumeSearchChainIndex !=
            *obligation.producer.resumeSearchChainIndex ||
        candidate->selectedSearchChainIndex !=
            *obligation.producer.selectedSearchChainIndex)
      return false;

    // The descendant #include_next metadata carries two redundant producer
    // witnesses for the selected target: include_next provenance and the normal
    // per-edge lookup record.  Require them to agree with the replayed
    // selection before treating the obligation as discharged; otherwise a
    // malformed or old mixed-schema map could prove the resume cursor against
    // one witness while preserving a directive whose edge lookup says another.
    if (includeNext->lookup) {
      if (includeNext->lookup->kind != candidate->selectedKind)
        return false;
      if (isSearchChainIncludeLookupKind(includeNext->lookup->kind)) {
        if (!includeNext->lookup->searchChainIndex ||
            *includeNext->lookup->searchChainIndex !=
                candidate->selectedSearchChainIndex)
          return false;
      }
    }

    const std::string physicalPath = candidate->physicalPath.generic_string();
    if (!services_.samePhysicalIncludeFile(physicalPath, *includeNext))
      return false;

    // A descendant #include_next can be selected through the same physical file
    // and search-chain entry while still exposing a different presumed
    // filename. That spelling matters only when the selected subtree keeps
    // filename observers under the active oracle.  Observer-free selected
    // subtrees require only physical identity plus cursor equivalence.
    FileObserverDemand observerDemand =
        IncludeSubtreeFileObserverDemand(includeNext->id);
    if (observerDemand.observesFile) {
      StringRef producerSpelling = producerEnteredFileSpelling(*includeNext);
      if (producerSpelling.empty() ||
          StringRef(candidate->enteredFileSpelling) != producerSpelling)
        return false;
    }
    if (observerDemand.observesFileName) {
      StringRef producerName = producerEnteredFileName(*includeNext);
      // Prefer the producer/replay EnteredFileName payload when available.  It
      // is the schema field intended to mirror Clang's processPathToFileName()
      // result; deriving a basename from EnteredFileSpelling is only a legacy
      // compatibility fallback.
      const std::string candidateName =
          !candidate->enteredFileName.empty()
              ? candidate->enteredFileName
              : stringutils::pathBasename(candidate->enteredFileSpelling).str();
      if (producerName.empty() || StringRef(candidateName) != producerName)
        return false;
    }
  }

  return true;
}

IncludeReplayProofContext::IncludeReplayProofResult
IncludeReplayProofContext::EvaluateIncludeReplayCandidate(
    const IncludeReplayCandidate &candidate,
    const RefoldModel::IncludeItem &child,
    const CleanChildIncludeReplayDemand &demand) const {
  IncludeReplayProofResult result;
  const std::string physicalPath = candidate.physicalPath.generic_string();
  result.samePhysicalFile =
      services_.samePhysicalIncludeFile(physicalPath, child);
  result.sameIncludeNextStack =
      IncludeNextObligationsAreProven(candidate, child, demand);
  if (demand.observesFileSpelling()) {
    if (demand.hasUnprovenFileSpellingObserver) {
      result.sameEnteredFileSpelling = false;
      result.sameEnteredFileName = false;
    }

    // File-spelling proof hierarchy: explicit entered_file_spelling (or the
    // best legacy witness selected when building the demand) is the primary
    // target for __FILE__ preservation.  Recovered macro payloads are used only
    // when no include-entry spelling witness exists, which keeps old maps
    // conservative without letting payload recovery override new-schema
    // producer metadata.
    if (demand.observesFile) {
      if (demand.producerChildFileSpelling) {
        result.sameEnteredFileSpelling &=
            StringRef(candidate.enteredFileSpelling) ==
            StringRef(*demand.producerChildFileSpelling);
      } else {
        for (const std::string &expected : demand.fileSpellingPayloads)
          result.sameEnteredFileSpelling &=
              StringRef(candidate.enteredFileSpelling) == StringRef(expected);
      }
    }

    const std::string candidateFileName =
        !candidate.enteredFileName.empty()
            ? candidate.enteredFileName
            : stringutils::pathBasename(candidate.enteredFileSpelling).str();
    if (demand.observesFileName) {
      if (demand.producerChildFileName) {
        result.sameEnteredFileName &= StringRef(candidateFileName) ==
                                      StringRef(*demand.producerChildFileName);
      } else {
        for (const std::string &expected : demand.fileNamePayloads)
          result.sameEnteredFileName &=
              StringRef(candidateFileName) == StringRef(expected);
      }
    }
  }
  return result;
}

void IncludeReplayProofContext::AppendQuotedChildIncludeRewriteCandidate(
    SmallVectorImpl<QuotedChildIncludeRewriteCandidate> &candidates,
    StringRef operand) const {
  if (operand.empty() || !safeSynthesizedRelativeIncludeOperand(operand))
    return;
  for (const QuotedChildIncludeRewriteCandidate &existing : candidates) {
    if (StringRef(existing.operand) == operand)
      return;
  }

  std::optional<IncludeReplayCandidate> replay =
      ComputeQuotedIncludeReplayCandidate(operand);
  if (!replay)
    return;

  QuotedChildIncludeRewriteCandidate rewrite;
  rewrite.operand = operand.str();
  rewrite.replay = std::move(*replay);
  candidates.push_back(std::move(rewrite));
}

SmallVector<IncludeReplayProofContext::QuotedChildIncludeRewriteCandidate, 8>
IncludeReplayProofContext::GenerateQuotedChildIncludeRewriteCandidates(
    const RefoldModel::IncludeItem &child) const {
  SmallVector<QuotedChildIncludeRewriteCandidate, 8> candidates;
  if (child.subkind != "#include" || child.angled)
    return candidates;

  std::optional<std::filesystem::path> producerPhysicalPath =
      producerPhysicalIncludePath(child);
  if (!producerPhysicalPath)
    return candidates;

  // These are spelling hypotheses, not acceptance shortcuts.  Every operand is
  // immediately replayed from the final .c.mod surface above and later accepted
  // only if physical identity, active file-observer demands, and descendant
  // #include_next obligations all prove against producer metadata.
  //
  // The final-output-relative candidate covers headers copied beside the
  // emitted source.  The search-dir-relative candidates cover the common
  // materialized-parent case where the producer child was originally found
  // relative to a header directory, but the best final spelling is through an
  // existing -I/-iquote entry, e.g. rewriting "leaf.h" to "headers/leaf.h".
  if (finalReplaySurface_) {
    if (std::optional<std::string> operand = StableContainedRelativeOperand(
            *producerPhysicalPath, finalReplaySurface_->outputDirectory))
      AppendQuotedChildIncludeRewriteCandidate(candidates, *operand);
  }

  SmallVector<std::string, 16> seenSearchDirectories;
  const RecordedIncludeSearchDirs &dirs = RecordedIncludeSearchDirsForReplay();
  for (const IncludeReplaySearchDir &dir : dirs.quotedLookupDirs) {
    if (dir.isUnsupportedBarrier || dir.lookupPath.empty())
      continue;
    const std::string key = dir.lookupPath.lexically_normal().generic_string();
    if (llvm::is_contained(seenSearchDirectories, key))
      continue;
    seenSearchDirectories.push_back(key);

    if (std::optional<std::string> operand = StableContainedRelativeOperand(
            *producerPhysicalPath, dir.lookupPath))
      AppendQuotedChildIncludeRewriteCandidate(candidates, *operand);
  }

  // New-schema entered-file spelling is also useful as a deterministic
  // hypothesis for direct source-relative children.  It still has to replay
  // from the final surface before it can be emitted, so it cannot bypass a
  // physical shadowing or file-observer mismatch.
  StringRef explicitEnteredSpelling =
      explicitProducerEnteredFileSpelling(child);
  if (!explicitEnteredSpelling.empty())
    AppendQuotedChildIncludeRewriteCandidate(candidates,
                                             explicitEnteredSpelling);

  // Legacy maps may have only resolved_path as the producer spelling.  Keep
  // this behind the split-schema path so resolved_path remains a compatibility
  // fallback without competing with explicit metadata.
  if (!child.enteredFileSpelling) {
    StringRef legacySpelling = legacyResolvedIncludePath(child);
    if (!legacySpelling.empty())
      AppendQuotedChildIncludeRewriteCandidate(candidates, legacySpelling);
  }

  return candidates;
}

std::optional<IncludeReplayProofContext::IncludeReplayCandidate>
IncludeReplayProofContext::ReplayDirectIncludeNextOrdinaryRewriteCandidate(
    StringRef operand, OrdinaryIncludeDelimiterKind delimiterKind,
    OrdinaryIncludeReplayFailureKind *failureKind) const {
  if (failureKind)
    *failureKind = OrdinaryIncludeReplayFailureKind::Unresolved;

  if (!safeOrdinaryIncludeRewriteOperand(operand))
    return std::nullopt;

  std::optional<OrdinaryIncludeReplayResult> replay;
  if (delimiterKind == OrdinaryIncludeDelimiterKind::Quoted) {
    std::optional<IncludeReplaySurface> surface =
        FinalOutputIncludeReplaySurface();
    if (!surface)
      return std::nullopt;
    replay = ComputeOrdinaryIncludeReplayResult(
        operand, OrdinaryIncludeDelimiterKind::Quoted, &*surface, failureKind);
  } else {
    replay = ComputeOrdinaryIncludeReplayResult(
        operand, OrdinaryIncludeDelimiterKind::Angled, nullptr, failureKind);
  }

  if (!replay)
    return std::nullopt;
  return IncludeReplayCandidateFromOrdinaryResult(*replay);
}

void IncludeReplayProofContext::AppendDirectIncludeNextOrdinaryRewriteCandidate(
    SmallVectorImpl<DirectIncludeNextOrdinaryRewriteCandidate> &candidates,
    StringRef operand, OrdinaryIncludeDelimiterKind delimiterKind,
    StringRef origin) const {
  if (operand.empty())
    return;
  for (const DirectIncludeNextOrdinaryRewriteCandidate &existing : candidates) {
    if (StringRef(existing.operand) == operand &&
        existing.delimiterKind == delimiterKind)
      return;
  }

  DirectIncludeNextOrdinaryRewriteCandidate candidate;
  candidate.operand = operand.str();
  candidate.delimiterKind = delimiterKind;
  candidate.origin = origin.str();
  candidate.replay = ReplayDirectIncludeNextOrdinaryRewriteCandidate(
      operand, delimiterKind, &candidate.replayFailure);
  candidates.push_back(std::move(candidate));
}

void IncludeReplayProofContext::AppendDirectIncludeNextWithPreferredDelimiters(
    SmallVectorImpl<DirectIncludeNextOrdinaryRewriteCandidate> &candidates,
    StringRef operand, StringRef origin, bool preferAngledDelimiter) const {
  // Preserve the original delimiter first, then try the other ordinary
  // spelling.  Ordering is only a tie-breaker among candidates that have
  // already replayed; it is never an acceptance proof.
  if (preferAngledDelimiter) {
    AppendDirectIncludeNextOrdinaryRewriteCandidate(
        candidates, operand, OrdinaryIncludeDelimiterKind::Angled, origin);
    AppendDirectIncludeNextOrdinaryRewriteCandidate(
        candidates, operand, OrdinaryIncludeDelimiterKind::Quoted, origin);
  } else {
    AppendDirectIncludeNextOrdinaryRewriteCandidate(
        candidates, operand, OrdinaryIncludeDelimiterKind::Quoted, origin);
    AppendDirectIncludeNextOrdinaryRewriteCandidate(
        candidates, operand, OrdinaryIncludeDelimiterKind::Angled, origin);
  }
}

void IncludeReplayProofContext::
    AppendSearchDirectoryRelativeDirectIncludeNextCandidate(
        SmallVectorImpl<DirectIncludeNextOrdinaryRewriteCandidate> &candidates,
        SmallVectorImpl<std::string> &seenSearchDirectories,
        const IncludeReplaySearchDir &dir,
        const std::filesystem::path &producerPhysicalPath,
        bool preferAngledDelimiter) const {
  if (dir.isUnsupportedBarrier || dir.lookupPath.empty())
    return;
  const std::string key = dir.lookupPath.lexically_normal().generic_string();
  if (llvm::is_contained(seenSearchDirectories, key))
    return;
  seenSearchDirectories.push_back(key);

  if (std::optional<std::string> operand =
          StableContainedRelativeOperand(producerPhysicalPath, dir.lookupPath))
    AppendDirectIncludeNextWithPreferredDelimiters(candidates, *operand,
                                                   "recorded-search-directory",
                                                   preferAngledDelimiter);
}

SmallVector<
    IncludeReplayProofContext::DirectIncludeNextOrdinaryRewriteCandidate, 16>
IncludeReplayProofContext::GenerateDirectIncludeNextOrdinaryRewriteCandidates(
    const RefoldModel::IncludeItem &child) const {
  SmallVector<DirectIncludeNextOrdinaryRewriteCandidate, 16> candidates;
  if (child.subkind != "#include_next")
    return candidates;

  std::optional<std::filesystem::path> producerPhysicalPath =
      producerPhysicalIncludePath(child);
  if (!producerPhysicalPath)
    return candidates;

  const bool preferAngledDelimiter = child.angled;

  // 1. Original #include_next operand, rewritten as an ordinary include.
  if (std::optional<std::string> originalOperand =
          child.angled ? angledIncludeReplayLookupOperand(child)
                       : quotedIncludeReplayLookupOperand(child))
    AppendDirectIncludeNextWithPreferredDelimiters(candidates, *originalOperand,
                                                   "original-operand",
                                                   preferAngledDelimiter);

  // 2. Target path relative to the producer search-chain entry that selected
  // this #include_next target, when the producer recorded such an entry.
  if (child.lookup && ReplayableDirectoryLookupKind(child.lookup->kind)) {
    if (child.lookup->directoryPath) {
      if (std::optional<std::string> operand = StableContainedRelativeOperand(
              *producerPhysicalPath,
              std::filesystem::path(child.lookup->directoryPath->str())))
        AppendDirectIncludeNextWithPreferredDelimiters(
            candidates, *operand, "selected-search-chain-directory",
            preferAngledDelimiter);
    }
  }

  // 3. Target path relative to every modeled recorded search directory that
  // physically contains it.  Unsupported entries are ignored here as operand
  // sources; they remain lookup barriers in the replay evaluator itself.
  SmallVector<std::string, 16> seenSearchDirectories;
  const RecordedIncludeSearchDirs &dirs = RecordedIncludeSearchDirsForReplay();
  for (const IncludeReplaySearchDir &dir : dirs.quotedLookupDirs)
    AppendSearchDirectoryRelativeDirectIncludeNextCandidate(
        candidates, seenSearchDirectories, dir, *producerPhysicalPath,
        preferAngledDelimiter);
  for (const IncludeReplaySearchDir &dir : dirs.angledLookupDirs)
    AppendSearchDirectoryRelativeDirectIncludeNextCandidate(
        candidates, seenSearchDirectories, dir, *producerPhysicalPath,
        preferAngledDelimiter);

  // 4. Output-directory-relative path.  Once a relocated #include_next is
  //    being rewritten as an ordinary include in the final .c.mod file, the
  //    most source-accurate direct-target spelling is the stable spelling from
  //    that final surface.  Prefer this over producer entered-file spelling
  //    when both replay, because entered-file spelling describes what Clang
  //    reported for the producer include stack; it is not necessarily the
  //    cleanest operand to emit from the relocated surface (for example,
  //    `./headers/b/next.h` versus `headers/b/next.h`).
  //
  //    This is still only a candidate: replay and observer proof below must
  //    prove physical identity, file-spelling stability when observed, and
  //    descendant #include_next obligations before anything is emitted.
  if (finalReplaySurface_) {
    if (std::optional<std::string> operand = StableContainedRelativeOperand(
            *producerPhysicalPath, finalReplaySurface_->outputDirectory))
      AppendDirectIncludeNextWithPreferredDelimiters(
          candidates, *operand, "output-directory-relative",
          preferAngledDelimiter);
  }

  // 5. Producer entered-file spelling, when the new-schema producer supplied it
  // explicitly.  Keep this after the final-surface spelling so it repairs real
  // file-observer cases without overriding the canonical emitted-source operand
  // when the two spellings are semantically equivalent.
  StringRef explicitEnteredSpelling =
      explicitProducerEnteredFileSpelling(child);
  if (!explicitEnteredSpelling.empty())
    AppendDirectIncludeNextWithPreferredDelimiters(
        candidates, explicitEnteredSpelling, "producer-entered-file-spelling",
        preferAngledDelimiter);

  // 6. Legacy resolved_path spelling, only for maps that lack the split
  // entered_file_spelling/opened_path facts.  When both split fields and
  // resolved_path are present, resolved_path remains weaker compatibility
  // evidence.
  if (!child.enteredFileSpelling) {
    StringRef legacySpelling = legacyResolvedIncludePath(child);
    if (!legacySpelling.empty())
      AppendDirectIncludeNextWithPreferredDelimiters(candidates, legacySpelling,
                                                     "legacy-resolved-path",
                                                     preferAngledDelimiter);
  }

  return candidates;
}

const char *
IncludeReplayProofContext::DirectIncludeNextOrdinaryRewriteRejectReason(
    const DirectIncludeNextOrdinaryRewriteCandidate &rewrite,
    const RefoldModel::IncludeItem &child,
    const CleanChildIncludeReplayDemand &demand,
    const IncludeReplayProofResult *proof, bool selectedTargetProven) {
  if (!rewrite.replay) {
    const char *failure =
        OrdinaryIncludeReplayFailureReasonName(rewrite.replayFailure);
    return StringRef(failure) == "none" ? "unresolved" : failure;
  }

  if (!proof)
    return "unresolved";
  if (!proof->samePhysicalFile)
    return "physical-mismatch";

  if (!selectedTargetProven) {
    if (rewrite.replay->kind == IncludeReplayCandidate::LookupKind::Unknown)
      return "unknown-search-chain-entry";

    if (child.lookup && isSearchChainIncludeLookupKind(child.lookup->kind) &&
        !rewrite.replay->searchChainIndex)
      return "missing-search-chain-index";

    return "shadowed-by-earlier-search-entry";
  }

  if ((demand.observesFile || demand.hasUnprovenFileSpellingObserver) &&
      !proof->sameEnteredFileSpelling)
    return "entered-file-spelling-mismatch";
  if (demand.observesFileName && !proof->sameEnteredFileName)
    return "entered-file-name-mismatch";
  if (!demand.includeNextObligations.empty() && !proof->sameIncludeNextStack)
    return "descendant-include-next-obligation-failed";

  return "unresolved";
}

void IncludeReplayProofContext::TraceDirectIncludeNextOrdinaryRewriteCandidate(
    const DirectIncludeNextOrdinaryRewriteCandidate &rewrite,
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
      rewrite.replay ? rewrite.replay->physicalPath.generic_string()
                     : std::string("(none)");
  const std::string replayLookupKind =
      rewrite.replay
          ? std::string(IncludeReplayLookupKindName(rewrite.replay->kind))
          : std::string("(none)");
  const std::string replaySearchChainIndex =
      rewrite.replay
          ? optionalSearchChainIndexForTrace(rewrite.replay->searchChainIndex)
          : std::string("(none)");
  const std::string producerLookupKind =
      child.lookup ? toString(child.lookup->kind).str() : std::string("(none)");
  const std::string producerSearchChainIndex =
      child.lookup
          ? optionalSearchChainIndexForTrace(child.lookup->searchChainIndex)
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
      IncludeReplayProofContext::OrdinaryIncludeDelimiterName(
          rewrite.delimiterKind),
      IncludeReplayProofContext::OrdinaryIncludeDelimiterOpen(
          rewrite.delimiterKind),
      rewrite.operand,
      IncludeReplayProofContext::OrdinaryIncludeDelimiterClose(
          rewrite.delimiterKind),
      replayPhysicalPath, producerPhysicalPath, replayLookupKind,
      replaySearchChainIndex, producerLookupKind, producerSearchChainIndex,
      producerIncludeNextSelectedIndex, demand.observesFile,
      demand.observesFileName, demand.hasUnprovenFileSpellingObserver,
      demand.includeNextObligations.size(), descendantObligationResult,
      selectedTargetProven, decisionReason);
}

void IncludeReplayProofContext::
    MarkMaterializationIfIncludeNextObligationsRemain(
        CleanChildIncludeReplayPlan &plan,
        const CleanChildIncludeReplayDemand &demand) {
  // If a clean child is ultimately materialized while its subtree contains
  // descendant #include_next directives, those directives are no longer in
  // their producer header-search context.  Unless the whole child include is
  // preserved as an include directive, recursive materialization must realize
  // nested include_next edges instead of leaving replay-sensitive directives
  // behind in relocated text.
  if (!demand.includeNextObligations.empty())
    plan.forceMaterializeDescendantIncludeNext = true;
}

IncludeReplayProofContext::CleanChildIncludeReplayPlan
IncludeReplayProofContext::PlanAngledOrdinaryChildReplay(
    const RefoldModel::IncludeItem &child,
    const CleanChildIncludeReplayDemand &demand) const {
  CleanChildIncludeReplayPlan plan;

  if (!demand.requiresReplayCandidateProof())
    return plan;

  std::optional<std::string> operand = angledIncludeReplayLookupOperand(child);
  if (!operand) {
    plan.action =
        IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
    plan.reason = "angled child include operand is not replay-provable "
                  "from materialized parent surface";
    MarkMaterializationIfIncludeNextObligationsRemain(plan, demand);
    return plan;
  }

  std::optional<IncludeReplayCandidate> candidate =
      ComputeAngledIncludeReplayCandidate(*operand);
  if (!candidate) {
    plan.action =
        IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
    plan.reason = "angled child include has no replay-surface lookup "
                  "target";
    MarkMaterializationIfIncludeNextObligationsRemain(plan, demand);
    return plan;
  }

  IncludeReplayProofResult proof =
      EvaluateIncludeReplayCandidate(*candidate, child, demand);
  if (proof.proves(demand))
    return plan;

  plan.action =
      IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
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
  MarkMaterializationIfIncludeNextObligationsRemain(plan, demand);
  return plan;
}

IncludeReplayProofContext::CleanChildIncludeReplayPlan
IncludeReplayProofContext::PlanDirectIncludeNextOrdinaryRewrite(
    const RefoldModel::IncludeItem &child,
    const CleanChildIncludeReplayDemand &demand) const {
  CleanChildIncludeReplayPlan plan;

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
  // final .c.mod surface by ComputeOrdinaryIncludeReplayResult(), and then
  // the proof layer checks physical identity, the producer-selected lookup
  // kind/search-chain index, observer spelling, and descendant include_next
  // cursor obligations before accepting it.
  std::string reason = "#include_next cannot be replayed from a "
                       "materialized parent surface without the original "
                       "include-next search stack";

  SmallVector<DirectIncludeNextOrdinaryRewriteCandidate, 16> rewrites =
      GenerateDirectIncludeNextOrdinaryRewriteCandidates(child);
  std::string firstRejectedCandidateReason;
  for (const DirectIncludeNextOrdinaryRewriteCandidate &rewrite : rewrites) {
    std::optional<IncludeReplayProofResult> proof;
    bool selectedTargetProven = false;

    if (rewrite.replay) {
      proof = EvaluateIncludeReplayCandidate(*rewrite.replay, child, demand);
      selectedTargetProven =
          DirectIncludeNextOrdinaryRewriteMatchesProducerSelection(
              *rewrite.replay, child, demand);
      if (selectedTargetProven && proof->proves(demand)) {
        TraceDirectIncludeNextOrdinaryRewriteCandidate(
            rewrite, child, demand, &*proof, selectedTargetProven, "accepted");
        plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::
            RewriteOperand;
        plan.rewrittenOperand = rewrite.operand;
        plan.rewrittenDelimiterKind = rewrite.delimiterKind;
        plan.reason = std::move(reason);
        return plan;
      }
    }

    const char *rejectReason = DirectIncludeNextOrdinaryRewriteRejectReason(
        rewrite, child, demand, proof ? &*proof : nullptr,
        selectedTargetProven);
    TraceDirectIncludeNextOrdinaryRewriteCandidate(
        rewrite, child, demand, proof ? &*proof : nullptr, selectedTargetProven,
        rejectReason);

    // Keep the materialization explanation concise, but base it on the same
    // stable rejection vocabulary emitted by the trace record.  The full
    // per-candidate proof ledger is intentionally available only at trace
    // level so normal diagnostics do not become enormous.
    if (firstRejectedCandidateReason.empty()) {
      firstRejectedCandidateReason = "; first ordinary include candidate ";
      firstRejectedCandidateReason +=
          IncludeReplayProofContext::OrdinaryIncludeDelimiterName(
              rewrite.delimiterKind);
      firstRejectedCandidateReason += " ";
      firstRejectedCandidateReason +=
          IncludeReplayProofContext::OrdinaryIncludeDelimiterOpen(
              rewrite.delimiterKind);
      firstRejectedCandidateReason += rewrite.operand;
      firstRejectedCandidateReason +=
          IncludeReplayProofContext::OrdinaryIncludeDelimiterClose(
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

  plan.action =
      IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
  plan.reason = std::move(reason);
  MarkMaterializationIfIncludeNextObligationsRemain(plan, demand);
  return plan;
}

IncludeReplayProofContext::CleanChildIncludeReplayPlan
IncludeReplayProofContext::PlanQuotedOrdinaryChildReplay(
    const RefoldModel::IncludeItem &child,
    const CleanChildIncludeReplayDemand &demand) const {
  CleanChildIncludeReplayPlan plan;

  bool mayRewriteUnstableOperand = false;
  std::string reason;

  std::optional<std::string> childReplayOperand =
      quotedIncludeReplayLookupOperand(child);
  if (!childReplayOperand) {
    plan.action =
        IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
    plan.reason = "quoted child include operand is not replay-provable "
                  "from materialized parent surface";
    MarkMaterializationIfIncludeNextObligationsRemain(plan, demand);
    return plan;
  }

  std::optional<IncludeReplayCandidate> replayCandidate =
      ComputeQuotedIncludeReplayCandidate(*childReplayOperand);
  if (replayCandidate) {
    IncludeReplayProofResult proof =
        EvaluateIncludeReplayCandidate(*replayCandidate, child, demand);
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
         GenerateQuotedChildIncludeRewriteCandidates(child)) {
      sawRewriteCandidate = true;
      IncludeReplayProofResult proof =
          EvaluateIncludeReplayCandidate(rewrite.replay, child, demand);
      if (proof.proves(demand)) {
        plan.action = IncludeReplayProofContext::CleanChildIncludeReplayAction::
            RewriteOperand;
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

  plan.action =
      IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
  plan.reason = std::move(reason);
  MarkMaterializationIfIncludeNextObligationsRemain(plan, demand);
  return plan;
}

IncludeReplayProofContext::CleanChildIncludeReplayPlan
IncludeReplayProofContext::PlanCleanChildIncludeReplayFromMaterializedParent(
    const RefoldModel::IncludeItem &child) const {
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
      BuildCleanChildIncludeReplayDemand(child.id);

  if (isDirectIncludeNext && !producerPhysicalIncludePath(child)) {
    plan.action =
        IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
    plan.reason = "#include_next has no producer physical target for "
                  "ordinary-include rewrite proof";
    MarkMaterializationIfIncludeNextObligationsRemain(plan, demand);
    return plan;
  }

  if (isDirectIncludeNext &&
      !directIncludeNextHasProducerSelectionProvenance(child)) {
    plan.action =
        IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
    plan.reason = "#include_next has no complete producer search-chain "
                  "selection proof for ordinary-include rewrite";
    MarkMaterializationIfIncludeNextObligationsRemain(plan, demand);
    return plan;
  }

  // __INCLUDE_LEVEL__ observes include-stack depth.  Physical identity and
  // entered-file spelling do not prove that a relocated clean child include
  // has the same stack depth as the producer run, so the child must be
  // realized through the existing materialization/line-state repair path.
  if (demand.observesIncludeLevel) {
    plan.action =
        IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
    plan.reason = "child subtree observes __INCLUDE_LEVEL__ across "
                  "materialized-parent include relocation";
    MarkMaterializationIfIncludeNextObligationsRemain(plan, demand);
    return plan;
  }

  // __BASE_FILE__ observes the top-level preprocessing file, not the entered
  // child header.  Include operand replay can prove only the physical child
  // file and, for __FILE__/__FILE_NAME__, the child entered-file spelling.
  if (demand.observesBaseFile) {
    plan.action =
        IncludeReplayProofContext::CleanChildIncludeReplayAction::Materialize;
    plan.reason = "child subtree observes __BASE_FILE__ across "
                  "materialized-parent include relocation";
    MarkMaterializationIfIncludeNextObligationsRemain(plan, demand);
    return plan;
  }

  // Do not special-case angled #include_next to materialization here.  Direct
  // #include_next directives are handled uniformly: copying one as
  // #include_next would require the original containing-file HeaderSearch
  // cursor, which a materialized parent surface does not have.  The safe
  // include-preserving alternative is therefore a separately proven ordinary
  // include rewrite; if that exact replay proof fails below, the child is
  // materialized.

  if (child.subkind == "#include" && child.angled)
    return PlanAngledOrdinaryChildReplay(child, demand);

  if (isDirectIncludeNext)
    return PlanDirectIncludeNextOrdinaryRewrite(child, demand);

  return PlanQuotedOrdinaryChildReplay(child, demand);
}

} // namespace refold
} // namespace clang
