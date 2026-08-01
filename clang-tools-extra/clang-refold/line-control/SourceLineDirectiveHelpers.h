//===--- SourceLineDirectiveHelpers.h --------------*- C++ -*-===//
//
// Source-line directive and trivia helpers.
//
// Include materialization and expansion fallback use these routines to parse,
// preserve, and rewrite source-authored line-control directive text with one
// deterministic implementation shared across both callers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_SOURCELINEDIRECTIVEHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_SOURCELINEDIRECTIVEHELPERS_H

#include "core/RefoldModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlFilename.h"
#include "source/TokenTextHelpers.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace clang {
namespace refold {

using llvm::ArrayRef;
using llvm::DenseSet;
using llvm::SmallVector;
using llvm::SmallVectorImpl;
using llvm::StringRef;
namespace sys = llvm::sys;

/// Return true iff `text` is only whitespace and complete C/C++ comments.
///
/// This is intentionally a lexical-trivia predicate, not a preprocessing
/// predicate. Include-closure may carry these bytes through a replacement
/// because comments and whitespace do not contribute preprocessing tokens.
/// Anything that would lex as a real token, including malformed or
/// unterminated comments, remains outside this proof class.
bool isWsOrCompleteCommentTrivia(StringRef text);

/// Parse one admitted conditional-control directive line.
///
/// This recognizes only the small directive grammar that include-closure is
/// allowed to preserve as an inert gap:
///
///   #if 0
///   #if 1
///   #elif 0
///   #elif 1
///   #else
///   #endif
///
/// The `depth` argument tracks balance across the preserved gap. The function
/// rejects escaped physical lines, macro-dependent conditions, trailing tokens,
/// and every directive with side effects. In particular, this is not a general
/// preprocessor directive parser.
bool parseLiteralEmptyConditionalDirectiveLine(StringRef line, unsigned &depth);

/// Return true iff `text` may be carried through a TU include-closure edit.
///
/// This predicate defines the source-preservation proof for bytes between
/// touched top-level include directives. The gap is accepted only when every
/// byte is either:
///
///   * whitespace/comment trivia, or
///   * part of a complete empty literal conditional-control island.
///
/// Accepted bytes are preserved verbatim in the emitted replacement. They are
/// never silently deleted. Anything that can affect macro state, include state,
/// diagnostics, line state, or later preprocessing remains outside this proof
/// class and forces the caller to fail closed.
bool isPreservableIncludeClosureGapTrivia(StringRef text);

/// Return true when preserved trivia begins with a preprocessing directive
/// after optional horizontal whitespace. Such trivia must be placed at the
/// start of a physical line when appended after materialized B tokens.
bool startsWithPreprocessorDirectiveTrivia(StringRef text);

/// Logical file/line state that must be re-established after consuming a
/// source-only line-control gap.
struct SourceLineDirectiveGapResume {
  size_t lineAtResume = 0;
  std::string fileSpelling;

  /// Optional numeric line-marker flags to replay with the resume directive.
  /// Empty means the canonical `#line` spelling is sufficient.
  std::string lineMarkerFlags;
};

/// Parsed result for one admitted source-spelled line-control logical line.
struct ParsedSourceLineDirectiveLogicalLine {
  size_t lineAfterDirective = 0;
  std::string fileSpelling;

  /// True iff this directive resets the active numeric line-marker flags.
  /// Canonical `#line` directives and filename-less numeric markers preserve
  /// the prior flag state; numeric markers with an explicit filename establish
  /// a fresh flag state, even when that state is empty.
  bool updatesLineMarkerFlags = false;

