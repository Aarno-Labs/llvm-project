//===--- RefoldPreprocessRecheck.cpp ----------------------------*- C++ -*-===//
//
// Preprocessing-recheck primitive implementations for clang-refold.
//
// See RefoldPreprocessRecheck.h for the public contract.  This file implements
// `preprocessToBytes` (which drives Clang's preprocessor on a candidate
// source) and `compareTokens` (byte-exact preprocessed-token comparison used
// by `--check`).  The raw-lexer producer that turns `-E -P` bytes into a
// `PPTok` stream lives next to `PPTok` itself in `model/RefoldToken.cpp`.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldPreprocessRecheck.h"

#include "model/RefoldModel.h"
#include "model/RefoldToken.h"
#include "support/RefoldLangOptions.h"
#include "support/RefoldLog.h"
#include "support/StringUtils.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Load \p path into \p out, fatal-erroring on I/O failure.  Used to read the
/// temp-file produced by `preprocessToBytes`; kept private to this TU rather
/// than depending on the driver's own `readFile` helper.
void readTempFileForPreprocessRecheck(StringRef path, std::string &out) {
  auto bufOrErr = MemoryBuffer::getFile(path);
  if (!bufOrErr) {
    REFOLD_LOG_FATAL("file/load", "cannot read file: {0} ({1})", path,
                     bufOrErr.getError().message());
  }
  out.assign(bufOrErr->get()->getBufferStart(),
             bufOrErr->get()->getBufferEnd());
}

} // namespace

Expected<std::string>
preprocessToBytes(StringRef inputPath,
                  const RefoldModel::PreprocessContext &ctx,
                  ArrayRef<std::string> extraArgs) {
  // Force an absolute input path so it remains valid after we chdir.
  SmallString<256> absInput(inputPath);
  if (std::error_code ec = sys::fs::make_absolute(absInput)) {
    return createStringError(
        ec, formatv("cannot resolve absolute path for '{0}'", inputPath));
  }

  // Create a temp file path for clang's preprocessor output.
  SmallString<256> tmpPath;
  if (std::error_code ec =
          sys::fs::createTemporaryFile("clang-refold-check", "i", tmpPath)) {
    return createStringError(ec, "failed to create temporary file");
  }

  auto removeTmp = make_scope_exit([&]() { (void)sys::fs::remove(tmpPath); });

  // Assemble a cc1-style argument list for in-process preprocessing.
  std::vector<std::string> args;
  args.reserve(ctx.argv.size() + 10);

  bool hasE = false;
  bool hasP = false;

  // Side-effect flags recorded from the producer build.  A recheck preprocesses
  // a *candidate* source, so honoring these would write build artifacts
  // describing a file the build never compiled -- and, worse, the paths are
  // relative to the producer's build directory rather than this process's
  // working directory, so the write usually fails and takes the whole recheck
  // down with it.  Each name here takes one separate value argument.
  static constexpr StringRef ValuedSideEffectFlags[] = {
      "-dependency-file",     "-MT", "-MF", "-dependency-dot",
      "-module-dependency-dir", "-header-include-file"};
  // The same, without a value argument.
  static constexpr StringRef ValuelessSideEffectFlags[] = {"-sys-header-deps",
                                                           "-show-includes"};

  for (size_t i = 0; i < ctx.argv.size(); ++i) {
    StringRef a(ctx.argv[i]);

    // The recorded invocation may include refold-map or output flags used when
    // producing the refold map. Strip these so we can redirect output to our
    // temp file.
    if (a == "--refold-map") {
      ++i; // skip value
      continue;
    }
    if (a.starts_with("--refold-map="))
      continue;
    if (a == "-o") {
      ++i; // skip value
      continue;
    }
    if (llvm::is_contained(ValuedSideEffectFlags, a)) {
      ++i; // skip value
      continue;
    }
    if (llvm::is_contained(ValuelessSideEffectFlags, a))
      continue;

    if (a == "-E")
      hasE = true;
    else if (a == "-P")
      hasP = true;

    args.push_back(ctx.argv[i]);
  }

  // Force preprocess-only and suppress line markers, but do not duplicate flags
  // that are already present in the recorded cc1 argv.
  if (!hasP)
    args.insert(args.begin(), "-P");
  if (!hasE)
    args.insert(args.begin(), "-E");

  // Appended last so every producer-recorded search path is tried first.
  args.insert(args.end(), extraArgs.begin(), extraArgs.end());

  // Ensure language is specified (important for non-.c suffixes like '.mod').
  bool hasX = false;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "-x") {
      hasX = true;
      break;
    }
  }
  if (!hasX && !ctx.lang.empty()) {
    args.push_back("-x");
    args.push_back(ctx.lang);
  }

  // Set output and input path.
  args.push_back("-o");
  args.push_back(tmpPath.str().str());
  args.push_back(absInput.str().str());

  std::vector<const char *> cargs;
  cargs.reserve(args.size());
  for (const std::string &s : args)
    cargs.push_back(s.c_str());

  clang::CompilerInstance ci;

  // In Clang 21.x, getVirtualFileSystem() depends on an existing FileManager,
  // so diagnostics must be created against an external VFS first.
  auto vfs = llvm::vfs::getRealFileSystem();
  ci.createDiagnostics(*vfs);
  if (!ci.hasDiagnostics()) {
    return createStringError(inconvertibleErrorCode(),
                             "failed to create diagnostics engine");
  }

  if (!clang::CompilerInvocation::CreateFromArgs(ci.getInvocation(),
                                                 ArrayRef<const char *>(cargs),
                                                 ci.getDiagnostics())) {
    return createStringError(inconvertibleErrorCode(),
                             "failed to parse clang invocation");
  }

  // Be explicit: '-P' should suppress line markers.
  ci.getPreprocessorOutputOpts().ShowLineMarkers = false;
  ci.getFileSystemOpts().WorkingDir = ctx.cwd;

  ci.createFileManager();
  ci.createSourceManager(ci.getFileManager());

  // Run clang's preprocessor.
  clang::PrintPreprocessedAction action;
  if (!ci.ExecuteAction(action)) {
    return createStringError(inconvertibleErrorCode(),
                             "clang preprocessing failed");
  }

  // Read back in the temporary preprocessed output file that was created
  // by clang.
  std::string out;
  readTempFileForPreprocessRecheck(tmpPath, out);
  return out;
}

