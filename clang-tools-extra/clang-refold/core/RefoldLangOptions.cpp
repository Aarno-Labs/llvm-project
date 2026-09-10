//===--- RefoldLangOptions.cpp ----------------------------------*- C++ -*-===//
//
// Shared raw-lexer language option construction for clang-refold.
//
// See RefoldLangOptions.h for the public contract.  This file deliberately
// contains only the CompilerInvocation setup needed to mirror the producer's
// `-x <language>` preprocessing mode, so callers outside RefoldEngine can share
// the exact same tokenization policy without gaining an engine dependency.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldLangOptions.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/CompilerInvocation.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

LangOptions makeRefoldLexLangOptions(StringRef langName,
                                     ArrayRef<std::string> producerArgv) {
  DiagnosticOptions diagOpts;
  IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
  auto *client = new IgnoringDiagConsumer();
  DiagnosticsEngine diags(diagIDs, diagOpts, client, /*ShouldOwnClient=*/true);

  // Replay the producer's own cc1 command line, then re-assert the recorded
  // language spelling.  The argv is what the producer preprocessed with, so
  // letting Clang interpret it is the only way to get every option that moves a
  // lexical boundary -- `-std=`, `-ftrigraphs`, `-fms-extensions`, and the rest
  // -- without this file keeping a second, weaker copy of Clang's rules.  A
  // hand-picked subset is exactly the bug this replaced: built from `-x <lang>`
  // alone, `LangOptions::Trigraphs` was false for every translation unit, so
  // the trigraph arm of every splice and directive-boundary predicate in the
  // scanner was unreachable and a `??/`-spliced logical line was read as two.
  //
  // `-x` is appended last so it wins over any `-x` inside the argv: the model's
  // language token is the one every other consumer in this tool already lexes
  // by, and the two must not diverge.  An empty argv (a map that recorded none)
  // falls back to the language token alone, which is the previous behaviour.
  std::vector<std::string> storage;
  storage.reserve(producerArgv.size() + 2);
  storage.assign(producerArgv.begin(), producerArgv.end());
  storage.emplace_back("-x");
  storage.emplace_back(langName.empty() ? "c" : langName.str());

  std::vector<const char *> args;
  args.reserve(storage.size());
  for (const std::string &arg : storage)
    args.push_back(arg.c_str());

  auto invocation = std::make_shared<CompilerInvocation>();
  CompilerInvocation::CreateFromArgs(*invocation, ArrayRef<const char *>(args),
                                     diags);
  return invocation->getLangOpts();
}

} // namespace refold
} // namespace clang
