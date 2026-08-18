//===--- clang-refold.cpp - Main C source refolding utility -----*- C++ -*-===//
//
// Command-line front end for the C source refolding engine.
//
// This tool takes an original preprocessed stream **A** (.i), an edited
// preprocessed stream **B** (.i.mod), and a refold map **M** (JSON), and emits
// a partially expanded C translation unit (**.mod**) where only those
// preprocessor constructs whose expanded bytes were changed in **B** are
// re-materialized in source form.
//
// Synopsis:
//   clang-refold [--log-level=<value>] -p <A.i> -P <B.i.mod> -r <map.json> \
//     -o <out.c> [--check <out.c>] [--no-lines] [--strict] \
//     [--proof-audit=<value>] [--emit-edit-map <path>]
//
// Options:
//   -p, --pp                Path to original preprocessed input A (.i).
//   -P, --pp-mod            Path to edited preprocessed input B (.i.mod).
//   -r, --refold-map        Path to refold map JSON produced by the modified
//                           Clang preprocessor (.refold.json).
//   -o, --out               Path to write the refolded TU (.c.mod).
//   -c, --check             Verification mode: re-preprocess the named output
//                           and confirm its token stream matches B.
//   -n, --no-lines          Relax `--check` token comparison for tokens whose
//                           values legitimately differ when `#line` directives
//                           are pruned (`__LINE__`, `__FILE__`, etc.).
//   -s, --strict            Fail closed when the proof lattice would have
//                           accepted a non-emitted-carrier path.
//   --proof-audit=<v>       Witness-resolver audit policy (off/probe/strict).
//   --emit-edit-map         Write B↔source byte ranges for materialized edits.
//   --log-level=<value>     Set log level (default is --info).
//     =trace, =debug, =info, =warn, =error, =fatal
//   --help                  Display available options (--help-hidden for more).
//   --version               Display the version of this program.
//
// Notes:
//   * All file paths must be absolute/canonical except textual include
//     targets, which are re-emitted as written in source.
//   * Output TU formatting is stable: internal whitespace of edited fragments
//     is preserved; only boundary spaces may be normalized to avoid double
//     spaces or token gluing at splice points.
//
// Determinism & Side Effects:
//   * No heuristics: if **M** lacks coverage for an edited region, the tool
//     fails with a specific diagnostic rather than guessing.
//   * Non-interactive; no network or environment-dependent state beyond
//     invoking Clang's raw lexer to tokenize the input PP streams and (in
//     `--check` mode) invoking Clang's preprocessor to verify the emitted
//     output.
//
// Example:
//   clang-refold \
//     -p test.c.i \
//     -P test.c.i.mod \
//     -r test.c.refold.json \
//     -o test.c.mod \
//     --log-level=debug
//
// See also:
//   RefoldEngine
//   RefoldModel
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#include "core/RefoldEngine.h"
#include "core/RefoldLangOptions.h"
#include "core/RefoldLog.h"
#include "core/RefoldPreprocessRecheck.h"
#include "core/RefoldSchema.h"
#include "line-control/RefoldNoLinesPruning.h"
#include "sideband/RefoldSidebandPragmaEdits.h"
#include "source/DiffAlgorithms.h"
#include "util/StringUtils.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/JSONSchemaValidator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

using namespace llvm;
using namespace clang;
using namespace clang::refold;

