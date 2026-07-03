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

  /// Return the first raw identifier observation of `name` in `text`.
  ///
  /// This is the object-like macro-state observation primitive.  It uses
  /// Clang's raw lexer instead of substring search so comments, string
  /// literals, character literals, and identifier prefixes/suffixes do not
  /// become false observations.  The returned byte offset names the beginning
  /// of the NAME token in the supplied scratch text.
  std::optional<size_t>
  FirstRawIdentifierObservationOffsetInText(llvm::StringRef name,
                                            llvm::StringRef text) const;

  /// Return true when `text` contains `name` as a real preprocessing identifier
  /// token.
  bool RawIdentifierAppearsInText(llvm::StringRef name,
                                  llvm::StringRef text) const;

  /// Return the byte offset of the first function-like invocation of `name`.
  ///
  /// A function-like macro is observed only by a macro-name preprocessing token
  /// followed by `(` after whitespace/comments are skipped.  `suffix` is
  /// included so an edit whose replacement ends at `name` can still detect an
  /// invocation whose opening parenthesis remains in the preserved source
  /// suffix.  The NAME token itself must start in `text`; only the following
  /// `(` may come from `suffix`.
  std::optional<size_t> FirstFunctionLikeInvocationOffsetInText(
      llvm::StringRef name, llvm::StringRef text,
      llvm::StringRef suffix = llvm::StringRef()) const;

  /// Return true when `text` plus the optional preserved `suffix` contains a
  /// real function-like invocation of `name`.
  bool FunctionLikeInvocationAppearsInText(
      llvm::StringRef name, llvm::StringRef text,
      llvm::StringRef suffix = llvm::StringRef()) const;

  /// True iff \p text contains a line-state builtin observer token.
  bool TextMentionsLineObserver(llvm::StringRef text) const;

  /// True iff \p text contains a file-spelling builtin observer token.
  bool TextMentionsFileObserver(llvm::StringRef text) const;

  /// True iff \p text contains a `__COUNTER__` observer/consumer token.
  bool TextMentionsCounterObserver(llvm::StringRef text) const;

  /// Return true when `text` contains a physical preprocessor directive line.
  ///
  /// Macro-state transitions may cross ordinary source bytes only when those
  /// bytes cannot observe the moved transition.  Crossing a directive line is
  /// unsafe by default because conditionals, includes, and macro transitions
  /// have structural effects beyond token-level identifier observation.  This
  /// remains a textual classification; callers choose the proof policy that
  /// consumes the result.
  bool TextContainsDirectiveLine(llvm::StringRef text) const;

private:
  clang::LangOptions lexLang_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKENTEXTANALYSIS_H
