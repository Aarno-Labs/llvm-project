// RUN: %clang-refold-tester-with-lines macro_arg_rewrite_arg_reused_in_expansion_args_only

// test64: Arg appears multiple times in expansion; still should be args-only.
#define TWICE(T, name) T name##_a; T name##_b;
struct S64 { TWICE(float, field) };
int main() { struct S64 s; s.field_a = 1; s.field_b = 2; return s.field_a + s.field_b; }
