//===--- RefoldArgTextRecovery.h -------------------------------*- C++ -*-===//
//
// Macro-argument text recovery helpers for clang-refold.
//
// This module owns the small raw-lexing operations used when a macro
// invocation patch has to reason about source spelling rather than producer
// token ranges.  It exposes:
//   - the free predicate `refoldMacroActualHasTopLevelComma`, which decides
//     whether a candidate replacement would split a function-like macro
//     actual;
//   - the free predicate `refoldMacroActualSubrangeIsFreeStanding`, which
//     decides whether a run of an actual's spelling lies outside every
//     parenthesis of that actual;
//   - the free predicate `refoldMacroActualRunReplacementPreservesInvocations`,
//     which decides whether an in-place edit of an actual's spelling leaves
//     the invocations around it intact; and
//   - the `RefoldArgTextRecovery` class, which inverts stringified literal
//     tokens and recovers per-actual byte ranges from a raw invocation
//     spelling.
//
// Everything here deliberately has no access to RefoldEngine state: callers
// provide the captured language options and receive either a fully proved text
// range/result or std::nullopt.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDARGTEXTRECOVERY_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDARGTEXTRECOVERY_H

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

/// Return true when \p text contains a comma that would split a single
/// function-like macro actual.
///
/// This deliberately models macro-argument collection rather than general C
/// expression tuple parsing: nested parentheses protect commas, while brackets
/// and braces do not.  Comments and literals are tokenized as opaque raw-lexer
/// tokens so commas inside them cannot produce a false positive.
bool refoldMacroActualHasTopLevelComma(llvm::StringRef text,
                                       const LangOptions &lang);

/// Return true when the half-open byte range [\p begin, \p end) of the macro
/// actual spelling \p actual is a run of whole raw tokens that no parenthesis
/// of \p actual encloses.
///
/// Both ends must fall on token boundaries (a boundary inside a token, comment
/// or literal fails), and every parenthesis the run closes must also open in
/// it.  This is the lexical half of proving that the rest of \p actual can be
/// kept verbatim around a replacement of the run: in a macro actual, a
/// parenthesis enclosing the run that does not survive into the expansion was
/// consumed by a function-like invocation, which would collect the replacement
/// as its own actuals.
bool refoldMacroActualSubrangeIsFreeStanding(llvm::StringRef actual,
                                             size_t begin, size_t end,
                                             const LangOptions &lang);

/// Return true when replacing the bytes [\p begin, \p end) of the macro actual
/// spelling \p actual with \p replacement leaves every macro invocation around
/// the edit as it was.
///
/// The edit is first widened to whole raw tokens, carrying the bytes it
/// crosses into the replacement, so the edited text is unchanged; an edge
/// inside a comment fails, because that edit could not reach the preprocessed
/// stream.  The edited text is then accepted only when:
///   * no token on either side of the edit merges with the replacement;
///   * no identifier on one side of an edit edge meets a `(` on the other,
///     before or after the edit, so no invocation outside the edit gains or
///     loses its argument list; and
///   * the replaced and replacing runs are each parenthesis-balanced and,
///     when a parenthesis of \p actual encloses the edit, free of commas at
///     their own top level, so an enclosing invocation keeps the number and
///     boundaries of its actuals.
/// A comma at the top level of \p actual itself is left to the caller, which
/// knows whether the formal it fills is variadic.
bool refoldMacroActualRunReplacementPreservesInvocations(
    llvm::StringRef actual, size_t begin, size_t end,
    llvm::StringRef replacement, const LangOptions &lang);

/// Recovers macro actual-argument spellings from textual surfaces that are not
/// already represented by producer-owned model ranges.
///
/// The class is intentionally small and read-only.  It uses Clang's raw lexer
/// with the producer-captured language options so comma splitting and token
/// offsets follow preprocessing-token rules instead of ad-hoc byte scanning.
class RefoldArgTextRecovery {
public:
  explicit RefoldArgTextRecovery(const LangOptions &lexLang);

  /// Attempt to invert a string literal token produced by macro
  /// stringification (`#param`) back to a single invocation-site argument
  /// spelling.
  ///
  /// Only ordinary/prefixed string literal spellings that can be reduced
  /// without changing preprocessing semantics are accepted.  Backslash and
  /// quote escapes introduced by stringification are undone; other escape
  /// sequences are preserved verbatim.  Unless \p allowTopLevelComma is true,
  /// a top-level comma in the recovered text rejects the candidate because it
  /// would split a synthesized function-like macro invocation.
  std::optional<std::string>
  UnstringifyLiteralToArgText(llvm::StringRef literalTok,
                              bool allowTopLevelComma = false) const;

  /// Lex a function-like macro invocation spelling and return half-open byte
  /// ranges for the contents of each actual argument.
  ///
  /// This is for candidate invocation text that is not backed by the producer's
  /// `inv_arg_ranges`.  Argument collection follows preprocessor rules:
  /// nested parentheses protect commas, while brackets and braces do not.
  ///
  /// Collection stops at the parenthesis that closes the actual list, as
  /// Clang's does.  When \p trailingTokens is non-null it is set to whether any
  /// token follows that parenthesis; the returned ranges are the same either
  /// way.
  static std::optional<std::vector<std::pair<size_t, size_t>>>
  LexMacroInvocationActualContentRanges(llvm::StringRef invText,
                                        const LangOptions &lang,
                                        bool *trailingTokens = nullptr);

private:
  const LangOptions &lexLang_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDARGTEXTRECOVERY_H
