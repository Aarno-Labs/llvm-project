// RUN: %clang-refold-tester-verify-off generated_call_source_argument_keeps_pasted_observer
// ADD's `b` is read from the source after CALL's callsite, and ADD both copies
// it and pastes it into `g_2`.  B edits only the copy.  The copy alone would
// make `b` `7`, but the paste would then spell `g_7`, so `b`'s every
// occurrence must be replayed before the `2` is rewritten in place.  None
// reproduces B, and the callsite is expanded.
#define ADD(a, b) a + b + g_##b
#define CALL(f, a) int k = f(a
int g_2 = 0;
CALL(ADD, 1), 2);