namespace {

// ----------------------------- JSON Parser -----------------------------------

/// \brief Parse a JSON value from a file path.
///
/// Opens \p filePath, reads its contents, and parses it as JSON using
/// `llvm::json::parse()`.
///
/// \param filePath Absolute or canonical path to a UTF-8 text file.
/// \returns
///   - On success: an `llvm::json::Value` containing the parsed JSON.
///   - On failure: an `llvm::Error` with a message such as:
///       * "failed to open file: <path>" if the file cannot be opened
///       * a parse error from `llvm::json::parse()` if the contents are
///         not valid JSON
///
/// \note The returned `Expected` may be consumed with `takeError()` to
///       propagate errors, or dereferenced on success.
Expected<json::Value> parseJSONFromFile(StringRef filePath) {
  auto bufOrErr = MemoryBuffer::getFile(filePath);
  if (!bufOrErr)
    return createStringError(inconvertibleErrorCode(),
                             "failed to open file: " + filePath);

  auto parsed = json::parse(bufOrErr.get()->getBuffer());
  if (!parsed)
    return parsed.takeError();

  return std::move(*parsed);
}

/// \brief Parse a JSON file and validate it against a JSON Schema.
///
/// Parses \p schemaJson as a JSON Schema, parses the JSON document at
/// \p jsonPath, validates the document against the schema, and returns the
/// validated top-level object.
///
/// \param jsonPath  Path to the JSON document to validate (UTF-8 text).
/// \param schemaJson JSON string containing the schema to validate against.
/// \returns
///   - On success: an `llvm::json::Object` representing the validated
///     top-level JSON object.
///   - On failure: an `llvm::Error` describing one of:
///       * Schema parse failure (invalid JSON in \p schemaJson)
///       * Schema is not an object
///       * Document read/parse failure (via `parseJSONFromFile`)
///       * Schema validation errors from `json::JSONSchemaValidator`
///       * Top-level JSON is not an object
///
/// \see parseJSONFromFile
Expected<json::Object> parseAndValidateJSON(StringRef jsonPath,
                                            StringRef schemaJson) {
  // Parse the schema from the provided string
  Expected<json::Value> schemaParsed = json::parse(schemaJson);
  if (!schemaParsed)
    return schemaParsed.takeError();

  const json::Object *schemaObj = schemaParsed->getAsObject();
  if (!schemaObj) {
    return createStringError(inconvertibleErrorCode(),
                             "provided schema is not a valid JSON object");
  }

  // Parse the JSON file to be validated
  Expected<json::Value> jsonParsed = parseJSONFromFile(jsonPath);
  if (!jsonParsed)
    return jsonParsed.takeError();

  // Validate
  json::JSONSchemaValidator Validator(*schemaObj);
  if (Error err = Validator.validate(*jsonParsed))
    return std::move(err);

  // Return the validated JSON object
  const json::Object *resultObj = jsonParsed->getAsObject();
  if (!resultObj)
    return createStringError(inconvertibleErrorCode(),
                             "top-level JSON is not an object");

  return *resultObj;
}

// ------------------------------ Misc Utils -----------------------------------

/// \brief Require a CLI option that is intended to appear exactly once.
///
/// Verifies that \p opt was provided (intended for flags that must appear
/// exactly once). If the option is missing, emits a fatal diagnostic and
/// terminates.
///
/// \param flag Spelling used in diagnostics (e.g. "--config").
/// \param opt  Parsed option to check.
void requireExactlyOnce(StringRef flag, const cl::Option &opt) {
  if (opt.getNumOccurrences() != 1)
    REFOLD_LOG_FATAL("cli", "option '{0}' must be specified exactly once",
                     flag);
}

/// Load the contents of \p path into \p out, fatal-erroring on I/O failure.
///
/// Driver-only helper used by `main` to slurp the original and modified
/// preprocessed input files into memory before they enter the engine.
void readFile(StringRef path, std::string &out) {
  auto bufOrErr = MemoryBuffer::getFile(path);
  if (!bufOrErr) {
    REFOLD_LOG_FATAL("file/load", "cannot read file: {0} ({1})", path,
                     bufOrErr.getError().message());
  }
  out.assign(bufOrErr->get()->getBufferStart(),
             bufOrErr->get()->getBufferEnd());
}

/// Write the optional materialized-edit map as stable, human-readable JSON.
///
/// The schema is deliberately small: each entry records one materialized edit
/// with a half-open B-byte range from `--pp-mod` and the half-open byte range
/// in the final `--out` source that represents that B-side materialization.
/// These are source-envelope ranges: a structure-preserving macro edit may map
/// an expanded B envelope to the rewritten invocation argument that regenerates
/// it.
static void
writeMaterializedEditMap(StringRef path, StringRef ppModPath,
                         StringRef modifiedSrcPath,
                         ArrayRef<MaterializedEditMapping> mappings) {
  std::error_code ec;
  raw_fd_ostream os(path, ec, sys::fs::OF_Text);
  if (ec)
    REFOLD_LOG_FATAL("edit-map/write", "cannot write {0}: {1}", path,
                     ec.message());

  // Keep the range shape identical for both sides of every edit.  The
  // surrounding object names say which file the range belongs to.
  auto writeByteRangeObject = [](json::OStream &j, uint64_t begin,
                                 uint64_t end) {
    j.attribute("begin", begin);
    j.attribute("end", end);
  };

  json::OStream j(os, /*IndentSize=*/2);
  j.object([&] {
    j.attribute("modified_pp_source", ppModPath);
    j.attribute("refolded_output", modifiedSrcPath);

    j.attributeArray("edits", [&] {
      for (const MaterializedEditMapping &m : mappings) {
        j.object([&] {
          j.attributeObject("modified_pp_byte_range", [&] {
            writeByteRangeObject(j, m.modifiedPreprocessedBegin,
                                 m.modifiedPreprocessedEnd);
          });
          j.attributeObject("refolded_output_byte_range", [&] {
            writeByteRangeObject(j, m.refoldedSourceBegin, m.refoldedSourceEnd);
          });
        });
      }
    });
  });

  os << '\n';
  os.close();
}

} // end anonymous namespace

