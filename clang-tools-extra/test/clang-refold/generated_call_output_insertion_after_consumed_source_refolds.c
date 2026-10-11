// RUN: %clang-refold-tester generated_call_output_insertion_after_consumed_source_refolds
// An insertion after a generated call's output is made in source after the
// source the call consumed.  `CALL(G, x)` leaves `G(x` open and the second `)`
// closes it.  B appends ` = 1` to `g_x`, and `CALL(G, x)) = 1;` preprocesses
// to B with both macros kept.
//
// The insertion lies outside every macro cover.  It is offered to CALL only
// because G's name came from CALL's actual, which opens the boundary to a
// proof that rewrites that callee selector.  No such proof applies, and an
// owner that can only replace its callsite does not need the boundary: the
// TU edit at the gap, before the `;` after the second `)`, keeps every call.
// After CALL's own `)` the inserted tokens would have joined G's argument.
#define G(v) g_##v
#define CALL(f, a) int a; int f(a
CALL(G, x));