Error compareTokens(ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks) {
  // Compare token spellings up to the min length first.
  const size_t n = std::min(aToks.size(), bToks.size());
  for (size_t i = 0; i < n; ++i) {
    if (aToks[i].spelling != bToks[i].spelling) {
      const std::string aDbg = stringutils::showWs(
          stringutils::clip(StringRef(aToks[i].spelling), 100));
      const std::string bDbg = stringutils::showWs(
          stringutils::clip(StringRef(bToks[i].spelling), 180));
      return createStringError(
          inconvertibleErrorCode(),
          formatv("token mismatch at index {0}: A='{1}' B='{2}'", i, aDbg, bDbg)
              .str());
    }
    if (inDebugMode()) {
      const std::string aDbg = stringutils::showWs(
          stringutils::clip(StringRef(aToks[i].spelling), 100));
      const std::string bDbg = stringutils::showWs(
          stringutils::clip(StringRef(bToks[i].spelling), 180));
      debug("compare", "token match at index {0}: A='{1}' B='{2}'", i, aDbg,
            bDbg);
    }
  }

  if (aToks.size() != bToks.size()) {
    return createStringError(
        inconvertibleErrorCode(),
        formatv("token count mismatch: A={0} B={1}", aToks.size(), bToks.size())
            .str());
  }
  return Error::success();
}

static Error writePruneValidationSource(StringRef path, StringRef bytes) {
  std::error_code ec;
  raw_fd_ostream os(path, ec, sys::fs::OF_Text);
  if (ec)
    return createStringError(
        ec, formatv("cannot write pruning validation source '{0}'", path));
  os << bytes;
  os.close();
  if (os.has_error())
    return createStringError(
        os.error(),
        formatv("failed to flush pruning validation source '{0}'", path));
  return Error::success();
}

/// Return true iff two already-preprocessed `-E -P` byte streams have the same
/// token sequence.
///
/// Final line-control pruning normally requires byte-for-byte preprocessing
/// equivalence.  That is intentionally stronger than the refolding checker, but
/// it is too strong for stale `#line` directives whose only remaining effect is
/// Clang's cosmetic blank-line accounting around zero-token/comment-only source
/// lines.  In those cases the correct semantic oracle is the same one used by
/// `--check`: the emitted source must replay to the same preprocessed token
/// stream, including any materialized `__LINE__`, `__FILE__`, and
/// `__FILE_NAME__` expansions.
static bool
preprocessedTokensEqualForLinePrune(StringRef currentPP, StringRef candidatePP,
                                    const RefoldModel::PreprocessContext &ctx,
                                    std::string &reason) {
  const LangOptions lexLang = makeRefoldLexLangOptions(ctx.lang, ctx.argv);
  std::vector<PPTok> currentTokens;
  std::vector<PPTok> candidateTokens;
  std::vector<std::size_t> currentOffsets;
  std::vector<std::size_t> candidateOffsets;

  lexPPTokens(currentPP.str(), currentTokens, currentOffsets, lexLang);
  lexPPTokens(candidatePP.str(), candidateTokens, candidateOffsets, lexLang);

  const size_t n = std::min(currentTokens.size(), candidateTokens.size());
  for (size_t i = 0; i < n; ++i) {
    if (currentTokens[i].spelling == candidateTokens[i].spelling)
      continue;

    reason = formatv("preprocessed bytes differ and token streams differ at "
                     "index {0}: current='{1}' candidate='{2}'",
                     i,
                     stringutils::showWs(stringutils::clip(
                         StringRef(currentTokens[i].spelling), 100)),
                     stringutils::showWs(stringutils::clip(
                         StringRef(candidateTokens[i].spelling), 100)))
                 .str();
    return false;
  }

  if (currentTokens.size() != candidateTokens.size()) {
    reason = formatv("preprocessed bytes differ and token counts differ: "
                     "current={0} candidate={1}",
                     currentTokens.size(), candidateTokens.size())
                 .str();
    return false;
  }

  reason.clear();
  return true;
}

