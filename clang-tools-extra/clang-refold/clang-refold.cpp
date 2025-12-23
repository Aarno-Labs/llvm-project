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

#include "RefoldEngine.h"
#include "RefoldLog.h"
#include "RefoldSchema.h"
#include "StringUtils.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/JSONSchemaValidator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <system_error>
#include <vector>

using namespace llvm;
using namespace clang;
using namespace clang::refold;

namespace {

// --------------------------- Tokenization ---------------------------------

#if 0
inline void writeTokensCSV(const std::vector<PPTok> &ppToks,
                           const std::vector<std::size_t> &startOffs,
                           raw_ostream &os) {
  if (ppToks.size() + 1 != startOffs.size()) {
    fatal("csv/write",
          "PP token count plus sentinel ({0}) does not equal the number of "
          "start offsets ({1})",
          ppToks.size() + 1, startOffs.size());
  }
  std::size_t i = 0;
  for (; i < ppToks.size(); ++i) {
    os << ppToks[i].spelling << ',' << startOffs[i] << '\n';
  }
  os << startOffs[i] << "\n";
  os.flush();
}

// Writes to ~/tokens.txt by default.
inline void writeTokensCSVToFile(const std::vector<PPTok> &ppToks,
                                 const std::vector<std::size_t> &startOffs,
                                 const std::string &path) {
  std::error_code ec;
  raw_fd_ostream os(path, ec, sys::fs::OF_Text);
  if (ec) {
    fatal("csv/open", "failed to open file '{0}': {1}", path, ec.message());
  }
  writeTokensCSV(ppToks, startOffs, os);
}
#endif

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
                 std::vector<std::size_t> &startOffs) {
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
  IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
  IntrusiveRefCntPtr<DiagnosticOptions> diagOpts(new DiagnosticOptions());
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

  LangOptions lang; // raw lexing
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
    PresumedLoc pl = sm.getPresumedLoc(tkn.getLocation());
    int line = pl.isValid() ? static_cast<int>(pl.getLine()) : -1;
    int col = pl.isValid() ? static_cast<int>(pl.getColumn()) : -1;
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
  if (opt.getNumOccurrences() == 0) {
    fatal("cli", "option '{0}' must be specified only once", flag);
  }
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
           cl::desc("Path to preprocessed file (.c.i) [required]"),
           cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -p (points to --pp)
static cl::alias PPPathShort("p", cl::desc("Alias for --pp"),
                             cl::aliasopt(PPPath), cl::cat(RefoldCategory));

static cl::opt<std::string> PPModPath(
    "pp-mod", // long name: --pp-mod
    cl::desc("Path to modified preprocessed file (.c.i.mod) [required]"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -P (points to --pp-mod)
static cl::alias PPModPathShort("P", cl::desc("Alias for --pp-mod"),
                                cl::aliasopt(PPModPath),
                                cl::cat(RefoldCategory));

static cl::opt<std::string> RefoldJSONPath(
    "refold-map", // long name: --refold-map
    cl::desc("Path to refold JSON file (.c.refold.json) [required]"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -r (points to --refold-map)
static cl::alias RefoldJSONPathShort("r", cl::desc("Alias for --refold-map"),
                                     cl::aliasopt(RefoldJSONPath),
                                     cl::cat(RefoldCategory));

static cl::opt<std::string> ModifiedSrcPath(
    "out", // long name: --out
    cl::desc("Path to refolded C source file (.c.mod) [required]"),
    cl::value_desc("file"), cl::cat(RefoldCategory));

// Short alias: -o (points to --out)
static cl::alias ModifiedSrcPathShort("o", cl::desc("Alias for --out"),
                                      cl::aliasopt(ModifiedSrcPath),
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

  Typical usage:
    clang -E -P --refold-map=foo.c.refold.json foo.c -o foo.c.i
    clang-refold -p foo.c.i -P foo.c.i.mod -r foo.c.refold.json -o foo.c.mod
  )";

// ----------------------------- Main Program ----------------------------------

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  cl::HideUnrelatedOptions(RefoldCategory);
  cl::ParseCommandLineOptions(argc, argv, Overview);

  info("log", "log level set to {0}", logLevelToString(LogLevelOpt));

  // We output a custom error message if the following flags appear more than
  // once and remove the cl::Required from the relevant cl::opt's. This is
  // because the error message would otherwise be misleading, stating that the
  // option must be specified at least once, which is not the case here.
  requireExactlyOnce("--out", ModifiedSrcPath);
  requireExactlyOnce("--refold-map", RefoldJSONPath);
  requireExactlyOnce("--pp", PPPath);
  requireExactlyOnce("--pp-mod", PPModPath);

  // Let A refer to the unmodified pp code, and B refer to the modified pp code.
  StringRef aPath = PPPath, bPath = PPModPath;

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
  readFile(aPath, aBytes);
  readFile(bPath, bBytes);

  std::vector<PPTok> aToks, bToks;
  std::vector<std::size_t> aTokByteOff, bTokByteOff;
  lexPPTokens(aBytes, aToks, aTokByteOff);
  lexPPTokens(bBytes, bToks, bTokByteOff);
  debug("lex", "{0} tokens={1} {2} tokens={3}", aPath, aToks.size(), bPath,
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

#if 0
  // XXX: Write out debug files so that I can compare the preprocessed lexical
  // tokens with the clang/llvm implementation of `clang-refold`. This can go
  // away once we have a stable working version of clang.
  SmallString<256> tokensAFile;
  sys::fs::expand_tilde("~/tokens.a-cpp.txt", tokensAFile);
  writeTokensCSVToFile(aToks, aTokByteOff, tokensAFile.str().str());
  SmallString<256> tokensBFile;
  sys::fs::expand_tilde("~/tokens.b-cpp.txt", tokensBFile);
  writeTokensCSVToFile(bToks, bTokByteOff, tokensBFile.str().str());
#endif

  // Generate the refolded C source as a string.
  Expected<std::string> refoldedOrErr = RefoldEngine::Refold(
      rootJson, aBytes, aToks, aTokByteOff, bBytes, bToks, bTokByteOff);
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