// ------------------------- Command-Line Options ------------------------------

static cl::opt<std::string>
    PPPath("pp", // long name: --pp
           cl::desc("Path to preprocessed file (.c.i) [required(1)]"),
           cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -p (points to --pp)
static cl::alias PPPathShort("p", cl::desc("Alias for --pp"),
                             cl::aliasopt(PPPath), cl::cat(RefoldCategory));

static cl::opt<std::string> PPModPath(
    "pp-mod", // long name: --pp-mod
    cl::desc("Path to modified preprocessed file (.c.i.mod) [required(1)(2)]"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -P (points to --pp-mod)
static cl::alias PPModPathShort("P", cl::desc("Alias for --pp-mod"),
                                cl::aliasopt(PPModPath),
                                cl::cat(RefoldCategory));

static cl::opt<std::string> RefoldJSONPath(
    "refold-map", // long name: --refold-map
    cl::desc("Path to refold JSON file (.c.refold.json) [required(1)(2)]"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -r (points to --refold-map)
static cl::alias RefoldJSONPathShort("r", cl::desc("Alias for --refold-map"),
                                     cl::aliasopt(RefoldJSONPath),
                                     cl::cat(RefoldCategory));

static cl::opt<std::string> ModifiedSrcPath(
    "out", // long name: --out
    cl::desc("Path to refolded C source file (.c.mod) [required(1)]"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -o (points to --out)
static cl::alias ModifiedSrcPathShort("o", cl::desc("Alias for --out"),
                                      cl::aliasopt(ModifiedSrcPath),
                                      cl::cat(RefoldCategory));

static cl::opt<std::string> EmitEditMapPath(
    "emit-edit-map",
    cl::desc("Path to write materialized edit map JSON containing "
             "B-to-refolded-output byte ranges"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

static cl::opt<std::string> CheckSrcPath(
    "check", // long name: --check
    cl::desc("Verify a refolding by preprocessing this refolded C source and "
             "comparing tokens to --pp-mod"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -c (points to --check)
static cl::alias CheckSrcPathShort("c", cl::desc("Alias for --check"),
                                   cl::aliasopt(CheckSrcPath),
                                   cl::cat(RefoldCategory));

static cl::opt<bool> NoLines(
    "no-lines",
    cl::desc(
        "Don't include #line directives in refold source (off by default)"),
    cl::init(false), cl::cat(RefoldCategory));

// Short alias: -n (points to --no-lines)
static cl::alias NoLinesShort("n", cl::desc("Alias for --no-lines"),
                              cl::aliasopt(NoLines), cl::cat(RefoldCategory));

static cl::opt<OutputVerificationMode> VerifyOutput(
    "verify-output",
    cl::desc("Verify the refolded source against --pp-mod by re-preprocessing "
             "it (default: off)"),
    cl::init(OutputVerificationMode::Off),
    cl::values(clEnumValN(OutputVerificationMode::Off, "off",
                          "Do not verify"),
               clEnumValN(OutputVerificationMode::Repair, "repair",
                          "Expand the smallest diverging region and retry"),
               clEnumValN(OutputVerificationMode::Fatal, "fatal",
                          "Report the divergence and fail")),
    cl::cat(RefoldCategory));

static cl::list<std::string> VerifyIncludeDirs(
    "verify-include-dir",
    cl::desc(
        "Extra include dir for re-preprocessing verification (--check, "
        "--verify-output). Repeatable; searched after producer paths miss."),
    cl::value_desc("dir"), cl::ZeroOrMore, cl::cat(RefoldCategory));

static cl::opt<bool> StrictMode(
    "strict", cl::desc("Enable strict refolding and authoritative proof audit"),
    cl::init(false), cl::cat(RefoldCategory));

// Short alias: -s (points to --strict)
static cl::alias StrictModeShort("s", cl::desc("Alias for --strict"),
                                 cl::aliasopt(StrictMode),
                                 cl::cat(RefoldCategory));

static cl::opt<ProofAuditMode> ProofAuditModeOpt(
    "proof-audit",
    cl::desc("Set witness-resolver audit policy (default: strict with "
             "--strict, off otherwise)"),
    cl::init(ProofAuditMode::Default),
    cl::values(clEnumValN(ProofAuditMode::Off, "off", "Do not audit"),
               clEnumValN(ProofAuditMode::Probe, "probe",
                          "Report resolver decisions without giving them "
                          "authority"),
               clEnumValN(ProofAuditMode::Strict, "strict",
                          "Give the resolver authority and fail closed")),
    cl::cat(RefoldCategory));

static constexpr char Overview[] = R"(
  Deterministically reconstruct partially expanded C source from edited
  preprocessed output.

  Consumes:
    (1) the original preprocessed TU (.c.i),
    (2) a modified preprocessed output (.c.i.mod), and
    (3) the refold map JSON (.c.refold.json) emitted by clang’s --refold-map.

  It re-lexes and aligns the original and modified token streams, then projects
  the edits back through recorded includes, macros, and conditional structure to
  produce a stable, semantically equivalent C source that preserves the original
  preprocessing hierarchy.

  Modes:
    (1) Produce a refolding:
          clang-refold --pp foo.c.i --pp-mod foo.c.i.mod \
            --refold-map foo.c.refold.json --out foo.c.mod
    (2) Verify a refolding (token check):
          clang-refold --check foo.c.mod --refold-map foo.c.refold.json \
            --pp-mod foo.c.i.mod

       This mode re-preprocesses foo.c.mod using the captured pp_ctx (cwd/argv)
       embedded in the refold map JSON, lexes both token streams, and compares
       them while ignoring whitespace.
  )";

// ----------------------------- Main Program ----------------------------------

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  sys::PrintStackTraceOnErrorSignal(argv[0], true);

  cl::HideUnrelatedOptions(RefoldCategory);
  cl::ParseCommandLineOptions(argc, argv, Overview);

  REFOLD_LOG_INFO("log", "log level set to {0}", LogLevelOpt);

  // We output a custom error message if the following flags appear more than
  // once and remove cl::Required from the relevant cl::opt's. This is because
  // the error message would otherwise be misleading, stating that the option
  // must be specified at least once, which is not the case here.
  const bool onlyCheck = (CheckSrcPath.getNumOccurrences() != 0);

  // `Default` is the unset state, and is deliberately not selectable: it means
  // "derive from --strict", which is resolved downstream rather than here.
  const ProofAuditMode proofAuditMode = ProofAuditModeOpt;
  if (StrictMode && proofAuditMode != ProofAuditMode::Default &&
      proofAuditMode != ProofAuditMode::Strict)
    REFOLD_LOG_FATAL("options", "--strict requires --proof-audit=strict");

  // Enforce exactly one supported invocation mode:
  //   (1) Refold:
  //       clang-refold --pp A.i --pp-mod B.i.mod --refold-map map.json --out \
  //         out.c.mod
  //   (2) Verify:
  //       clang-refold --check out.c.mod --refold-map map.json --pp-mod B.i.mod
  requireExactlyOnce("--refold-map", RefoldJSONPath);
  requireExactlyOnce("--pp-mod", PPModPath);

  const bool emitEditMap = EmitEditMapPath.getNumOccurrences() != 0;

  if (onlyCheck) {
    requireExactlyOnce("--check", CheckSrcPath);
    // Verify mode.
    if (PPPath.getNumOccurrences() != 0 ||
        ModifiedSrcPath.getNumOccurrences() != 0 || emitEditMap) {
      REFOLD_LOG_FATAL(
          "cli", "invalid option combination: --check cannot be used with "
                 "--pp, --out, or --emit-edit-map");
    }
  } else {
    // Refold mode.
    requireExactlyOnce("--pp", PPPath);
    requireExactlyOnce("--out", ModifiedSrcPath);
    if (emitEditMap && EmitEditMapPath.getValue().empty())
      REFOLD_LOG_FATAL("cli",
                       "--emit-edit-map requires a non-empty output path");
  }

  // Parse and validate the refold map JSON file.
  auto parsedObjOrErr = parseAndValidateJSON(RefoldJSONPath, RefoldSchema);
  if (!parsedObjOrErr) {
    handleAllErrors(parsedObjOrErr.takeError(), [&](const ErrorInfoBase &e) {
      REFOLD_LOG_FATAL("json", "failed to parse refold '{0}' JSON file: {1}",
                       RefoldJSONPath, e.message());
    });
  } else {
    REFOLD_LOG_INFO("json", "refold JSON file '{0}' validated", RefoldJSONPath);
  }
  const json::Object &rootJson = *parsedObjOrErr;
  assert(!rootJson.empty() && "parsed refold map JSON object is empty");

  // Read files and tokenize.
  //
  // Refold mode deliberately treats --pp-mod as the raw edited PP replay
  // surface. That preserves comment/trivia insertions and directive-shaped
  // edits as source text for the existing owner/proof lattice. Check mode is
  // different: it preprocesses both the emitted source and --pp-mod because the
  // final oracle asks whether both replay to the same PP token stream.
  std::string aBytes, bBytes;
  auto ctxOrErr = RefoldModel::ParsePreprocessContext(rootJson);
  if (!ctxOrErr) {
    handleAllErrors(ctxOrErr.takeError(), [&](const ErrorInfoBase &e) {
      REFOLD_LOG_FATAL("model", "failed to parse pp_ctx from refold map: {0}",
                       e.message());
    });
  }
  const PPCtx ctx = *ctxOrErr;
  const LangOptions lexLang = makeRefoldLexLangOptions(ctx.lang);

  std::optional<PPCtx> checkCtx;
  if (onlyCheck) {
    checkCtx = ctx;

    // `preprocessToBytes` appends these verbatim to the recorded cc1 argv, so a
    // declared directory must arrive already spelled as a search-path flag; a
    // bare path would be read as another input file.  This mirrors the engine's
    // observer-audit replay, which formats the same list the same way.
    std::vector<std::string> verifyIncludeArgs;
    verifyIncludeArgs.reserve(VerifyIncludeDirs.size() * 2);
    for (const std::string &dir : VerifyIncludeDirs) {
      verifyIncludeArgs.push_back("-I");
      verifyIncludeArgs.push_back(dir);
    }

    // Preprocess the refolded C source:
    {
      auto ppOrErr = preprocessToBytes(CheckSrcPath, ctx, verifyIncludeArgs);
      if (!ppOrErr) {
        handleAllErrors(ppOrErr.takeError(), [&](const ErrorInfoBase &e) {
          REFOLD_LOG_ERROR("pp", "failed to preprocess --check input: {0}",
                           e.message());
        });
        // Fatal logging exits only in fatal mode, so the failure must be
        // reported here as well; falling through would read an Expected
        // that holds an error and abort.
        return 1;
      }
      aBytes = std::move(*ppOrErr);
    }

    // Preprocess the edited preprocessed replay file.
    {
      auto ppOrErr = preprocessToBytes(PPModPath, ctx, verifyIncludeArgs);
      if (!ppOrErr) {
        handleAllErrors(ppOrErr.takeError(), [&](const ErrorInfoBase &e) {
          REFOLD_LOG_ERROR("pp", "failed to preprocess --pp-mod input: {0}",
                           e.message());
        });
        // Fatal logging exits only in fatal mode, so the failure must be
        // reported here as well; falling through would read an Expected
        // that holds an error and abort.
        return 1;
      }
      bBytes = std::move(*ppOrErr);
    }
  } else {
    readFile(PPPath, aBytes);
    readFile(PPModPath, bBytes);
  }

  std::vector<PPTok> aToks, bToks;
  std::vector<std::size_t> aTokByteOff, bTokByteOff;
  std::vector<SidebandPragmaEdit> sidebandPragmaEdits;
  lexPPTokens(aBytes, aToks, aTokByteOff, lexLang);
  lexPPTokens(bBytes, bToks, bTokByteOff, lexLang);

  if (!onlyCheck) {
    std::vector<SidebandPragmaLine> aSidebandPragmas =
        collectSidebandPragmaLines(aBytes);
    std::vector<SidebandPragmaLine> bSidebandPragmas =
        collectSidebandPragmaLines(bBytes);
    annotateSidebandPragmaTokenGaps(aSidebandPragmas, aToks, aTokByteOff);
    annotateSidebandPragmaTokenGaps(bSidebandPragmas, bToks, bTokByteOff);

    if (!aSidebandPragmas.empty() || !bSidebandPragmas.empty()) {
      if (buildSidebandPragmaSourceEdits(rootJson, RefoldJSONPath,
                                         aSidebandPragmas, bSidebandPragmas,
                                         aToks, aTokByteOff, bBytes, bToks,
                                         bTokByteOff, sidebandPragmaEdits)) {
        // Keep the raw `.i` byte buffers intact, but remove preserved pragma
        // directive tokens from the sequences fed to the structural diff.  The
        // matching source directive edits are carried separately in
        // sidebandPragmaEdits, so comments/trivia around the original source
        // pragma stay on the normal TU edit path instead of being lost to
        // terminal raw-B fallback.
        filterSidebandPragmaTokens(aSidebandPragmas, aToks, aTokByteOff,
                                   aBytes.size());
        filterSidebandPragmaTokens(bSidebandPragmas, bToks, bTokByteOff,
                                   bBytes.size());
        REFOLD_LOG_DEBUG(
            "pragma/sideband",
            "normalized sideband pragmas: A={0} B={1} sourceEdits={2}",
            aSidebandPragmas.size(), bSidebandPragmas.size(),
            sidebandPragmaEdits.size());
      } else {
        // Unsupported sideband forms, such as B-only pragma insertions without
        // a map-backed source anchor, remain in the token stream.  The existing
        // token-count/domain checks will route them through the explicit
        // fallback path rather than guessing a source placement.
        sidebandPragmaEdits.clear();
        REFOLD_LOG_DEBUG(
            "pragma/sideband",
            "sideband pragma stream not fully modelled; keeping raw tokens "
            "for fallback classification");
      }
    }
  }

  REFOLD_LOG_DEBUG("lex", "{0} tokens={1} {2} tokens={3}", PPPath, aToks.size(),
                   PPModPath, bToks.size());

  // Append the sentinel to both source offsets.
  if (bTokByteOff.empty() || bTokByteOff.back() != bBytes.size()) {
    if (bTokByteOff.empty() || bTokByteOff.back() < bBytes.size())
      bTokByteOff.push_back(bBytes.size());
  }
  if (aTokByteOff.empty() || aTokByteOff.back() != aBytes.size()) {
    if (aTokByteOff.empty() || aTokByteOff.back() < aBytes.size())
      aTokByteOff.push_back(aBytes.size());
  }

  if (onlyCheck) {
    if (!checkCtx)
      REFOLD_LOG_FATAL("cli", "internal error: missing pp_ctx in --check mode");

    // Verification composes zero or more per-B-token relaxation masks and
    // compares mask-aware; with no mask this is an exact token comparison.
    std::vector<uint8_t> ignoreMask;
    bool haveMask = false;

    auto mergeMask = [&](std::vector<uint8_t> mask) {
      if (!haveMask) {
        ignoreMask = std::move(mask);
        haveMask = true;
      } else if (mask.size() == ignoreMask.size()) {
        for (size_t i = 0; i < ignoreMask.size(); ++i)
          ignoreMask[i] |= mask[i];
      }
    };

    // Under --no-lines, relax comparison for location-sensitive predefined
    // macros whose values legitimately differ once #line directives are pruned.
    if (NoLines) {
      auto maskOrErr = buildNoLinesIgnoreMask(rootJson, *checkCtx, bToks);
      if (!maskOrErr) {
        handleAllErrors(maskOrErr.takeError(), [&](const ErrorInfoBase &e) {
          REFOLD_LOG_FATAL("check",
                           "failed to build --no-lines ignore mask: {0}",
                           e.message());
        });
      }
      mergeMask(std::move(*maskOrErr));
    }

    // In relaxed (non-strict) mode, a folded argument edit may leave a
    // stringified occurrence stale, so re-preprocessing the refolded source
    // regenerates a different `#arg` than B carried.  Tolerate that difference
    // only where B kept the original stringification.  Strict verification is
    // byte-exact and skips this relaxation.
    if (!StrictMode) {
      auto maskOrErr =
          buildRelaxedStringifyIgnoreMask(rootJson, *checkCtx, bToks);
      if (!maskOrErr) {
        handleAllErrors(maskOrErr.takeError(), [&](const ErrorInfoBase &e) {
          REFOLD_LOG_FATAL(
              "check", "failed to build relaxed stringify ignore mask: {0}",
              e.message());
        });
      }
      mergeMask(std::move(*maskOrErr));
    }

    Error err = haveMask ? compareTokensNoLinesAware(aToks, bToks, ignoreMask)
                         : compareTokens(aToks, bToks);
    if (err) {
      outs() << toString(std::move(err)) << "\n";
      outs() << "FAILURE!\n";
      return 1;
    }
    outs() << "SUCCESS!\n";
    return 0;
  }

  // Default behavior: single refold.
  std::vector<MaterializedEditMapping> materializedEditMappings;
  auto refoldedOrErr = RefoldEngine::Refold(
      rootJson, aBytes, aToks, aTokByteOff, bBytes, bToks, bTokByteOff, NoLines,
      StrictMode, proofAuditMode, ModifiedSrcPath, sidebandPragmaEdits,
      emitEditMap ? &materializedEditMappings : nullptr,
      buildFinalLineControlValidationCallback(ModifiedSrcPath, ctx),
      VerifyOutput, VerifyIncludeDirs);
  if (!refoldedOrErr) {
    // A malformed map and a refold that fails its closing verification both
    // arrive here, and both are answers about the input rather than internal
    // invariant breaks.  Report and exit non-zero: REFOLD_LOG_FATAL would abort
    // with a crash banner, which misrepresents a refusal as a tool defect.
    logAllUnhandledErrors(refoldedOrErr.takeError(), errs(), "clang-refold: ");
    return 1;
  }

  // Finally, serialize the refolded C source to the output file.
  std::error_code ec;
  raw_fd_ostream os(ModifiedSrcPath, ec, sys::fs::OF_Text);
  if (ec) {
    REFOLD_LOG_FATAL("src/write", "cannot write {0}: {1}", ModifiedSrcPath,
                     ec.message());
  }
  os << *refoldedOrErr;
  os.close();
  REFOLD_LOG_INFO("finished", "wrote refolded C source: {0}", ModifiedSrcPath);

  if (emitEditMap) {
    writeMaterializedEditMap(EmitEditMapPath.getValue(), PPModPath,
                             ModifiedSrcPath, materializedEditMappings);
    REFOLD_LOG_INFO("finished", "wrote materialized edit map: {0}",
                    EmitEditMapPath.getValue());
  }
  return 0;
}