  /// Canonicalized replayable flags from a numeric line marker.  This accepts
  /// only flags that can be locally re-emitted without needing to prove a
  /// preprocessor include-stack pop.
  std::string lineMarkerFlags;
};

/// Rewritten logical line plus the macro invocations that supplied its
/// deterministic replacement text.
struct SourceLineDirectiveLogicalLineRewrite {
  std::string line;
  SmallVector<uint64_t, 4> macroInvocationIds;
};

/// Recovered macro replacement text for a source-only line-control operand.
struct SourceLineDirectiveMacroReplacement {
  std::string text;
  SmallVector<uint64_t, 4> macroInvocationIds;
};

using SourceLineDirectiveBuiltinMacroResolver =
    std::function<std::optional<std::string>(
        const RefoldModel::MacroInvocation &)>;

using SourceLineDirectiveLogicalLineRewriter =
    std::function<std::optional<SourceLineDirectiveLogicalLineRewrite>(
        StringRef, ArrayRef<uint64_t>,
        SourceLineDirectiveBuiltinMacroResolver)>;

/// Return `text` after applying the backslash-newline splice deletion rule.
///
/// The owner-envelope line-control proof operates on a line-spliced logical
/// directive line, while the producer records macro invocation and argument
/// byte ranges in the original source spelling.  When a recorded invocation
/// crosses a literal backslash-newline pair, compare and substitute the
/// post-splice spelling rather than requiring physically contiguous source
/// bytes.
std::optional<std::string> removeLineSplicesForLineControl(StringRef text);

/// Build the directive-logical spelling of one source line after splice
/// deletion.
///
/// Source-level `#line` gaps are parsed from raw file bytes because they do not
/// have ordinary PP tokens in the refold map.  Directive recognition occurs
/// after C line splicing, so a `#` line whose trailing backslash splices the
/// following `line 123` text is the same logical directive as `#line 123`.
/// Block comments inside a directive are recognized before choosing the
/// terminating newline, because comment replacement turns a complete comment
/// into one whitespace character; physical newlines inside such comments are
/// therefore part of the directive spelling, not the directive terminator. This
/// helper removes only literal backslash-newline and backslash-CRLF pairs that
/// are fully contained in the proved source gap, then returns the byte just
/// after the terminating non-spliced newline.
bool collectLineSpliceLogicalLine(
    StringRef fileText, uint64_t lineBegin, uint64_t limit,
    std::string &logicalLine, uint64_t &afterLine,
    SmallVectorImpl<uint64_t> *logicalLineSourceOffsets = nullptr);

/// Return the raw source offset from which physical newlines inside a proved
/// line-control directive affect the resumed line state.
///
/// `collectLineSpliceLogicalLine()` removes backslash-newline splices before
/// the directive is parsed, but not every removed physical newline advances the
/// presumed line established by the directive.  Splices that merely form the
/// directive introducer itself, such as `#\nline 123` or `#li\nne 123`, are
/// consumed before the line operand is recognized and do not add to the resumed
/// value.  Physical newlines after the directive has reached its operand field,
/// including line continuations inside the filename macro argument and newlines
/// inside block comments, do advance the line observed by the copied suffix.
///
/// The returned offset is just after the whitespace that separates the
/// directive keyword/marker from its line operand.  Counting physical LF bytes
/// from that raw offset through the collected directive line, minus the
/// terminating directive newline, gives exactly the internal physical-line
/// advance that must be added to the parsed line number.
std::optional<uint64_t> sourceLineDirectiveResumeCountBegin(
    StringRef logicalLine, ArrayRef<uint64_t> logicalLineSourceOffsets);

/// Count physical source lines inside a complete line-control directive that
/// are part of the directive body rather than the terminating directive
/// newline.
size_t countLineControlDirectiveBodyPhysicalNewlines(StringRef fileText,
                                                     uint64_t countBegin,
                                                     uint64_t afterLine);

/// Return true if the source prefix may already have changed the presumed
/// line-control state before a zero-token `__LINE__` operand is encountered.
///
/// Predefined `__LINE__` expands to the current presumed line number, not
/// blindly to the physical file line.  The refold map records the builtin
/// invocation site, but not a first-class line-control state snapshot for that
/// site.  Therefore this local proof uses the physical line number only when
/// the same source file prefix contains no earlier line-control directive.
/// Inside a source gap, later directives are handled by
/// computeSourceLineDirectiveGapResume()'s running state before the builtin is
/// substituted.
bool sourcePrefixMayContainLineControlDirective(StringRef fileText,
                                                uint64_t limit);

// #line filename string-literal parsing is declared in
// RefoldLineControlFilename.h and shared with include replay.
/// Parse one source-spelled line-control directive logical line.
///
/// The refold map does not currently record line-control directives as
/// first-class artifacts because they contribute no ordinary PP tokens.  Mixed
/// source-envelope proofs therefore recover this narrow directive shape
/// directly from the source bytes when a source-only line-control line lies
/// between otherwise required owner pieces.
///
/// The input is a line-spliced logical line: any C backslash-newline
/// splices have already been removed by the caller.  This parser is still
/// intentionally stricter than the general preprocessor grammar: it accepts
/// only source-spelled `#line <digits> ["file"]` and the Clang/GCC numeric
/// line-marker spelling `# <digits> ["file"]`, both with optional horizontal
/// whitespace.  Numeric line-marker flags are accepted only for the replayable
/// flag forms that can be re-emitted as the local resume directive;
/// macro-dependent spellings and include-stack-pop flag forms remain outside
/// this proof class.
std::optional<ParsedSourceLineDirectiveLogicalLine>
parseSourceLineDirectiveLogicalLine(StringRef line,
                                    StringRef currentFileSpelling);

/// Return true iff a macro invocation produced ordinary PP material that would
/// need the normal macro-realization machinery rather than line-control repair.
bool sourceLineDirectiveMacroHasMaterializedPPTokens(
    const RefoldModel::MacroInvocation &macro);

/// Return true iff the untouched suffix may observe the current presumed file.
///
/// Volatile string-valued predefined macros such as `__DATE__` can be valid
/// filename operands in a source-only line-control directive, but the refold
/// map does not record their concrete expansion value.  Such a gap is still
/// token-preservable when the remaining source can only observe the resumed
/// line number.  This conservative predicate rejects any later `__FILE__` or
/// `__FILE_NAME__` use recorded by the producer, and also rejects raw source
/// spellings of those builtins in the suffix as a fail-closed backstop for
/// cases not represented by an invocation with a usable source range.
bool sourceSuffixMayObservePresumedFileSpelling(
    const RefoldModel &model, StringRef file, uint64_t resumeOffset,
    const RefoldPathIdentity &paths, StringRef fileText);

/// Return the end offset of an admitted raw backslash escape in an argument
/// that is about to be macro-stringified for a line-control filename operand.
///
/// C macro stringification only doubles backslashes that occur inside string or
/// character literals.  A backslash that appears as its own preprocessing-token
/// spelling outside a literal is copied into the generated filename string
/// literal verbatim.  This helper admits exactly the raw escape spellings that
/// parseLineControlFilenameLiteral() can decode and
/// formatSourceLineDirectiveGapResume() can replay without guessing:
/// * warningful ordinary unknown escapes;
/// * escaped quote, question mark, backslash, and horizontal whitespace; and
/// * bounded octal/hexadecimal escapes that decode to replayable filename
///   bytes;
/// * named C escapes whose decoded bytes can be replayed exactly by the #line
///   formatter; and
/// * universal-character names whose decoded scalar can be replayed as UTF-8.
///
/// Invalid universal-character names remain fail-closed.  The admitted subset
/// is deliberately limited to valid Unicode scalars outside the basic/control
/// character range so replay can preserve the filename bytes without guessing.
std::optional<size_t>
findAdmittedLineControlRawStringifyEscapeEnd(StringRef text,
                                             size_t backslashPos);

/// Produce the string literal spelling for a macro argument used by the `#`
/// operator inside a source-only line-control directive.
///
/// This is the small, proof-local subset of C macro stringification needed for
/// line-control operands.  It mirrors the observable pieces of the preprocessor
/// stringification rule that can be proved from the complete recorded argument
/// spelling: trim leading/trailing whitespace, collapse each run of inter-token
/// whitespace to one space, preserve string/character literal token spelling,
/// and finally quote the resulting character sequence as a C string token.
///
/// Complete block comments are modeled as the single whitespace character
/// produced before macro replacement, even when the comment body spans physical
/// source lines.  A raw backslash outside a literal is admitted only when the
/// resulting filename string-literal escape is decoded by the bounded
/// line-control filename parser above; all other escape forms fail closed and
/// must be handled by a wider proof or terminal fallback.  Named C escapes are
/// admitted because the parser decodes them to exact bytes and the #line
/// formatter knows how to spell those bytes again.  Universal-character names
/// are admitted only when their decoded scalar can be replayed as UTF-8 bytes.
std::optional<std::string>
stringifyLineControlMacroArgument(StringRef argument);

/// Return true iff a recorded macro argument contains at least one
/// preprocessing token after the local comment-to-whitespace rule.
///
/// `__VA_OPT__` is controlled by whether the variadic tail contains any
/// preprocessing tokens, not by whether the raw argument spelling is non-empty.
/// Use Clang's raw lexer for token recognition so comments, escaped-newline
/// trivia, and string/character/raw literals follow the same lexical rules as
/// the rest of the refold proof.  Line comments and malformed block comments
/// still fail closed because they are not local token-empty trivia inside this
/// single-line replacement-list proof.
std::optional<bool>
lineControlArgumentContainsPPTokens(StringRef argument,
                                    const LangOptions &lang);

/// Return the raw payload inside one `__VA_OPT__(...)` occurrence.
///
/// Clang's raw lexer owns the delimiter and literal/comment boundaries here.
/// The helper only verifies that `openParen` starts the operator argument list
/// and then tracks nested parentheses until the matching close-paren.  The
/// payload is not interpreted here; after `__VA_OPT__` is erased or exposed,
/// the normal bounded line-control replacement-list evaluator still performs
/// argument substitution, stringification, paste-witness replay, and final
/// strict #line parsing.
std::optional<std::pair<StringRef, size_t>>
parseLineControlVaOptPayload(StringRef replacement, size_t openParen,
                             const LangOptions &lang);

/// Expand top-level `__VA_OPT__(...)` operators in a recorded replacement list.
///
/// This is not a general macro expander.  It implements only the local C99/C23
/// variadic gate needed before the existing line-control evaluator runs: if the
/// producer-recorded variadic tail contains tokens, expose the `__VA_OPT__`
/// payload; otherwise erase it.  The exposed payload is then processed by the
/// normal proof path, so unsupported contents still fail closed at argument
/// substitution, paste-witness replay, or the final strict line-control parse.
std::optional<std::string> expandLineControlVaOptOperators(
    StringRef replacement, ArrayRef<RefoldModel::MacroDefParam> params,
    ArrayRef<StringRef> invocationArgTexts, const LangOptions &lang);

/// Substitute invocation arguments through a simple function-like replacement
/// list used inside a source-only line-control directive.
///
/// This is intentionally a token-spelling substitution, not a general macro
/// expander.  The caller supplies exact source spellings for complete
/// invocation arguments that are wholly inside the already-proved logical
/// directive line. Direct formal substitution is supported, and `# formal` is
/// supported through the conservative line-control stringification helper
/// above.  The resulting replacement text is still parsed by the strict
/// line-control parser, so any unsupported macro semantics remain fail-closed.
std::optional<std::string> substituteLineControlMacroParameters(
    StringRef replacement, ArrayRef<RefoldModel::MacroDefParam> params,
    ArrayRef<StringRef> invocationArgTexts,
    ArrayRef<RefoldModel::PasteToken> pasteTokens, const LangOptions &lang);

/// Recover a simple macro replacement spelling that can participate in a
/// source-only line-control directive.
///
/// This is intentionally not a general macro expander.  The refold map records
/// the macro invocation, the defining directive, and exact invocation argument
/// byte ranges, but it does not record a dedicated expanded token stream for
/// #line directive operands.  Therefore the repair only substitutes complete
/// callsites whose replacement list is spelled directly in the recorded #define
/// text.  For function-like macros with parameters, direct formal-token
/// substitution, conservative `# formal` stringification, and
/// producer-witnessed
/// `##` paste runs are allowed.  Variadic formals use the producer-recorded
/// tail argument as an ordinary substitution operand; comments, multiline
/// replacement lists, and any resulting non-line-control spelling fail closed
/// when the strict parser is re-run on the rewritten directive line.
std::optional<std::string> simpleLineControlMacroReplacementText(
    const RefoldModel &model, const RefoldModel::MacroInvocation &macro,
    ArrayRef<StringRef> invocationArgTexts, const LangOptions &lang);

/// Find whole-token occurrences of `name` inside recovered replacement text.
///
/// Recursive line-control macro repair never treats these textual occurrences
/// as independent expansion candidates.  The producer has already recorded the
/// child macro invocations that occurred while expanding the parent macro; this
/// helper only recovers the corresponding byte ranges in the parent's recovered
/// replacement-list spelling.
SmallVector<std::pair<size_t, size_t>, 4>
findLineControlMacroNameOccurrences(StringRef text, StringRef name);

/// Locate the replacement-list occurrence that corresponds to one recorded
/// child macro invocation.
///
/// The earlier proof required the child invocation spelling to occur exactly
/// once in the parent's recovered replacement text.  That was sound but too
/// narrow for definitions such as `#define FILE_NAME F F`: the refold map
/// records both child invocations, and a sound correspondence disambiguates
/// the two identical `F` spellings.  When repeated spellings are present,
/// require a one-to-one correspondence between recorded same-spelling children
/// and textual whole-token occurrences, then assign them soundly: if every such
/// sibling expands to the identical text the assignment is order-independent
/// (any bijection is correct), and otherwise the siblings must all be body
/// invocations spelled within the parent's own `#define` (source order ==
/// replacement-list order).  \p siblingExpandedText returns a sibling's already
/// computed line-control expansion so the identical-expansion path can be
/// proven.  Any unrecorded extra occurrence, missing source range, argument or
/// cross-file reordering, or other ambiguity still fails closed.
std::optional<std::pair<size_t, size_t>> findLineControlMacroOccurrenceForChild(
    const RefoldModel &model, const RefoldModel::MacroInvocation &parent,
    const RefoldModel::MacroInvocation &child, StringRef text,
    llvm::function_ref<std::optional<llvm::StringRef>(
        const RefoldModel::MacroInvocation &)>
        siblingExpandedText);

/// Recover invocation argument spellings for a macro invocation that happened
/// while expanding another macro replacement list.
///
/// The producer records `normalized_inv_text` for macro calls after macro
/// argument prescan.  When the child call itself has parameters, those
/// normalized argument ranges are the only source-local spellings this narrow
/// line-control proof can use without running a full preprocessor.  If any
/// argument is missing or malformed, the caller fails closed.
std::optional<SmallVector<StringRef, 4>>
lineControlNestedMacroInvocationArgTexts(
    const RefoldModel::MacroInvocation &macro);

/// Recursively expand the bounded macro DAG recorded for a line-control
/// operand.
///
/// This is still not a general macro expander.  It starts from the same simple
/// replacement-list recovery used for directly spelled macro operands, then
/// follows only producer-recorded child macro invocations whose
/// `caller_macro_id` is the macro being expanded.  Each child must occur
/// unambiguously as a whole identifier in the current replacement text and must
/// itself satisfy the same simple line-control replacement rules, or be a
/// recorded predefined builtin accepted by the caller's line-control builtin
/// resolver.  The builtin hook is needed for forms such as `#line
/// LINE_NO(__LINE__)`: after substituting `LINE_NO(x)` with its argument, the
/// producer-recorded child `__LINE__` invocation is the only deterministic
/// witness for the numeric operand. Cycles, unsupported macro operators,
/// ambiguous repeated child spellings, and excessive expansion depth all fail
/// closed before the strict line-control parser sees the rewritten directive.
std::optional<SourceLineDirectiveMacroReplacement>
expandLineControlMacroReplacementText(
    const RefoldModel &model, const RefoldModel::MacroInvocation &macro,
    ArrayRef<StringRef> invocationArgTexts,
    SmallVectorImpl<uint64_t> &activeMacroIds, const LangOptions &lang,
    SourceLineDirectiveBuiltinMacroResolver builtinMacroResolver = nullptr);

/// Substitute simple recorded macro invocations into one source-only
/// line-control logical line.
///
/// The substitution is used only as a precursor to the strict line-control
/// parser.  It accepts complete callsites whose recorded source spelling maps
/// into the line-spliced logical line after deleting literal backslash-newline
/// splices, and whose replacement text is recovered from a simple recorded
/// #define.  Partial callsites, nested/overlapping source ranges, unsupported
/// parameterized replacement semantics, and unrecorded expansions all fail
/// closed.  Variadic formals are allowed only as ordinary recorded argument
/// substitutions: the producer has already collapsed the variadic tail into the
/// matching argument range, and the final strict line-control parse remains the
/// authority on whether the substituted spelling is a valid directive operand.
std::optional<SourceLineDirectiveLogicalLineRewrite>
rewriteSourceLineDirectiveLogicalLineMacros(
    const RefoldModel &model, StringRef file, StringRef logicalLine,
    ArrayRef<uint64_t> logicalLineSourceOffsets,
    const RefoldPathIdentity &paths, const LangOptions &lang,
    std::optional<uint64_t> ownerIncludeId = std::nullopt,
    SourceLineDirectiveBuiltinMacroResolver builtinMacroResolver = nullptr);

/// Format the local resume directive for a consumed source-only line-control
/// gap.  Canonical #line spelling is used unless the consumed state carries
/// numeric line-marker flags that must be replayed to preserve the suffix
/// state.
std::string
formatSourceLineDirectiveGapResume(const SourceLineDirectiveGapResume &resume);

/// Prove that a source gap is made only of trivia plus complete source-spelled
/// `#line` directives, and compute the logical file/line state at
/// `resumeOffset`.
///
/// The returned directive state is the state that must be re-established before
/// the untouched suffix beginning at `resumeOffset` is copied.  Counting
/// continues from the last accepted `#line` directive through the consumed
/// source interval, so a directive before a consumed include line is rewritten
/// to the line that the suffix would have observed after that include line had
/// executed.
std::optional<SourceLineDirectiveGapResume> computeSourceLineDirectiveGapResume(
    StringRef fileText, uint64_t gapBegin, uint64_t gapEnd,
    uint64_t resumeOffset, StringRef defaultFileSpelling,
    SourceLineDirectiveLogicalLineRewriter logicalLineRewriter = nullptr,
    SmallVectorImpl<uint64_t> *acceptedMacroInvocationIds = nullptr,
    StringRef baseFileSpelling = StringRef(),
    bool allowUnknownFilenameOperand = false);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_SOURCELINEDIRECTIVEHELPERS_H
