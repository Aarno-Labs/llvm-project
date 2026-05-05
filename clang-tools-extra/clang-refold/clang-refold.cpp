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
//     -o <out.c>
//
// Options:
//   -p, --pp           Path to original preprocessed input A (.i).
//   -P, --pp-mod       Path to edited preprocessed input B (.i.mod).
//   -r, --refold-map   Path to refold map JSON produced by the modified
//                      Clang preprocessor (.refold.json).
//   -o, --out          Path to write the refolded TU (.c.mod).
//  --log-level=<value> Set log level (default is --info)
//    =trace             -   Trace
//    =debug             -   Debug
//    =info              -   Info
//    =warn              -   Warn
//    =error             -   Error
//    =fatal             -   Fatal
//   --help             Display available options (--help-hidden for more)
//   --version          Display the version of this program
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
//     invoking Clang for raw token dumps.
//
// Example:
//   clang-refold \
//     -p test.c.i \
//     -P test.c.i.mod \
//     -r test.c.refold.json \
//     -o test.c.mod \
//     --verbose
//
// See also:
//   RefoldEngine
//   RefoldModel
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#include "RefoldLog.h"
#include "RefoldEngine.h"
#include "RefoldSchema.h"
#include "StringUtils.h"
#include "DiffAlgorithms.h"

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
#include "llvm/Support/Path.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/JSONSchemaValidator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
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

// --------------------------- Tokenization ---------------------------------

/// \brief Lex a preprocessed byte buffer into Clang-style tokens.
///
/// Tokenizes the given preprocessed text using Clang's raw lexer so token
/// boundaries match `-E -P` behavior. The function clears and fills both
/// output arrays:
///  - `out` receives one `PPTok` per token with its exact byte spelling.
///  - `startOffs` receives the starting byte offset in the input buffer for
///    each token, plus a **sentinel** offset equal to `bytes.size()` at the
///    end (so `startOffs.size() == out.size() + 1`).
///
/// \details
///  - Ensures the buffer ends with a newline (Clang driver expects it).
///  - Uses `clang::Lexer` in raw-lex mode; whitespace tokens are **not**
///    emitted (`SetKeepWhitespaceMode(false)`), matching `-E -P`.
///  - Offsets are computed from a `clang::SourceManager` over a
///    `MemoryBuffer`, so they are stable and monotonic.
///  - UTF-8 is treated as raw bytes; spellings are byte slices of the input.
///  - Emits trace/debug logs for each token (kind, visible-WS spelling,
///    location) via the project logging helpers.
///  - On internal failures (e.g., invalid buffer retrieval) the function
///    calls `fatal(...)` and aborts; it does not throw.
///
/// \param bytes
///   Entire preprocessed file content to lex. If it does not end with '\n',
///   the function temporarily appends one for lexing.
/// \param[out] out
///   Destination vector of tokens (cleared on entry).
/// \param[out] startOffs
///   Destination vector of starting byte offsets per token, followed by a
///   one-past-end sentinel (cleared on entry).
///
/// \invariant
///   `startOffs.size() == out.size() + 1` and `startOffs` is non-decreasing.
///
/// \note
///   This path deliberately avoids the full preprocessor.
void lexPPTokens(const std::string &bytes, std::vector<PPTok> &out,
                 std::vector<std::size_t> &startOffs,
                 const LangOptions &lang) {
  out.clear();
  startOffs.clear();

  // Ensure a trailing newline.
  std::string buf = bytes;
  bool addedNL = false;
  if (buf.empty() || buf.back() != '\n') {
    buf.push_back('\n');
    addedNL = true;
  }

  debug("lexer", "entered: bytes={0} (addedNL={1})", buf.size(),
        addedNL ? "YES" : "NO");

  using namespace clang;

  // Diagnostics: heap-owning client to avoid double free on destruction.
  DiagnosticOptions diagOpts;
  IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
  auto *client = new IgnoringDiagConsumer(); // owned by Diags
  DiagnosticsEngine diags(diagIDs, diagOpts, client, /*ShouldOwnClient*/ true);

  // FS & source management.
  FileSystemOptions fso;
  FileManager fm(fso);
  SourceManager sm(diags, fm);

  // Back the "file" with our bytes.
  std::unique_ptr<MemoryBuffer> mb =
      MemoryBuffer::getMemBuffer(StringRef(buf), "pp",
                                 /*RequiresNullTerminator*/ true);
  FileID fid = sm.createFileID(std::move(mb));

  bool invalid = false;
  StringRef data = sm.getBufferData(fid, &invalid);
  if (invalid) {
    fatal("lexer", "getBufferData returned Invalid");
  }

  const char *b = data.begin();
  const char *e = data.end();

  Lexer lex(sm.getLocForStartOfFile(fid), lang, b, b, e);
  lex.SetKeepWhitespaceMode(false);
  lex.SetCommentRetentionState(true);

  // Tokenize
  for (;;) {
    Token tkn;
    lex.LexFromRawLexer(tkn);
    if (tkn.is(tok::eof))
      break;

    unsigned len = tkn.getLength();
    std::size_t off = sm.getFileOffset(sm.getFileLoc(tkn.getLocation()));
    std::size_t end = std::min(buf.size(), off + static_cast<std::size_t>(len));

    PPTok ppt;
    if (off <= end && end <= buf.size()) {
      ppt.spelling.assign(buf.data() + off, buf.data() + end);
    } else {
      ppt.spelling.clear(); // defensive
    }

    // Debug: kind, spelled (with visible WS), and location.
    const char *kindName = tok::getTokenName(tkn.getKind());
    ppt.kind = kindName;
    PresumedLoc pl = sm.getPresumedLoc(tkn.getLocation());

    std::optional<unsigned> line;
    std::optional<unsigned> col;
    if (pl.isValid()) {
      line = pl.getLine();
      col = pl.getColumn();
    }

    trace("lexer/parsed", "kind={0} spelled={1} off={2} len={3} li={4} co={5}",
          kindName,
          stringutils::showWS(stringutils::clip(StringRef(ppt.spelling), 80)),
          off, len, line, col);

    out.push_back(std::move(ppt));
    startOffs.push_back(off);
  }

  // Sentinel: one-past-end
  startOffs.push_back(buf.size());
  // Sanity: monotone offsets and size relationship.
  assert(startOffs.size() == out.size() + 1 && "need sentinel in startOffs");
  assert(std::is_sorted(startOffs.begin(), startOffs.end()));

  debug("lexer", "done: tokens={0}", out.size());
}

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