/// Create an internal final-pruning validation callback.
///
/// The callback does not implement a user-facing `--check` mode.  It is an
/// executable guard for one proposed `#line` deletion: preprocess the current
/// accepted final source and the candidate final source through the same
/// `clang -E -P` context, using one stable temporary source path located beside
/// the producer's own source.  Reusing the same source path for both inputs
/// keeps `__FILE__` and quoted-include lookup comparable, and anchoring on the
/// producer's directory is what makes a quoted include resolve to the header the
/// producer actually read.  Byte-for-byte equality is accepted first; if the
/// only difference is preprocessing trivia, token-sequence equality is also
/// accepted because the refolding soundness oracle is token equivalence.
FinalSourcePreprocessCallback
buildFinalSourcePreprocessCallback(StringRef anchorPath,
                                   const RefoldModel::PreprocessContext &ctx,
                                   ArrayRef<std::string> verifyIncludeDirs) {
  SmallString<256> outputDir(anchorPath);
  sys::path::remove_filename(outputDir);
  if (outputDir.empty())
    outputDir = ".";

  SmallString<256> model(outputDir);
  sys::path::append(model, ".clang-refold-observer-audit-%%%%%%.c");
  std::string modelText = model.str().str();

  std::vector<std::string> extraArgs;
  extraArgs.reserve(verifyIncludeDirs.size() * 2);
  for (const std::string &dir : verifyIncludeDirs) {
    extraArgs.push_back("-I");
    extraArgs.push_back(dir);
  }

  return [modelText, ctx,
          extraArgs](StringRef finalSource) -> std::optional<std::string> {
    SmallString<256> tmpPath;
    int tmpFD = -1;
    if (sys::fs::createUniqueFile(modelText, tmpFD, tmpPath))
      return std::nullopt;
    {
      raw_fd_ostream closeStream(tmpFD, /*shouldClose=*/true);
      closeStream.close();
    }
    auto cleanup = make_scope_exit([&]() { (void)sys::fs::remove(tmpPath); });

    if (Error err = writePruneValidationSource(tmpPath, finalSource)) {
      consumeError(std::move(err));
      return std::nullopt;
    }
    auto ppOrErr = preprocessToBytes(tmpPath, ctx, extraArgs);
    if (!ppOrErr) {
      consumeError(ppOrErr.takeError());
      return std::nullopt;
    }
    return std::move(*ppOrErr);
  };
}

FinalLineControlValidationCallback buildFinalLineControlValidationCallback(
    StringRef outputPath, const RefoldModel::PreprocessContext &ctx) {
  SmallString<256> outputDir(outputPath);
  sys::path::remove_filename(outputDir);
  if (outputDir.empty())
    outputDir = ".";

  SmallString<256> model(outputDir);
  sys::path::append(model, ".clang-refold-line-prune-%%%%%%.c");
  std::string modelText = model.str().str();

  return [modelText, ctx](StringRef currentOutput, StringRef candidateOutput,
                          std::string &reason) -> bool {
    SmallString<256> tmpPath;
    int tmpFD = -1;
    if (std::error_code ec =
            sys::fs::createUniqueFile(modelText, tmpFD, tmpPath)) {
      reason = formatv("could not create pruning validation source '{0}': {1}",
                       modelText, ec.message())
                   .str();
      return false;
    }

    {
      raw_fd_ostream closeStream(tmpFD, /*shouldClose=*/true);
      closeStream.close();
    }

    auto cleanup = make_scope_exit([&]() { (void)sys::fs::remove(tmpPath); });

    if (Error err = writePruneValidationSource(tmpPath, currentOutput)) {
      reason = toString(std::move(err));
      return false;
    }

    auto currentPPOrErr = preprocessToBytes(tmpPath, ctx);
    if (!currentPPOrErr) {
      reason = formatv("failed to preprocess current final source: {0}",
                       toString(currentPPOrErr.takeError()))
                   .str();
      return false;
    }

    if (Error err = writePruneValidationSource(tmpPath, candidateOutput)) {
      reason = toString(std::move(err));
      return false;
    }

    auto candidatePPOrErr = preprocessToBytes(tmpPath, ctx);
    if (!candidatePPOrErr) {
      reason = formatv("failed to preprocess candidate final source: {0}",
                       toString(candidatePPOrErr.takeError()))
                   .str();
      return false;
    }

    if (*currentPPOrErr != *candidatePPOrErr) {
      if (!preprocessedTokensEqualForLinePrune(*currentPPOrErr,
                                               *candidatePPOrErr, ctx, reason))
        return false;
      reason.clear();
      return true;
    }

    reason.clear();
    return true;
  };
}

} // namespace refold
} // namespace clang
