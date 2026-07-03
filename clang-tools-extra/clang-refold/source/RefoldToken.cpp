//===--- RefoldToken.cpp ----------------------------------------*- C++ -*-===//
//
// Lexer for producing `PPTok` streams from preprocessed byte buffers.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldToken.h"

#include "core/RefoldLog.h"
#include "util/StringUtils.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

void lexPPTokens(const std::string &bytes, std::vector<PPTok> &out,
                 std::vector<std::size_t> &startOffs, const LangOptions &lang) {
  out.clear();
  startOffs.clear();

  // Ensure a trailing newline.
  std::string buf = bytes;
  bool addedNL = false;
  if (buf.empty() || buf.back() != '\n') {
    buf.push_back('\n');
    addedNL = true;
  }

  REFOLD_LOG_DEBUG("lexer", "entered: bytes={0} (addedNL={1})", buf.size(),
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
    REFOLD_LOG_FATAL("lexer", "getBufferData returned Invalid");
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

    const char *kindName = tok::getTokenName(tkn.getKind());
    ppt.kind = kindName;

    if (inTraceMode()) {
      // Presumed location recovery and visible-whitespace spelling are only
      // used by the per-token lexer trace. Keep them out of the hot lexing path
      // when trace logging is disabled.
      PresumedLoc pl = sm.getPresumedLoc(tkn.getLocation());
      std::optional<unsigned> line;
      std::optional<unsigned> col;
      if (pl.isValid()) {
        line = pl.getLine();
        col = pl.getColumn();
      }

      trace("lexer/parsed",
            "kind={0} spelled={1} off={2} len={3} li={4} co={5}", kindName,
            stringutils::showWs(stringutils::clip(StringRef(ppt.spelling), 80)),
            off, len, line, col);
    }

    out.push_back(std::move(ppt));
    startOffs.push_back(off);
  }

  // Sentinel: one-past-end
  startOffs.push_back(buf.size());
  // Sanity: monotone offsets and size relationship.
  assert(startOffs.size() == out.size() + 1 && "need sentinel in startOffs");
  assert(std::is_sorted(startOffs.begin(), startOffs.end()));

  REFOLD_LOG_DEBUG("lexer", "done: tokens={0}", out.size());
}

} // namespace refold
} // namespace clang
