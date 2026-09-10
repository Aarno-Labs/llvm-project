// RUN: %clang-refold-tester-clang-flags-verify-off pragma_once_operator_trigraph_spliced_line_guards_at_logical_line -- -trigraphs -I headers/t_trigraph_once
// Regression: the trigraph spelling of a line splice is a line splice, both for
// the once-guard placement rule and for the language options the raw lexer
// decides it with.
//
// This is pragma_once_operator_spliced_line_guards_at_logical_line.c with the
// backslash written `??/`:
//
//     int from_trigraph_op = 0 + ??/
//       _Pragma("once")
//
// Under `-trigraphs` those two physical lines are one logical line, exactly as
// they are with a backslash, so the guard `#define` must again be given a line
// of its own or it is spliced mid-expression and introduces no directive.
//
// Placing it correctly needed a fix one layer below the placement rule.  Every
// splice predicate in RefoldPreprocessingDirectiveScanner has a trigraph arm
// guarded on `LangOptions::Trigraphs`, and that flag was false for every
// translation unit: makeRefoldLexLangOptions built its options from the
// recorded language token alone -- `-x c` -- discarding the producer's own cc1
// command line and with it `-ftrigraphs`, `-std=`, and every other option that
// moves a lexical boundary.  Each trigraph arm was therefore unreachable, and
// the backslash test could not have caught that: it never consults one.
//
// The options are now replayed from the recorded argv, so this asserts both
// halves at once -- that the placement rule is stated in logical lines, and
// that the lexer is configured to agree with the producer about where one ends.
//
// Output verification is off so the assertion is on the planner; see the
// backslash companion for why.
#ifndef __CLANG_REFOLD_ONCE_1
int from_trigraph_op = 0 + ??/
  
#define __CLANG_REFOLD_ONCE_1
9;
#define TRIGRAPH_OP_V 4
int trigraph_op_use = TRIGRAPH_OP_V;
#endif

int mid = 0;

#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#include "guard_once_operator_trigraph.h"
#endif

int tail = TRIGRAPH_OP_V;
