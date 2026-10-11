// RUN: %clang-refold-tester-verify-off generated_call_source_argument_keeps_descendant_stringified_observer
// ADD's `b` is read from the source after CALL's callsite.  ADD copies it and
// passes it to STR, which stringifies it.  B edits only the copy.  ADD's own
// occurrences of `b` agree with that edit; the stringification is STR's, and
// what refuses the in-place rewrite of `2` is that the unchanged `"2"` depends
// on `b` through STR's actual.  The callsite is expanded.
#define STR(x) #x
#define ADD(a, b) a + b + sizeof STR(b)
#define CALL(f, a) int k = f(a
int k = 1 + 7 + sizeof "2";
