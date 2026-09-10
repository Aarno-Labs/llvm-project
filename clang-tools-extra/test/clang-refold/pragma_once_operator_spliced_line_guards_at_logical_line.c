// RUN: %clang-refold-tester-verify-off pragma_once_operator_spliced_line_guards_at_logical_line
// Regression: the guard directive written over a `_Pragma("once")` operator is
// placed by translated logical line, not by physical line -- for every spelling
// of a line splice, not for the one that was thought of first.
//
// Both headers here put their operator first on its physical line, and in both
// that line is the continuation of the one above:
//
//     int from_spliced_op = 0 + \        <- plain backslash-newline
//       _Pragma("once")
//
//     int from_crlf_op = 0 + \           <- backslash, then the `\n\r` pairing
//       _Pragma("once")
//
// Deciding line ownership with the last `'\n'` before the site reads "nothing
// precedes the operator" and writes the synthetic guard `#define` straight over
// its bytes.  (The guard's reserved name prefix is deliberately not spelled in
// this comment: the catalog refuses a translation unit whose own source already
// contains it.)  Phase two then deletes the escaped newline and the two
// physical lines become one, so the emitted `#` is mid-line and introduces no
// directive at all -- the guard is never defined, `#` and `define` become
// ordinary tokens, and the second include re-emits the body.
//
// The repair is to open a line for the directive whenever the site does not
// begin a translated logical line.  The newline the original backslash consumes
// is the physical one that was already there; the newline added here is the one
// that survives, so each `#define` below starts a logical line of its own.  It
// costs the same single line of body drift a mid-line operator already costs,
// admissible under the suffix line-observer proof the `#ifndef` prologue needs.
//
// The second header is here because the first is not enough.  A hand-written
// scan for the splice paired `\r\n` but not `\n\r`, which Clang folds into one
// escaped newline just the same, so that spelling still emitted a mid-line
// `#`.  The predicates now ask Clang instead -- a raw `Lexer` over the text
// plus a sentinel, reading `Token::isAtStartOfLine()` -- so the set of splice
// spellings is the compiler's rather than a list maintained here.
// pragma_once_operator_trigraph_spliced_line_guards_at_logical_line.c is the
// third spelling, `??/`, which needs `-trigraphs` on the producer and so has a
// test of its own.
//
// The headers indent their operators on purpose.  With no leading white-space
// the producer reports the callback location inside the spliced first line and
// records no operator range at all, so the catalog cannot bind the once record
// and the header fails closed on `UnboundPragmaRecord` before any of this is
// reached.  That shape is sound today for a reason that has nothing to do with
// line placement, and pinning the placement rule needs the one the producer
// does bind.
//
// The companion pragma_once_operator_midline_guards_at_include_site.c covers an
// operator that shares its line with ordinary source.  There the physical and
// logical answers agree, so that shape never reached this defect and is left
// unchanged by the repair.
//
// Output verification is deliberately off: the assertion is that the planner
// places the directives correctly on its own.  Under `--verify-output=fatal`
// the closing check catches a spliced `#` as a token mismatch and the tool
// exits non-zero, which would make this a test of the verifier instead.
#include "guard_once_operator_spliced.h"
#include "guard_once_operator_spliced_nr.h"

int mid = 0;

#include "guard_once_operator_spliced.h"
#include "guard_once_operator_spliced_nr.h"

int tail = SPLICED_OP_V + CRLF_OP_V;
