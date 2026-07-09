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

LangOptions makeRefoldLexLangOptions(StringRef langName) {
  DiagnosticOptions diagOpts;
  IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
  auto *client = new IgnoringDiagConsumer();
  DiagnosticsEngine diags(diagIDs, diagOpts, client, /*ShouldOwnClient=*/true);

  auto invocation = std::make_shared<CompilerInvocation>();
  std::string lang = langName.empty() ? "c" : langName.str();
  std::vector<const char *> args = {"-x", lang.c_str()};
  CompilerInvocation::CreateFromArgs(*invocation, ArrayRef<const char *>(args),
                                     diags);
  return invocation->getLangOpts();
}

} // namespace refold
} // namespace clang
