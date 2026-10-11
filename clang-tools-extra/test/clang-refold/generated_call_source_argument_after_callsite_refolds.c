// RUN: %clang-refold-tester generated_call_source_argument_after_callsite_refolds
// An edit to an argument that a generated call reads from source after the
// callsite is made in place.  `CALL(ADD, 1)` leaves `ADD(1` open and `, 2)`
// completes it, so the `2` is ADD's `b`, spelled in source and copied once into
// the output.  B edits it, and `CALL(ADD, 1), 7);` preprocesses to B with both
// macros kept.
//
// ADD's recorded invocation text, `ADD, 1), 2)`, runs from the `ADD` that CALL
// received to the closing `)`, so it is not a callsite and args-only cannot
// rewrite it.  The producer records where ADD read `b` from instead
// (`use_arg_ranges`) and credits the copy to `b`.  The fold is admitted only
// because that copy is `b`'s one occurrence, it replays B, no other token of
// CALL's cover depends on `b`, and nothing else is spelled in the bytes
// rewritten.
#define ADD(a, b) a + b
#define CALL(f, a) int k = f(a
CALL(ADD, 1), 2);
