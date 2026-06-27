//===--- RefoldTokenTextAnalysis.h ----------------------------*- C++ -*-===//
//
// Token/text observation service for clang-refold.
//
// This service owns raw-lexer based textual observation predicates that are
// shared by proof modules.  It intentionally uses Clang tokenization instead of
// substring search so comments, string literals, character literals, and
// identifier prefixes/suffixes do not become false macro or builtin observer
// evidence.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENTEXTANALYSIS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENTEXTANALYSIS_H

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <optional>

namespace clang {
namespace refold {

/// Read-only lexical text-analysis oracle for one refold run.
///
/// The object stores only the producer language mode.  Callers provide the
/// concrete text slices to analyze, which keeps token/text proofs independent
/// from RefoldEngine source buffers and avoids turning the engine into a
/// service locator for raw lexical predicates.
class RefoldTokenTextAnalysis {
public:
  explicit RefoldTokenTextAnalysis(const clang::LangOptions &lexLang);

  /// Return the byte offset of the first identifier token in \p text whose
  /// spelling is exactly \p name, if any.
  ///
  /// This is the object-like macro-state observation primitive.  It lexes the
  /// supplied text as a raw scratch buffer and ignores comments, literals, and
  /// partial identifier matches.
  std::optional<size_t>
  FirstRawIdentifierObservationOffsetInText(llvm::StringRef name,
                                            llvm::StringRef text) const;

  /// True iff \p text contains \p name as a preprocessing identifier token.
  bool RawIdentifierAppearsInText(llvm::StringRef name,
                                  llvm::StringRef text) const;

  /// Return the byte offset of the NAME token that starts the first
  /// function-like macro invocation observation in \p text, if any.
  ///
  /// The following `(` may be in \p text or in \p suffix.  Observations are
  /// still reported only when the NAME token itself starts in \p text.
  std::optional<size_t>
  FirstFunctionLikeInvocationOffsetInText(llvm::StringRef name,
                                          llvm::StringRef text,
                                          llvm::StringRef suffix =
                                              llvm::StringRef()) const;

  /// True iff \p text plus the optional preserved \p suffix contains a real
  /// function-like invocation of \p name.
  bool FunctionLikeInvocationAppearsInText(llvm::StringRef name,
                                           llvm::StringRef text,
                                           llvm::StringRef suffix =
                                               llvm::StringRef()) const;

  /// True iff \p text contains a line-state builtin observer token.
  bool TextMentionsLineObserver(llvm::StringRef text) const;

  /// True iff \p text contains a file-spelling builtin observer token.
  bool TextMentionsFileObserver(llvm::StringRef text) const;

  /// True iff \p text contains a `__COUNTER__` observer/consumer token.
  bool TextMentionsCounterObserver(llvm::StringRef text) const;

  /// True iff \p text contains a physical preprocessor directive line.
  ///
  /// This remains a textual classification, not a macro-state policy decision:
  /// callers decide whether a directive line is a barrier for the proof they
  /// are constructing.
  bool TextContainsDirectiveLine(llvm::StringRef text) const;

private:
  clang::LangOptions lexLang_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENTEXTANALYSIS_H
