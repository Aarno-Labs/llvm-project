//===--- RefoldOwnerStateProof.h -------------------------------*- C++ -*-===//
//
// Part of the clang-refold proof system.
//
// This header marks the owner-state proof extraction boundary.  The owner-state
// vocabulary still lives on RefoldEngine for this transitional phase because it
// is referenced by existing edit, lattice, and materialization carriers, but the
// implementation of the owner-state transition graph and gateway is compiled
// from RefoldOwnerStateProof.cpp.  Keeping the split at the definition boundary
// avoids changing proof policy while making the next dependency-narrowing patch
// easier to audit.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATEPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATEPROOF_H

#include "RefoldLog.h"
#include "RefoldEngine.h"

namespace clang {
namespace refold {

/// Diagnostic pragma state action admitted by the owner-state proof.
///
/// The parser intentionally recognizes only the deterministic diagnostic
/// push/pop/setting sublanguage whose stack effect can be modeled exactly.
enum class DiagnosticPragmaStateAction { Push, Pop, Setting };

/// Parsed spelling for a diagnostic pragma-state directive.
///
/// StringRef fields point into the caller-provided source slice.  Callers must
/// not retain this object beyond the lifetime of that slice.
struct ParsedDiagnosticPragmaStateDirective {
  StringRef namespaceName;
  StringRef actionName;
  StringRef optionSpelling;
  DiagnosticPragmaStateAction action = DiagnosticPragmaStateAction::Setting;
};

/// Return true iff a retained raw-lexer comment token has complete spelling.
///
/// This is shared with the expansion fallback helpers because they are textually
/// included into RefoldEngine.cpp and still need the same fail-closed raw-comment
/// completeness predicate after owner-state proof extraction.
bool rawLexerCommentTokenIsComplete(StringRef spelling);

/// Return true iff `text` is whitespace plus complete C/C++ comments.
///
/// This is the shared raw-lexer trivia theorem used by both owner-state proof
/// extraction and the remaining engine-side mixed-owner tiling code.
bool sourceTextIsOnlyWhitespaceAndCompleteComments(StringRef text,
                                                   const LangOptions &lang);

/// Parse the strict diagnostic pragma-state sublanguage modeled by refold.
///
/// Unknown pragmas, malformed diagnostic directives, and directives with
/// non-trivia suffix bytes are rejected fail-closed.
std::optional<ParsedDiagnosticPragmaStateDirective>
parseDiagnosticPragmaStateDirective(StringRef text, const LangOptions &lang);

/// Return true iff two non-empty half-open intervals overlap.
bool intervalsOverlap(uint64_t beginA, uint64_t endA, uint64_t beginB,
                      uint64_t endB);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERSTATEPROOF_H