/// \brief Read the entire file into \p out.
///
/// Opens \p path and assigns its contents to \p out. On failure, emits a
/// fatal diagnostic and terminates.
///
/// \param path Filesystem path to read.
/// \param out  Destination string for the file contents.
void readFile(StringRef path, std::string &out) {
  auto bufOrErr = MemoryBuffer::getFile(path);
  if (!bufOrErr) {
    fatal("file/load", "cannot read file: {0} ({1})", path,
          bufOrErr.getError().message());
  }
  out.assign(bufOrErr->get()->getBufferStart(),
             bufOrErr->get()->getBufferEnd());
}

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
    fatal("cli", "option '{0}' must be specified exactly once", flag);
}

// ------------------------------ PP Context ----------------------------------

struct PPCtx {
  std::string cwd;
  std::vector<std::string> argv;
  std::string lang;
};

Expected<PPCtx> parsePPCtx(const json::Object &rootJson) {
  const json::Object *ctxObj = rootJson.getObject("pp_ctx");
  if (!ctxObj) {
    return createStringError(inconvertibleErrorCode(),
                             "missing required top-level key 'pp_ctx'");
  }

  PPCtx ctx;

  // cwd
  if (auto cwd = ctxObj->getString("cwd")) {
    ctx.cwd = cwd->str();
  } else {
    return createStringError(inconvertibleErrorCode(),
                             "pp_ctx.cwd must be a string");
  }

  // lang
  if (auto lang = ctxObj->getString("lang")) {
    ctx.lang = lang->str();
  } else {
    return createStringError(inconvertibleErrorCode(),
                             "pp_ctx.lang must be a string");
  }

  // argv
  const json::Array *argvArr = ctxObj->getArray("argv");
  if (!argvArr) {
    return createStringError(inconvertibleErrorCode(),
                             "pp_ctx.argv must be an array");
  }

  ctx.argv.reserve(argvArr->size());
  for (const json::Value &v : *argvArr) {
    auto s = v.getAsString();
    if (!s) {
      return createStringError(inconvertibleErrorCode(),
                               "pp_ctx.argv must contain only strings");
    }
    ctx.argv.emplace_back(s->str());
  }

  return ctx;
}

Expected<std::string> preprocessToBytes(StringRef inputPath, const PPCtx &ctx) {
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

  if (!clang::CompilerInvocation::CreateFromArgs(
          ci.getInvocation(), ArrayRef<const char *>(cargs),
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
  readFile(tmpPath, out);
  return out;
}

static bool isLineDirectiveSensitiveBuiltin(StringRef name) {
  return name == "__LINE__" || name == "__FILE__" ||
         name == "__FILE_NAME__" || name == "__BASE_FILE__";
}

static bool canIgnoreNoLinesMismatch(const PPTok &aTok, const PPTok &bTok) {
  if (aTok.kind != bTok.kind)
    return false;
  return aTok.kind == "numeric_constant" || aTok.kind == "string_literal" ||
         aTok.kind == "header_name";
}

/// Minimal macro metadata needed by the --no-lines checker recovery path.
///
/// The producer can report zero-length `spans` for predefined location
/// builtins. When that happens, the checker uses the macro call chain and
/// caller `body_spans` to recover the generated token position without
/// broadening the normal ignore rule.
struct NoLinesMacroInfo {
  uint64_t id = 0;
  size_t ordinal = 0;
  std::string name;
  std::string invFile;
  std::optional<uint64_t> invBegin;
  std::optional<uint64_t> callerId;
  std::vector<RefoldModel::PPSpan> spans;
  std::vector<RefoldModel::PPSpan> bodySpans;
};

/// A zero-length predefined builtin expansion that still needs one token
/// marked as line-directive-sensitive in the original preprocessed token
/// stream.
struct NoLinesSensitiveEvent {
  const NoLinesMacroInfo *item = nullptr;
  const NoLinesMacroInfo *anchor = nullptr;
  size_t ordinal = 0;
  std::set<std::string> allowedSpellings;
};

/// Read a non-negative JSON integer as an unsigned metadata identifier/offset.
static std::optional<uint64_t> getJSONUInt64(const json::Object &obj,
                                             StringRef key) {
  auto value = obj.getInteger(key);
  if (!value || *value < 0)
    return std::nullopt;
  return static_cast<uint64_t>(*value);
}

/// Return true when any span covers at least one token after clamping to the
/// token stream length. Zero-length spans intentionally return false.
static bool spanMarksAnyToken(ArrayRef<RefoldModel::PPSpan> spans,
                              size_t tokenCount) {
  for (const auto &sp : spans) {
    uint64_t begin = std::min<uint64_t>(sp.begin, tokenCount);
    uint64_t end = std::min<uint64_t>(sp.end, tokenCount);
    if (end > begin)
      return true;
  }
  return false;
}

/// Return the basename spelling used by `__FILE_NAME__`.
static std::string filenameComponent(StringRef path) {
  return sys::path::filename(path).str();
}

/// Read a source file used for `__LINE__` recovery, caching both successes and
/// failures so repeated builtin events do not repeatedly touch the filesystem.
static std::optional<std::string>
readSourceForNoLines(StringRef path, const PPCtx &ctx,
                     std::map<std::string, std::optional<std::string>> &cache) {
  std::string key = path.str();
  auto it = cache.find(key);
  if (it != cache.end())
    return it->second;

  SmallString<256> resolved(path);
  if (sys::path::is_relative(resolved)) {
    resolved = ctx.cwd;
    sys::path::append(resolved, path);
  }

  auto bufOrErr = MemoryBuffer::getFile(resolved);
  if (!bufOrErr) {
    debug("check",
          "--no-lines: could not read source '{0}' while recovering builtin "
          "location token spans",
          path);
    cache.emplace(std::move(key), std::nullopt);
    return std::nullopt;
  }

  std::string bytes(bufOrErr.get()->getBufferStart(),
                    bufOrErr.get()->getBufferEnd());
  auto inserted = cache.emplace(std::move(key), std::move(bytes));
  return inserted.first->second;
}

/// Compute the 1-based physical source line containing `offset`.
///
/// The value is used only as one exact candidate spelling for zero-length
/// `__LINE__` metadata. If the source cannot be read, callers fail closed by
/// leaving the allowed-spelling set empty.
static std::optional<uint64_t> lineNumberAtSourceOffset(
    StringRef path, uint64_t offset, const PPCtx &ctx,
    std::map<std::string, std::optional<std::string>> &cache) {
  auto bytes = readSourceForNoLines(path, ctx, cache);
  if (!bytes || offset > bytes->size())
    return std::nullopt;

  uint64_t line = 1;
  for (uint64_t i = 0; i < offset; ++i) {
    if ((*bytes)[static_cast<size_t>(i)] == '\n')
      ++line;
  }
  return line;
}

/// Walk `caller_macro_id` from a builtin expansion toward the outermost macro.
///
/// Cycles or missing references terminate the chain so malformed metadata cannot
/// make recovery loop indefinitely.
static std::vector<const NoLinesMacroInfo *>
callerChainForNoLines(const NoLinesMacroInfo &item,
                      const std::map<uint64_t, NoLinesMacroInfo> &macrosById) {
  std::vector<const NoLinesMacroInfo *> chain;
  std::set<uint64_t> seen;
  std::optional<uint64_t> cur = item.callerId;
  while (cur) {
    if (!seen.insert(*cur).second)
      break;
    auto it = macrosById.find(*cur);
    if (it == macrosById.end())
      break;
    chain.push_back(&it->second);
    cur = it->second.callerId;
  }
  return chain;
}

/// Add the exact string-token spellings a path-valued builtin may have produced
/// for a particular source path.
static void addPathSpellingsForNoLinesBuiltin(
    StringRef builtinName, StringRef path, std::set<std::string> &spellings) {
  if (path.empty())
    return;

  if (builtinName == "__FILE_NAME__") {
    spellings.insert(stringutils::quoteCString(filenameComponent(path)));
    return;
  }

  if (builtinName == "__FILE__" || builtinName == "__BASE_FILE__")
    spellings.insert(stringutils::quoteCString(path));
}

/// Build the finite set of token spellings that may correspond to a builtin
/// event under `--no-lines`.
///
/// For path-valued builtins, include the builtin's file, each caller's file, and
/// the TU source path where applicable. For `__LINE__`, compute exact physical
/// line numbers from invocation offsets when the source bytes are available.
static std::set<std::string> buildAllowedSpellingsForNoLinesBuiltin(
    const NoLinesMacroInfo &item, ArrayRef<const NoLinesMacroInfo *> chain,
    StringRef sourcePath, const PPCtx &ctx,
    std::map<std::string, std::optional<std::string>> &sourceCache) {
  std::set<std::string> spellings;

  if (item.name == "__LINE__") {
    auto addLine = [&](const NoLinesMacroInfo &macro) {
      if (macro.invFile.empty() || !macro.invBegin)
        return;
      auto line = lineNumberAtSourceOffset(macro.invFile, *macro.invBegin, ctx,
                                           sourceCache);
      if (line)
        spellings.insert(std::to_string(*line));
    };

    addLine(item);
    for (const NoLinesMacroInfo *macro : chain)
      addLine(*macro);
    return spellings;
  }

  if (item.name == "__BASE_FILE__") {
    addPathSpellingsForNoLinesBuiltin(item.name, sourcePath, spellings);
    return spellings;
  }

  addPathSpellingsForNoLinesBuiltin(item.name, item.invFile, spellings);
  for (const NoLinesMacroInfo *macro : chain)
    addPathSpellingsForNoLinesBuiltin(item.name, macro->invFile, spellings);
  addPathSpellingsForNoLinesBuiltin(item.name, sourcePath, spellings);
  return spellings;
}

/// Test whether one original preprocessed token is a plausible expansion of a
/// particular zero-length builtin event.
static bool tokenMatchesNoLinesBuiltinEvent(const PPTok &tok,
                                            const NoLinesSensitiveEvent &event) {
  StringRef name(event.item->name);
  if (name == "__LINE__") {
    if (tok.kind != "numeric_constant")
      return false;
    if (event.allowedSpellings.empty())
      return std::all_of(tok.spelling.begin(), tok.spelling.end(),
                         [](char c) { return c >= '0' && c <= '9'; });
    return event.allowedSpellings.count(tok.spelling) != 0;
  }

  if (tok.kind != "string_literal" && tok.kind != "header_name")
    return false;
  return event.allowedSpellings.count(tok.spelling) != 0;
}

/// Materialize the sorted unique token indices covered by a caller macro body.
/// These are the only positions considered when recovering a child builtin
/// whose own span did not cover any token.
static std::vector<size_t>
bodyTokenCandidatesForNoLines(ArrayRef<RefoldModel::PPSpan> bodySpans,
                              size_t tokenCount) {
  std::vector<size_t> candidates;
  for (const auto &sp : bodySpans) {
    uint64_t begin = std::min<uint64_t>(sp.begin, tokenCount);
    uint64_t end = std::min<uint64_t>(sp.end, tokenCount);
    for (uint64_t i = begin; i < end; ++i)
      candidates.push_back(static_cast<size_t>(i));
  }

  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  return candidates;
}

/// Recover ignore-mask bits for predefined location builtins with zero-length
/// producer spans.
///
/// Recovery is deliberately conservative: each builtin event is anchored to the
/// nearest caller macro with token-covering `body_spans`, then the events for
/// an anchor are matched to body tokens in producer order. Tokens are marked
/// only when the monotone assignment is unique; ambiguous or unanchored cases
/// remain unmarked so the validator fails closed.
static void recoverZeroLengthNoLinesBuiltinSpans(
    const json::Object &rootJson, const PPCtx &ctx, ArrayRef<PPTok> a0Toks,
    MutableArrayRef<uint8_t> a0Sensitive, StringRef sourcePath) {
  const json::Array *items = rootJson.getArray("items");
  if (!items)
    return;

  // Reparse only the macro metadata needed by the checker. The ordinal records
  // producer item order, which is the order used for monotone recovery below.
  std::map<uint64_t, NoLinesMacroInfo> macrosById;
  size_t ordinal = 0;
  for (const json::Value &it : *items) {
    const auto *obj = it.getAsObject();
    if (!obj)
      continue;
    auto kind = obj->getString("kind");
    if (!kind || *kind != "macro")
      continue;
    auto id = getJSONUInt64(*obj, "id");
    auto name = obj->getString("name");
    if (!id || !name)
      continue;

    NoLinesMacroInfo info;
    info.id = *id;
    info.ordinal = ordinal++;
    info.name = name->str();
    if (auto invFile = obj->getString("inv_file"))
      info.invFile = invFile->str();
    info.invBegin = getJSONUInt64(*obj, "inv_b");
    info.callerId = getJSONUInt64(*obj, "caller_macro_id");

    if (const json::Value *spansVal = obj->get("spans")) {
      auto spans = parsePPSpans(*spansVal, "no-lines.recover.macro.spans");
      if (!spans) {
        consumeError(spans.takeError());
        continue;
      }
      info.spans = std::move(*spans);
    }

    if (const json::Value *bodyVal = obj->get("body_spans")) {
      auto bodySpans =
          parsePPSpans(*bodyVal, "no-lines.recover.macro.body_spans");
      if (!bodySpans) {
        consumeError(bodySpans.takeError());
        continue;
      }
      info.bodySpans = std::move(*bodySpans);
    }

    macrosById.emplace(info.id, std::move(info));
  }

  std::map<std::string, std::optional<std::string>> sourceCache;
  std::map<uint64_t, std::vector<NoLinesSensitiveEvent>> eventsByAnchor;

  // Existing non-empty spans already participate in the normal mask builder.
  // Only zero-length sensitive builtins need the caller-body recovery path.
  for (const auto &entry : macrosById) {
    const NoLinesMacroInfo &item = entry.second;
    if (!isLineDirectiveSensitiveBuiltin(item.name))
      continue;
    if (spanMarksAnyToken(item.spans, a0Toks.size()))
      continue;

    std::vector<const NoLinesMacroInfo *> chain =
        callerChainForNoLines(item, macrosById);

    // Anchor recovery to the nearest caller whose body covers concrete tokens.
    // That bounds matching to the macro surface that actually contains the
    // generated builtin token.
    const NoLinesMacroInfo *anchor = nullptr;
    for (const NoLinesMacroInfo *macro : chain) {
      if (spanMarksAnyToken(macro->bodySpans, a0Toks.size())) {
        anchor = macro;
        break;
      }
    }
    if (!anchor) {
      debug("check",
            "--no-lines: no body-span anchor for zero-length builtin {0}#{1}",
            item.name, item.id);
      continue;
    }

    NoLinesSensitiveEvent event;
    event.item = &item;
    event.anchor = anchor;
    event.ordinal = item.ordinal;
    event.allowedSpellings = buildAllowedSpellingsForNoLinesBuiltin(
        item, chain, sourcePath, ctx, sourceCache);
    eventsByAnchor[anchor->id].push_back(std::move(event));
  }

  for (auto &entry : eventsByAnchor) {
    std::vector<NoLinesSensitiveEvent> &events = entry.second;

    // Match events to tokens in metadata order. This preserves the original
    // macro-expansion order and prevents arbitrary same-spelling token choices.
    std::sort(events.begin(), events.end(),
              [](const NoLinesSensitiveEvent &lhs,
                 const NoLinesSensitiveEvent &rhs) {
      return lhs.ordinal < rhs.ordinal;
    });

    const NoLinesMacroInfo *anchor = events.front().anchor;
    std::vector<size_t> candidates =
        bodyTokenCandidatesForNoLines(anchor->bodySpans, a0Toks.size());
    const size_t eCount = events.size();
    const size_t cCount = candidates.size();
    if (eCount == 0 || cCount == 0)
      continue;

    // matches[ei][ci] records whether event ei could be represented by the
    // candidate body token ci based on token kind and exact spelling.
    std::vector<std::vector<uint8_t>> matches(
        eCount, std::vector<uint8_t>(cCount, 0));
    for (size_t ei = 0; ei < eCount; ++ei) {
      for (size_t ci = 0; ci < cCount; ++ci) {
        if (tokenMatchesNoLinesBuiltinEvent(a0Toks[candidates[ci]], events[ei]))
          matches[ei][ci] = 1;
      }
    }

    // Count monotone assignments, saturated at two. We only need to know
    // whether the assignment is absent, unique, or ambiguous.
    std::vector<std::vector<uint8_t>> ways(
        eCount + 1, std::vector<uint8_t>(cCount + 1, 0));
    for (size_t ci = 0; ci <= cCount; ++ci)
      ways[eCount][ci] = 1;

    for (size_t ei = eCount; ei-- > 0;) {
      for (size_t ci = cCount; ci-- > 0;) {
        unsigned count = ways[ei][ci + 1];
        if (matches[ei][ci])
          count += ways[ei + 1][ci + 1];
        ways[ei][ci] = static_cast<uint8_t>(std::min<unsigned>(count, 2));
      }
    }

    // Ambiguity is not repaired heuristically: if more than one token sequence
    // satisfies the metadata constraints, leave the ignore mask unchanged.
    if (ways[0][0] != 1) {
      debug("check",
            "--no-lines: refusing ambiguous zero-length builtin recovery for "
            "anchor macro #{0}: events={1} candidates={2} assignments={3}",
            anchor->id, eCount, cCount, ways[0][0]);
      continue;
    }

    // The unique assignment is now recoverable greedily because the DP table
    // proves there is exactly one valid suffix choice at every selected token.
    size_t ci = 0;
    for (size_t ei = 0; ei < eCount; ++ei) {
      for (; ci < cCount; ++ci) {
        if (!matches[ei][ci] || ways[ei + 1][ci + 1] == 0)
          continue;
        size_t tokIndex = candidates[ci++];
        if (tokIndex < a0Sensitive.size()) {
          a0Sensitive[tokIndex] = 1;
          debug("check",
                "--no-lines: recovered zero-length builtin {0}#{1} at "
                "original token index {2} under anchor macro #{3}",
                events[ei].item->name, events[ei].item->id, tokIndex,
                anchor->id);
        }
        break;
      }
    }
  }
}

static Expected<std::string> parseSourcePath(const json::Object &rootJson) {
  auto s = rootJson.getString("source");
  if (!s || s->empty())
    return createStringError(inconvertibleErrorCode(),
                             "refold map missing required 'source' path");
  return s->str();
}

// In --check + --no-lines mode, allow token mismatches for *unmodified* tokens
// that originate from line-directive-sensitive predefined macros such as
// __LINE__/__FILE__/__FILE_NAME__/__BASE_FILE__.
//
// We identify these tokens structurally:
//   1) Preprocess the original TU from refold-map 'source' under pp_ctx.
//   2) Mark A-token indices covered by macro items whose name is one of the
//      sensitive builtins above. If producer metadata recorded a zero-length
//      span for such a builtin, recover the token from its caller body spans
//      only when there is a unique structural assignment.
//   3) Diff original A tokens to edited B tokens and mark the corresponding B
//      indices only for those tokens that remain unchanged (EQUAL steps).
static Expected<std::vector<uint8_t>>
buildNoLinesIgnoreMask(const json::Object &rootJson, const PPCtx &ctx,
                       ArrayRef<PPTok> bPPToks) {
  auto sourceOrErr = parseSourcePath(rootJson);
  if (!sourceOrErr)
    return sourceOrErr.takeError();

  auto ppOrErr = preprocessToBytes(*sourceOrErr, ctx);
  if (!ppOrErr)
    return ppOrErr.takeError();
  std::string a0Bytes = std::move(*ppOrErr);

  std::vector<PPTok> a0Toks;
  std::vector<std::size_t> a0Off;
  const LangOptions lexLang =
      RefoldEngine::MakeLexLangOptions(ctx.lang);
  lexPPTokens(a0Bytes, a0Toks, a0Off, lexLang);

  std::vector<uint8_t> a0Sensitive(a0Toks.size(), 0);
  if (auto items = rootJson.getArray("items")) {
    for (const auto &it : *items) {
      const auto *obj = it.getAsObject();
      if (!obj)
        continue;
      auto kind = obj->getString("kind");
      if (!kind || *kind != "macro")
        continue;
      auto name = obj->getString("name");
      if (!name || !isLineDirectiveSensitiveBuiltin(*name))
        continue;
      auto *spansVal = obj->get("spans");
      if (!spansVal)
        continue;
      auto spansOrErr = parsePPSpans(*spansVal, "no-lines.items.macro.spans");
      if (!spansOrErr)
        return spansOrErr.takeError();
      for (const auto &sp : *spansOrErr) {
        uint64_t bb = sp.begin;
        uint64_t ee = sp.end;
        if (ee < bb)
          continue;
        if (bb > a0Sensitive.size())
          continue;
        ee = std::min<uint64_t>(ee, a0Sensitive.size());
        for (uint64_t i = bb; i < ee; ++i)
          a0Sensitive[static_cast<size_t>(i)] = 1;
      }
    }
  }

  recoverZeroLengthNoLinesBuiltinSpans(rootJson, ctx, a0Toks, a0Sensitive,
                                       *sourceOrErr);

  SmallVector<StringRef, 0> aSeq;
  SmallVector<StringRef, 0> bSeq;
  aSeq.reserve(a0Toks.size());
  bSeq.reserve(bPPToks.size());
  for (const auto &t : a0Toks)
    aSeq.push_back(StringRef(t.spelling));
  for (const auto &t : bPPToks)
    bSeq.push_back(StringRef(t.spelling));

  std::vector<uint8_t> ignore(bPPToks.size(), 0);
  auto steps = diffutils::diff(aSeq, bSeq);
  for (const auto &st : steps) {
    if (st.op != diffutils::Op::Equal)
      continue;
    const uint64_t len = st.aHi - st.aLo;
    for (uint64_t k = 0; k < len; ++k) {
      const uint64_t ai = st.aLo + k;
      const uint64_t bi = st.bLo + k;
      if (ai < a0Sensitive.size() && bi < ignore.size() && a0Sensitive[ai])
        ignore[bi] = 1;
    }
  }
  return ignore;
}

static Error compareTokens(ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks) {
  // Compare token spellings up to the min length first.
  const size_t n = std::min(aToks.size(), bToks.size());
  for (size_t i = 0; i < n; ++i) {
    const std::string aDbg = stringutils::showWS(
        stringutils::clip(StringRef(aToks[i].spelling), 100));
    const std::string bDbg = stringutils::showWS(
        stringutils::clip(StringRef(bToks[i].spelling), 180));
    if (aToks[i].spelling != bToks[i].spelling) {
      return createStringError(
          inconvertibleErrorCode(),
          formatv("token mismatch at index {0}: A='{1}' B='{2}'", i, aDbg, bDbg)
              .str());
    }
    debug("compare", "token match at index {0}: A='{1}' B='{2}'", i, aDbg,
          bDbg);
  }

  if (aToks.size() != bToks.size()) {
    return createStringError(
        inconvertibleErrorCode(),
        formatv("token count mismatch: A={0} B={1}", aToks.size(), bToks.size())
            .str());
  }
  return Error::success();
}

static Error compareTokensNoLinesAware(ArrayRef<PPTok> aToks,
                                       ArrayRef<PPTok> bToks,
                                       ArrayRef<uint8_t> ignoreMask) {
  // Compare token spellings up to the min length first.
  const size_t n = std::min(aToks.size(), bToks.size());
  for (size_t i = 0; i < n; ++i) {
    const std::string aDbg = stringutils::showWS(
        stringutils::clip(StringRef(aToks[i].spelling), 100));
    const std::string bDbg = stringutils::showWS(
        stringutils::clip(StringRef(bToks[i].spelling), 180));

    if (aToks[i].spelling != bToks[i].spelling) {
      const bool ign = (i < ignoreMask.size()) && ignoreMask[i] &&
                       canIgnoreNoLinesMismatch(aToks[i], bToks[i]);
      if (ign) {
        debug("compare",
              "--no-lines: ignoring builtin loc macro mismatch at index {0}: "
              "A='{1}' B='{2}'",
              i, aDbg, bDbg);
        continue;
      }

      return createStringError(
          inconvertibleErrorCode(),
          formatv("token mismatch at index {0}: A='{1}' B='{2}'", i, aDbg, bDbg)
              .str());
    }

    debug("compare", "token match at index {0}: A='{1}' B='{2}'", i, aDbg,
          bDbg);
  }

  if (aToks.size() != bToks.size()) {
    return createStringError(
        inconvertibleErrorCode(),
        formatv("token count mismatch: A={0} B={1}", aToks.size(), bToks.size())
            .str());
  }
  return Error::success();
}

} // end anonymous namespace

// ------------------------- Command-Line Options ------------------------------

static cl::OptionCategory RefoldCategory("clang-refold options");

cl::opt<LogLevel> LogLevelOpt(
    "log-level", cl::desc("Set log level"),
    cl::values(clEnumValN(LogLevel::trace, "trace", "Trace"),
               clEnumValN(LogLevel::debug, "debug", "Debug"),
               clEnumValN(LogLevel::info, "info", "Info  (default)"),
               clEnumValN(LogLevel::warn, "warn", "Warn"),
               clEnumValN(LogLevel::error, "error", "Error"),
               clEnumValN(LogLevel::fatal, "fatal", "Fatal")),
    cl::init(LogLevel::info), cl::cat(RefoldCategory));

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

static cl::opt<bool> StrictMode(
    "strict",
    cl::desc("Treat stringified arguments to be significant (off by default)"),
    cl::init(false), cl::cat(RefoldCategory));

// Short alias: -s (points to --strict)
static cl::alias StrictModeShort("s", cl::desc("Alias for --strict"),
                                 cl::aliasopt(StrictMode),
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

  info("log", "log level set to {0}", LogLevelOpt);

  // We output a custom error message if the following flags appear more than
  // once and remove cl::Required from the relevant cl::opt's. This is because
  // the error message would otherwise be misleading, stating that the option
  // must be specified at least once, which is not the case here.
  const bool onlyCheck = (CheckSrcPath.getNumOccurrences() != 0);

  // Enforce exactly one supported invocation mode:
  //   (1) Refold:
  //       clang-refold --pp A.i --pp-mod B.i.mod --refold-map map.json --out \
  //         out.c.mod
  //   (2) Verify:
  //       clang-refold --check out.c.mod --refold-map map.json --pp-mod B.i.mod
  requireExactlyOnce("--refold-map", RefoldJSONPath);
  requireExactlyOnce("--pp-mod", PPModPath);

  if (onlyCheck) {
    requireExactlyOnce("--check", CheckSrcPath);
    // Verify mode.
    if (PPPath.getNumOccurrences() != 0 ||
        ModifiedSrcPath.getNumOccurrences() != 0) {
      fatal("cli", "invalid option combination: --check cannot be used with "
                   "--pp or --out");
    }
  } else {
    // Refold mode.
    requireExactlyOnce("--pp", PPPath);
    requireExactlyOnce("--out", ModifiedSrcPath);
  }

  // Parse and validate the refold map JSON file.
  auto parsedObjOrErr = parseAndValidateJSON(RefoldJSONPath, RefoldSchema);
  if (!parsedObjOrErr) {
    handleAllErrors(parsedObjOrErr.takeError(), [&](const ErrorInfoBase &e) {
      fatal("json", "failed to parse refold '{0}' JSON file: {1}",
            RefoldJSONPath, e.message());
    });
  } else {
    info("json", "refold JSON file '{0}' validated", RefoldJSONPath);
  }
  const json::Object &rootJson = *parsedObjOrErr;
  assert(!rootJson.empty() && "parsed refold map JSON object is empty");

  // Read files and tokenize.
  std::string aBytes, bBytes;
  auto ctxOrErr = parsePPCtx(rootJson);
  if (!ctxOrErr) {
    handleAllErrors(ctxOrErr.takeError(), [&](const ErrorInfoBase &e) {
      fatal("model", "failed to parse pp_ctx from refold map: {0}",
            e.message());
    });
  }
  const PPCtx ctx = *ctxOrErr;
  const LangOptions lexLang =
      RefoldEngine::MakeLexLangOptions(ctx.lang);

  std::optional<PPCtx> checkCtx;
  if (onlyCheck) {
    checkCtx = ctx;

    // Preprocess the refolded C source:
    {
      auto ppOrErr = preprocessToBytes(CheckSrcPath, ctx);
      if (!ppOrErr) {
        handleAllErrors(ppOrErr.takeError(), [&](const ErrorInfoBase &e) {
          fatal("pp", "failed to preprocess --check input: {0}", e.message());
        });
      }
      aBytes = std::move(*ppOrErr);
    }

    // Preprocess the edited prerpocessed output file:
    {
      auto ppOrErr = preprocessToBytes(PPModPath, ctx);
      if (!ppOrErr) {
        handleAllErrors(ppOrErr.takeError(), [&](const ErrorInfoBase &e) {
          fatal("pp", "failed to preprocess --pp-mod input: {0}", e.message());
        });
      }
      bBytes = std::move(*ppOrErr);
    }
  } else {
    readFile(PPPath, aBytes);
    readFile(PPModPath, bBytes);
  }

  std::vector<PPTok> aToks, bToks;
  std::vector<std::size_t> aTokByteOff, bTokByteOff;
  lexPPTokens(aBytes, aToks, aTokByteOff, lexLang);
  lexPPTokens(bBytes, bToks, bTokByteOff, lexLang);
  debug("lex", "{0} tokens={1} {2} tokens={3}", PPPath, aToks.size(), PPModPath,
        bToks.size());

  // Append the sentinel to both source offsets.
  if (bTokByteOff.empty() || bTokByteOff.back() != bBytes.size()) {
    if (bTokByteOff.empty() || bTokByteOff.back() < bBytes.size())
      bTokByteOff.push_back(bBytes.size());
  }
  if (aTokByteOff.empty() || aTokByteOff.back() != aBytes.size()) {
    if (aTokByteOff.empty() || aTokByteOff.back() < aBytes.size())
      aTokByteOff.push_back(aBytes.size());
  }

  if (onlyCheck && NoLines) {
    // Relax token comparison for location-sensitive predefined macros when
    // verifying a refolding produced with --no-lines.
    if (!checkCtx)
      fatal("cli", "internal error: missing pp_ctx in --check mode");
    auto maskOrErr = buildNoLinesIgnoreMask(rootJson, *checkCtx, bToks);
    if (!maskOrErr) {
      handleAllErrors(maskOrErr.takeError(), [&](const ErrorInfoBase &e) {
        fatal("check", "failed to build --no-lines ignore mask: {0}",
              e.message());
      });
    }
    if (Error err = compareTokensNoLinesAware(aToks, bToks, *maskOrErr)) {
      outs() << toString(std::move(err)) << "\n";
      outs() << "FAILURE!\n";
      return 1;
    }
    outs() << "SUCCESS!\n";
    return 0;
  }

  if (onlyCheck) {
    // Verification mode (normal comparison): compare preprocessed token
    // streams directly. The --no-lines special-case is handled above.
    if (Error err = compareTokens(aToks, bToks)) {
      outs() << toString(std::move(err)) << "\n";
      outs() << "FAILURE!\n";
      return 1;
    }
    outs() << "SUCCESS!\n";
    return 0;
  }

  // Default behavior: single refold.
  auto refoldedOrErr = RefoldEngine::Refold(
      rootJson, aBytes, aToks, aTokByteOff, bBytes, bToks, bTokByteOff,
      NoLines, StrictMode);
  if (!refoldedOrErr) {
    handleAllErrors(refoldedOrErr.takeError(), [&](const ErrorInfoBase &e) {
      fatal("model", "failed to parse refold model: {0}", e.message());
    });
  }

  // Finally, serialize the refolded C source to the output file.
  std::error_code ec;
  raw_fd_ostream os(ModifiedSrcPath, ec, sys::fs::OF_Text);
  if (ec) {
    fatal("src/write", "cannot write {0}: {1}", ModifiedSrcPath, ec.message());
  }
  os << *refoldedOrErr;
  os.close();
  info("finished", "wrote refolded C source: {0}", ModifiedSrcPath);
  return 0;
}
