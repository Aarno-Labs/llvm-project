// RUN: %clang-refold-tester-verify-off recursive_tuple_generated_callee_keeps_descendant_stringified_observer
// The recursive tuple replay may take a token outside its replayed surfaces as
// fixed only when nothing that produced the token observes the tuple formal.
//
// `STR(PAIR)` stringifies the whole tuple.  B renames both elements but keeps
// "(x, z)", which no rewrite of `PAIR` reproduces.  The coverage certificate
// accepted the literal because B had not changed it, so this used to emit
// `CALL(ADD, (y, w))` at verify-off.
#define ADD(a, b) int a; int b;
#define STR(v) #v
#define CALL(F, PAIR) F PAIR char *s = STR(PAIR);
CALL(ADD, (x, z))
